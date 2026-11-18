// SPDX-License-Identifier: MIT
// 1.11.0 (plan item A5): where Streamline's Vulkan queues go when the addon hosts it.
#include <cstdio>
#include "../src/addons/mfgunlock/xemfg/sl_vulkan_queues.hpp"

using namespace mfgunlock::xemfg::native::slvk::queues;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
  // A typical NVIDIA device: 0 graphics+compute (16), 1 transfer (2), 2 compute (8).
  const std::vector<Family> nvidia{{kGraphics | kCompute | 0x4, 16}, {0x4, 2}, {kCompute | 0x4, 8}};

  // Room everywhere: compute after the game's compute queues, graphics after its graphics queue.
  {
    const auto plan = PlanQueues(nvidia, {{0, 1, false, 0}, {2, 2, false, 1}}, 1, 1);
    CHECK(plan.ok);
    CHECK(plan.compute.family == 2 && plan.compute.index == 2 && !plan.compute.shared);
    CHECK(plan.graphics.family == 0 && plan.graphics.index == 1 && !plan.graphics.shared);
    CHECK(plan.entries.size() == 2 && plan.entries[0].count == 2 && plan.entries[1].count == 3);
  }
  // RDR2 (2026-11-16): every graphics queue taken. Graphics falls back to no other family (none has
  // graphics), so the game's last graphics queue is shared; compute still finds room.
  {
    const auto plan = PlanQueues(nvidia, {{0, 16, false, 0}, {2, 8, false, 1}}, 1, 1);
    CHECK(plan.ok);
    CHECK(plan.graphics.family == 0 && plan.graphics.shared && plan.graphics.index == 15);
    // Compute-only family full: the graphics family also computes and has no room either -> shared.
    CHECK(plan.compute.family == 2 && plan.compute.shared && plan.compute.index == 7);
    CHECK(plan.entries[0].count == 16 && plan.entries[1].count == 8);
  }
  // Compute family full but the graphics family has room: compute moves there.
  {
    const auto plan = PlanQueues(nvidia, {{0, 1, false, 0}, {2, 8, false, 1}}, 1, 0);
    CHECK(plan.compute.family == 0 && plan.compute.index == 1 && !plan.compute.shared);
    CHECK(plan.entries[0].count == 2);
  }
  // A family the game did not use gets a new entry of its own.
  {
    const auto plan = PlanQueues(nvidia, {{0, 1, false, 0}}, 1, 0);
    CHECK(plan.compute.family == 2 && plan.compute.index == 0);
    CHECK(plan.entries.size() == 2 && plan.entries[1].family == 2 && plan.entries[1].source == -1);
  }
  // One queue in all: shared, index 0.
  {
    const std::vector<Family> single{{kGraphics | kCompute, 1}};
    const auto plan = PlanQueues(single, {{0, 1, false, 0}}, 1, 1);
    CHECK(plan.compute.family == 0 && plan.compute.shared && plan.compute.index == 0);
    CHECK(plan.graphics.family == 0 && plan.graphics.shared && plan.graphics.index == 0);
  }
  // Protected (flagged) queues are never extended.
  {
    const auto plan = PlanQueues(nvidia, {{0, 1, true, 0}}, 0, 1);
    CHECK(plan.entries.size() == 2 && plan.entries[0].count == 1 && plan.entries[1].count == 1);
    CHECK(plan.graphics.family == 0 && plan.graphics.index == 0 && !plan.graphics.shared);
  }
  // Nothing asked for: the default families, untouched entries.
  {
    const auto plan = PlanQueues(nvidia, {{0, 1, false, 0}}, 0, 0);
    CHECK(plan.compute.family == 2 && plan.graphics.family == 0 && plan.entries[0].count == 1);
  }
  // No graphics family at all.
  CHECK(!PlanQueues({{kCompute, 4}}, {}, 1, 1).ok);
  if (failures == 0) std::printf("Vulkan queue placement: room, full families, one queue, protected queues\n");
  return failures == 0 ? 0 : 1;
}
