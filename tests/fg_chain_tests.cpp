// SPDX-License-Identifier: MIT
// 1.9.0: where each link of the frame generation chain came from.
#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/fg_chain.hpp"

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition \
                << '\n';                                                    \
      std::abort();                                                         \
    }                                                                       \
  } while (false)

using namespace mfgunlock::fgchain;

int main() {
  CHECK(ClassifyStreamline(nullptr) == Source::kUnknown);
  CHECK(ClassifyStreamline(L"") == Source::kUnknown);
  CHECK(ClassifyStreamline(LR"(C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_override_0\versions\1\files\sl.dlss_g.dll)") ==
        Source::kNvidiaAppOverride);
  CHECK(ClassifyStreamline(LR"(C:\ProgramData\NVIDIA\NGX\MODELS\SL_DLSS_G_0\versions\1\files\sl.dlss_g.dll)") ==
        Source::kNvidiaAppUpdate);
  CHECK(ClassifyStreamline(LR"(D:\Games\Cyberpunk 2077\bin\x64\sl.dlss_g.dll)") == Source::kGame);

  CHECK(ClassifyProvider(LR"(C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\20318480\files\160_E658700.bin)") ==
        Source::kNvidiaApp);
  CHECK(ClassifyProvider(LR"(C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_x\nvngx_dlssg.dll)") ==
        Source::kDriver);
  CHECK(ClassifyProvider(LR"(C:\Steam\steamapps\common\ELDEN RING\Game\EldenRingDLSS\nvngx_dlssg.dll)") == Source::kGame);
  CHECK(ClassifyProvider(nullptr) == Source::kUnknown);
  // 1.10.1: Add frame generation's own files are not the game's.
  CHECK(ClassifyStreamline(LR"(D:\Steam\steamapps\common\Metro Exodus Enhanced Edition\MFGUnlock-Streamline\sl.dlss_g.dll)") ==
        Source::kMfgUnlock);
  CHECK(ClassifyProvider(LR"(D:\Games\Red Dead Redemption 2\mfgunlock-streamline\nvngx_dlssg.dll)") == Source::kMfgUnlock);
  CHECK(std::string(Name(Source::kMfgUnlock)).find("MFGUnlock-Streamline") != std::string::npos);
  std::cout << "fg_chain tests passed\n";
  return 0;
}
