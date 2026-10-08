#include <cstdlib>
#include <iostream>
#include <string_view>

#include "../src/addons/mfgunlock/blackwell.hpp"

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
    return EXIT_FAILURE; \
  } \
} while (false)

int main() {
  using namespace mfgunlock;
  using namespace blackwell;
  using namespace std::literals;
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  constexpr bool local_build = true;
#else
  constexpr bool local_build = false;
#endif
  CHECK(qualitybuild::kConfidenceHistoryEnabled == !local_build);
  // Defaults, saved modes and invalid inputs after public normalization.
  for (unsigned int normalized : {1u, 2u}) {
    CHECK(qualitybuild::StabilityMode(normalized) ==
          (local_build ? 1u : normalized));
  }
  for (unsigned int normalized : {0u, 1u, 2u}) {
    CHECK(qualitybuild::InpaintMode(normalized) ==
          (local_build ? 0u : normalized));
  }
  CHECK(g_adaptive_quality_v3_temporal_geometry == !local_build);
  CHECK(g_adaptive_quality_v3_inpaint_mode == (local_build ? 0u : 2u));

  g_adaptive_quality_enabled = true;
  g_adaptive_quality_profile = adaptivequality::Profile::kLuminanceDirectionalV3;
  // Even a stale direct request cannot install a temporal payload in this build.
  for (bool temporal : {false, true}) {
    g_adaptive_quality_v3_temporal_geometry = temporal;
    CHECK(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced) ==
          ((local_build || !temporal) ? "adaptive_quality_geometry_v31_local"sv
                                     : "adaptive_quality_geometry_v31_temporal"sv));
  }
  for (unsigned int mode : {0u, 1u, 2u}) {
    g_adaptive_quality_v3_inpaint_mode = mode;
    CHECK(AdaptiveInpaintMechanism() ==
          ((local_build || mode == 0) ? "adaptive_inpaint_decision_v2"sv
           : mode == 1 ? "adaptive_inpaint_decision_v3_local"sv
                       : "adaptive_inpaint_decision_v3_temporal"sv));
  }
  g_adaptive_quality_v3_oriented_geometry = false;
  CHECK(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced) ==
        "adaptive_quality_geometry_v2"sv);
  for (auto profile : {adaptivequality::Profile::kStableV1,
                       adaptivequality::Profile::kFlickerReducedV2}) {
    g_adaptive_quality_profile = profile;
    CHECK(AdaptiveInpaintMechanism() ==
          (profile == adaptivequality::Profile::kStableV1
               ? "adaptive_inpaint_decision_v1"sv : "adaptive_inpaint_decision_v2"sv));
  }

#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
  bool geometry_found = false, inpaint_found = false;
  for (const auto& variant : generated_thin_geometry::kThinGeometryCubins) {
    const std::string_view mechanism(variant.mechanism);
    if (mechanism != "adaptive_quality_geometry_v31_local" &&
        mechanism != "adaptive_inpaint_decision_v2") continue;
    internal::ElfFingerprint fingerprint{};
    CHECK(internal::FingerprintElf(variant.data, variant.size, fingerprint));
    if (mechanism == "adaptive_quality_geometry_v31_local") {
#if defined(MFGUNLOCK_LOCAL_STABILITY)
      CHECK(fingerprint.text <= 39680u);  // Approved 1.4.2 paired-diagonal cubin.
#else
      CHECK(fingerprint.text <= 39552u);
#endif
      CHECK(fingerprint.registers == 40u);
      CHECK(fingerprint.shared == 7776u);
      geometry_found = true;
    } else {
      CHECK(fingerprint.registers <= 48u);
      CHECK(fingerprint.shared == 784u);
      CHECK(variant.size <= variant.slot_size);
      inpaint_found = true;
    }
  }
  CHECK(geometry_found && inpaint_found);
#endif
  std::cout << (local_build ? "local low-overhead" : "research baseline")
            << " quality build policy tests passed\n";
  return EXIT_SUCCESS;
}
