// SPDX-License-Identifier: MIT
// 1.11.0 (H1): the draws the HUD fix takes as a full-screen pass.
#include <cstdio>
#include "../src/addons/mfgunlock/hud_pass.hpp"
using mfgunlock::hudcapture::IsFullScreenPass;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
int main() {
  CHECK(IsFullScreenPass(false, 3, 1));   // KCD2: draw 3x1
  CHECK(IsFullScreenPass(false, 4, 1));   // a strip quad
  CHECK(IsFullScreenPass(true, 3, 1));    // Hi-Fi RUSH (Unreal): draw_indexed 3x1
  CHECK(!IsFullScreenPass(true, 6, 1));   // a HUD quad
  CHECK(!IsFullScreenPass(true, 18, 1));  // HUD glyphs
  CHECK(!IsFullScreenPass(false, 3, 2));  // instanced
  CHECK(!IsFullScreenPass(true, 3, 4));
  if (failures == 0) std::printf("full-screen passes: draw 3/4, draw_indexed 3; not HUD quads or instanced\n");
  return failures == 0 ? 0 : 1;
}
