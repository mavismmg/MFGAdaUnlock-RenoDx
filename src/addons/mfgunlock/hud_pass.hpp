// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (H1): which draws the HUD fix (hud_capture.hpp) takes as a full-screen pass. Pure, for the test.
//
// KCD2 draws its last scene pass as `draw 3x1`. Unreal Engine games draw it indexed:
// Hi-Fi RUSH (HudTrace, 2026-11-19) tonemaps with `draw_indexed 3x1` into the
// 2560x1440 HDR10 target and then draws its HUD there as quads (`draw_indexed 6x1`,
// 18x1, 60x1 ...), so the fix never armed. A single indexed triangle is taken as
// full-screen too; six indices are not, since a HUD quad has six.
#include <cstdint>

namespace mfgunlock::hudcapture {

constexpr bool IsFullScreenPass(bool indexed, uint32_t count, uint32_t instances) {
  if (instances != 1) return false;
  return indexed ? count == 3 : (count == 3 || count == 4);
}

}  // namespace mfgunlock::hudcapture
