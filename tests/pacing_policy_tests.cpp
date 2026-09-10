#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/pacing_policy.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int main() {
  using mfgunlock::pacing::IsReady;

  // Regression: 310.9.1 uses native pacing even though its old metering field
  // no longer has a patchable store. A missing legacy patch must not block MFG.
  CHECK(IsReady(false, false));
  CHECK(IsReady(false, true));
  CHECK(!IsReady(true, false));
  CHECK(IsReady(true, true));

  using mfgunlock::pacing::TargetFpsToFrameLimitUs;
  CHECK(TargetFpsToFrameLimitUs(0) == 0);
  CHECK(TargetFpsToFrameLimitUs(60) == 16667);
  CHECK(TargetFpsToFrameLimitUs(100) == 10000);
  CHECK(TargetFpsToFrameLimitUs(120) == 8333);
  CHECK(TargetFpsToFrameLimitUs(144) == 6944);

  using mfgunlock::pacing::ShouldApplyReflexSourceCap;
  CHECK(!ShouldApplyReflexSourceCap(true, true, true, true, true, false, 100));
  CHECK(ShouldApplyReflexSourceCap(true, true, true, true, true, true, 100));
  CHECK(!ShouldApplyReflexSourceCap(true, true, true, true, true, true, 0));
  CHECK(!ShouldApplyReflexSourceCap(true, false, true, true, true, true, 100));
  CHECK(!ShouldApplyReflexSourceCap(true, true, true, false, true, true, 100));

  std::cout << "pacing policy tests passed\n";
  return EXIT_SUCCESS;
}
