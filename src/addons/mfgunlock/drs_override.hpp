// SPDX-License-Identifier: MIT
#pragma once
// 1.8.5: the Dynamic MFG multiplier ceiling, as the NVIDIA app sets it.
//
// The NVIDIA app's "Frame Generation Mode: Dynamic, Multiplier: Up to Nx"
// writes the driver profile setting 0x10562D0F ("Override maximum DLSSG
// dynamic multi frame count"). Streamline's sl.dlss_g reads it through
// NvAPI_DRS_GetSetting ("DRS limits max generated frames to %u") and hands it
// to NVIDIA's provider as DLSSG.DynamicMaxFrameCount; NVIDIA's own Dynamic MFG
// then never picks more. The addon answers that one read inside the game's
// process with the ceiling chosen in its panel. Nothing is written to the
// driver profile: the NVIDIA app and other games are unaffected, and closing
// the game undoes it. Auto (0) leaves the driver's answer unchanged.
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>

#include <include/reshade.hpp>

#include "./ngx_hook.hpp"

namespace mfgunlock::drsoverride {
constexpr uint32_t kGetSettingId = 0x73bf8338;            // NvAPI_DRS_GetSetting
constexpr uint32_t kDynamicMaxFrameCount = 0x10562D0F;    // max generated frames, Dynamic
constexpr uint32_t kDlssgMode = 0x10308298;
constexpr uint32_t kDlssgMultiFrameCount = 0x104D6667;
constexpr uint32_t kDlssgTargetFrameRate = 0x10CF4125;

// NVDRS_SETTING_V1 (nvapi.h R5xx): version 0x13020, 12320 bytes.
constexpr size_t kSettingSize = 12320;
constexpr size_t kSettingIdOffset = 4100;
constexpr size_t kSettingTypeOffset = 4104;
constexpr size_t kSettingLocationOffset = 4108;
constexpr size_t kIsCurrentPredefinedOffset = 4112;
constexpr size_t kCurrentValueOffset = 8220;
constexpr uint32_t kSettingVersion1 = static_cast<uint32_t>(kSettingSize) | (1u << 16);

// 0 = Auto (the driver's answer); 2..6 = the highest multiplier.
inline std::atomic<uint32_t> g_max_multiplier{0};
// The value the driver profile would give, last seen (UINT32_MAX: not read yet).
inline std::atomic<uint32_t> g_driver_value{UINT32_MAX};
inline std::atomic<uint32_t> g_answered_value{UINT32_MAX};
inline std::atomic_bool g_read_seen{false};
inline std::atomic_bool g_hooked{false};

constexpr bool IsSelectable(uint32_t multiplier) { return multiplier == 0 || (multiplier >= 2 && multiplier <= 6); }
// The setting counts generated frames, as 0x104D6667 does: 1 = 2x ... 5 = 6x.
constexpr uint32_t DrsValueFor(uint32_t multiplier) { return multiplier >= 2 ? multiplier - 1 : 0; }

using GetSettingFn = int(__cdecl*)(void* session, void* profile, uint32_t setting_id, void* setting);
inline GetSettingFn g_real_get_setting = nullptr;

inline std::string CallerModule(void* address) {
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH]{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &module) ||
      GetModuleFileNameW(module, path, MAX_PATH) == 0)
    return "unknown";
  return std::filesystem::path(path).filename().string();
}

inline void LogRead(uint32_t id, int result, uint32_t value, bool answered, void* caller) {
  // One line per setting and value; DRS reads are rare (startup, mode changes).
  static std::mutex lock;
  static uint64_t seen[8]{};
  const uint64_t key = (static_cast<uint64_t>(id) << 32) ^ value ^ (answered ? 0x80000000ull : 0);
  {
    std::lock_guard guard(lock);
    for (auto& slot : seen) {
      if (slot == key) return;
      if (slot == 0) {
        slot = key;
        break;
      }
    }
  }
  std::ostringstream text;
  text << "mfgunlock: DRS 0x" << std::hex << id << " read by " << CallerModule(caller) << ": driver "
       << (result == 0 ? "0x" : "status ") << (result == 0 ? value : static_cast<uint32_t>(result));
  if (answered) text << ", answered 0x" << DrsValueFor(g_max_multiplier.load(std::memory_order_relaxed));
  reshade::log::message(reshade::log::level::info, text.str().c_str());
}

inline int __cdecl HookedGetSetting(void* session, void* profile, uint32_t setting_id, void* setting) {
  const int result = g_real_get_setting(session, profile, setting_id, setting);
  if (setting == nullptr) return result;
  auto* bytes = static_cast<uint8_t*>(setting);
  uint32_t version = 0;
  std::memcpy(&version, bytes, sizeof(version));
  if (version != kSettingVersion1) return result;
  const bool dlssg_setting = setting_id == kDynamicMaxFrameCount || setting_id == kDlssgMode ||
                             setting_id == kDlssgMultiFrameCount || setting_id == kDlssgTargetFrameRate;
  if (!dlssg_setting) return result;
  uint32_t driver_value = 0;
  if (result == 0) std::memcpy(&driver_value, bytes + kCurrentValueOffset, sizeof(driver_value));
  const uint32_t multiplier = g_max_multiplier.load(std::memory_order_relaxed);
  const bool answer = setting_id == kDynamicMaxFrameCount && multiplier != 0;
  if (setting_id == kDynamicMaxFrameCount) {
    g_driver_value.store(result == 0 ? driver_value : 0, std::memory_order_relaxed);
    g_read_seen.store(true, std::memory_order_release);
  }
  LogRead(setting_id, result, driver_value, answer, _ReturnAddress());
  if (!answer) return result;
  const uint32_t value = DrsValueFor(multiplier);
  const uint32_t dword_type = 0, current_profile = 0, not_predefined = 0;
  std::memcpy(bytes + kSettingIdOffset, &setting_id, sizeof(setting_id));
  std::memcpy(bytes + kSettingTypeOffset, &dword_type, sizeof(dword_type));
  if (result != 0) std::memcpy(bytes + kSettingLocationOffset, &current_profile, sizeof(current_profile));
  std::memcpy(bytes + kIsCurrentPredefinedOffset, &not_predefined, sizeof(not_predefined));
  std::memcpy(bytes + kCurrentValueOffset, &value, sizeof(value));
  g_answered_value.store(value, std::memory_order_relaxed);
  return 0;  // NVAPI_OK
}

// may_load: only outside the loader lock (slInit, device creation). Elsewhere
// the hook goes in only once nvapi64.dll is already mapped.
inline void TryInstall(bool may_load = false) {
  if (g_hooked.load(std::memory_order_acquire)) return;
  static std::once_flag once;
  HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
  if (nvapi == nullptr && may_load) nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (nvapi == nullptr) return;
  std::call_once(once, [nvapi] {
    using QueryFn = void*(__cdecl*)(uint32_t);
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    void* target = query ? query(kGetSettingId) : nullptr;
    if (target == nullptr) {
      reshade::log::message(reshade::log::level::warning, "mfgunlock: NvAPI_DRS_GetSetting not found; the Dynamic MFG ceiling is unavailable.");
      return;
    }
    g_real_get_setting = reinterpret_cast<GetSettingFn>(target);
    if (hook::InstallOne(reinterpret_cast<void**>(&g_real_get_setting), reinterpret_cast<void*>(&HookedGetSetting)))
      g_hooked.store(true, std::memory_order_release);
    else
      reshade::log::message(reshade::log::level::warning, "mfgunlock: NvAPI_DRS_GetSetting hook failed; the Dynamic MFG ceiling is unavailable.");
  });
}

inline void Uninstall() {
  if (g_hooked.load() &&
      hook::Uninstall({{"NvAPI_DRS_GetSetting", reinterpret_cast<void**>(&g_real_get_setting),
                        reinterpret_cast<void*>(&HookedGetSetting)}}))
    g_hooked.store(false);
}
}  // namespace mfgunlock::drsoverride
