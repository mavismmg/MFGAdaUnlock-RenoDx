#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/addons/mfgunlock/blackwell.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int main() {
  using namespace mfgunlock::blackwell;

  CHECK(internal::RoleFromSharedMemory(7776) == KernelRole::MotionVector);
  CHECK(internal::RoleFromSharedMemory(3920) == KernelRole::Inpaint);
  CHECK(internal::RoleFromSharedMemory(784) == KernelRole::InpaintDecision);
  CHECK(internal::RoleFromSharedMemory(0) == KernelRole::Unknown);

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  CHECK(HasGeneratedCubins());
  CHECK(generated::kCubinsBuiltFor[0] != '\0');
  for (const auto& replacement : generated::kCubinPatches) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    CHECK(replacement.size <= replacement.orig_size);
    CHECK(internal::RoleFromSharedMemory(replacement.shared) != KernelRole::Unknown);
  }
#else
  CHECK(!HasGeneratedCubins());
#endif

#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
  for (const auto& replacement : generated_thin_geometry::kThinGeometryCubins) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    CHECK(replacement.size <= replacement.slot_size);
    CHECK(replacement.source_fnv1a64 != 0);
    CHECK(std::string(replacement.mechanism) == "intermediate_scatter");
  }
#endif

  std::cout << "blackwell kernel tests passed\n";
  return EXIT_SUCCESS;
}
