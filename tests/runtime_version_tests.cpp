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

int main() {
  using namespace mfgunlock::runtimeversion;

  constexpr uint64_t dlssg = Pack(310, 9, 1, 0);
  constexpr uint64_t streamline = Pack(2, 14, 1, 0);
  CHECK(Is(dlssg, 310, 9, 1));
  CHECK(!Is(dlssg, 310, 9, 0));
  CHECK(Is(streamline, 2, 14, 1));
  CHECK(!Is(Pack(2, 14, 1, 1), 2, 14, 1));
  CHECK(!Is(0, 2, 14, 1));

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const Version current = FromModule(kernel32);
  CHECK(current.valid);
  CHECK(IsMappedImage(kernel32));
  CHECK(!IsMappedImage(nullptr));

  std::cout << "runtime version tests passed\n";
  return EXIT_SUCCESS;
}
