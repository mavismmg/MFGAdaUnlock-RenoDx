// SPDX-License-Identifier: MIT
#pragma once
// 1.8.3: XMX on GPUs older than Ada (RTX 20 and 30, GTX 16). XMX replaces only
// NVIDIA's generator: the game still needs NVIDIA's DLSS-G runtime to accept the
// GPU, create the feature and present. On these GPUs Streamline refuses the
// adapter before any of that (an RTX 3060 Ti log: slIsFeatureSupported for
// DLSS-G returned 6, eErrorNoSupportedAdapterFound), so the game hides Frame
// Generation and XMX is never called. XeMFG answers that query itself and
// presents with XeFG, and it runs on these GPUs. So XMX does not hook here and
// the panel offers XeMFG. Nothing in this file touches the Ada or Blackwell
// paths: it reads only its own adapter census and test key.
#include <windows.h>
#include "backend_selection.hpp"
#include "gpu_arch.hpp"

namespace mfgunlock::xmx::preada {
// Test only, INI only: [DLSSG] SimulatePreAda=1 takes this path on an Ada or
// Blackwell GPU, to check the panel and the switch to XeMFG.
inline bool Simulated() {
  return GetPrivateProfileIntW(L"DLSSG", L"SimulatePreAda", 0, (backend::Directory() / L"MFGAdaUnlock.ini").c_str()) == 1;
}
// Read once; safe under the loader lock.
inline bool Active() {
  static const bool value = gpuarch::AllNvidiaAdaptersPreAda() || Simulated();
  return value;
}
constexpr const char* kReason =
    "NVIDIA's DLSS-G runtime does not run on this GPU (RTX 20 and 30 series), and XMX needs it to present frames";
}  // namespace mfgunlock::xmx::preada
