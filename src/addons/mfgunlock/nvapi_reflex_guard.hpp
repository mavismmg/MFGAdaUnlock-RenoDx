// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (H2): the game's own NVAPI Reflex while MFG Unlock hosts Streamline.
//
// Hi-Fi RUSH (test build 2, 2026-11-19): Add frame generation ran 2 s, then DLSS-G
// reported status 0x2 (eFailReflexNotDetectedAtRuntime) nearly all the time and the
// panel said "Compatibility issue". The game has NVIDIA Reflex of its own, called
// straight through NVAPI on the same device, while the hosted Streamline runs Reflex
// too (sl_host.hpp turns it on and marks the game's frames). Two owners of one sleep
// mode and one marker stream: DLSS-G stops seeing its markers.
//
// While the host runs, the game's NvAPI_D3D_SetSleepMode, NvAPI_D3D_Sleep,
// NvAPI_D3D_SetLatencyMarker and NvAPI_D3D12_SetAsyncFrameMarker return NVAPI_OK
// without reaching the driver. Streamline's own calls (from its sl.*.dll) pass.
// [DLSSG] HostGameReflex=1 lets the game's calls through again.
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>

#include <include/reshade.hpp>

#include "ngx_hook.hpp"
#include "sl_host.hpp"

namespace mfgunlock::nvapireflex {

// Pure, for the test: whether a call from a module with this file name is the game's.
inline bool IsStreamlineModule(const wchar_t* file_name) {
  return file_name != nullptr && _wcsnicmp(file_name, L"sl.", 3) == 0;
}

namespace internal {
using Fn2 = int(__cdecl*)(void*, void*);  // (device, params): SetSleepMode, SetLatencyMarker
using Fn1 = int(__cdecl*)(void*);         // (device): Sleep
using Fn3 = int(__cdecl*)(void*, void*);  // (queue, params): D3D12 SetAsyncFrameMarker
inline Fn2 real_set_sleep_mode = nullptr, real_set_marker = nullptr;
inline Fn1 real_sleep = nullptr;
inline Fn3 real_async_marker = nullptr;
inline std::atomic<uint64_t> blocked{0};
inline std::atomic_bool enabled{true};

inline bool FromStreamline(void* return_address) {
  HMODULE module = nullptr;
  constexpr DWORD kFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (!GetModuleHandleExW(kFlags, static_cast<LPCWSTR>(return_address), &module)) return false;
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
  const wchar_t* name = wcsrchr(path, L'\\');
  return IsStreamlineModule(name != nullptr ? name + 1 : path);
}
// True when the call is the game's and must not reach the driver.
inline bool Block(void* return_address) {
  if (!enabled.load(std::memory_order_relaxed) || !slhost::Hosting() || FromStreamline(return_address)) return false;
  if (blocked.fetch_add(1, std::memory_order_relaxed) == 0)
    reshade::log::message(reshade::log::level::info,
                          "mfgunlock: Streamline host: the game calls NVIDIA Reflex through NVAPI itself; those calls "
                          "are held back while MFG Unlock's Streamline runs Reflex (DLSS-G needs a single owner).");
  return true;
}
__declspec(noinline) inline int __cdecl SetSleepMode(void* device, void* params) {
  return Block(_ReturnAddress()) ? 0 : real_set_sleep_mode(device, params);
}
__declspec(noinline) inline int __cdecl SleepHook(void* device) {
  return Block(_ReturnAddress()) ? 0 : real_sleep(device);
}
__declspec(noinline) inline int __cdecl SetLatencyMarker(void* device, void* params) {
  return Block(_ReturnAddress()) ? 0 : real_set_marker(device, params);
}
__declspec(noinline) inline int __cdecl SetAsyncFrameMarker(void* queue, void* params) {
  return Block(_ReturnAddress()) ? 0 : real_async_marker(queue, params);
}
inline std::atomic_bool installed{false};
inline SRWLOCK lock = SRWLOCK_INIT;
}  // namespace internal

// Once nvapi64.dll is mapped and Add frame generation is wanted. Safe to call again.
inline void TryInstall(const std::filesystem::path& ini) {
  using namespace internal;
  if (installed.load(std::memory_order_acquire) || !slhost::Wanted()) return;
  const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
  if (nvapi == nullptr) return;
  AcquireSRWLockExclusive(&lock);
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&internal::lock); } } unlock;
  if (installed.exchange(true)) return;
  enabled.store(GetPrivateProfileIntW(L"DLSSG", L"HostGameReflex", 0, ini.c_str()) == 0, std::memory_order_relaxed);
  using QueryFn = void*(__cdecl*)(uint32_t);
  const auto query = reinterpret_cast<QueryFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
  if (query == nullptr) return;
  int hooked = 0;
  const auto hook_one = [&](uint32_t id, void** real, void* replacement) {
    *real = query(id);
    if (*real != nullptr && hook::InstallOne(real, replacement)) ++hooked;
    else *real = nullptr;
  };
  hook_one(0xAC1CA9E0, reinterpret_cast<void**>(&real_set_sleep_mode), reinterpret_cast<void*>(&SetSleepMode));
  hook_one(0x852CD1D2, reinterpret_cast<void**>(&real_sleep), reinterpret_cast<void*>(&SleepHook));
  hook_one(0xD9984C05, reinterpret_cast<void**>(&real_set_marker), reinterpret_cast<void*>(&SetLatencyMarker));
  hook_one(0x13C98F73, reinterpret_cast<void**>(&real_async_marker), reinterpret_cast<void*>(&SetAsyncFrameMarker));
  reshade::log::message(reshade::log::level::info,
                        ("mfgunlock: Streamline host: " + std::to_string(hooked) +
                         " NVAPI Reflex entry point(s) watched for the game's own Reflex" +
                         (enabled.load() ? "." : " ([DLSSG] HostGameReflex=1: the game's calls pass).")).c_str());
}
inline uint64_t Blocked() { return internal::blocked.load(std::memory_order_relaxed); }

}  // namespace mfgunlock::nvapireflex
