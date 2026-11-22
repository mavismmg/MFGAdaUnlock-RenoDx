// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (O1, O2): frame time statistics for the performance overlay (perf_overlay.inl).
//
// One writer per ring (the thread that presents), any number of readers (the overlay). A
// reader copies the newest timestamps and computes on the copy; a timestamp being written
// while it copies costs one wrong frame time in a window of hundreds, which the overlay can
// live with, and nothing on the Present path waits.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mfgunlock::perfstats {

constexpr size_t kCapacity = 2048;  // power of two

struct Ring {
  std::array<std::atomic<uint64_t>, kCapacity> ticks{};
  std::atomic<uint64_t> count{0};
  // The Present path: one relaxed store and one release increment.
  void Push(uint64_t tick) {
    const uint64_t n = count.load(std::memory_order_relaxed);
    ticks[n & (kCapacity - 1)].store(tick, std::memory_order_relaxed);
    count.store(n + 1, std::memory_order_release);
  }
  // Frame times (in ticks), oldest first, from the newest `max_frames` + 1 timestamps.
  std::vector<uint64_t> Intervals(size_t max_frames) const {
    const uint64_t n = count.load(std::memory_order_acquire);
    const size_t stamps = static_cast<size_t>(std::min<uint64_t>(n, std::min(max_frames + 1, kCapacity)));
    std::vector<uint64_t> out;
    if (stamps < 2) return out;
    out.reserve(stamps - 1);
    uint64_t previous = ticks[(n - stamps) & (kCapacity - 1)].load(std::memory_order_relaxed);
    for (size_t i = stamps - 1; i > 0; --i) {
      const uint64_t t = ticks[(n - i) & (kCapacity - 1)].load(std::memory_order_relaxed);
      out.push_back(t > previous ? t - previous : 0);
      previous = t;
    }
    return out;
  }
  // The newest timestamp, 0 before the first.
  uint64_t Last() const {
    const uint64_t n = count.load(std::memory_order_acquire);
    return n == 0 ? 0 : ticks[(n - 1) & (kCapacity - 1)].load(std::memory_order_relaxed);
  }
};

struct Summary {
  uint32_t frames = 0;         // frame times in the window
  double fps = 0.0;            // frames / time of the window
  double average_ms = 0.0;
  double p99_ms = 0.0;         // 99th percentile frame time
  double low1_fps = 0.0;       // average frame rate of the slowest 1% of frames
  double low01_fps = 0.0;      // the same, 0.1%
  double max_ms = 0.0;
  uint32_t hitches = 0;        // frames over the hitch threshold
};

// Pure, for the test. `intervals` in ticks of `frequency` per second; frames of 0 ticks are ignored.
inline Summary Summarize(std::vector<uint64_t> intervals, uint64_t frequency, double hitch_ms = 50.0) {
  Summary s;
  intervals.erase(std::remove(intervals.begin(), intervals.end(), uint64_t{0}), intervals.end());
  if (intervals.empty() || frequency == 0) return s;
  const double to_ms = 1000.0 / static_cast<double>(frequency);
  double total = 0.0;
  for (const uint64_t i : intervals) {
    const double ms = static_cast<double>(i) * to_ms;
    total += ms;
    if (ms > hitch_ms) ++s.hitches;
  }
  s.frames = static_cast<uint32_t>(intervals.size());
  s.average_ms = total / static_cast<double>(s.frames);
  s.fps = total > 0.0 ? 1000.0 * static_cast<double>(s.frames) / total : 0.0;
  std::sort(intervals.begin(), intervals.end());
  s.max_ms = static_cast<double>(intervals.back()) * to_ms;
  const size_t p99 = std::min(intervals.size() - 1, static_cast<size_t>(static_cast<double>(intervals.size()) * 0.99));
  s.p99_ms = static_cast<double>(intervals[p99]) * to_ms;
  const auto low = [&](double fraction) {
    const size_t n = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(intervals.size()) * fraction));
    double slow = 0.0;
    for (size_t i = intervals.size() - n; i < intervals.size(); ++i) slow += static_cast<double>(intervals[i]) * to_ms;
    return slow > 0.0 ? 1000.0 * static_cast<double>(n) / slow : 0.0;
  };
  s.low1_fps = low(0.01);
  s.low01_fps = low(0.001);
  return s;
}

}  // namespace mfgunlock::perfstats
