// SPDX-License-Identifier: MIT
#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "../src/addons/mfgunlock/xmx_policy.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using namespace mfgunlock::xmx::policy;

int main() {
  // A retired texture outlives the frames that may still be using it.
  {
    auto texture = std::make_shared<int>(1);
    RetireQueue<std::shared_ptr<int>> queue;
    queue.Push(texture, 100);
    queue.Collect(100 + kRetireAfterPairs - 1);
    CHECK(queue.Size() == 1 && texture.use_count() == 2);
    queue.Collect(100 + kRetireAfterPairs);
    CHECK(queue.Size() == 0 && texture.use_count() == 1);
    // Resizing on every frame stays bounded: age empties it.
    for (uint64_t pair = 0; pair < 1000; ++pair) {
      queue.Collect(pair);
      for (int i = 0; i < 4; ++i) queue.Push(texture, pair);
    }
    CHECK(queue.Size() <= 4 * kRetireAfterPairs);
  }

  // A new device needs a new context even with the same shape.
  int deviceA = 0, deviceB = 0;
  const ContextShape shape{&deviceA, 3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM, 1};
  ContextShape same = shape;
  CHECK(shape == same);
  same.device = &deviceB;
  CHECK(!(shape == same));

  CHECK(!PairIsStale(0, 100000));                 // nothing captured yet
  CHECK(!PairIsStale(1000, 1000 + kStalePairMs));
  CHECK(PairIsStale(1000, 1000 + kStalePairMs + 1));

  // The model can produce everything asked for.
  auto plan = PlanFrames(3, 2, 5);
  CHECK(plan.count == 3 && plan.index == 2 && !plan.repeat && !plan.limited);
  // A model limited to one frame: the rest show that frame, no permanent failure.
  plan = PlanFrames(3, 1, 1);
  CHECK(plan.count == 1 && plan.index == 1 && !plan.repeat && plan.limited);
  plan = PlanFrames(3, 3, 1);
  CHECK(plan.count == 1 && plan.index == 1 && plan.repeat && plan.limited);
  plan = PlanFrames(0, 0, 5);
  CHECK(plan.count == 1 && plan.index == 1 && !plan.repeat && !plan.limited);
  plan = PlanFrames(2, 9, 5);
  CHECK(plan.index == 2 && !plan.repeat);

  CHECK(CanCopy(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM, 1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM));
  CHECK(CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
  CHECK(CanCopy(1920, 1080, DXGI_FORMAT_R11G11B10_FLOAT, 1920, 1080, DXGI_FORMAT_R11G11B10_FLOAT));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R11G11B10_FLOAT, 1920, 1080, DXGI_FORMAT_R9G9B9E5_SHAREDEXP));

  // Dying Light 2: the active provider was mapped before the addon, the two idle
  // copies after it. Each gets its slot, whichever path reports it first.
  CHECK(PlanSlot(false, 0, 4) == SlotAction::Install);
  CHECK(PlanSlot(false, 3, 4) == SlotAction::Install);
  CHECK(PlanSlot(true, 1, 4) == SlotAction::Known);
  CHECK(PlanSlot(true, 4, 4) == SlotAction::Known);
  CHECK(PlanSlot(false, 4, 4) == SlotAction::Full);

  CHECK(Bypassed(true, false, true));
  CHECK(!Bypassed(true, true, true));    // the generator has run: not bypassed
  CHECK(!Bypassed(true, false, false));  // frame generation is off in the game
  CHECK(!Bypassed(false, false, true));  // no hook: XMX did not start, reported apart

  // A declared area is used only when it fits its texture.
  CHECK((InputArea(1488, 836, {0, 0, 1485, 835}) == Area{0, 0, 1485, 835}));
  CHECK((InputArea(1488, 836, {0, 0, 0, 0}) == Area{0, 0, 1488, 836}));
  CHECK((InputArea(1488, 836, {4, 0, 1485, 836}) == Area{0, 0, 1488, 836}));
  CHECK((InputArea(1488, 836, {0xFFFFFFFF, 0, 2, 836}) == Area{0, 0, 1488, 836}));
  // Phantom Blade Zero without declared areas: both become the size they share.
  auto [depth, motion] = SharedArea(InputArea(1488, 836, {}), InputArea(1485, 836, {}));
  CHECK((depth == Area{0, 0, 1485, 836} && motion == Area{0, 0, 1485, 836}));
  // Each input keeps its own origin, and the result stays inside both textures.
  auto [offsetDepth, offsetMotion] = SharedArea({8, 4, 1480, 830}, {0, 0, 1485, 836});
  CHECK((offsetDepth == Area{8, 4, 1480, 830} && offsetMotion == Area{0, 0, 1480, 830}));

  for (int i = 1; i < int(Idle::Count); ++i) CHECK(IdleText(Idle(i))[0] != '\0');
  CHECK(IdleText(Idle::None)[0] == '\0');
  std::cout << "PASS: XMX decisions\n";
  return 0;
}
