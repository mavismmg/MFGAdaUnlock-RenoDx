// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (R2): who else hooks the entry points MFG Unlock depends on, for the support report.
//
// The reports of 2026-11-19..21 each needed a log read to find the module in the way: RenoDX's
// DLSS addon on Streamline's factory, "DLSS 5" NR addons and GITC Uplift on NGX, OptiScaler on
// DXGI. One line now names them: "other_hooks=_nvngx.dll!NVSDK_NGX_D3D12_EvaluateFeature ->
// renodx-dlss.addon64; ...". MFG Unlock's own hooks are left out; a hook placed over ours cannot
// be told from the bytes and is not listed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>

#include "ngx_hook.hpp"

namespace mfgunlock::foreignhooks {

// The module a jump at the start of `module!name` leads to, if it is not `module` or this addon.
inline std::string Owner(HMODULE module, const char* name) {
  if (module == nullptr) return {};
  const auto* proc = reinterpret_cast<const uint8_t*>(GetProcAddress(module, name));
  if (proc == nullptr || hook::JumpsIntoSelf(proc)) return {};
  const uint8_t* end = hook::FollowJumps(proc);
  if (end == proc) return {};
  HMODULE owner = nullptr;
  constexpr DWORD kFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (!GetModuleHandleExW(kFlags, reinterpret_cast<LPCWSTR>(end), &owner)) return "code outside any module";
  if (owner == module) return {};
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(owner, path, MAX_PATH);
  const wchar_t* file = std::wcsrchr(path, L'\\');
  file = file ? file + 1 : path;
  std::string narrow;
  for (const wchar_t* c = file; *c; ++c) narrow += *c < 128 ? static_cast<char>(*c) : '?';
  return narrow;
}

inline std::string Describe() {
  struct Entry {
    const wchar_t* module;
    const char* name;
  };
  static constexpr Entry kEntries[] = {
      {L"_nvngx.dll", "NVSDK_NGX_D3D12_CreateFeature"},  {L"_nvngx.dll", "NVSDK_NGX_D3D12_EvaluateFeature"},
      {L"_nvngx.dll", "NVSDK_NGX_VULKAN_CreateFeature"}, {L"_nvngx.dll", "NVSDK_NGX_VULKAN_EvaluateFeature"},
      {L"sl.interposer.dll", "slInit"},                  {L"sl.interposer.dll", "slUpgradeInterface"},
      {L"sl.interposer.dll", "slGetFeatureFunction"},    {L"nvapi64.dll", "nvapi_QueryInterface"},
  };
  std::string text;
  for (const auto& e : kEntries) {
    const HMODULE module = GetModuleHandleW(e.module);
    const std::string owner = Owner(module, e.name);
    if (owner.empty()) continue;
    if (!text.empty()) text += "; ";
    char module_name[64]{};
    std::snprintf(module_name, sizeof(module_name), "%ls", e.module);
    text += std::string(module_name) + "!" + e.name + " -> " + owner;
  }
  return text.empty() ? "none seen" : text;
}

}  // namespace mfgunlock::foreignhooks
