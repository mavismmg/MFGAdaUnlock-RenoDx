/*
 * Presentation-pacing policy shared by the addon and its regression test.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace mfgunlock::pacing {

// Modern Streamline pacing is the native/default path and needs no memory
// patch. Only an explicit request for legacy software-flip compatibility makes
// the old metering-field patch a prerequisite for raising the multiplier.
inline constexpr bool IsReady(bool legacy_software_flip_requested,
                              bool legacy_patch_applied) {
  return !legacy_software_flip_requested || legacy_patch_applied;
}

// Reflex expresses its limiter as an integer frame interval in microseconds.
// Round to nearest instead of truncating so common targets (60/100/120/144)
// do not acquire a systematic high-FPS bias.
inline constexpr uint32_t TargetFpsToFrameLimitUs(uint32_t target_fps) {
  if (target_fps == 0) return 0;
  return static_cast<uint32_t>((1000000ull + target_fps / 2u) / target_fps);
}

inline constexpr bool ShouldApplyReflexSourceCap(
    bool dynamic_enabled, bool d3d12, bool support_seen, bool supported,
    bool dynamic_applied, bool explicit_source_cap, uint32_t target_fps) {
  return dynamic_enabled && d3d12 && support_seen && supported &&
         dynamic_applied && explicit_source_cap && target_fps != 0;
}

}  // namespace mfgunlock::pacing
