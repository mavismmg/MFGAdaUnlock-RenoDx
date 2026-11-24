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
#include <psapi.h>

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <string>

#include "ngx_hook.hpp"

namespace mfgunlock::foreignhooks {

inline std::string FileName(HMODULE module) {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* file = std::wcsrchr(path, L'\\');
  file = file ? file + 1 : path;
  std::string narrow;
  for (const wchar_t* c = file; *c; ++c) narrow += *c < 128 ? static_cast<char>(*c) : '?';
  return narrow;
}

// The module a jump at the start of `module!name` leads to, if it is not `module` or this addon
// (`*outside`: the jump leads to code outside any module).
inline HMODULE OwnerModule(HMODULE module, const char* name, bool* outside = nullptr) {
  if (outside) *outside = false;
  if (module == nullptr) return nullptr;
  const auto* proc = reinterpret_cast<const uint8_t*>(GetProcAddress(module, name));
  if (proc == nullptr || hook::JumpsIntoSelf(proc)) return nullptr;
  const uint8_t* end = hook::FollowJumps(proc);
  if (end == proc) return nullptr;
  HMODULE owner = nullptr;
  constexpr DWORD kFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (!GetModuleHandleExW(kFlags, reinterpret_cast<LPCWSTR>(end), &owner)) {
    if (outside) *outside = true;
    return nullptr;
  }
  return owner == module ? nullptr : owner;
}
inline std::string Owner(HMODULE module, const char* name) {
  bool outside = false;
  const HMODULE owner = OwnerModule(module, name, &outside);
  if (outside) return "code outside any module";
  return owner ? FileName(owner) : std::string();
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

// 1.12.1: the modules that may have taken the game's swap chain creation away from Add frame
// generation. Kingdom Come: Deliverance II with GITC Uplift loaded (2026-11-23, FusselBob, who thought
// it was off): Uplift hooked DXGI's factories and the swap chain creation 6 ms before the game made its
// swap chain, Streamline's factory was upgraded by someone else, and slSetD3DDevice never ran. Named:
// another module's jump at the start of a DXGI factory export (ReShade's own left out), and add-ons
// known to place themselves in that path.
inline std::string SwapChainTakers() {
  std::string found;
  const auto add = [&](const std::string& name) {
    if (!name.empty() && found.find(name) == std::string::npos) found += (found.empty() ? "" : ", ") + name;
  };
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return found;
  for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
    const std::string file = FileName(modules[i]);
    std::string lower = file;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "dxgi.dll") {
      for (const char* name : {"CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2"}) {
        const HMODULE owner = OwnerModule(modules[i], name);
        if (owner != nullptr && GetProcAddress(owner, "ReShadeRegisterAddon") == nullptr) add(FileName(owner));
      }
    }
    if (lower.find("uplift") != std::string::npos) add(file);
  }
  return found;
}

}  // namespace mfgunlock::foreignhooks
