// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (S1): when the game starts closing, for the session marker (support_files.hpp).
//
// The marker used to be removed only in DLL_PROCESS_DETACH, which TerminateProcess skips:
// Unreal games, launchers, anti-cheats and Steam's Stop then left it behind, and the next start
// said "the last session may have closed unexpectedly" after a normal exit. Two signals now mark
// the session as closing:
// - the game's last swap chain destroyed (a resize does not count; a new swap chain is running
//   again, as after a fullscreen switch);
// - TerminateProcess on the game's own process.
#include <windows.h>

#include <atomic>
#include <filesystem>

#include "ngx_hook.hpp"
#include "support_files.hpp"

namespace mfgunlock::sessionexit {

namespace internal {
inline std::atomic<int> live{0};
inline std::filesystem::path folder;
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
inline TerminateProcessFn real_terminate = nullptr;
inline std::atomic_bool installed{false};
inline BOOL WINAPI HookedTerminateProcess(HANDLE process, UINT code) {
  if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
    support::MarkStage(folder, support::Stage::kClosing);
  return real_terminate(process, code);
}
}  // namespace internal

inline void SetFolder(const std::filesystem::path& folder) { internal::folder = folder; }

// ReShade's init_swapchain / destroy_swapchain (resize: ResizeBuffers, not a new swap chain).
inline void OnInitSwapchain(bool resize) {
  if (resize || internal::folder.empty()) return;
  if (internal::live.fetch_add(1) <= 0) support::MarkStage(internal::folder, support::Stage::kRunning);
}
inline void OnDestroySwapchain(bool resize) {
  if (resize || internal::folder.empty()) return;
  int expected = internal::live.load();
  while (expected > 0 && !internal::live.compare_exchange_weak(expected, expected - 1)) {}
  if (expected == 1) support::MarkStage(internal::folder, support::Stage::kClosing);
}

// Only from a copy of the addon that stays loaded (pinned): the hook is never removed.
// Not under the loader lock (Detours suspends the other threads).
inline void InstallTerminateHook() {
  if (internal::installed.exchange(true)) return;
  const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto* proc = kernel32 ? reinterpret_cast<internal::TerminateProcessFn>(GetProcAddress(kernel32, "TerminateProcess"))
                        : nullptr;
  if (proc == nullptr) return;
  internal::real_terminate = proc;
  if (!hook::InstallOne(reinterpret_cast<void**>(&internal::real_terminate),
                        reinterpret_cast<void*>(&internal::HookedTerminateProcess)))
    internal::real_terminate = nullptr;
}

}  // namespace mfgunlock::sessionexit
