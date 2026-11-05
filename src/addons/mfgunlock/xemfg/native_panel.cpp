// SPDX-License-Identifier: MIT
#include "native_internal.hpp"
#include "backend_api.hpp"
#include "dependency_manager.hpp"
#include "native_ngx.hpp"
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif
#include "../panel_ui.hpp"
#include "../advanced_settings.hpp"
#include <sstream>
namespace mfgunlock::xemfg::native {
static std::atomic<uint64_t> panelDraws{};
// The folder XeMFG loads the Intel pair from (beside the executable).
static std::string GameFolder() {
  const auto dir=backend::Directory();
  if(dir.empty()) return {};
  const auto w=dir.wstring();
  const int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),int(w.size()),nullptr,0,nullptr,nullptr);
  std::string text(size_t(n>0?n:0),'\0');
  if(n>0) WideCharToMultiByte(CP_UTF8,0,w.c_str(),int(w.size()),text.data(),n,nullptr,nullptr);
  return text;
}
// 1.5.1: a game whose only usable source is DLSS-SR (no Streamline, no FSR or
// game XeFG). The adapter is experimental, so it stays an explicit choice.
static bool DlssSrSuggested() {
  if(!ngx::Suggested()) return false;
  mfgunlock::xemfg::Status model;
  return TryModelStatus(model) && !model.contexts;
}
static void DlssSrSaveFailed(bool failed) { if(failed) ui::Notice("The DLSS-SR input was not saved: settings could not be written."); }
void Draw() {
  ++panelDraws;
  auto& s=State(); const auto status=Snapshot();
  const bool unavailable=ModelStartupFailed();
  const bool generating=status.enabled && status.sdkResult>=0 && status.submitted>1;
  const bool paused=status.output && s.nativeOff && !s.requested;
  const auto multiplier=std::to_string(s.multiplier.load())+"x";
  // 1.8: what is actually generating. The status locks are only tried, so a busy
  // frame keeps the last answer instead of making the label flicker.
  static const char* generator="XeMFG";
  if(const auto controls=ControlsSnapshot();!controls.modelUpdating && !controls.cudaUpdating && controls.modelInitialized)
    generator=!controls.modelPort?"XeFG 2x (stock)":controls.cudaState==1 && controls.cudaLaunches?"XeMFG (CUDA)":
        generating?"XeMFG (shaders)":"XeMFG";
  bool vulkanGame=false; // 1.7 (plan F): the DX12 input notices below do not apply
#ifdef MFGUNLOCK_XEMFG_VULKAN
  // 1.7 (plan F): a Vulkan game runs through the Vulkan host, not the DX12 output.
  if(const auto vk=vulkan::Status();vk.game) {
    vulkanGame=true;
    const bool vkGenerating=vk.active && vk.frames>1;
    const char* waiting=unavailable?"XeMFG unavailable":vk.active && vk.dynamic?"Dynamic MFG has paused generation":vk.active?"Frame generation is off in the game"
        :vk.reason=="no_dlss_inputs"?"Waiting for DLSS (turn DLSS on in the game)"
        :vk.reason=="game_frame_generation"?"Waiting for the game's frame generation switch":"Waiting for the game";
    ui::Hero(multiplier.c_str(),"XeMFG","Vulkan",vkGenerating?"Generating frames":waiting,vkGenerating);
    if(vk.active && vk.baseFpsMilli && vk.outputFpsMilli) {
      char line[96];
      snprintf(line,sizeof(line),"%.0f base fps -> %.0f fps output",vk.baseFpsMilli/1000.0,vk.outputFpsMilli/1000.0);
      ui::Description(line);
    }
    if(vk.active && vk.dynamic) {
      char line[96];
      snprintf(line,sizeof(line),"Dynamic MFG: %ux now, target %.0f fps",vk.frames,vk.dynamicTargetMilli/1000.0);
      ui::Description(line);
    }
    if(vk.active && vk.fpsLimit) {
      char line[64];
      snprintf(line,sizeof(line),"Output limited to %u fps",vk.fpsLimit);
      ui::Description(line);
    }
    if(!vk.active && !vk.reason.empty() && vk.reason!="no_dlss_inputs" && vk.reason!="active") {
      const auto text="Vulkan host: "+vk.reason;
      ui::Description(text.c_str());
    }
  } else
#endif
  ui::Hero(multiplier.c_str(),generator,"DX12",generating?"Generating frames":
      unavailable?"XeMFG unavailable":paused?"Game has paused frame generation":status.output?"Waiting for compatible frame inputs":"Waiting for the game",generating);
  const auto ref=reframework::Snapshot();
  if(ref.knownMonitorIssue) ui::Notice("This REFramework build has an observed XeFG hook-monitor conflict. Update REFramework to a version with the XeFG monitor fix.");
  const auto report=[] { return CompatibilityReport()+"\n"+mfgunlock::system::Report(); };
  if(s.retirementFailed || !reframework::Safe(ref.result)) {
    ui::ProblemCard problem("##xe_retirement","XeMFG stopped",ui::Danger);
    ImGui::TextWrapped("Output retirement is blocked. Restart the game before creating another XeMFG output.");
    ui::SupportActions(report);
  }
  if(unavailable) {
    ui::ProblemCard problem("##xe_startup","XeMFG could not start",ui::Danger);
    mfgunlock::xemfg::Status model;
    TryModelStatus(model);
    RuntimeFileReport files[2]{};
    if(RuntimeFilesRejected(files)) {
      // 1.5.5: name each file's problem and offer the install here.
      const auto describe=[](const char* name,const RuntimeFileReport& file,const char* supported) {
        const std::string version=file.version[0]?file.version:"unknown version";
        const auto state=file.state==RuntimeFileState::Missing?std::string("missing"):
            file.state==RuntimeFileState::Unreadable?std::string("cannot be read"):
            file.state==RuntimeFileState::Different?version+", not the supported "+supported:
            file.state==RuntimeFileState::AcceptedOlder?version+" (supported)":std::string("supported");
        return std::string(name)+": "+state;
      };
      const auto folder=GameFolder();
      const auto text="XeMFG needs Intel's frame-generation files in the game executable's folder"+(folder.empty()?std::string():":\n"+folder)
          +"\n"+describe("libxess_fg.dll",files[0],"1.3.1.78")+"\n"+describe("libxell.dll",files[1],"1.3.2.10")
          +"\nInstall the supported files, close the game when the helper asks, then start it again.";
      ui::Notice(text.c_str());
      ui::ButtonRow actions(1);
      if(actions("Install / update files","Opens the external helper with the list of files it will change. Nothing changes until you confirm; it then waits for the game to exit and keeps backups."))
        dependencies::Repair();
      const auto repair=dependencies::Snapshot();
      if(repair.helperRunning || !repair.message.empty()) ui::Description(repair.message.c_str());
    }
    else if(strstr(model.detail,"missing_Intel_runtime") || strstr(model.detail,"hash_file_open")) {
      const auto folder=GameFolder();
      const auto text="Intel frame generation files are missing. libxess_fg.dll and libxell.dll must be in the game executable's folder"
          +(folder.empty()?std::string():":\n"+folder)+"\nUse Diagnostics > Intel runtime files > Install / update files, then restart.";
      ui::Notice(text.c_str());
    }
    else if(strstr(model.detail,"unreviewed_Intel_runtime_pair"))
      ui::Notice("This game's Intel runtime pair is unsupported. Update XeFG and XeLL together, including the mod loader's active copies, then restart.");
    else if(strstr(model.detail,"remove_OptiScaler") || strstr(model.detail,"other_mfg_plugin") || strstr(model.detail,"remove_companion"))
      ui::Notice("Another frame-generation loader is active. Use this addon with ReShade directly, then restart the game.");
    else ui::Notice("XeMFG could not initialize. Save the support files to see the reason.");
    ui::SupportActions(report);
  }
  else if(vulkanGame) {}
  else if(!status.output && ngx::Saved()==1) ui::Notice("DLSS-SR input saved. Restart the game to apply it.",ui::Accent);
  else if(!status.output && DlssSrSuggested()) {
    ui::Notice("This game has no frame generation XeMFG can use, but it runs DLSS Super Resolution. "
        "The experimental DLSS-SR input can feed XeMFG from it. Applies after a restart.");
    static bool failed{};
    ui::ButtonRow actions(1);
    if(actions("Enable DLSS-SR input (experimental)","Saves [Adapters] DlssSr=1 in MFGAdaUnlock.ini. The camera is assumed and the HUD is interpolated with the scene."))
      failed=!ngx::Save(true);
    DlssSrSaveFailed(failed);
  }
  // Only meaningful when Streamline is in the process: without it (a game with
  // no frame generation) nothing was loaded too late (1.5.1, KCD2).
  else if(!status.output && strstr(status.detail,"needs early Streamline") && GetModuleHandleW(L"sl.interposer.dll"))
    ui::Notice("XeMFG was loaded too late to take over presentation safely. Restart the game with ReShade and this addon loaded at startup.");
  else if(!status.output && strstr(status.detail,"no DLSS-G plugin")) ui::Notice(status.detail);
  else if(!status.output && !GetModuleHandleW(L"sl.interposer.dll") && !ngx::Configured())
    ui::Notice("No frame-generation inputs from this game yet. If it has no frame generation of its own, select DLSS as the upscaler: "
        "the panel then offers the experimental DLSS-SR input.");
  else if(!status.output) ui::Notice("Load a scene with Frame Generation enabled. XeMFG currently uses Streamline inputs in DirectX 12.");
  else if(paused) ui::Notice("The game's latest request has frame generation disabled. If it is enabled in the game, copy Diagnostics so the connection can be checked.");
  else if(status.enabled && status.sdkResult<0) ui::Notice("Frame generation is recovering. If this persists, switch FG off and on in the game.");
  else if(status.output && !status.enabled) ui::Description(status.detail);
  ui::SessionNotice(report,false);
  static PanelPage page=PanelPage::General;
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    int selected=int(page); ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##xe_page",&selected,"General\0Quality\0Latency\0Diagnostics\0")) page=PanelPage(selected);
  } else if(const float tab=ui::TabWidth(4); ui::BeginTabs("##xemfg_pages")) {
    if(ui::TabItem("General",tab)) { page=PanelPage::General; ImGui::EndTabItem(); }
    if(ui::TabItem("Quality",tab)) { page=PanelPage::Quality; ImGui::EndTabItem(); }
    if(ui::TabItem("Latency",tab)) { page=PanelPage::Latency; ImGui::EndTabItem(); }
    if(ui::TabItem("Diagnostics",tab)) { page=PanelPage::Diagnostics; ImGui::EndTabItem(); }
    ui::EndTabs();
  }
  if(page==PanelPage::General) {
    ui::Section("Frame generation","Keep Frame Generation enabled in the game.");
    ui::Card card("##xemfg_generation");
    ui::BackendSelector();
    int choice=int(s.multiplier.load())-2;
    static bool multiplierSaveFailed{};
    ui::Setting("##xe_multiplier_row","Frame multiplier","Applies while playing. 2x adds up to one generated frame per rendered frame; 4x adds up to three. Higher values use more GPU time.",[&] {
      constexpr const char* kMultipliers[]={"2x","3x","4x","5x","6x"};
      if(ui::Segmented("##xe_multiplier",choice,kMultipliers,5)) {
        const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
        const auto text=std::to_wstring(choice+2);
        multiplierSaveFailed=!config::Save(ini,{{L"XeFG",L"Multiplier",text.c_str()}});
        if(!multiplierSaveFailed) { s.multiplier=uint32_t(choice+2); SetGameMultiplier(s.multiplier); }
      }
    });
    if(multiplierSaveFailed) ui::Notice("Multiplier was not applied: settings could not be saved.");
    // Shown only where it can matter: opted in, or a game that runs DLSS-SR alone.
    if(ngx::Configured() || ngx::Saved()>=0 || DlssSrSuggested()) {
      bool enabled=ngx::Saved()>=0?ngx::Saved()==1:ngx::Configured();
      static bool failed{};
      ui::Setting("##xe_dlss_sr_row","DLSS-SR input (experimental)","For games without frame generation that use DLSS Super Resolution. "
          "The camera is assumed and the HUD is interpolated with the scene. Requires a game restart.",[&] {
        if(ImGui::Checkbox("##xe_dlss_sr",&enabled)) failed=!ngx::Save(enabled);
      });
      DlssSrSaveFailed(failed);
      if(ngx::Saved()>=0 && (ngx::Saved()==1)!=ngx::Configured()) ui::Description("Restart the game to apply this change.");
    }
  }
  DrawControls(page);
  if(page==PanelPage::Diagnostics && ui::Expander("All settings (advanced)")) {
    ui::Description("Applies after a game restart. Change these only to test or to diagnose a problem.");
    ui::advanced::Draw(ui::advanced::kXeMfg);
    bool dlssSr=ngx::Saved()>=0?ngx::Saved()==1:ngx::Configured();
    static bool dlssSrFailed{};
    ui::Setting("##xe_all_dlss_sr","DLSS-SR input (experimental)","For games without frame generation that use DLSS Super Resolution. The camera is assumed and the HUD is interpolated with the scene.",[&] {
      if(ImGui::Checkbox("##xe_all_dlss_sr_value",&dlssSr)) dlssSrFailed=!ngx::Save(dlssSr);
    });
    DlssSrSaveFailed(dlssSrFailed);
    ImGui::Separator(); ui::Description("Vulkan");
    ui::advanced::Draw(ui::advanced::kVulkan);
    ImGui::Separator(); ui::Description("Diagnostics");
    ui::advanced::Draw(ui::advanced::kDiagnostics);
    ui::advanced::Result();
  }
  if(page==PanelPage::Diagnostics || (page==PanelPage::General && unavailable)) dependencies::Draw();
  if(page==PanelPage::General) ui::SystemCard(nullptr,false);
  ui::Footer();
}
}
extern "C" __declspec(dllexport) uint64_t MFGAXePanelDraws() {
  return mfgunlock::xemfg::native::panelDraws.load();
}
