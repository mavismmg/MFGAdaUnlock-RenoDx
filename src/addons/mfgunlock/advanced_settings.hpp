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
struct Choice { const char* label; const wchar_t* value; };
struct Option {
  const wchar_t* section; const wchar_t* key;
  const char* label; const char* help;
  const wchar_t* fallback;
  const Choice* choices; int choiceCount;  // null: a number from minimum to maximum
  int minimum, maximum;
};
inline constexpr Choice kOnOff[]{{"Off",L"0"},{"On",L"1"}};
// -1 means no explicit value: the profile or the learned convention decides.
inline constexpr Choice kAutomatic[]{{"Automatic",L"-1"},{"Off",L"0"},{"On",L"1"}};
inline constexpr Choice kFirstFrame[]{{"Ready",L"ready"},{"Buffer",L"buffer"}};
inline constexpr Choice kRounding[]{{"Down",L"0"},{"Nearest",L"1"},{"Up",L"2"}};
inline constexpr Choice kInputCopy[]{{"Off",L"0"},{"Copy",L"1"},{"No dispatch (test)",L"2"},{"Slots only (test)",L"3"}};
#define MFGUNLOCK_CHOICES(list) list,int(std::size(list)),0,0
#define MFGUNLOCK_NUMBER(low,high) nullptr,0,low,high

inline constexpr Option kXeMfg[]{
  {L"XeFG",L"XeFGStock2xModel","Intel's stock 2x model","Uses Intel's own 2x model instead of the ported multi-frame model. Set the multiplier to 2x: above it, frame generation does not start.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"XeFGFlipMeteringLeadUs","Flip metering lead (us)","How early frames are handed to the driver's flip metering.",L"2500",MFGUNLOCK_NUMBER(0,8000)},
  {L"XeFG",L"XeFGFlipQueue","Flip queue","Experimental presentation queue. Off by default.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"XeFGFlipQueueBufferPct","Flip queue buffer (%)","How much of the frame interval the flip queue buffers.",L"50",MFGUNLOCK_NUMBER(0,100)},
  {L"XeFG",L"XeFGFlipQueueFirst","Flip queue first frame","What the flip queue presents first.",L"ready",MFGUNLOCK_CHOICES(kFirstFrame)},
  {L"XeFG",L"XeFGNativeBuffers","Flip queue buffers","Back buffers used by the flip queue.",L"10",MFGUNLOCK_NUMBER(2,16)},
  {L"XeFG",L"XeFGNativeLatency","Native latency (frames)","Latency requested from the SDK. 0 leaves it to the SDK.",L"0",MFGUNLOCK_NUMBER(0,16)},
  {L"XeFG",L"InvertedDepth","Inverted depth","Automatic uses the game's profile or what was learned for this executable. Wrong values cause heavy artifacts.",L"-1",MFGUNLOCK_CHOICES(kAutomatic)},
  {L"XeFG",L"JitteredMotion","Jittered motion vectors","Automatic uses the game's profile or what was learned for this executable. Wrong values cause shimmering.",L"-1",MFGUNLOCK_CHOICES(kAutomatic)},
  {L"Compatibility",L"RetainNativeOutput","Retain native output","Keeps the game's own swap chain alive, for overlays that hold references to it. Automatic follows the game's profile.",L"-1",MFGUNLOCK_CHOICES(kAutomatic)},
  {L"Adapters",L"FidelityFX","FidelityFX input","Feeds XeMFG from the game's FSR frame generation.",L"1",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kVulkan[]{
  {L"XeFG",L"Vulkan","Vulkan host","Runs XeMFG in Vulkan games.",L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanGenerate","Generate frames","Off keeps the host running without generating, for comparison.",L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanInputCopy","Input copy","How the game's inputs reach the host. The test modes are for diagnosis.",L"1",MFGUNLOCK_CHOICES(kInputCopy)},
  {L"XeFG",L"VulkanPresentMetering","Present metering","Spaces the generated frames on the CPU.",L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanGameLatency","Game latency (experimental)","Drives XeLL from the game's latency markers. Not measured in a game yet.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanDynamicMFG","Dynamic MFG (experimental)","Chooses the multiplier from a target frame rate. Not measured in a game yet.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanDynamicTargetFps","Dynamic target (FPS)","Target output frame rate. 0 uses no target.",L"0",MFGUNLOCK_NUMBER(0,1000)},
  {L"XeFG",L"VulkanDynamicRounding","Dynamic rounding","How the multiplier is rounded to reach the target.",L"1",MFGUNLOCK_CHOICES(kRounding)},
  {L"XeFG",L"VulkanDynamicAllow1x","Dynamic allows 1x","Lets Dynamic MFG pause generation when the game already reaches the target.",L"1",MFGUNLOCK_CHOICES(kOnOff)},
  {L"XeFG",L"VulkanDynamicTargetAsCap","Dynamic target as limit","Also limits the output to the target.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kDiagnostics[]{
  {L"Diagnostics",L"StallTrace","Stall trace","Logs long waits. Adds overhead.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"GpuTiming","GPU timing","Logs GPU timing. Adds overhead.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"SummaryIntervalMs","Log summary interval (ms)","How often the log receives a summary.",L"10000",MFGUNLOCK_NUMBER(100,600000)},
  {L"Diagnostics",L"ForceDRED","Force DRED","Records DirectX crash data. Adds overhead.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"D3D12DebugLayer","D3D12 debug layer","Requires the Graphics Tools Windows feature. Slows the game heavily.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
  {L"Diagnostics",L"VulkanFaults","Vulkan fault watch","Watches the Vulkan host for crashes and hangs.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
};
inline constexpr Option kDlssG[]{
  {L"DLSSG",L"ForceBorderless","Force borderless","Serves the game as a borderless window. Removed from the main controls in 1.7.",L"0",MFGUNLOCK_CHOICES(kOnOff)},
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
  Setting("##advanced_row",option.label,option.help,[&] {
    if(option.choices) {
      int selected=0;
      for(int i=0;i<option.choiceCount;++i) if(!_wcsicmp(option.choices[i].value,Current(option).c_str())) selected=i;
      if(ImGui::BeginCombo("##advanced_choice",option.choices[selected].label)) {
        for(int i=0;i<option.choiceCount;++i)
          if(ImGui::Selectable(option.choices[i].label,i==selected) && i!=selected) Store(option,option.choices[i].value);
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
  if(SaveFailed()) Notice("Setting was not saved. Check write access to the game folder.");
  else if(Changed()) Notice("Saved. Restart the game to apply it.",Accent);
}
}
