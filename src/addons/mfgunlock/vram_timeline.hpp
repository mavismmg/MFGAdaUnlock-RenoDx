// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (plan XMX/VRAM, item 1): how much video memory turning frame
// generation on costs in this process, for the support report. A reporter saw
// XMX take almost twice NVIDIA's frame generation on an RTX 4080; before
// changing anything, the report has to say how much and where.
//
// The present callback samples the process's local DXGI usage once a second
// until 20 s after the game first turns frame generation on (D3D only: DXGI
// has no budget for Vulkan). "Before" is the last sample with frame generation
// off; "with" is the highest sample from 3 to 20 s after it went on, when the
// runtime has created its resources. Other allocations the game makes in that
// window count too, so the report gives it as a measurement, not an exact cost.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

namespace mfgunlock::vramtimeline {

constexpr uint64_t kSettleMs = 3000, kWindowMs = 20000;

struct Timeline {
  uint64_t before = 0, with = 0;
  uint32_t before_samples = 0, with_samples = 0;
  // first_on: tick of the game's first frame generation request (0: not yet).
  void Sample(uint64_t now, uint64_t first_on, uint64_t usage) {
    if (usage == 0) return;
    if (first_on == 0 || now < first_on) {
      before = usage;
      ++before_samples;
      return;
    }
    const uint64_t since = now - first_on;
    if (since < kSettleMs || since > kWindowMs) return;
    with = (std::max)(with, usage);
    ++with_samples;
  }
  bool Done(uint64_t now, uint64_t first_on) const { return first_on != 0 && now > first_on + kWindowMs; }
};

inline std::string Mib(uint64_t bytes) { return std::to_string((bytes + (1u << 19)) >> 20) + " MiB"; }

// One line for the report. xmx_bytes: what XMX itself created (0 when not in use).
inline std::string Describe(const Timeline& t, uint64_t xmx_bytes, uint32_t xmx_resources) {
  std::string text;
  if (t.before_samples == 0 && t.with_samples == 0) {
    text = "not measured (frame generation not turned on yet, or Vulkan)";
  } else if (t.with_samples == 0) {
    text = "before frame generation " + Mib(t.before) + "; not on long enough yet";
  } else if (t.before_samples == 0) {
    text = "with frame generation " + Mib(t.with) + " (it was on from the start: no baseline)";
  } else {
    const int64_t delta = static_cast<int64_t>(t.with) - static_cast<int64_t>(t.before);
    text = "before frame generation " + Mib(t.before) + ", with it " + Mib(t.with) + " (" +
           (delta >= 0 ? "+" + Mib(static_cast<uint64_t>(delta)) : "-" + Mib(static_cast<uint64_t>(-delta))) + ")";
  }
  if (xmx_resources != 0)
    text += "; XMX's own copies " + Mib(xmx_bytes) + " in " + std::to_string(xmx_resources) + " resource(s) created";
  return text;
}

}  // namespace mfgunlock::vramtimeline
