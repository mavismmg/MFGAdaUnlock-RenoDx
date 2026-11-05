// SPDX-License-Identifier: MIT
#pragma once
// 1.8.3: XMX on GPUs older than Ada (RTX 20 and 30). XMX replaces only NVIDIA's
// generator, so the game still needs NVIDIA's DLSS-G runtime to accept the GPU
// and create the feature; preada_admission.hpp makes it do so. This file holds
// only the decision, read by the generator and the panel. Nothing here touches
// the Ada or Blackwell paths: it reads only its own adapter census and test key.
#include <windows.h>
#include "backend_selection.hpp"
#include "gpu_arch.hpp"

namespace mfgunlock::xmx::preada {
// Test only, INI only: [DLSSG] SimulatePreAda=1 takes this path on an Ada or
// Blackwell GPU. There every answer it changes is already Ada's, so it shows
// that the path runs and changes nothing, not that an RTX 30 is accepted.
inline bool Simulated() {
  return GetPrivateProfileIntW(L"DLSSG", L"SimulatePreAda", 0, (backend::Directory() / L"MFGAdaUnlock.ini").c_str()) == 1;
}
// Read once; safe under the loader lock.
inline bool Active() {
  static const bool value = gpuarch::AllNvidiaAdaptersPreAda() || Simulated();
  return value;
}
}  // namespace mfgunlock::xmx::preada
