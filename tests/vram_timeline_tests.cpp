// SPDX-License-Identifier: MIT
#include <cstdio>

#include "../src/addons/mfgunlock/vram_timeline.hpp"

using namespace mfgunlock::vramtimeline;

static int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

int main() {
  constexpr uint64_t MiB = 1ull << 20;
  Timeline t;
  CHECK(Describe(t, 0, 0).find("not measured") != std::string::npos);
  t.Sample(1000, 0, 4000 * MiB);
  t.Sample(2000, 0, 4100 * MiB);  // the last sample before FG is the baseline
  CHECK(t.before == 4100 * MiB);
  CHECK(Describe(t, 0, 0).find("not on long enough") != std::string::npos);
  const uint64_t on = 5000;
  t.Sample(on + 1000, on, 9000 * MiB);  // inside the settle time: ignored
  CHECK(t.with_samples == 0);
  t.Sample(on + 3000, on, 5000 * MiB);
  t.Sample(on + 8000, on, 5600 * MiB);
  t.Sample(on + 12000, on, 5500 * MiB);
  t.Sample(on + 25000, on, 9999 * MiB);  // after the window: ignored
  CHECK(t.with == 5600 * MiB && t.with_samples == 3);
  CHECK(!t.Done(on + 20000, on) && t.Done(on + 20001, on) && !t.Done(99999, 0));
  const auto text = Describe(t, 300 * MiB, 7);
  CHECK(text.find("before frame generation 4100 MiB, with it 5600 MiB (+1500 MiB)") != std::string::npos);
  CHECK(text.find("XMX's own copies 300 MiB in 7 resource(s)") != std::string::npos);
  Timeline started_on;
  started_on.Sample(on + 4000, on, 6000 * MiB);
  CHECK(Describe(started_on, 0, 0).find("no baseline") != std::string::npos);
  if (failures == 0) std::printf("vram_timeline: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
