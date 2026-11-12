// SPDX-License-Identifier: MIT
#pragma once
// 1.8: every MFGAdaUnlock.ini setting without a control of its own, so the file
// is never the only way to change one. tests/ini_panel_parity_tests.py fails when
// a key read by the addon has no control. These values are read at startup.
#include <algorithm>
#include <cwchar>
#include <map>
#include <string>
#include "backend_selection.hpp"
#include "panel_ui.hpp"

namespace mfgunlock::ui::advanced {
// 1.10.0: labels and help are keys into the panel's language tables.
struct Choice { Str label; const wchar_t* value; };
struct Option {
  const wchar_t* section; const wchar_t* key;
  Str label; Str help;
  const wchar_t* fallback;
  const Choice* choices; int choiceCount;  // null: a number from minimum to maximum
  int minimum, maximum;
};
inline constexpr Choice kOnOff[]{{Str::kOff_b4aa,L"0"},{Str::kOn_7df0,L"1"}};
// -1 means no explicit value: the profile or the learned convention decides.
inline constexpr Choice kAutomatic[]{{Str::kAutomatic_6784,L"-1"},{Str::kOff_b4aa,L"0"},{Str::kOn_7df0,L"1"}};
inline constexpr Choice kFirstFrame[]{{Str::kReady_3294,L"ready"},{Str::kBuffer_a49b,L"buffer"}};
inline constexpr Choice kRounding[]{{Str::kDown_5035,L"0"},{Str::kNearest_e6b9,L"1"},{Str::kUp_5940,L"2"}};
inline constexpr Choice kInputCopy[]{{Str::kOff_b4aa,L"0"},{Str::kCopy_3664,L"1"},{Str::kNoDispatchTest_6017,L"2"},{Str::kSlotsOnlyTest_9663,L"3"}};
#define MFGUNLOCK_CHOICES(list) list,int(std::size(list)),0,0
#define MFGUNLOCK_NUMBER(low,high) nullptr,0,low,high

inline constexpr Option kXeMfg[]{
  {L"XeFG",L"XeFGStock2xModel",Str::kIntelSStock2xModel_8fea,Str::kUsesIntelSOwn2x_e79e,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"XeFGFlipMeteringLeadUs",Str::kFlipMeteringLeadUs_c51c,Str::kHowEarlyFramesAreHanded_040c,L"2500",MFGUNLOCK_NUMBER(0,8000)},
  {L"XeFG",L"XeFGFlipQueue",Str::kFlipQueue_ab75,Str::kExperimentalPresentationQueueOffBy_93db,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"XeFGFlipQueueBufferPct",Str::kFlipQueueBuffer_7f5b,Str::kHowMuchOfTheFrame_67ce,L"50",MFGUNLOCK_NUMBER(0,100)},
  {L"XeFG",L"XeFGFlipQueueFirst",Str::kFlipQueueFirstFrame_ed62,Str::kWhatTheFlipQueuePresents_b6d0,L"ready",MFGUNLOCK_CHOICES(kFirstFrame)},
  {L"XeFG",L"XeFGNativeBuffers",Str::kFlipQueueBuffers_8ac2,Str::kBackBuffersUsedByThe_f04a,L"10",MFGUNLOCK_NUMBER(2,16)},
  {L"XeFG",L"XeFGNativeLatency",Str::kNativeLatencyFrames_e6f5,Str::kLatencyRequestedFromTheSdk_d057,L"0",MFGUNLOCK_NUMBER(0,16)},
  {L"XeFG",L"InvertedDepth",Str::kInvertedDepth_87ad,Str::kAutomaticUsesTheGameS_5452,L"-1",MFGUNLOCK_CHOICES(kAutomatic)},
  {L"XeFG",L"JitteredMotion",Str::kJitteredMotionVectors_8ca8,Str::kAutomaticUsesTheGameS_a119,L"-1",MFGUNLOCK_CHOICES(kAutomatic)},
  {L"Compatibility",L"RetainNativeOutput",Str::kRetainNativeOutput_0a2c,Str::kKeepsTheGameSOwn_5882,L"-1",MFGUNLOCK_CHOICES(kAutomatic)},
  {L"Adapters",L"FidelityFX",Str::kFidelityfxInput_29f5,Str::kFeedsXemfgFromTheGame_aa94,L"1",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kVulkan[]{
  {L"XeFG",L"Vulkan",Str::kVulkanHost_c3c0,Str::kRunsXemfgInVulkanGames_b543,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanGenerate",Str::kGenerateFrames_b2dc,Str::kOffKeepsTheHostRunning_ad56,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanInputCopy",Str::kInputCopy_8368,Str::kHowTheGameSInputs_71c2,L"1",MFGUNLOCK_CHOICES(kInputCopy)},
  {L"XeFG",L"VulkanPresentMetering",Str::kPresentMetering_dff3,Str::kSpacesTheGeneratedFramesOn_fd0c,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanGameLatency",Str::kGameLatencyExperimental_2e56,Str::kDrivesXellFromTheGame_8db0,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanDynamicMFG",Str::kDynamicMfgExperimental_af7b,Str::kChoosesTheMultiplierFromA_826f,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanDynamicTargetFps",Str::kDynamicTargetFps_e0c1,Str::kTargetOutputFrameRate0_89fa,L"0",MFGUNLOCK_NUMBER(0,1000)},
  {L"XeFG",L"VulkanDynamicRounding",Str::kDynamicRounding_fc46,Str::kHowTheMultiplierIsRounded_984a,L"1",MFGUNLOCK_CHOICES(kRounding)},
  {L"XeFG",L"VulkanDynamicAllow1x",Str::kDynamicAllows1x_42b5,Str::kLetsDynamicMfgPauseGeneration_b3d0,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanDynamicTargetAsCap",Str::kDynamicTargetAsLimit_91a6,Str::kAlsoLimitsTheOutputTo_79cd,L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kDiagnostics[]{
  {L"Diagnostics",L"StallTrace",Str::kStallTrace_7190,Str::kLogsLongWaitsAddsOverhead_8c2a,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"GpuTiming",Str::kGpuTiming_5189,Str::kLogsGpuTimingAddsOverhead_1c0a,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"SummaryIntervalMs",Str::kLogSummaryIntervalMs_0045,Str::kHowOftenTheLogReceives_da5d,L"10000",MFGUNLOCK_NUMBER(100,600000)},
  {L"Diagnostics",L"ForceDRED",Str::kForceDred_693f,Str::kRecordsDirectxCrashDataAdds_ed3f,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"D3D12DebugLayer",Str::kD3d12DebugLayer_a465,Str::kRequiresTheGraphicsToolsWindows_c732,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"VulkanFaults",Str::kVulkanFaultWatch_f2ed,Str::kWatchesTheVulkanHostFor_cd42,L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kDlssG[]{
  {L"DLSSG",L"GenericGate",Str::kGenericProviderGate_0296,Str::kUnlocksMfgOnADlss_eb88,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  // 1.9.0: RTX 20/30.
  {L"DLSSG",L"PreAdaAdmission",Str::kRtx2030FrameGeneration_6d86,Str::kRtx2030OnlyAdmits_076f,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"AutoEarlyLoad",Str::kRtx2030AutomaticEarly_ece5,Str::kRtx2030OnlyThe_996e,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"XmxJitter",Str::kXmxMultiFrameWithJittered_cab2,Str::kXmxGamesWithJitteredMotion_3256,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  // 1.10.0: Streamline's own log, for support (streamline_log.hpp).
  {L"DLSSG",L"StreamlineLog",Str::kStreamlineLogSlLog_55a1,Str::kWritesStreamlineSOwnLog_531a,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"ForceBorderless",Str::kForceBorderless_315b,Str::kServesTheGameAsA_53d5,L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
#undef MFGUNLOCK_CHOICES
#undef MFGUNLOCK_NUMBER

inline bool& SaveFailed() { static bool value{}; return value; }
inline bool& Changed() { static bool value{}; return value; }
// Cached: this draws every frame, and a read here would touch the file each time.
inline std::wstring& Current(const Option& option) {
  static std::map<const Option*,std::wstring> values;
  const auto found=values.find(&option);
  if(found!=values.end()) return found->second;
  wchar_t text[64]{};
  GetPrivateProfileStringW(option.section,option.key,option.fallback,text,64,(backend::Directory()/L"MFGAdaUnlock.ini").c_str());
  return values[&option]=text;
}
inline void Store(const Option& option,const std::wstring& value) {
  SaveFailed()=!config::Save(backend::Directory()/L"MFGAdaUnlock.ini",{{option.section,option.key,value.c_str()}});
  if(SaveFailed()) return;
  Current(option)=value; Changed()=true;
}
inline void DrawOption(const Option& option) {
  ImGui::PushID(&option);
  Setting("##advanced_row",T(option.label),T(option.help),[&] {
    if(option.choices) {
      int selected=0;
      for(int i=0;i<option.choiceCount;++i) if(!_wcsicmp(option.choices[i].value,Current(option).c_str())) selected=i;
      if(ImGui::BeginCombo("##advanced_choice",T(option.choices[selected].label))) {
        for(int i=0;i<option.choiceCount;++i)
          if(ImGui::Selectable(T(option.choices[i].label),i==selected) && i!=selected) Store(option,option.choices[i].value);
        ImGui::EndCombo();
      }
      return;
    }
    static std::map<const Option*,int> edited;
    const auto found=edited.find(&option);
    int& number=found!=edited.end()?found->second:(edited[&option]=std::clamp(_wtoi(Current(option).c_str()),option.minimum,option.maximum));
    ImGui::SliderInt("##advanced_number",&number,option.minimum,option.maximum,"%d",
        option.maximum>=100000?ImGuiSliderFlags_Logarithmic:ImGuiSliderFlags_None);
    if(ImGui::IsItemDeactivatedAfterEdit()) {
      number=std::clamp(number,option.minimum,option.maximum);
      Store(option,std::to_wstring(number));
    }
  });
  ImGui::PopID();
}
template<size_t N> void Draw(const Option (&options)[N]) { for(const auto& option:options) DrawOption(option); }
inline void Result() {
  if(SaveFailed()) Notice(T(Str::kSettingWasNotSavedCheck_b1e3));
  else if(Changed()) Notice(T(Str::kSavedRestartTheGameTo_ff48),Accent);
}
}
