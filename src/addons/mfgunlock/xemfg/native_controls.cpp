// SPDX-License-Identifier: MIT
// Own overlay/control implementation. No OptiScaler code is incorporated.
#include "native_internal.hpp"
#include "native_controls.hpp"
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif
#include "native_overlay.hpp"
#include "backend_api.hpp"
#include "../backend_selection.hpp"
#include "vendor/cuda/cuda_route.h"
#include "vendor/sdk_pacing.h"
#include "../panel_ui.hpp"
#include <sstream>

namespace mfgunlock::xemfg::native {
namespace {
std::mutex settingsLock;
ControlSettings settings;
std::atomic<bool> settingsSaved{true};
std::atomic<bool> invalidConfiguration{};
std::atomic<bool> forceVSyncOff{};
std::atomic<bool> exclusiveAsBorderless{};
std::atomic<bool> uiStabilization{};
auto Ini() { return backend::Directory()/L"MFGAdaUnlock.ini"; }
bool ReadChoice(const wchar_t* section,const wchar_t* key,const wchar_t* choice) {
  wchar_t value[64]{};
  GetPrivateProfileStringW(section,key,L"Auto",value,64,Ini().c_str());
  return _wcsicmp(value,choice)==0;
}
bool ReadBool(const wchar_t* section,const wchar_t* key,bool fallback) {
  wchar_t value[32]{}; GetPrivateProfileStringW(section,key,L"",value,32,Ini().c_str());
  if(!value[0]) return fallback;
  if(!_wcsicmp(value,L"1") || !_wcsicmp(value,L"true") || !_wcsicmp(value,L"on")) return true;
  if(!_wcsicmp(value,L"0") || !_wcsicmp(value,L"false") || !_wcsicmp(value,L"off")) return false;
  invalidConfiguration=true; return fallback;
}
}
void LoadControls() {
  std::lock_guard guard(settingsLock);
  invalidConfiguration=false;
  if(!ReadChoice(L"Latency",L"Input",L"Auto") && !ReadChoice(L"Latency",L"Input",L"Reflex")) invalidConfiguration=true;
  if(!ReadChoice(L"Latency",L"Output",L"Auto") && !ReadChoice(L"Latency",L"Output",L"Reflex") && !ReadChoice(L"Latency",L"Output",L"XeLL")) invalidConfiguration=true;
  settings.input=ReadChoice(L"Latency",L"Input",L"Reflex")?LatencyInput::StreamlineReflex:LatencyInput::Auto;
  settings.queueAhead=ReadBool(L"XeFG",L"MeteredQueueAhead",true);
  sdk_pacing::RequestQueueAhead(settings.queueAhead);
  settings.output=ReadChoice(L"Latency",L"Output",L"Reflex")?LatencyOutput::Reflex:
      ReadChoice(L"Latency",L"Output",L"XeLL")?LatencyOutput::XeLL:LatencyOutput::Auto;
  State().latencyOutput=settings.output==LatencyOutput::Reflex?LatencyOutput::Reflex:LatencyOutput::XeLL;
  // Kept in the settings ABI for older callers. XeMFG never changes window mode.
  settings.forceBorderless=false;
  settings.uiComposition=ReadBool(L"XeFG",L"UIComposition",false);
  settings.uiStabilization=ReadBool(L"XeFG",L"UIStabilization",false);
  uiStabilization=settings.uiStabilization;
  sdk_pacing::RequestNativeLatency(GetPrivateProfileIntW(L"XeFG",L"XeFGNativeLatency",0,Ini().c_str()));
  settings.sdkDeadlines=ReadBool(L"XeFG",L"SdkDeadlines",false);
  sdk_pacing::RequestSdkDeadlines(settings.sdkDeadlines);
  settings.forceVSyncOff=ReadBool(L"XeFG",L"ForceVSyncOff",false);
  forceVSyncOff=settings.forceVSyncOff;
  settings.displayCap=ReadBool(L"XeFG",L"DisplayCap",false);
  State().displayCap=settings.displayCap;
  settings.exclusiveAsBorderless=ReadBool(L"XeFG",L"ExclusiveAsBorderless",false);
  exclusiveAsBorderless=settings.exclusiveAsBorderless;
  wchar_t flip[32]{}; GetPrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",L"auto",flip,32,Ini().c_str());
  settings.flipMetering=_wcsicmp(flip,L"off") && _wcsicmp(flip,L"false") && wcscmp(flip,L"0");
  if(_wcsicmp(flip,L"auto") && _wcsicmp(flip,L"on") && _wcsicmp(flip,L"true") && wcscmp(flip,L"1")
      && _wcsicmp(flip,L"off") && _wcsicmp(flip,L"false") && wcscmp(flip,L"0")) invalidConfiguration=true;
}
bool ConfigureControls(const ControlSettings& incoming) {
  auto value=incoming;
  value.forceBorderless=false;
  // Reject unknown ABI/enums. The latency output is fixed for this process;
  // changing the preference never replays a game setter on the UI thread.
  if(value.version!=5 || uint32_t(value.input)>1 || uint32_t(value.output)>2) return false;
  std::unique_lock settingsGuard(settingsLock);
  const auto old=settings;
  const auto ini=Ini();
  const bool saved=config::Save(ini,{
    {L"Latency",L"Input",value.input==LatencyInput::Auto?L"Auto":L"Reflex"},
    {L"Latency",L"Output",value.output==LatencyOutput::Auto?L"Auto":value.output==LatencyOutput::Reflex?L"Reflex":L"XeLL"},
    {L"XeFG",L"MeteredQueueAhead",value.queueAhead?L"1":L"0"},
    {L"XeFG",L"XeFGFlipMetering",value.flipMetering?L"on":L"off"},
    {L"XeFG",L"ForceBorderless",value.forceBorderless?L"1":L"0"},
    {L"XeFG",L"UIComposition",value.uiComposition?L"1":L"0"},
    {L"XeFG",L"UIStabilization",value.uiStabilization?L"1":L"0"},
    {L"XeFG",L"SdkDeadlines",value.sdkDeadlines?L"1":L"0"},
    {L"XeFG",L"ForceVSyncOff",value.forceVSyncOff?L"1":L"0"},
    {L"XeFG",L"DisplayCap",value.displayCap?L"1":L"0"},
    {L"XeFG",L"ExclusiveAsBorderless",value.exclusiveAsBorderless?L"1":L"0"}});
  settingsSaved=saved;
  if(!saved) return false;
  settings=value; invalidConfiguration=false; settingsGuard.unlock();
  forceVSyncOff=value.forceVSyncOff;
  State().displayCap=value.displayCap;
  exclusiveAsBorderless=value.exclusiveAsBorderless;
  uiStabilization=value.uiStabilization;
  if(old.exclusiveAsBorderless!=value.exclusiveAsBorderless)
    if(auto output=Active(); output && output->window) output->window->BlockAltEnter(value.exclusiveAsBorderless);
  sdk_pacing::RequestQueueAhead(value.queueAhead);
  if(old.flipMetering!=value.flipMetering) sdk_pacing::RequestFlipMetering(value.flipMetering);
  if(old.sdkDeadlines!=value.sdkDeadlines) sdk_pacing::RequestSdkDeadlines(value.sdkDeadlines);
  xess_mod::WriteLog("native_controls","\"input\":"+std::to_string(uint32_t(value.input))
      +",\"output\":"+std::to_string(uint32_t(value.output))+",\"flip_requested\":"+(value.flipMetering?"true":"false")
      +",\"borderless_requested\":"+(value.forceBorderless?"true":"false")
      +",\"ui_composition_requested\":"+(value.uiComposition?"true":"false")
      +",\"ui_stabilization\":"+(value.uiStabilization?"true":"false")
      +",\"force_vsync_off\":"+(value.forceVSyncOff?"true":"false")+",\"display_cap\":"+(value.displayCap?"true":"false")
      +",\"exclusive_as_borderless\":"+(value.exclusiveAsBorderless?"true":"false")+",\"saved\":"+(saved?"true":"false"));
  return saved;
}
ControlSettings ControlsSettings() { std::lock_guard guard(settingsLock); return settings; }
bool ForceVSyncOff() noexcept { return forceVSyncOff.load(std::memory_order_relaxed); }
bool ExclusiveAsBorderless() noexcept { return exclusiveAsBorderless.load(std::memory_order_relaxed); }
bool UiStabilization() noexcept { return uiStabilization.load(std::memory_order_relaxed); }
ControlStatus ControlsSnapshot() {
  ControlStatus result;
  result.settings=ControlsSettings();
  result.settingsSaved=settingsSaved;
  result.invalidConfiguration=invalidConfiguration;
  mfgunlock::xemfg::Status model;
  result.modelUpdating=!TryModelStatus(model);
  if(!result.modelUpdating) { result.modelPort=model.model!=0; result.modelInitialized=model.initialized!=0; }
  auto& s=State(); const auto now=GetTickCount64();
  result.sleeps=s.latencySleeps; result.markers=s.latencyMarkers;
  result.modeUpdates=s.latencyModeUpdates; result.lastSleepUs=s.lastSleepUs;
  result.reflexObserved=s.inputTick && now-s.inputTick.load()<2000;
  result.activeLatencyOutput=s.latencyOutput;
  const auto wanted=result.settings.output==LatencyOutput::Reflex?LatencyOutput::Reflex:LatencyOutput::XeLL;
  result.latencyRestartPending=wanted!=result.activeLatencyOutput;
  result.reflexOutputObserved=s.reflexOutputTick && now-s.reflexOutputTick.load()<2000;
  result.reflexOptionsReady=s.reflexOptionsReady;
  result.reflexSleeps=s.reflexSleeps; result.xellSleeps=s.xellSleeps;
  result.xellObserved=result.activeLatencyOutput==LatencyOutput::XeLL && s.outputTick && now-s.outputTick.load()<2000;
  result.nativeReflexDisabled=s.reflexOff; result.gameReflexMode=s.gameReflexMode;
  result.gameSyncInterval=s.gameSyncInterval;
  result.displayRefreshMilliHz=s.displayRefreshMilliHz; result.sourceIntervalUs=s.sourceIntervalUs;
  result.displayCapIntervalUs=DisplayCapIntervalForOutput();
  result.hudIssues=s.lastHudIssues; result.hudInputs=s.lastHudInputs; result.hudRejected=s.hudRejected;
  result.hudFiltered=s.lastHudFiltered; result.hudCopied=s.lastHudCopied;
  xefg_cuda::Status cuda;
  result.cudaUpdating=!xefg_cuda::TrySnapshot(cuda);
  result.cudaState=uint32_t(cuda.state); result.cudaLayers=cuda.layers;
  result.cudaMajor=cuda.major; result.cudaMinor=cuda.minor;
  result.cudaLaunches=cuda.launches; result.cudaFallbacks=cuda.fallbacks;
  strncpy_s(result.cudaReason,cuda.reason.c_str(),_TRUNCATE);
  auto output=Active();
  void* pacingHandle{};
  if(output) {
    // The panel must not wait for a presenting thread, so it only tries the
    // lock. When it is busy (often at 4x and above), the last values read
    // from this output are shown instead of defaults; defaults made the HUD
    // and pacing lines, and the rows below them, flicker (1.5.7).
    struct LastRead {
      uint64_t output{}; void* pacing{};
      bool available{},applied{},known{}; int result{}; uint64_t updates{}; bool stabilized{};
    };
    static std::mutex lastLock; static LastRead last;
    std::unique_lock guard(output->io,std::try_to_lock);
    std::lock_guard lastGuard(lastLock);
    if(guard.owns_lock())
      last={output->diagnosticOutput,output->fg,output->uiCompositionAvailable,output->uiCompositionApplied,
          output->uiCompositionKnown,output->uiCompositionResult,output->uiCompositionUpdates,output->uiStabilized};
    if(last.output==output->diagnosticOutput) {
      pacingHandle=last.pacing;
      result.uiCompositionAvailable=last.available;
      result.uiCompositionApplied=last.applied;
      result.uiCompositionKnown=last.known;
      result.uiCompositionResult=last.result;
      result.uiCompositionUpdates=last.updates;
      result.uiStabilized=last.stabilized;
    }
  }
  if(output && output->window) {
    result.window=output->window->Snapshot();
    result.altEnterBlocked=output->window->AltEnterBlocked();
  }
  else if(output) result.window.error=output->windowBindError;
  result.exclusiveServed=output && output->exclusiveServed.load();
  result.exclusiveRequests=s.exclusiveRequests;
  const auto flip=sdk_pacing::Snapshot(pacingHandle);
  result.flipAvailable=flip.available; result.flipInstalled=flip.installed;
  result.flipRegistered=flip.registered; result.flipQueue=flip.queue; result.flipBusy=flip.busy;
  result.flipRequested=flip.requested; result.flipEnabled=flip.enabled;
  result.flipConfigured=flip.configured; result.flipMetered=flip.metered; result.flipRefused=flip.refused;
  result.queueAheadActive=flip.queueAheadActive; result.queueAheadDeadlines=flip.queueAheadDeadlines;
  result.flipOutputs=flip.outputs; result.flipFeedback=flip.feedback; result.flipResult=flip.status; result.flipCalls=flip.calls;
  result.sdkDeadlinesRequested=flip.sdkDeadlinesRequested; result.sdkDeadlinesActive=flip.sdkDeadlinesActive;
  return result;
}
void DrawControls(PanelPage page) {
  const auto status=ControlsSnapshot(); auto value=status.settings;
  const bool metering=status.flipMetered && !status.flipBusy;
#ifdef MFGUNLOCK_XEMFG_VULKAN
  // 1.7 (plan F): the Vulkan host paces with XeLL and the CPU pacer (no NVIDIA flip metering on its device).
  const bool vulkanHost=vulkan::Status().game;
#else
  const bool vulkanHost=false;
#endif
  const char* flip=vulkanHost?"XeFG pacing (Vulkan)":status.sdkDeadlinesActive?"Intel SDK timing (experimental)":status.flipBusy?"Checking...":!status.flipAvailable?"XeFG pacing (driver unavailable)":
      !status.flipRegistered || !status.flipQueue?"Waiting for frame generation":
      status.flipRefused?"XeFG pacing (driver declined)":metering?"NVIDIA driver - active":
      status.flipConfigured?"XeFG pacing (current display mode)":!status.flipEnabled?"XeFG pacing (driver metering off)":"Waiting for a stable frame rate";
  const auto& w=status.window;
  const char* window=!w.available?"Window not available":w.mode==DisplayMode::Unknown?"Unknown - no mode change":
      w.mode==DisplayMode::Exclusive?"Exclusive fullscreen":w.mode==DisplayMode::NativeBorderless?"Borderless (game controlled)":
      w.mode==DisplayMode::ManagedBorderless?"Borderless (addon controlled)":"Windowed";
  const char* acceleration=status.modelUpdating || status.cudaUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":!status.modelPort?"Native XeFG model":
      status.cudaState==1 && status.cudaLaunches?"NVIDIA Tensor Cores (CUDA)":
      status.cudaState==1?"NVIDIA acceleration ready":"Compatibility shaders";
  constexpr const char* flipHelp="Uses the NVIDIA driver to space generated frames evenly. Leave enabled for driver pacing when available; otherwise XeFG uses its own pacing. Changes apply at the next frame group. Active reports driver acceptance, not a monitor scanout measurement.";
  constexpr const char* compositionHelp="Off by default: XeFG interpolates the HUD with the image. Enable only if HUD interpolation causes artifacts. Composition needs HUD-less color from the game and updates the HUD at the real frame rate, so moving HUD elements may look less smooth. Changes apply at the next source frame.";
  constexpr const char* stabilizationHelp="Experimental, off by default. For games that do not provide HUD-less color: pixels that stay unchanged for a few real frames (static HUD text, icons and the minimap frame) are composed from the newest real frame over every generated frame instead of being interpolated, which removes their flicker. Moving or semi-transparent HUD elements are still interpolated. Costs one copy and one small compute pass per real frame. Also works in games that provide HUD-less color, as an alternative to UI Composition: UI Composition updates the whole HUD at the real frame rate, while this keeps moving HUD interpolated at the output rate. When on, it replaces UI Composition.";
  const char* composition=!status.uiCompositionKnown?"Waiting for frame status":status.uiStabilized?"HUD stabilization (static HUD composed)":status.uiCompositionApplied?"UI composition":
      value.uiComposition && !status.uiCompositionAvailable?"Waiting for HUD-less input":
      value.uiComposition?"Change pending":value.uiStabilization && !status.uiCompositionAvailable?"Waiting for HUD stabilization":"UI interpolation (default)";
  if(page==PanelPage::General) {
    ui::Section("Smoothness and display");
    ui::Card card("##xe_presentation");
    ui::Setting("##xe_flip_row","Driver flip metering",flipHelp,[&] {
      ImGui::BeginDisabled(value.sdkDeadlines);
      if(ImGui::Checkbox("##xe_flip",&value.flipMetering)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    ui::Readout("##xe_flip_state","Pacing status",flip,flipHelp,metering?ui::Accent:ui::Muted);
    if(!value.sdkDeadlines && status.flipRequested!=status.flipEnabled) ui::Description("Change pending - applies with the next group of generated frames.");
    constexpr const char* vsyncHelp="Presents the game's frames with sync interval 0, as with VSync off in the game. Use it for games whose VSync cannot be turned off in their settings, such as Elden Ring: game VSync keeps driver flip metering and generated-frame pacing from applying. Tearing is not enabled. Applies at the next frame.";
    ui::Setting("##xe_vsync_row","Force game VSync off",vsyncHelp,[&] {
      if(ImGui::Checkbox("##xe_force_vsync_off",&value.forceVSyncOff)) ConfigureControls(value);
    });
    const auto sync=status.gameSyncInterval;
    const std::string gameVSync=sync==UINT32_MAX?"Not observed yet":sync==0?"Off":value.forceVSyncOff?"On (forced off)":"On";
    ui::Readout("##xe_vsync_state","Game VSync",gameVSync.c_str(),vsyncHelp,sync && sync!=UINT32_MAX && !value.forceVSyncOff?ui::Warning:ui::Muted);
    // 1.5.5: warn instead of changing it; the user decides (no profile does).
    if(sync && sync!=UINT32_MAX && !value.forceVSyncOff && State().requested.load())
      ui::Notice("The game presents with VSync, so frame pacing does not apply to generated frames. "
          "Turn off VSync in the game's settings. If the game has no VSync setting, turn on Force game VSync off.");
    // 1.5.7: optional cap to the display refresh (XeLL output only).
    constexpr const char* capHelp="Limits the game's frame rate so that, with the multiplier, frames are presented slightly below the display refresh (3% margin, for variable-refresh displays). Without a limit, a high game frame rate presents generated frames faster than the display shows them, and motion becomes uneven. The game's own frame limit still applies when it is lower. Available with the XeLL latency output. Applies at the next frame.";
    const bool xellOutput=status.activeLatencyOutput==LatencyOutput::XeLL;
    ui::Setting("##xe_display_cap_row","Limit to display refresh",capHelp,[&] {
      ImGui::BeginDisabled(!xellOutput);
      if(ImGui::Checkbox("##xe_display_cap",&value.displayCap)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    const auto refreshHz=status.displayRefreshMilliHz/1000;
    const std::string capState=!xellOutput?"Not available with the Reflex output":!value.displayCap?"Off":
        !status.displayCapIntervalUs?"Waiting for the display refresh":
        "Up to "+std::to_string(1000000/status.displayCapIntervalUs)+" FPS shown, game up to "
        +std::to_string(1000000/status.displayCapIntervalUs/(std::max)(State().multiplier.load(),1u))+" FPS ("+std::to_string(refreshHz)+" Hz display)";
    ui::Readout("##xe_display_cap_state","Frame rate limit",capState.c_str(),capHelp,value.displayCap && status.displayCapIntervalUs?ui::Accent:ui::Muted);
    // Warn, never enable it automatically (the user decides).
    if(xellOutput && !value.displayCap && State().requested.load() && (!sync || sync==UINT32_MAX)
        && ExceedsDisplay(status.sourceIntervalUs,status.displayRefreshMilliHz,State().multiplier.load()))
      ui::Notice(("The game's frame rate times the multiplier is above the display refresh ("+std::to_string(refreshHz)
          +" Hz), so some generated frames are not shown and motion can look uneven. Turn on Limit to display refresh, or cap the game's frame rate.").c_str());
    if(ui::Expander("Advanced pacing comparison")) {
      ui::Setting("##xe_queue_row","Queue metered frames early","Lets the driver space a verified group of frames while the SDK prepares its next work. GPU fences and resource waits remain owned by Intel. Applies only when the driver accepts flip metering and Present uses sync interval 0. Turn off to compare with the previous CPU timing.",[&] {
        ImGui::BeginDisabled(value.sdkDeadlines || !value.flipMetering);
        if(ImGui::Checkbox("##xe_queue_ahead",&value.queueAhead)) ConfigureControls(value);
        ImGui::EndDisabled();
      });
      ui::Setting("##xe_deadline_row","Use Intel SDK timing (test)","Off keeps the current driver/adaptive path. On keeps Intel's original deadlines and disables driver metering at the next generated group. This experimental comparison does not add a pacer, wait or FPS cap. Keep the same scene and compare displayed intervals and mouse response; turn off to restore the usual path.",[&] {
        if(ImGui::Checkbox("##xe_sdk_deadlines",&value.sdkDeadlines)) ConfigureControls(value);
      });
      ui::Description(status.sdkDeadlinesActive?"Intel SDK timing active (experimental)":"Driver / adaptive timing active");
      if(status.sdkDeadlinesRequested!=status.sdkDeadlinesActive) ui::Description("Waiting for the next generated group.");
    }
    ImGui::Separator();
    // 1.5.8: exclusive fullscreen served as borderless (off by default).
    constexpr const char* exclusiveHelp="Intel frame generation does not support exclusive fullscreen. When the game asks for it, XeMFG keeps the game windowed instead: the window covers the monitor without borders, and the game is told it is in fullscreen. The monitor resolution and refresh rate are not changed; a lower game resolution is scaled to the screen. Alt+Enter is held back so the game cannot switch to exclusive fullscreen. Applies at the game's next display mode change, or after a restart if the game is already in exclusive fullscreen.";
    ui::Setting("##xe_exclusive_row","Exclusive fullscreen as borderless",exclusiveHelp,[&] {
      if(ImGui::Checkbox("##xe_exclusive_borderless",&value.exclusiveAsBorderless)) ConfigureControls(value);
    });
    ui::Readout("##xe_window_state","Display mode",w.pending?"Change pending":status.exclusiveServed?"Borderless (game asked for exclusive fullscreen)":window,
        "The game controls its display mode. Exclusive fullscreen is not supported by the Intel frame-generation SDK; with Exclusive fullscreen as borderless on, XeMFG serves it as borderless.",
        status.exclusiveServed?ui::Accent:w.mode==DisplayMode::Exclusive?ui::Warning:ui::Muted);
    // Suggest it, never enable it automatically (the user decides).
    if(w.mode==DisplayMode::Exclusive && State().requested.load())
      ui::Notice(value.exclusiveAsBorderless?
          "The game is in exclusive fullscreen, so frame generation is off. Switch the game's display mode once (or restart the game) to apply Exclusive fullscreen as borderless.":
          "The game is in exclusive fullscreen, which Intel frame generation does not support. Set the game to Borderless, or turn on Exclusive fullscreen as borderless.");
  }
  if(page==PanelPage::Quality) {
    ui::Section("Image quality");
    ui::Card card("##xe_quality");
    ui::Readout("##xe_model_quality","Model",status.modelUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":status.modelPort?"Intel XeMFG (automatic)":"Intel XeFG (compatibility)",
        "The multi-frame model is selected automatically. DLSS-G quality profiles only affect the NVIDIA backend.");
    ui::Readout("##xe_acceleration_quality","Acceleration",acceleration,
        "CUDA runs the supported model layers on NVIDIA Tensor Cores. Compatibility shaders handle other dispatches. This is the execution route, not a different quality preset.",ui::Accent);
    ImGui::Separator();
    ui::Setting("##xe_composition_row","UI Composition",compositionHelp,[&] {
      ImGui::BeginDisabled(!status.uiCompositionAvailable && !value.uiComposition);
      if(ImGui::Checkbox("##xe_ui_composition",&value.uiComposition)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    ui::Setting("##xe_stabilization_row","HUD stabilization (experimental)",stabilizationHelp,[&] {
      if(ImGui::Checkbox("##xe_ui_stabilization",&value.uiStabilization)) ConfigureControls(value);
    });
    ui::Readout("##xe_composition_state","HUD handling",composition,compositionHelp);
    // 1.5.7: what the game provides, per kind: received, converted, filtered or absent.
    const auto hudKind=[&](unsigned type,const char* name) {
      const uint32_t bit=1u<<type;
      return std::string(name)+(status.hudInputs&bit?(status.hudCopied&bit?": received (converted)":": received")
          :status.hudFiltered&bit?": filtered":": not provided");
    };
    const std::string hudInputs=hudKind(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR,"HUD-less")+" | "+hudKind(XEFG_SWAPCHAIN_RES_UI,"UI");
    ui::Readout("##xe_hud_safety","Game HUD inputs",hudInputs.c_str(),
        "The game's optional HUD textures: HUD-less color (the image without HUD) and UI (the HUD alone). Converted means the texture used another type of the output's format and is copied into the output format. Filtered textures do not match the output (format, size or shader input) and are not used; frame generation then interpolates the HUD with the image. This does not prove matching HDR encoding or remove every model artifact.");
    // Suggest, never enable it automatically (the user decides).
    if(status.uiCompositionAvailable && !value.uiComposition)
      ui::Description("This game provides HUD-less color. If HUD elements show artifacts in generated frames, turn on UI Composition.");
    if(status.uiCompositionResult<0) ui::Notice("UI composition could not be applied. Copy Diagnostics for details.");
    ui::Description("For best results, keep the game's rendered frame rate stable.");
    ui::Tooltip("A higher base frame rate supplies more recent motion information. Generated frames improve smoothness but do not add new input samples.");
  }
  if(page==PanelPage::Latency) {
    ui::Section("Mouse and input response");
    ui::Card card("##xe_latency");
    ui::Readout("##xe_latency_state","Low latency",status.xellObserved?"XeLL - active":status.reflexOutputObserved?"Reflex - active":"Waiting for game timing",
        "The selected output controls latency: native Reflex or XeLL. The other latency limiter is disabled. Active means recent successful calls, not a physical latency measurement.",(status.xellObserved || status.reflexOutputObserved)?ui::Accent:ui::Warning);
    ui::Readout("##xe_latency_source","Timing source",status.reflexObserved?"Reflex / Streamline":"Not observed yet",
        "Uses the game's Reflex timing and markers for the selected latency output. Keep the game's low-latency option enabled when available.");
    ui::Description("Keep the game's low-latency option enabled.");
    ui::Tooltip("Avoid stacking external FPS limiters while comparing responsiveness. Generated FPS and physical input latency measure different things.");
    if(status.activeLatencyOutput==LatencyOutput::Reflex && status.reflexOptionsReady && !status.gameReflexMode) ui::Notice("Enable NVIDIA Reflex in the game to use this latency output.");
    if(status.latencyRestartPending) ui::Notice("Latency output saved. Restart the game to apply it.");
    if(ui::Expander("Advanced latency routing")) {
      int input=int(value.input),output=int(value.output);
      ui::Setting("##xe_input_row","Timing input","Auto uses Reflex / Streamline timing. This bridge currently supports that input route.",[&] {
        if(ImGui::Combo("##xe_timing_input",&input,"Auto (recommended)\0Reflex / Streamline\0")) { value.input=LatencyInput(input); ConfigureControls(value); }
      });
      ui::Setting("##xe_output_row","Latency output","Auto keeps XeLL. Reflex uses the game's native Reflex sleep and options, with the XeLL latency limiter disabled. Keep Reflex enabled in the game's settings. Restart the game after changing this selection.",[&] {
        if(ImGui::Combo("##xe_latency_output",&output,"Auto (XeLL)\0XeLL\0Reflex\0")) { value.output=LatencyOutput(output); ConfigureControls(value); }
      });
    }
  }
  if(page==PanelPage::Diagnostics) {
    const auto bridge=Snapshot();
    const auto overlay=OverlaySnapshot();
    const auto overlayInput=OverlayInputSnapshot();
    const auto ref=reframework::Snapshot();
    ui::Section("System status","Use Copy compatibility report when reporting a problem.");
    ui::Card card("##xe_model_details");
    ui::Readout("##xe_diag_bridge","Game connection",bridge.streamline && bridge.output?"Connected":"Waiting for the game",
        "Checks the Streamline input hooks and DirectX 12 output. Load a scene and enable Frame Generation in the game.");
    ui::Readout("##xe_diag_overlay","ReShade output",overlay.created>overlay.destroyed?"Final output linked":"Automatic hook / waiting",
        "Links ReShade effects and keyboard input to Intel's final output when its private queue is not automatically hooked. Recreated on resize and released before output destruction.");
    ui::Readout("##xe_diag_model","Model",status.modelUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":status.modelPort?"Intel XeMFG - ready":"Intel XeFG (compatibility)");
    ui::Readout("##xe_diag_cuda","Acceleration",acceleration,
        "NVIDIA Tensor Core execution is observed through the CUDA route. Raw counters are included in the copied report.");
    ui::Readout("##xe_diag_flip","Flip metering",flip,flipHelp,metering?ui::Accent:ui::Muted);
    ui::Readout("##xe_diag_latency","Latency output",status.xellObserved?"XeLL - active":status.reflexOutputObserved?"Reflex - active":"Waiting for game timing");
    ui::Readout("##xe_diag_ui","HUD handling",composition,compositionHelp);
    ui::Readout("##xe_diag_window","Display mode",window);
    ui::Readout("##xe_diag_ref","REFramework",reframework::Description(ref),
        "Optional cooperation before replacing the XeMFG swapchain. Older builds continue through the legacy path. This does not repair REFramework's internal hook monitor or certify an unknown version.");
    if(ref.knownMonitorIssue) ui::Notice("This REFramework build was observed repeatedly reinstalling its XeFG hooks. Update to a version with the XeFG monitor fix.");
    if(State().retirementFailed || !reframework::Safe(ref.result)) ui::Notice("Output retirement could not be coordinated safely. Restart the game before creating another XeMFG output.");
    if(w.error) ui::Notice("Window mode could not be applied. Copy the report for details.");
    if(bridge.sdkResult<0) ui::Notice("Frame generation reported an error. Copy the report for details.");
    ImGui::Spacing();
    ui::ButtonRow reports(2);
    if(reports("Copy compatibility report","Copies structured metadata: module hashes, the first unmet frame requirement, input validation and DirectX errors. No resource contents, personal paths or GPU addresses."))
      ImGui::SetClipboardText(CompatibilityReport().c_str());
    if(reports("Copy detailed counters","Copies technical status, error codes and accumulated counters for support. These counters are not displayed FPS or physical input latency.")) {
      mfgunlock::xemfg::Status model;
      const bool modelAvailable=TryModelStatus(model);
      std::ostringstream report; report<<"MFG Unlock 1.7.1\nstartup_failed="<<ModelStartupFailed()
          <<" model_status_available="<<modelAvailable<<" model_detail="<<(modelAvailable?model.detail:"busy")
          <<"\nmodel="<<status.modelPort<<" initialized="<<status.modelInitialized
          <<"\nCUDA="<<status.cudaState<<" compute="<<status.cudaMajor<<'.'<<status.cudaMinor<<" layers="<<status.cudaLayers
          <<" launches="<<status.cudaLaunches<<" fallbacks="<<status.cudaFallbacks<<" detail="<<status.cudaReason
          <<"\nqueue_ahead="<<status.queueAheadActive<<" deadlines="<<status.queueAheadDeadlines
          <<"\nflip="<<flip<<" result="<<status.flipResult<<" feedback="<<status.flipFeedback<<" outputs="<<status.flipOutputs<<" calls="<<status.flipCalls
          <<"\nsdk_deadlines_requested="<<status.sdkDeadlinesRequested<<" active="<<status.sdkDeadlinesActive
          <<"\nhud_inputs="<<status.hudInputs<<" issues="<<status.hudIssues<<" filtered_tags="<<status.hudRejected
          <<"\noverlay_created="<<overlay.created<<" destroyed="<<overlay.destroyed<<" updates="<<overlay.updates<<" failed="<<overlay.failed
          <<"\noverlay_requests="<<overlayInput.requests<<" recovered_taps="<<overlayInput.recovered<<" dropped_taps="<<overlayInput.dropped
          <<"\nwindow="<<window<<" error="<<w.error<<" managed="<<w.managed
          <<"\nlatency_output="<<uint32_t(status.activeLatencyOutput)<<" pending_restart="<<status.latencyRestartPending
          <<" reflex_sleeps="<<status.reflexSleeps<<" xell_sleeps="<<status.xellSleeps
          <<"\nnative_reflex_disabled="<<status.nativeReflexDisabled<<" game_reflex_mode="<<status.gameReflexMode
          <<"\nsleeps="<<status.sleeps<<" markers="<<status.markers<<" mode_changes="<<status.modeUpdates<<" last_sleep_us="<<status.lastSleepUs
          <<"\nui_composition_requested="<<value.uiComposition<<" applied="<<status.uiCompositionApplied<<" known="<<status.uiCompositionKnown
          <<" available="<<status.uiCompositionAvailable<<" result="<<status.uiCompositionResult<<" updates="<<status.uiCompositionUpdates
          <<"\nbridge="<<bridge.detail<<" source_presents="<<bridge.frames<<" incomplete_inputs="<<bridge.rejected
          <<" submissions="<<bridge.submitted<<" SDK_result="<<bridge.sdkResult;
      const auto log=LogSnapshot();
      report<<"\nasync_log_started="<<log.started<<" pending="<<log.pending<<" dropped="<<log.dropped<<" failed_batches="<<log.failed;
      report<<"\nreframework="<<reframework::Description(ref)<<" v1="<<ref.v1<<" known_monitor_issue="<<ref.knownMonitorIssue
          <<" result="<<int(ref.result)<<" raw="<<ref.rawResult<<" version="<<ref.version
          <<"\nreframework_sha256="<<ref.sha256;
      report<<"\n\nCompatibility metadata (JSON v1):\n"<<CompatibilityReport();
      ImGui::SetClipboardText(report.str().c_str());
    }
    if(ui::Expander("Performance capture (advanced)")) {
      bool enabled=TimingEnabled();
      ui::Setting("##xe_cpu_capture","Collect CPU timings","Off by default. Records a bounded window of source Present timing, SDK-call duration and XeLL sleep. Adds profiling overhead; does not measure displayed frames or GPU execution.",[&] {
        if(ImGui::Checkbox("##xe_cpu_enabled",&enabled)) {
          SetTimingEnabled(enabled);
          WritePrivateProfileStringW(L"Diagnostics",L"CpuTiming",enabled?L"1":L"0",Ini().c_str());
        }
      });
      ui::ButtonRow timing(2);
      if(timing("Copy timing summary","Includes CPU p95/p99/p99.9 and reports unavailable display/GPU/physical measurements explicitly. Use PresentMon for displayed intervals."))
        ImGui::SetClipboardText(TimingReport().c_str());
      if(timing("Clear timing window")) ClearTiming();
    }
  }
  if(status.latencyRestartPending) ui::Notice("Latency output saved. Restart the game to apply it.");
  if(!status.settingsSaved) ui::Notice("Change was not applied: settings could not be saved. Check write access to the game folder.");
  if(status.invalidConfiguration) ui::Notice("Some saved settings are invalid. Safe defaults are active; save valid choices to replace them.");
}
}
extern "C" __declspec(dllexport) bool MFGAXeControlsStatus(mfgunlock::xemfg::native::ControlStatus* out,uint32_t size) {
  if(!out || size!=sizeof(*out) || out->version!=5) return false;
  *out=mfgunlock::xemfg::native::ControlsSnapshot(); return true;
}
extern "C" __declspec(dllexport) bool MFGAXeConfigure(const mfgunlock::xemfg::native::ControlSettings* value,uint32_t size) {
  return value && size==sizeof(*value) && mfgunlock::xemfg::native::ConfigureControls(*value);
}
