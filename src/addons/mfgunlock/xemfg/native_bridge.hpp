// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdint>
namespace mfgunlock::xemfg {
void AttachModel(HMODULE module);
bool EnsureModel();
namespace native {
struct Status {
  bool factoryHook{}, streamline{}, output{}, xell{}, inputs{}, enabled{};
  uint32_t multiplier{4}, frames{}, rejected{};
  uint32_t submitted{1};
  uint32_t lateSubmitEnds{};
  int32_t sdkResult{};
  uint64_t outputPresents{};
  char detail[256]{"Waiting for a D3D12 device"};
};
void Attach(HMODULE module);
void Draw();
Status Snapshot();
// Called outside the loader by ReShade's D3D12 device event, or the harness.
bool Install();
// Export hooks only: safe to capture a mapped interposer during addon attach.
void CaptureStreamlineStartup();
bool OwnsPresentationHooks() noexcept;
const char* PresentationNotOwned();
bool StreamlineStopping() noexcept;
}
}
