// SPDX-License-Identifier: MIT
#pragma once
// 1.10.1 (plan item 1): which sl.interposer.dll the addon's hooks belong to.
//
// Another addon can map an sl.interposer.dll without ever calling slInit
// (Shadow of the Tomb Raider with RenoDX's DLSS addon). Add frame generation
// then loads its own copy from MFGUnlock-Streamline beside it, and
// GetModuleHandleW("sl.interposer.dll") still answers with the first one.
// While hosting, every hook on Streamline goes to the host's copy.
#include <windows.h>

#include <atomic>

namespace mfgunlock::slmodule {

// Set by the D3D12 and Vulkan hosts right after they load their copy.
inline std::atomic<HMODULE> g_host{nullptr};
// Set by addon.cpp: moves the Streamline hooks to Interposer(). Called by a host
// after it set g_host and before its slInit.
inline std::atomic<void (*)()> g_rehook{nullptr};

inline HMODULE Interposer() {
  if (HMODULE host = g_host.load(std::memory_order_acquire)) return host;
  return GetModuleHandleW(L"sl.interposer.dll");
}

// A game's Streamline has run slInit by the time the game creates its device:
// slInit maps sl.common.dll. An interposer mapped without it was loaded by
// something that never started Streamline.
inline bool StartedByAnother() {
  return GetModuleHandleW(L"sl.interposer.dll") != nullptr && GetModuleHandleW(L"sl.common.dll") != nullptr;
}

// Called by a host once its copy is loaded.
inline void HostLoaded(HMODULE module) {
  const HMODULE before = GetModuleHandleW(L"sl.interposer.dll");
  g_host.store(module, std::memory_order_release);
  if (before != nullptr && before != module)
    if (auto rehook = g_rehook.load(std::memory_order_acquire)) rehook();
}

}  // namespace mfgunlock::slmodule
