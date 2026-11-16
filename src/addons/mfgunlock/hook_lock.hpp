// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0: one lock for every code patch in the process. The XMX hooks (Detours, ngx_hook.hpp) and the
// XeMFG/Vulkan hooks (MinHook, xemfg/native_bridge.cpp) both suspend the other threads while they patch; at
// DOOM: The Dark Ages' first Present both ran at once, each suspended the other, and the game hung on a black screen.
#include <windows.h>
#include <mutex>

namespace mfgunlock {
inline std::mutex& HookTransactionLock() {
  static std::mutex lock;
  return lock;
}
// The process heap's lock for the whole patch. Both libraries suspend the other threads and then allocate (Detours'
// DetourUpdateThread does); a thread suspended while it held the heap's lock hung DOOM: The Dark Ages at its first
// Present. Taken while every other thread still runs, so none can hold it once suspended; recursive for this thread.
struct ProcessHeapGuard {
  HANDLE heap = GetProcessHeap();
  bool locked = heap != nullptr && HeapLock(heap) != FALSE;
  ProcessHeapGuard() = default;
  ~ProcessHeapGuard() { if (locked) HeapUnlock(heap); }
  ProcessHeapGuard(const ProcessHeapGuard&) = delete;
  ProcessHeapGuard& operator=(const ProcessHeapGuard&) = delete;
};
}  // namespace mfgunlock
