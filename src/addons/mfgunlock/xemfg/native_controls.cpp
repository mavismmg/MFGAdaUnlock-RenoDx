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
  // 1.10.0 (dashdogy v1.7.1): the metered queue-ahead and SDK-deadline comparison paths are gone with
  // upstream's pacer rewrite; their fields stay in the settings ABI, always off.
  settings.queueAhead=false;
  settings.output=ReadChoice(L"Latency",L"Output",L"Reflex")?LatencyOutput::Reflex:
      ReadChoice(L"Latency",L"Output",L"XeLL")?LatencyOutput::XeLL:LatencyOutput::Auto;
  State().latencyOutput=settings.output==LatencyOutput::Reflex?LatencyOutput::Reflex:LatencyOutput::XeLL;
  // Kept in the settings ABI for older callers. XeMFG never changes window mode.
  settings.forceBorderless=false;
  settings.uiComposition=ReadBool(L"XeFG",L"UIComposition",false);
  settings.uiStabilization=ReadBool(L"XeFG",L"UIStabilization",false);
  uiStabilization=settings.uiStabilization;
  sdk_pacing::RequestNativeLatency(GetPrivateProfileIntW(L"XeFG",L"XeFGNativeLatency",0,Ini().c_str()));
  settings.sdkDeadlines=false;
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
  // 1.10.0 (dashdogy v1.7.1): kept in the ABI, inert since upstream removed both paths.
  value.queueAhead=false; value.sdkDeadlines=false;
  // Reject unknown ABI/enums. The latency output is fixed for this process;
  // changing the preference never replays a game setter on the UI thread.
  if(value.version!=5 || uint32_t(value.input)>1 || uint32_t(value.output)>2) return false;
  std::unique_lock settingsGuard(settingsLock);
  const auto old=settings;
  const auto ini=Ini();
  const bool saved=config::Save(ini,{
    {L"Latency",L"Input",value.input==LatencyInput::Auto?L"Auto":L"Reflex"},
    {L"Latency",L"Output",value.output==LatencyOutput::Auto?L"Auto":value.output==LatencyOutput::Reflex?L"Reflex":L"XeLL"},
    {L"XeFG",L"XeFGFlipMetering",value.flipMetering?L"on":L"off"},
    {L"XeFG",L"ForceBorderless",value.forceBorderless?L"1":L"0"},
    {L"XeFG",L"UIComposition",value.uiComposition?L"1":L"0"},
    {L"XeFG",L"UIStabilization",value.uiStabilization?L"1":L"0"},
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
  if(old.flipMetering!=value.flipMetering) sdk_pacing::RequestFlipMetering(value.flipMetering);
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
  result.flipOutputs=flip.outputs; result.flipFeedback=flip.feedback; result.flipResult=flip.status; result.flipCalls=flip.calls;
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
  const char* flip=vulkanHost?T(Str::kXefgPacingVulkan_fc5f):status.flipBusy?"Checking...":!status.flipAvailable?T(Str::kXefgPacingDriverUnavailable_bc92):
      !status.flipRegistered || !status.flipQueue?T(Str::kWaitingForFrameGeneration_08ec):
      status.flipRefused?T(Str::kXefgPacingDriverDeclined_67cc):metering?T(Str::kNvidiaDriverActive_f98b):
      status.flipConfigured?T(Str::kXefgPacingCurrentDisplayMode_155c):!status.flipEnabled?T(Str::kXefgPacingDriverMeteringOff_e37c):T(Str::kWaitingForAStableFrame_9fe8);
  const auto& w=status.window;
  const char* window=!w.available?T(Str::kWindowNotAvailable_2ec3):w.mode==DisplayMode::Unknown?T(Str::kUnknownNoModeChange_0ff0):
      w.mode==DisplayMode::Exclusive?T(Str::kExclusiveFullscreen_cabc):w.mode==DisplayMode::NativeBorderless?T(Str::kBorderlessGameControlled_5839):
      w.mode==DisplayMode::ManagedBorderless?T(Str::kBorderlessAddonControlled_b5cd):T(Str::kWindowed_9b44);
  const char* acceleration=status.modelUpdating || status.cudaUpdating?"Checking...":!status.modelInitialized?T(Str::kWaitingForTheModel_75a1):!status.modelPort?T(Str::kNativeXefgModel_918b):
      status.cudaState==1 && status.cudaLaunches?T(Str::kNvidiaTensorCoresCuda_73cb):
      status.cudaState==1?T(Str::kNvidiaAccelerationReady_376d):T(Str::kCompatibilityShaders_2d95);
  constexpr const char* flipHelp="Uses the NVIDIA driver to space generated frames evenly. Leave enabled for driver pacing when available; otherwise XeFG uses its own pacing. Changes apply at the next frame group. Active reports driver acceptance, not a monitor scanout measurement.";
  constexpr const char* compositionHelp="Off by default: XeFG interpolates the HUD with the image. Enable only if HUD interpolation causes artifacts. Composition needs HUD-less color from the game and updates the HUD at the real frame rate, so moving HUD elements may look less smooth. Changes apply at the next source frame.";
  constexpr const char* stabilizationHelp="Experimental, off by default. For games that do not provide HUD-less color: pixels that stay unchanged for a few real frames (static HUD text, icons and the minimap frame) are composed from the newest real frame over every generated frame instead of being interpolated, which removes their flicker. Moving or semi-transparent HUD elements are still interpolated. Costs one copy and one small compute pass per real frame. Also works in games that provide HUD-less color, as an alternative to UI Composition: UI Composition updates the whole HUD at the real frame rate, while this keeps moving HUD interpolated at the output rate. When on, it replaces UI Composition.";
  const char* composition=!status.uiCompositionKnown?T(Str::kWaitingForFrameStatus_d01c):status.uiStabilized?T(Str::kHudStabilizationStaticHudComposed_5805):status.uiCompositionApplied?T(Str::kUiComposition_76ad):
      value.uiComposition && !status.uiCompositionAvailable?T(Str::kWaitingForHudLessInput_46f0):
      value.uiComposition?T(Str::kChangePending_d126):value.uiStabilization && !status.uiCompositionAvailable?T(Str::kWaitingForHudStabilization_8a25):T(Str::kUiInterpolationDefault_22e1);
  if(page==PanelPage::General) {
    ui::Section(T(Str::kSmoothnessAndDisplay_cf1b));
    ui::Card card("##xe_presentation");
    ui::Setting("##xe_flip_row",T(Str::kDriverFlipMetering_944d),flipHelp,[&] {
      if(ImGui::Checkbox("##xe_flip",&value.flipMetering)) ConfigureControls(value);
    });
    ui::Readout("##xe_flip_state",T(Str::kPacingStatus_8f41),flip,flipHelp,metering?ui::Accent:ui::Muted);
    if(status.flipRequested!=status.flipEnabled) ui::Description(T(Str::kChangePendingAppliesWithThe_3d02));
    constexpr const char* vsyncHelp="Presents the game's frames with sync interval 0, as with VSync off in the game. Use it for games whose VSync cannot be turned off in their settings, such as Elden Ring: game VSync keeps driver flip metering and generated-frame pacing from applying. Tearing is not enabled. Applies at the next frame.";
    ui::Setting("##xe_vsync_row",T(Str::kForceGameVsyncOff_c2e2),vsyncHelp,[&] {
      if(ImGui::Checkbox("##xe_force_vsync_off",&value.forceVSyncOff)) ConfigureControls(value);
    });
    const auto sync=status.gameSyncInterval;
    const std::string gameVSync=sync==UINT32_MAX?T(Str::kNotObservedYet_87d4):sync==0?T(Str::kOff_b4aa):value.forceVSyncOff?T(Str::kOnForcedOff_9657):T(Str::kOn_7df0);
    ui::Readout("##xe_vsync_state",T(Str::kGameVsync_2a26),gameVSync.c_str(),vsyncHelp,sync && sync!=UINT32_MAX && !value.forceVSyncOff?ui::Warning:ui::Muted);
    // 1.5.5: warn instead of changing it; the user decides (no profile does).
    if(sync && sync!=UINT32_MAX && !value.forceVSyncOff && State().requested.load())
      ui::Notice(T(Str::kTheGamePresentsWithVsync_7019));
    // 1.5.7: optional cap to the display refresh (XeLL output only).
    constexpr const char* capHelp="Limits the game's frame rate so that, with the multiplier, frames are presented slightly below the display refresh (3% margin, for variable-refresh displays). Without a limit, a high game frame rate presents generated frames faster than the display shows them, and motion becomes uneven. The game's own frame limit still applies when it is lower. Available with the XeLL latency output. Applies at the next frame.";
    const bool xellOutput=status.activeLatencyOutput==LatencyOutput::XeLL;
    ui::Setting("##xe_display_cap_row",T(Str::kLimitToDisplayRefresh_0704),capHelp,[&] {
      ImGui::BeginDisabled(!xellOutput);
      if(ImGui::Checkbox("##xe_display_cap",&value.displayCap)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    const auto refreshHz=status.displayRefreshMilliHz/1000;
    const std::string capState=!xellOutput?T(Str::kNotAvailableWithTheReflex_9fdf):!value.displayCap?T(Str::kOff_b4aa):
        !status.displayCapIntervalUs?T(Str::kWaitingForTheDisplayRefresh_cf55):
        "Up to "+std::to_string(1000000/status.displayCapIntervalUs)+" FPS shown, game up to "
        +std::to_string(1000000/status.displayCapIntervalUs/(std::max)(State().multiplier.load(),1u))+" FPS ("+std::to_string(refreshHz)+" Hz display)";
    ui::Readout("##xe_display_cap_state",T(Str::kFrameRateLimit_6563),capState.c_str(),capHelp,value.displayCap && status.displayCapIntervalUs?ui::Accent:ui::Muted);
    // Warn, never enable it automatically (the user decides).
    if(xellOutput && !value.displayCap && State().requested.load() && (!sync || sync==UINT32_MAX)
        && ExceedsDisplay(status.sourceIntervalUs,status.displayRefreshMilliHz,State().multiplier.load()))
      ui::Notice(("The game's frame rate times the multiplier is above the display refresh ("+std::to_string(refreshHz)
          +" Hz), so some generated frames are not shown and motion can look uneven. Turn on Limit to display refresh, or cap the game's frame rate.").c_str());
    ImGui::Separator();
    // 1.5.8: exclusive fullscreen served as borderless (off by default).
    constexpr const char* exclusiveHelp="Intel frame generation does not support exclusive fullscreen. When the game asks for it, XeMFG keeps the game windowed instead: the window covers the monitor without borders, and the game is told it is in fullscreen. The monitor resolution and refresh rate are not changed; a lower game resolution is scaled to the screen. Alt+Enter is held back so the game cannot switch to exclusive fullscreen. Applies at the game's next display mode change, or after a restart if the game is already in exclusive fullscreen.";
    ui::Setting("##xe_exclusive_row",T(Str::kExclusiveFullscreenAsBorderless_9f2b),exclusiveHelp,[&] {
      if(ImGui::Checkbox("##xe_exclusive_borderless",&value.exclusiveAsBorderless)) ConfigureControls(value);
    });
    ui::Readout("##xe_window_state",T(Str::kDisplayMode_2e08),w.pending?T(Str::kChangePending_d126):status.exclusiveServed?T(Str::kBorderlessGameAskedForExclusive_23cf):window,
        T(Str::kTheGameControlsItsDisplay_0e50),
        status.exclusiveServed?ui::Accent:w.mode==DisplayMode::Exclusive?ui::Warning:ui::Muted);
    // Suggest it, never enable it automatically (the user decides).
    if(w.mode==DisplayMode::Exclusive && State().requested.load())
      ui::Notice(value.exclusiveAsBorderless?
          T(Str::kTheGameIsInExclusive_3a50):
          T(Str::kTheGameIsInExclusive_6d7f));
  }
  if(page==PanelPage::Quality) {
    ui::Section(T(Str::kImageQuality_d36b));
    ui::Card card("##xe_quality");
    ui::Readout("##xe_model_quality",T(Str::kModel_e8fa),status.modelUpdating?"Checking...":!status.modelInitialized?T(Str::kWaitingForTheModel_75a1):status.modelPort?T(Str::kIntelXemfgAutomatic_8218):T(Str::kIntelXefgCompatibility_7f66),
        T(Str::kTheMultiFrameModelIs_1a0a));
    ui::Readout("##xe_acceleration_quality",T(Str::kAcceleration_a3bd),acceleration,
        T(Str::kCudaRunsTheSupportedModel_6cb8),ui::Accent);
    ImGui::Separator();
    ui::Setting("##xe_composition_row",T(Str::kUiComposition_8a4d),compositionHelp,[&] {
      ImGui::BeginDisabled(!status.uiCompositionAvailable && !value.uiComposition);
      if(ImGui::Checkbox("##xe_ui_composition",&value.uiComposition)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    ui::Setting("##xe_stabilization_row",T(Str::kHudStabilizationExperimental_1bb2),stabilizationHelp,[&] {
      if(ImGui::Checkbox("##xe_ui_stabilization",&value.uiStabilization)) ConfigureControls(value);
    });
    ui::Readout("##xe_composition_state",T(Str::kHudHandling_ff3f),composition,compositionHelp);
    // 1.5.7: what the game provides, per kind: received, converted, filtered or absent.
    const auto hudKind=[&](unsigned type,const char* name) {
      const uint32_t bit=1u<<type;
      return std::string(name)+(status.hudInputs&bit?(status.hudCopied&bit?T(Str::kReceivedConverted_b525):T(Str::kReceived_bef8))
          :status.hudFiltered&bit?T(Str::kFiltered_a0e8):T(Str::kNotProvided_6683));
    };
    const std::string hudInputs=hudKind(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR,"HUD-less")+" | "+hudKind(XEFG_SWAPCHAIN_RES_UI,"UI");
    ui::Readout("##xe_hud_safety",T(Str::kGameHudInputs_d5bf),hudInputs.c_str(),
        T(Str::kTheGameSOptionalHud_493a));
    // Suggest, never enable it automatically (the user decides).
    if(status.uiCompositionAvailable && !value.uiComposition)
      ui::Description(T(Str::kThisGameProvidesHudLess_57aa));
    if(status.uiCompositionResult<0) ui::Notice(T(Str::kUiCompositionCouldNotBe_dc19));
    ui::Description(T(Str::kForBestResultsKeepThe_51a7));
    ui::Tooltip(T(Str::kAHigherBaseFrameRate_0cc7));
  }
  if(page==PanelPage::Latency) {
    ui::Section(T(Str::kMouseAndInputResponse_701c));
    ui::Card card("##xe_latency");
    ui::Readout("##xe_latency_state",T(Str::kLowLatency_036b),status.xellObserved?T(Str::kXellActive_1039):status.reflexOutputObserved?T(Str::kReflexActive_29aa):T(Str::kWaitingForGameTiming_bab7),
        T(Str::kTheSelectedOutputControlsLatency_566d),(status.xellObserved || status.reflexOutputObserved)?ui::Accent:ui::Warning);
    ui::Readout("##xe_latency_source",T(Str::kTimingSource_a3c8),status.reflexObserved?T(Str::kReflexStreamline_ef68):T(Str::kNotObservedYet_87d4),
        T(Str::kUsesTheGameSReflex_9ca0));
    ui::Description(T(Str::kKeepTheGameSLow_8dde));
    ui::Tooltip(T(Str::kAvoidStackingExternalFpsLimiters_56e3));
    if(status.activeLatencyOutput==LatencyOutput::Reflex && status.reflexOptionsReady && !status.gameReflexMode) ui::Notice(T(Str::kEnableNvidiaReflexInThe_e3f6));
    if(status.latencyRestartPending) ui::Notice(T(Str::kLatencyOutputSavedRestartThe_c3d7));
    if(ui::Expander(T(Str::kAdvancedLatencyRouting_6d9f))) {
      int input=int(value.input),output=int(value.output);
      ui::Setting("##xe_input_row",T(Str::kTimingInput_e70b),T(Str::kAutoUsesReflexStreamlineTiming_4464),[&] {
        if(ImGui::Combo("##xe_timing_input",&input,T(Str::kAutoRecommendedReflexStreamline_5561))) { value.input=LatencyInput(input); ConfigureControls(value); }
      });
      ui::Setting("##xe_output_row",T(Str::kLatencyOutput_7886),T(Str::kAutoKeepsXellReflexUses_9a9b),[&] {
        if(ImGui::Combo("##xe_latency_output",&output,T(Str::kAutoXellXellReflex_7591))) { value.output=LatencyOutput(output); ConfigureControls(value); }
      });
    }
  }
  if(page==PanelPage::Diagnostics) {
    const auto bridge=Snapshot();
    const auto overlay=OverlaySnapshot();
    const auto overlayInput=OverlayInputSnapshot();
    const auto ref=reframework::Snapshot();
    ui::Section(T(Str::kSystemStatus_be3e),T(Str::kUseCopyCompatibilityReportWhen_cf8d));
    ui::Card card("##xe_model_details");
    ui::Readout("##xe_diag_bridge",T(Str::kGameConnection_e319),bridge.streamline && bridge.output?T(Str::kConnected_9370):T(Str::kWaitingForTheGame_18de),
        T(Str::kChecksTheStreamlineInputHooks_2e5b));
    ui::Readout("##xe_diag_overlay",T(Str::kReshadeOutput_bcc6),overlay.created>overlay.destroyed?T(Str::kFinalOutputLinked_f33d):T(Str::kAutomaticHookWaiting_e855),
        T(Str::kLinksReshadeEffectsAndKeyboard_5e78));
    ui::Readout("##xe_diag_model",T(Str::kModel_e8fa),status.modelUpdating?"Checking...":!status.modelInitialized?T(Str::kWaitingForTheModel_75a1):status.modelPort?T(Str::kIntelXemfgReady_1526):T(Str::kIntelXefgCompatibility_7f66));
    ui::Readout("##xe_diag_cuda",T(Str::kAcceleration_a3bd),acceleration,
        T(Str::kNvidiaTensorCoreExecutionIs_f0d5));
    ui::Readout("##xe_diag_flip",T(Str::kFlipMetering_a6c5),flip,flipHelp,metering?ui::Accent:ui::Muted);
    ui::Readout("##xe_diag_latency",T(Str::kLatencyOutput_7886),status.xellObserved?T(Str::kXellActive_1039):status.reflexOutputObserved?T(Str::kReflexActive_29aa):T(Str::kWaitingForGameTiming_bab7));
    ui::Readout("##xe_diag_ui",T(Str::kHudHandling_ff3f),composition,compositionHelp);
    ui::Readout("##xe_diag_window",T(Str::kDisplayMode_2e08),window);
    ui::Readout("##xe_diag_ref","REFramework",reframework::Description(ref),
        T(Str::kOptionalCooperationBeforeReplacingThe_ab06));
    if(ref.knownMonitorIssue) ui::Notice(T(Str::kThisReframeworkBuildWasObserved_590b));
    if(State().retirementFailed || !reframework::Safe(ref.result)) ui::Notice(T(Str::kOutputRetirementCouldNotBe_b429));
    if(w.error) ui::Notice(T(Str::kWindowModeCouldNotBe_a3af));
    if(bridge.sdkResult<0) ui::Notice(T(Str::kFrameGenerationReportedAnError_ea17));
    ImGui::Spacing();
    ui::ButtonRow reports(2);
    if(reports(T(Str::kCopyCompatibilityReport_b1c6),T(Str::kCopiesStructuredMetadataModuleHashes_9706)))
      ImGui::SetClipboardText(CompatibilityReport().c_str());
    if(reports(T(Str::kCopyDetailedCounters_50f9),T(Str::kCopiesTechnicalStatusErrorCodes_2238))) {
      mfgunlock::xemfg::Status model;
      const bool modelAvailable=TryModelStatus(model);
      std::ostringstream report; report<<"MFG Unlock 1.10.1\nstartup_failed="<<ModelStartupFailed()
          <<" model_status_available="<<modelAvailable<<" model_detail="<<(modelAvailable?model.detail:"busy")
          <<"\nmodel="<<status.modelPort<<" initialized="<<status.modelInitialized
          <<"\nCUDA="<<status.cudaState<<" compute="<<status.cudaMajor<<'.'<<status.cudaMinor<<" layers="<<status.cudaLayers
          <<" launches="<<status.cudaLaunches<<" fallbacks="<<status.cudaFallbacks<<" detail="<<status.cudaReason
          <<"\nflip="<<flip<<" result="<<status.flipResult<<" feedback="<<status.flipFeedback<<" outputs="<<status.flipOutputs<<" calls="<<status.flipCalls
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
    if(ui::Expander(T(Str::kPerformanceCaptureAdvanced_3050))) {
      bool enabled=TimingEnabled();
      ui::Setting("##xe_cpu_capture",T(Str::kCollectCpuTimings_26c4),T(Str::kOffByDefaultRecordsA_ad06),[&] {
        if(ImGui::Checkbox("##xe_cpu_enabled",&enabled)) {
          SetTimingEnabled(enabled);
          WritePrivateProfileStringW(L"Diagnostics",L"CpuTiming",enabled?L"1":L"0",Ini().c_str());
        }
      });
      ui::ButtonRow timing(2);
      if(timing(T(Str::kCopyTimingSummary_3a1e),T(Str::kIncludesCpuP95P99P99_7402)))
        ImGui::SetClipboardText(TimingReport().c_str());
      if(timing(T(Str::kClearTimingWindow_dac4))) ClearTiming();
    }
  }
  if(status.latencyRestartPending) ui::Notice(T(Str::kLatencyOutputSavedRestartThe_c3d7));
  if(!status.settingsSaved) ui::Notice(T(Str::kChangeWasNotAppliedSettings_c358));
  if(status.invalidConfiguration) ui::Notice(T(Str::kSomeSavedSettingsAreInvalid_8064));
}
}
extern "C" __declspec(dllexport) bool MFGAXeControlsStatus(mfgunlock::xemfg::native::ControlStatus* out,uint32_t size) {
  if(!out || size!=sizeof(*out) || out->version!=5) return false;
  *out=mfgunlock::xemfg::native::ControlsSnapshot(); return true;
}
extern "C" __declspec(dllexport) bool MFGAXeConfigure(const mfgunlock::xemfg::native::ControlSettings* value,uint32_t size) {
  return value && size==sizeof(*value) && mfgunlock::xemfg::native::ConfigureControls(*value);
}
