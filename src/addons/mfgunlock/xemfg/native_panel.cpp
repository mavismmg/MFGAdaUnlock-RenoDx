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
static void DlssSrSaveFailed(bool failed) { if(failed) ui::Notice(T(Str::kTheDlssSrInputWas_81e8)); }
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
    generator=!controls.modelPort?T(Str::kXefg2xStock_b63c):controls.cudaState==1 && controls.cudaLaunches?T(Str::kXemfgCuda_9d34):
        generating?T(Str::kXemfgShaders_d7bf):"XeMFG";
  bool vulkanGame=false; // 1.7 (plan F): the DX12 input notices below do not apply
#ifdef MFGUNLOCK_XEMFG_VULKAN
  // 1.7 (plan F): a Vulkan game runs through the Vulkan host, not the DX12 output.
  if(const auto vk=vulkan::Status();vk.game) {
    vulkanGame=true;
    const bool vkGenerating=vk.active && vk.frames>1;
    const char* waiting=unavailable?T(Str::kXemfgUnavailable_77cc):vk.active && vk.dynamic?T(Str::kDynamicMfgHasPausedGeneration_5b1e):vk.active?T(Str::kFrameGenerationIsOffIn_1fd3)
        :vk.reason=="no_dlss_inputs"?T(Str::kWaitingForDlssTurnDlss_0df4)
        :vk.reason=="game_frame_generation"?T(Str::kWaitingForTheGameS_cdcf):T(Str::kWaitingForTheGame_18de);
    ui::Hero(multiplier.c_str(),"XeMFG",T(Str::kVulkan_d012),vkGenerating?T(Str::kGeneratingFrames_c5e5):waiting,vkGenerating);
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
  ui::Hero(multiplier.c_str(),generator,"DX12",generating?T(Str::kGeneratingFrames_c5e5):
      unavailable?T(Str::kXemfgUnavailable_77cc):paused?T(Str::kGameHasPausedFrameGeneration_126c):status.output?T(Str::kWaitingForCompatibleFrameInputs_4ad3):T(Str::kWaitingForTheGame_18de),generating);
  const auto ref=reframework::Snapshot();
  if(ref.knownMonitorIssue) ui::Notice(T(Str::kThisReframeworkBuildHasAn_cf1b));
  const auto report=[] { return CompatibilityReport()+"\n"+mfgunlock::system::Report(); };
  if(s.retirementFailed || !reframework::Safe(ref.result)) {
    ui::ProblemCard problem("##xe_retirement",T(Str::kXemfgStopped_ebff),ui::Danger);
    ImGui::TextWrapped(T(Str::kOutputRetirementIsBlockedRestart_4fb4));
    ui::SupportActions(report);
  }
  if(unavailable) {
    ui::ProblemCard problem("##xe_startup",T(Str::kXemfgCouldNotStart_0f7a),ui::Danger);
    mfgunlock::xemfg::Status model;
    TryModelStatus(model);
    RuntimeFileReport files[2]{};
    if(RuntimeFilesRejected(files)) {
      // 1.5.5: name each file's problem and offer the install here.
      const auto describe=[](const char* name,const RuntimeFileReport& file,const char* supported) {
        const std::string version=file.version[0]?file.version:T(Str::kUnknownVersion_7e55);
        const auto state=file.state==RuntimeFileState::Missing?std::string("missing"):
            file.state==RuntimeFileState::Unreadable?std::string(T(Str::kCannotBeRead_b119)):
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
      if(actions(T(Str::kInstallUpdateFiles_73d5),T(Str::kOpensTheExternalHelperWith_d3e3)))
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
      ui::Notice(T(Str::kThisGameSIntelRuntime_283d));
    else if(strstr(model.detail,"remove_OptiScaler") || strstr(model.detail,"other_mfg_plugin") || strstr(model.detail,"remove_companion"))
      ui::Notice(T(Str::kAnotherFrameGenerationLoaderIs_1e91));
    else ui::Notice(T(Str::kXemfgCouldNotInitializeSave_b8d3));
    ui::SupportActions(report);
  }
  else if(vulkanGame) {}
  else if(!status.output && ngx::Saved()==1) ui::Notice(T(Str::kDlssSrInputSavedRestart_73c0),ui::Accent);
  else if(!status.output && DlssSrSuggested()) {
    ui::Notice(T(Str::kThisGameHasNoFrame_9117));
    static bool failed{};
    ui::ButtonRow actions(1);
    if(actions(T(Str::kEnableDlssSrInputExperimental_6293),T(Str::kSavesAdaptersDlsssr1In_bc18)))
      failed=!ngx::Save(true);
    DlssSrSaveFailed(failed);
  }
  // Only meaningful when Streamline is in the process: without it (a game with
  // no frame generation) nothing was loaded too late (1.5.1, KCD2).
  else if(!status.output && strstr(status.detail,"needs early Streamline") && GetModuleHandleW(L"sl.interposer.dll"))
    ui::Notice(T(Str::kXemfgWasLoadedTooLate_3588));
  else if(!status.output && strstr(status.detail,T(Str::kNoDlssGPlugin_3dff))) ui::Notice(status.detail);
  else if(!status.output && !GetModuleHandleW(L"sl.interposer.dll") && !ngx::Configured())
    ui::Notice(T(Str::kNoFrameGenerationInputsFrom_70b6));
  else if(!status.output) ui::Notice(T(Str::kLoadASceneWithFrame_62dd));
  else if(paused) ui::Notice(T(Str::kTheGameSLatestRequest_6e82));
  else if(status.enabled && status.sdkResult<0) ui::Notice(T(Str::kFrameGenerationIsRecoveringIf_7684));
  else if(status.output && !status.enabled) ui::Description(status.detail);
  ui::SessionNotice(report,false);
  static PanelPage page=PanelPage::General;
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    int selected=int(page); ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##xe_page",&selected,T(Str::kGeneralQualityLatencyDiagnostics_b9c8))) page=PanelPage(selected);
  } else if(const float tab=ui::TabWidth(4); ui::BeginTabs("##xemfg_pages")) {
    if(ui::TabItem(T(Str::kGeneral_fd0b),tab)) { page=PanelPage::General; ImGui::EndTabItem(); }
    if(ui::TabItem(T(Str::kQuality_bec6),tab)) { page=PanelPage::Quality; ImGui::EndTabItem(); }
    if(ui::TabItem(T(Str::kLatency_5ef9),tab)) { page=PanelPage::Latency; ImGui::EndTabItem(); }
    if(ui::TabItem(T(Str::kDiagnostics_1397),tab)) { page=PanelPage::Diagnostics; ImGui::EndTabItem(); }
    ui::EndTabs();
  }
  if(page==PanelPage::General) {
    ui::Section(T(Str::kFrameGeneration_83ac),T(Str::kKeepFrameGenerationEnabledIn_1cbc));
    ui::Card card("##xemfg_generation");
    ui::BackendSelector();
    int choice=int(s.multiplier.load())-2;
    static bool multiplierSaveFailed{};
    ui::Setting("##xe_multiplier_row",T(Str::kFrameMultiplier_dfe5),T(Str::kAppliesWhilePlaying2xAdds_6962),[&] {
      const char* kMultipliers[] = {"2x","3x","4x","5x","6x"};
      if(ui::Segmented("##xe_multiplier",choice,kMultipliers,5)) {
        const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
        const auto text=std::to_wstring(choice+2);
        multiplierSaveFailed=!config::Save(ini,{{L"XeFG",L"Multiplier",text.c_str()}});
        if(!multiplierSaveFailed) { s.multiplier=uint32_t(choice+2); SetGameMultiplier(s.multiplier); }
      }
    });
    if(multiplierSaveFailed) ui::Notice(T(Str::kMultiplierWasNotAppliedSettings_9d14));
    // Shown only where it can matter: opted in, or a game that runs DLSS-SR alone.
    if(ngx::Configured() || ngx::Saved()>=0 || DlssSrSuggested()) {
      bool enabled=ngx::Saved()>=0?ngx::Saved()==1:ngx::Configured();
      static bool failed{};
      ui::Setting("##xe_dlss_sr_row",T(Str::kDlssSrInputExperimental_e488),T(Str::kForGamesWithoutFrameGeneration_b2e8),[&] {
        if(ImGui::Checkbox("##xe_dlss_sr",&enabled)) failed=!ngx::Save(enabled);
      });
      DlssSrSaveFailed(failed);
      if(ngx::Saved()>=0 && (ngx::Saved()==1)!=ngx::Configured()) ui::Description(T(Str::kRestartTheGameToApply_2696));
    }
  }
  DrawControls(page);
  if(page==PanelPage::Diagnostics && ui::Expander(T(Str::kAllSettingsAdvanced_cf72))) {
    ui::Description(T(Str::kAppliesAfterAGameRestart_e6f3));
    ui::advanced::Draw(ui::advanced::kXeMfg);
    bool dlssSr=ngx::Saved()>=0?ngx::Saved()==1:ngx::Configured();
    static bool dlssSrFailed{};
    ui::Setting("##xe_all_dlss_sr",T(Str::kDlssSrInputExperimental_e488),T(Str::kForGamesWithoutFrameGeneration_a97e),[&] {
      if(ImGui::Checkbox("##xe_all_dlss_sr_value",&dlssSr)) dlssSrFailed=!ngx::Save(dlssSr);
    });
    DlssSrSaveFailed(dlssSrFailed);
    ImGui::Separator(); ui::Description(T(Str::kVulkan_d012));
    ui::advanced::Draw(ui::advanced::kVulkan);
    ImGui::Separator(); ui::Description(T(Str::kDiagnostics_1397));
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
