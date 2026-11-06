#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/runtime_version.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int wmain(int argc, wchar_t** argv) {
  using namespace mfgunlock::runtimeversion;
  CHECK(argc == 1 || argc == 3);

  constexpr uint64_t dlssg = Pack(310, 9, 1, 0);
  constexpr uint64_t streamline = Pack(2, 14, 1, 0);
  CHECK(Is(dlssg, 310, 9, 1));
  CHECK(!Is(dlssg, 310, 9, 0));
  CHECK(Is(streamline, 2, 14, 1));
  CHECK(!Is(Pack(2, 14, 1, 1), 2, 14, 1));
  CHECK(!Is(0, 2, 14, 1));

  CHECK(IsDynamicDlssgVersion(dlssg));
  CHECK(IsDynamicDlssgVersion(Pack(310, 9, 16, 0)));
  CHECK(IsDynamicStreamlineVersion(streamline));
  CHECK(IsDynamicStreamlineVersion(Pack(2, 14, 16, 0)));
  // 1.8.5: a minimum (DLSS-G 310.9.1, Streamline 2.14.0), not an exact list.
  for (uint16_t patch : {2, 15, 17}) CHECK(IsDynamicDlssgVersion(Pack(310, 9, patch, 0)));
  for (uint16_t patch : {0, 2, 15, 17}) CHECK(IsDynamicStreamlineVersion(Pack(2, 14, patch, 0)));
  CHECK(IsDynamicDlssgVersion(Pack(310, 10, 16, 0)));
  CHECK(IsDynamicStreamlineVersion(Pack(2, 15, 16, 0)));
  CHECK(IsDynamicDlssgVersion(Pack(310, 9, 16, 1)));
  CHECK(IsDynamicStreamlineVersion(Pack(2, 14, 16, 1)));
  // Below the minimum: no Dynamic ABI.
  CHECK(!IsDynamicDlssgVersion(Pack(310, 9, 0, 0)));
  CHECK(!IsDynamicDlssgVersion(Pack(310, 6, 0, 0)));
  CHECK(!IsDynamicDlssgVersion(Pack(310, 4, 0, 0)));
  CHECK(!IsDynamicStreamlineVersion(Pack(2, 13, 99, 0)));
  CHECK(!IsDynamicStreamlineVersion(Pack(2, 12, 3, 0)));
  CHECK(!IsDynamicStreamlineVersion(Pack(2, 7, 32, 0)));
  // Another product line is never read as a newer version.
  CHECK(!IsDynamicDlssgVersion(Pack(311, 0, 0, 0)));
  CHECK(!IsDynamicDlssgVersion(Pack(3, 10, 0, 0)));
  CHECK(!IsDynamicStreamlineVersion(Pack(3, 0, 0, 0)));
  CHECK(!IsDynamicStreamlineVersion(Pack(310, 9, 16, 0)));
  CHECK(!IsDynamicDlssgVersion(0));
  CHECK(!IsDynamicStreamlineVersion(0));

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const Version current = FromModule(kernel32);
  CHECK(current.valid);
  CHECK(IsMappedImage(kernel32));
  CHECK(!IsMappedImage(nullptr));

  // Optional official-DLL smoke test; fixtures are not redistributed.
  if (argc == 3) {
    HMODULE provider = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE wrapper = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(provider != nullptr && wrapper != nullptr);
    const Version provider_version = FromModule(provider);
    const Version wrapper_version = FromModule(wrapper);
    CHECK(IsDynamicDlssgVersion(Pack(provider_version)));
    CHECK(IsDynamicStreamlineVersion(Pack(wrapper_version)));
    CHECK(FreeLibrary(wrapper));
    CHECK(FreeLibrary(provider));
    std::cout << "official DLL version checks passed\n";
  }

  std::cout << "runtime version tests passed\n";
  return EXIT_SUCCESS;
}
