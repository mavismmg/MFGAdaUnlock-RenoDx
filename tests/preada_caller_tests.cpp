// SPDX-License-Identifier: MIT
// 1.10.0 (plan 2.1): which callers the RTX 20/30 architecture answer admits
// outside the slInit window: Streamline's own modules and the NGX core, never
// the game or a system DLL.
#include <windows.h>

#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/preada_admission.hpp"

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition \
                << '\n';                                                    \
      std::abort();                                                         \
    }                                                                       \
  } while (false)

using namespace mfgunlock::xmx::preada;

int main() {
  // The game's own Streamline and the NGX core.
  CHECK(RuntimeModulePath(LR"(C:\Games\Rift\sl.dlss_g.dll)"));
  CHECK(RuntimeModulePath(LR"(C:\Games\Rift\SL.COMMON.DLL)"));
  CHECK(RuntimeModulePath(LR"(C:\Games\Rift\sl.interposer.dll)"));
  CHECK(RuntimeModulePath(LR"(C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_x\_nvngx.dll)"));
  // The frame generation files, and the NVIDIA app's renamed copies (traced on
  // the development PC under SimulatePreAda=2).
  CHECK(RuntimeModulePath(LR"(C:\Games\Rift\nvngx_dlssg.dll)"));
  CHECK(RuntimeModulePath(LR"(C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\20318480\files\160_E658700.bin)"));
  CHECK(RuntimeModulePath(LR"(C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\1\files\190_E658700.dll)"));
  // Never the game, a system DLL or a look-alike name.
  CHECK(!RuntimeModulePath(LR"(C:\Games\Rift\RiftApart.exe)"));
  CHECK(!RuntimeModulePath(LR"(C:\Games\Rift\slang.dll)"));
  CHECK(!RuntimeModulePath(LR"(C:\Games\Rift\sl.dll)"));
  CHECK(!RuntimeModulePath(LR"(C:\WINDOWS\System32\nvapi64.dll)"));
  CHECK(!RuntimeModulePath(LR"(C:\Games\NVIDIA\models\game.dll)"));
  CHECK(!RuntimeModulePath(nullptr) && !RuntimeModulePath(L""));
  // Real addresses: this test's own code and a system DLL are never the runtime.
  CHECK(!CallerIsRuntime(reinterpret_cast<const void*>(&main)));
  CHECK(!CallerIsRuntime(reinterpret_cast<const void*>(&GetTickCount64)));
  CHECK(!CallerIsRuntime(nullptr));
  // 1.10.0 (dashdogy A3): Off only on an admitted GPU, after three refusals in a row.
  CHECK(!HoldOffDecision(true, 0) && !HoldOffDecision(true, kHoldOffAfter - 1));
  CHECK(HoldOffDecision(true, kHoldOffAfter) && HoldOffDecision(true, kHoldOffAfter + 10));
  CHECK(!HoldOffDecision(false, kHoldOffAfter + 10));  // RTX 40/50, or the admission off
  std::cout << "preada_caller tests passed\n";
  return 0;
}
