// SPDX-License-Identifier: MIT
// 1.11.0 (O5): the performance overlay's frame time statistics (perf_stats.hpp).
#include "../src/addons/mfgunlock/perf_stats.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                         \
  do {                                                                   \
    if (!(x)) {                                                          \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);         \
      std::exit(1);                                                      \
    }                                                                    \
  } while (0)

static bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

int main() {
  using namespace mfgunlock::perfstats;
  constexpr uint64_t kHz = 1'000'000;  // ticks: microseconds

  // Empty and degenerate input.
  CHECK(Summarize({}, kHz).frames == 0);
  CHECK(Summarize({0, 0}, kHz).frames == 0);
  CHECK(Summarize({1000}, 0).frames == 0);

  // 1000 frames of 10 ms: 100 fps everywhere.
  std::vector<uint64_t> steady(1000, 10'000);
  Summary s = Summarize(steady, kHz);
  CHECK(s.frames == 1000 && Near(s.fps, 100.0) && Near(s.average_ms, 10.0) && Near(s.p99_ms, 10.0));
  CHECK(Near(s.low1_fps, 100.0) && Near(s.low01_fps, 100.0) && Near(s.max_ms, 10.0) && s.hitches == 0);

  // 990 frames of 10 ms and 10 of 40 ms: the 1% low is the 40 ms frames (25 fps), 0.1% the single worst.
  std::vector<uint64_t> mixed(990, 10'000);
  mixed.insert(mixed.end(), 10, 40'000);
  s = Summarize(mixed, kHz);
  CHECK(s.frames == 1000 && Near(s.low1_fps, 25.0) && Near(s.low01_fps, 25.0) && Near(s.max_ms, 40.0));
  CHECK(Near(s.average_ms, 10.3) && Near(s.fps, 1000.0 / 10.3, 1e-9));
  CHECK(Near(s.p99_ms, 40.0) && s.hitches == 0);

  // A hitch over 50 ms is counted.
  mixed.push_back(120'000);
  s = Summarize(mixed, kHz);
  CHECK(s.hitches == 1 && Near(s.max_ms, 120.0) && Near(s.low01_fps, 1000.0 / 120.0));

  // The ring: intervals oldest first, wrapping past the capacity.
  Ring ring;
  CHECK(ring.Intervals(10).empty() && ring.Last() == 0);
  ring.Push(100);
  CHECK(ring.Intervals(10).empty() && ring.Last() == 100);
  for (uint64_t i = 1; i <= kCapacity + 5; ++i) ring.Push(100 + i * 10);
  const auto recent = ring.Intervals(4);
  CHECK(recent.size() == 4);
  for (const uint64_t v : recent) CHECK(v == 10);
  CHECK(ring.Intervals(100000).size() == kCapacity - 1);
  CHECK(ring.Last() == 100 + (kCapacity + 5) * 10);

  std::printf("perf_stats_tests: ok\n");
  return 0;
}
