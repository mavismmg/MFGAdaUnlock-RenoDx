// SPDX-License-Identifier: MIT
#pragma once
#include "panel_ui.hpp"
#include "latency_marker_policy.hpp"
namespace mfgunlock::dlssglatency {
// The shared marker admission policy is tested, but does not establish safe
// activation of XeLL on NVIDIA's live queues. Never equate admission with SDK
// quiescence or with validation of replacing slReflexSleep.
inline constexpr bool XeLLRuntimeValidated=false;
inline void Draw() {
  static std::wstring preference=[] {
    wchar_t text[32]{};
    GetPrivateProfileStringW(L"DLSSG.Latency",L"Output",L"Reflex",text,32,(backend::Directory()/L"MFGAdaUnlock.ini").c_str());
    return std::wstring(text);
  }();
  static bool saveFailed{};
  ui::Card card("##dlssg_latency_route");
  ui::Readout("##dlssg_latency_current",T(Str::kActiveLatencyOutput_3ade),T(Str::kNvidiaReflex_dea0),
      T(Str::kPreservesTheGameSNative_8402),ui::Accent);
  ui::Setting("##dlssg_latency_requested",T(Str::kLatencyOutput_7886),
      T(Str::kExperimentalXellIsUnavailableIn_52ad),[&] {
    if(ImGui::BeginCombo("##dlssg_latency_choice",T(Str::kNvidiaReflexRecommended_f8be))) {
      if(ImGui::Selectable(T(Str::kNvidiaReflex_dea0),true)) {
        saveFailed=!config::Save(backend::Directory()/L"MFGAdaUnlock.ini",{{L"DLSSG.Latency",L"Output",L"Reflex"}});
        if(!saveFailed) preference=L"Reflex";
      }
      ImGui::BeginDisabled(); ImGui::Selectable(T(Str::kXellUnavailable_922b),false); ImGui::EndDisabled();
      ui::Tooltip(T(Str::kNoSafeModeSwitchQuiescence_5f62));
      ImGui::EndCombo();
    }
  });
  if(_wcsicmp(preference.c_str(),L"Reflex")) ui::Notice("The requested latency route is unavailable. Native Reflex remains active.");
  if(saveFailed) ui::Notice(T(Str::kTheLatencyPreferenceCouldNot_92c5));
}
}
