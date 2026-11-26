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
// 1.10.0 (D4): [DLSSG] StreamlineLog modes (streamline_log.hpp).
inline constexpr Choice kStreamlineLog[]{{Str::kNvidiaSSetting_d4m0,L"0"},{Str::kVerbose_d4m1,L"1"},{Str::kStandardLevel_d4m2,L"2"},{Str::kOff_b4aa,L"3"}};
inline constexpr Choice kRounding[]{{Str::kDown_5035,L"0"},{Str::kNearest_e6b9,L"1"},{Str::kUp_5940,L"2"}};
inline constexpr Choice kInputCopy[]{{Str::kOff_b4aa,L"0"},{Str::kCopy_3664,L"1"},{Str::kNoDispatchTest_6017,L"2"},{Str::kSlotsOnlyTest_9663,L"3"}};
#define MFGUNLOCK_CHOICES(list) list,int(std::size(list)),0,0
#define MFGUNLOCK_NUMBER(low,high) nullptr,0,low,high

inline constexpr Option kXeMfg[]{
  {L"XeFG",L"XeFGStock2xModel",Str::kIntelSStock2xModel_8fea,Str::kUsesIntelSOwn2x_e79e,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"XeFGFlipMeteringLeadUs",Str::kFlipMeteringLeadUs_c51c,Str::kHowEarlyFramesAreHanded_040c,L"2500",MFGUNLOCK_NUMBER(0,8000)},
  {L"XeFG",L"XeFGFlipQueue",Str::kFlipQueue_ab75,Str::kExperimentalPresentationQueueOffBy_93db,L"0",MFGUNLOCK_CHOICES(kOnOff)},
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
  // 1.10.0 (dashdogy B1, crash_record.hpp).
  {L"Diagnostics",L"CrashDump",Str::kCrashRecord_b1c1,Str::kWhenTheGameCrashes_b1c2,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"ForceDRED",Str::kForceDred_693f,Str::kRecordsDirectxCrashDataAdds_ed3f,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"D3D12DebugLayer",Str::kD3d12DebugLayer_a465,Str::kRequiresTheGraphicsToolsWindows_c732,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"VulkanFaults",Str::kVulkanFaultWatch_f2ed,Str::kWatchesTheVulkanHostFor_cd42,L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kDlssG[]{
  {L"DLSSG",L"GenericGate",Str::kGenericProviderGate_0296,Str::kUnlocksMfgOnADlss_eb88,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  // 1.9.0: RTX 20/30.
  {L"DLSSG",L"PreAdaAdmission",Str::kRtx2030FrameGeneration_6d86,Str::kRtx2030OnlyAdmits_076f,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  // 1.10.0 (dashdogy A1, ngx_requirements.hpp).
  {L"DLSSG",L"PreAdaRequirementsAnswer",Str::kRtx2030StreamlineStartAnswer_a1r1,Str::kRtx2030OnlyWhenNgxs_a1r2,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"AutoEarlyLoad",Str::kRtx2030AutomaticEarly_ece5,Str::kRtx2030OnlyThe_996e,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"XmxJitter",Str::kXmxMultiFrameWithJittered_cab2,Str::kXmxGamesWithJitteredMotion_3256,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  // 1.10.0: Streamline's own log, for support (streamline_log.hpp).
  {L"DLSSG",L"StreamlineLog",Str::kStreamlineLogSlLog_55a1,Str::kWritesStreamlineSOwnLog_531a,L"0",MFGUNLOCK_CHOICES(kStreamlineLog)},
  {L"DLSSG",L"ForceBorderless",Str::kForceBorderless_315b,Str::kServesTheGameAsA_53d5,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  // 1.10.0: the Streamline host's Vulkan route alone (xemfg/sl_vulkan.cpp), and XeFG's own UI composition for XMX.
  {L"DLSSG",L"HostVulkan",Str::kHostVulkan_p21,Str::kHostVulkanDesc_p22,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"XmxUiComposition",Str::kXmxUiComposition_p23,Str::kXmxUiCompositionDesc_p24,L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
// 1.10.0: frame generation in more games, drawn on the General page next to the method
// (players did not find them in the advanced list): [0] forced DLSS-G, [1] XMX on Vulkan.
inline constexpr Option kRoutes[]{
  {L"DLSSG",L"ForceFrameGeneration",Str::kForceDlssFrameGeneration_f4g1,Str::kForStreamlineGamesWith_f4g2,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSSG",L"XmxVulkan",Str::kXmxInVulkanGames_x3v1,Str::kExperimentalXmxInVulkan_x3v2,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  // [2] Streamline hosted for games that call DLSS SR through NGX only (sl_host.hpp).
  {L"DLSSG",L"HostStreamline",Str::kHostStreamline_h1a,Str::kHostStreamlineDesc_h1b,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  // [3] The panel's single switch for both: the route follows the game (Streamline or not).
  {L"DLSSG",L"AddFrameGeneration",Str::kAddFrameGeneration_g1a,Str::kAddFrameGenerationDesc_g1b,L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
// 1.10.0: XMX without NVIDIA's unused generator (addon_provider.inl), on the General page under XMX.
// 1.10.0: the Streamline host's HUD fix (hud_capture.hpp) and XMX's HUD mask view (xmx_hud_composite.hpp).
inline constexpr Option kHudFix{L"DLSSG",L"HudFix",Str::kHudFix_p15,Str::kHudFixDesc_p16,L"1",MFGUNLOCK_CHOICES(kOnOff)};
inline constexpr Option kXmxHudDebug{L"DLSSG",L"XmxHudDebug",Str::kHudDebug_p17,Str::kHudDebugDesc_p18,L"0",MFGUNLOCK_CHOICES(kOnOff)};
// 1.10.0 (plan XMX Vulkan pacing): the output limit XeMFG DLSS-G's Vulkan provider applies when the GPU falls behind.
inline constexpr Option kXmxVulkanPacing{L"DLSSG",L"XmxVulkanPacing",Str::kXmxVulkanPacing_q1,Str::kXmxVulkanPacingDesc_q2,L"1",MFGUNLOCK_CHOICES(kOnOff)};
// 1.11.0 (O3): the performance overlay (perf_overlay.inl). Read live from the panel's cache.
inline constexpr Choice kOverlayLevels[]{{Str::kMinimal_o5,L"0"},{Str::kStandard_o6,L"1"},{Str::kFull_o7,L"2"}};
inline constexpr Choice kOverlayCorners[]{{Str::kTopLeft_o10,L"0"},{Str::kTopRight_o11,L"1"},
                                          {Str::kBottomLeft_o12,L"2"},{Str::kBottomRight_o13,L"3"}};
inline constexpr Choice kOverlayModifiers[]{{Str::kNone_o22,L"0"},{Str::kShift_o23,L"1"},{Str::kCtrl_o24,L"2"},{Str::kAlt_o25,L"3"}};
inline constexpr Option kOverlay[]{
  {L"Overlay",L"PerfOverlay",Str::kPerfOverlay_o1,Str::kPerfOverlayDesc_o2,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Overlay",L"PerfOverlayLevel",Str::kPerfOverlayLevel_o3,Str::kPerfOverlayLevelDesc_o4,L"1",MFGUNLOCK_CHOICES(kOverlayLevels)},
  {L"Overlay",L"PerfOverlayCorner",Str::kPerfOverlayCorner_o8,Str::kPerfOverlayCornerDesc_o9,L"0",MFGUNLOCK_CHOICES(kOverlayCorners)},
  {L"Overlay",L"PerfOverlayScale",Str::kPerfOverlayScale_o14,Str::kPerfOverlayScaleDesc_o15,L"100",MFGUNLOCK_NUMBER(50,200)},
  {L"Overlay",L"PerfOverlayOpacity",Str::kPerfOverlayOpacity_o16,Str::kPerfOverlayOpacityDesc_o17,L"70",MFGUNLOCK_NUMBER(0,100)},
  {L"Overlay",L"PerfOverlayKey",Str::kPerfOverlayKey_o18,Str::kPerfOverlayKeyDesc_o19,L"121",MFGUNLOCK_NUMBER(0,254)},
  {L"Overlay",L"PerfOverlayModifier",Str::kPerfOverlayModifier_o20,Str::kPerfOverlayModifierDesc_o21,L"1",MFGUNLOCK_CHOICES(kOverlayModifiers)},
};
// 1.12.0 (DLSS 5, addon_dlss5.inl): read live, except AllowUnsigned (when the runtime is first found).
inline constexpr Choice kDlss5HookPoints[]{{Str::kDlss5HookAuto_n42,L"0"},{Str::kDlss5HookUpscaled_n43,L"1"},{Str::kDlss5HookPresent_n44,L"2"}};
inline constexpr Choice kDlss5Motion[]{{Str::kDlss5MotionAuto_n49,L"0"},{Str::kDlss5MotionGame_n50,L"1"},{Str::kDlss5MotionOff_n51,L"2"}};
inline constexpr Choice kDlss5Coexist[]{{Str::kDlss5CoexistAside_n62,L"0"},{Str::kDlss5CoexistRun_n63,L"1"}};
inline constexpr Choice kDlss5Networks[]{{Str::kDlss5NetworkNew_n7,L"0"},{Str::kDlss5NetworkOriginal_n8,L"1"}};
inline constexpr Choice kDlss5Styles[]{{Str::kDlss5StyleDefault_n11,L"0"},{Str::kDlss5StyleNatural_n12,L"1"},
                                       {Str::kDlss5StyleCinematic_n13,L"2"}};
inline constexpr Choice kDlss5Passes[]{{Str::kDlss5Pass1_n99,L"1"},{Str::kDlss5Pass2_n100,L"2"},{Str::kDlss5Pass3_n101,L"3"}};
inline constexpr Option kDlss5[]{
  {L"DLSS5",L"Enabled",Str::kDlss5Enabled_n1,Str::kDlss5EnabledDesc_n2,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSS5",L"Network",Str::kDlss5Network_n5,Str::kDlss5NetworkDesc_n6,L"0",MFGUNLOCK_CHOICES(kDlss5Networks)},
  {L"DLSS5",L"Style",Str::kDlss5Style_n9,Str::kDlss5StyleDesc_n10,L"0",MFGUNLOCK_CHOICES(kDlss5Styles)},
  {L"DLSS5",L"Intensity",Str::kDlss5Intensity_n14,Str::kDlss5IntensityDesc_n15,L"100",MFGUNLOCK_NUMBER(0,200)},
  {L"DLSS5",L"LocalTone",Str::kDlss5Tone_n16,Str::kDlss5ToneDesc_n17,L"100",MFGUNLOCK_NUMBER(0,200)},
  {L"DLSS5",L"LocalStructure",Str::kDlss5Structure_n18,Str::kDlss5StructureDesc_n19,L"100",MFGUNLOCK_NUMBER(0,200)},
  {L"DLSS5",L"Skin",Str::kDlss5Skin_n20,Str::kDlss5SkinDesc_n21,L"100",MFGUNLOCK_NUMBER(0,200)},
  {L"DLSS5",L"ProtectHud",Str::kDlss5AutoMask_n22,Str::kDlss5AutoMaskDesc_n23,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSS5",L"Scale",Str::kDlss5Scale_n24,Str::kDlss5ScaleDesc_n25,L"100",MFGUNLOCK_NUMBER(25,100)},
  {L"DLSS5",L"Stability",Str::kDlss5Stability_n38,Str::kDlss5StabilityDesc_n39,L"60",MFGUNLOCK_NUMBER(0,100)},
  {L"DLSS5",L"HookPoint",Str::kDlss5HookPoint_n40,Str::kDlss5HookPointDesc_n41,L"0",MFGUNLOCK_CHOICES(kDlss5HookPoints)},
  {L"DLSS5",L"PaperWhite",Str::kDlss5PaperWhite_n45,Str::kDlss5PaperWhiteDesc_n46,L"0",MFGUNLOCK_NUMBER(0,1000)},
  {L"DLSS5",L"Motion",Str::kDlss5Motion_n47,Str::kDlss5MotionDesc_n48,L"0",MFGUNLOCK_CHOICES(kDlss5Motion)},
  {L"DLSS5",L"TaaFinder",Str::kDlss5TaaFinder_n94,Str::kDlss5TaaFinderDesc_n95,L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSS5",L"ToggleKey",Str::kDlss5ToggleKey_n52,Str::kDlss5ToggleKeyDesc_n53,L"119",MFGUNLOCK_NUMBER(0,254)},
  {L"DLSS5",L"CompareKey",Str::kDlss5CompareKey_n54,Str::kDlss5CompareKeyDesc_n55,L"118",MFGUNLOCK_NUMBER(0,254)},
  {L"DLSS5",L"KeyModifier",Str::kDlss5KeyModifier_n56,Str::kDlss5KeyModifierDesc_n57,L"2",MFGUNLOCK_CHOICES(kOverlayModifiers)},
  {L"DLSS5",L"VramMargin",Str::kDlss5VramMargin_n58,Str::kDlss5VramMarginDesc_n59,L"512",MFGUNLOCK_NUMBER(0,4096)},
  {L"DLSS5",L"Coexist",Str::kDlss5Coexist_n60,Str::kDlss5CoexistDesc_n61,L"0",MFGUNLOCK_CHOICES(kDlss5Coexist)},
  {L"DLSS5",L"AllowUnsigned",Str::kDlss5AllowUnsigned_n3,Str::kDlss5AllowUnsignedDesc_n4,L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"DLSS5",L"Passes",Str::kDlss5Passes_n97,Str::kDlss5PassesDesc_n98,L"1",MFGUNLOCK_CHOICES(kDlss5Passes)},
  // 1.12.1 (D5): a GPU time for NR in ms (0 off): NR's resolution follows it, up to Scale.
  {L"DLSS5",L"Budget",Str::kDlss5Budget_n103,Str::kDlss5BudgetDesc_n104,L"0",MFGUNLOCK_NUMBER(0,20)},
  // 1.12.1: how much NR's change gives way at silhouettes (0 off), with the game's vectors and depth.
  {L"DLSS5",L"Silhouettes",Str::kDlss5Silhouettes_n107,Str::kDlss5SilhouettesDesc_n108,L"0",MFGUNLOCK_NUMBER(0,100)},
};
inline constexpr Option kXmxSaveVram{L"DLSSG",L"XmxSaveVram",Str::kXmxSaveVram_v1a,Str::kXmxSaveVramDesc_v1b,L"1",MFGUNLOCK_CHOICES(kOnOff)};
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
    if(option.choices==kOnOff) {
      bool on=Current(option)==L"1";
      if(Toggle("##advanced_switch",on)) Store(option,on?L"1":L"0");
      return;
    }
    // 1.12.0: a short choice (up to four, labels that fit the row) shown whole, as the backend selector.
    if(option.choices && option.choiceCount<=4) {
      const char* labels[4]{};
      float needed=0.f;
      int selected=0;
      for(int i=0;i<option.choiceCount;++i) {
        labels[i]=T(option.choices[i].label);
        needed+=ImGui::CalcTextSize(labels[i]).x+ImGui::GetStyle().FramePadding.x*2.f;
        if(!_wcsicmp(option.choices[i].value,Current(option).c_str())) selected=i;
      }
      if(needed<=ImGui::GetContentRegionAvail().x) {
        if(Segmented("##advanced_segments",selected,labels,option.choiceCount))
          Store(option,option.choices[selected].value);
        return;
      }
    }
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
