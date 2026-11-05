// SPDX-License-Identifier: MIT
// 1.7 (plan C.1): GPU family from the adapter's PCI identity.
#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/gpu_arch.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using namespace mfgunlock::gpuarch;

int main() {
  // Ada: 4090 (AD102), 4070 SUPER (AD104), 4060 laptop (AD107).
  CHECK(ClassifyDeviceId(0x2684) == Family::kAda);
  CHECK(ClassifyDeviceId(0x2783) == Family::kAda);
  CHECK(ClassifyDeviceId(0x28A0) == Family::kAda);
  // Blackwell: 5090 (GB202), 5070 Ti (GB203), 5060 (GB206), 5050 (GB207),
  // 5070 (GB205).
  CHECK(ClassifyDeviceId(0x2B85) == Family::kBlackwell);
  CHECK(ClassifyDeviceId(0x2C05) == Family::kBlackwell);
  CHECK(ClassifyDeviceId(0x2D05) == Family::kBlackwell);
  CHECK(ClassifyDeviceId(0x2D83) == Family::kBlackwell);
  CHECK(ClassifyDeviceId(0x2F04) == Family::kBlackwell);
  // Ampere (3080), GB100 data centre and future IDs stay Unknown.
  CHECK(ClassifyDeviceId(0x2206) == Family::kUnknown);
  CHECK(ClassifyDeviceId(0x2901) == Family::kUnknown);
  CHECK(ClassifyDeviceId(0x3000) == Family::kUnknown);

  // 1.8.3, pre-Ada: 3060 Ti (GA104), 3070 (GA104), 2070 (TU106), GTX 1660
  // (TU116); never Ada, Blackwell or data centre.
  CHECK(IsPreAdaDeviceId(0x2489) && IsPreAdaDeviceId(0x2484));
  CHECK(IsPreAdaDeviceId(0x1F02) && IsPreAdaDeviceId(0x2184));
  CHECK(!IsPreAdaDeviceId(0x2684) && !IsPreAdaDeviceId(0x2783) && !IsPreAdaDeviceId(0x28A0));
  CHECK(!IsPreAdaDeviceId(0x2B85) && !IsPreAdaDeviceId(0x2901));
  {
    Census ampere;
    ampere.AddDevice(0x2489);
    CHECK(ampere.AllPreAda() && !ampere.AllBlackwell());
    ampere.AddDevice(0x2783);  // an Ada card beside it can run DLSS-G
    CHECK(!ampere.AllPreAda() && !ampere.AllBlackwell());
    Census blackwell;
    blackwell.AddDevice(0x2C05);
    CHECK(blackwell.AllBlackwell() && !blackwell.AllPreAda());
    CHECK(!Census{}.AllPreAda());
  }

  uint32_t id = 0;
  CHECK(ParseNvidiaDeviceId(L"pci\\ven_10de&dev_2c05", id) && id == 0x2C05);
  CHECK(ParseNvidiaDeviceId(L"PCI\\VEN_10DE&DEV_2783&SUBSYS_00000000", id) &&
        id == 0x2783);
  CHECK(!ParseNvidiaDeviceId(L"pci\\ven_8086&dev_a780", id));
  CHECK(!ParseNvidiaDeviceId(L"pci\\ven_10de&dev_27", id));
  CHECK(!ParseNvidiaDeviceId(nullptr, id));

  // Blackwell only when every NVIDIA adapter is Blackwell.
  Census none;
  CHECK(!none.AllBlackwell());
  Census single;
  single.Add(Family::kBlackwell);
  CHECK(single.AllBlackwell());
  Census mixed = single;
  mixed.Add(Family::kAda);
  CHECK(!mixed.AllBlackwell());
  Census unknown;
  unknown.Add(Family::kUnknown);
  CHECK(!unknown.AllBlackwell());

  // This machine's registry read must not fail or throw; print what it saw.
  const Census local = ReadInstalledAdapters();
  std::cout << "NVIDIA adapters " << local.nvidia << ", Blackwell "
            << local.blackwell << "\n";
  std::cout << "gpu_arch tests passed\n";
  return 0;
}
