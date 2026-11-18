// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (plan item A5): where Streamline's Vulkan queues go when the addon hosts it
// (sl_vulkan.cpp, EditDevice). Pure, for the test: no Vulkan call, flags as Vulkan's bits.
//
// Each queue Streamline asks for goes to its preferred family first, then to any
// other family with the capability, after the game's own queues there. When every
// such family is full (RDR2 asks for every queue of its graphics family), the
// game's last queue in the preferred family is shared: Streamline submits on it.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace mfgunlock::xemfg::native::slvk::queues {

inline constexpr uint32_t kGraphics = 0x1;  // VK_QUEUE_GRAPHICS_BIT
inline constexpr uint32_t kCompute = 0x2;   // VK_QUEUE_COMPUTE_BIT
inline constexpr uint32_t kNone = UINT32_MAX;

struct Family {
  uint32_t flags = 0;
  uint32_t count = 0;  // queues the family has
};
// One VkDeviceQueueCreateInfo: the game's (source = its index) or one added here (source -1).
struct Entry {
  uint32_t family = 0;
  uint32_t count = 0;
  bool flagged = false;  // created with flags (protected queues): never extended
  int source = -1;
};
struct Placement {
  uint32_t family = kNone;
  uint32_t index = 0;
  bool shared = false;
};
struct Plan {
  bool ok = false;  // false: no graphics family
  std::vector<Entry> entries;
  Placement compute, graphics;
};

// The default families: the first with graphics, and the first compute-only one (else graphics).
inline void DefaultFamilies(const std::vector<Family>& families, uint32_t& graphics, uint32_t& compute) {
  graphics = compute = kNone;
  for (uint32_t f = 0; f < families.size(); ++f) {
    if (graphics == kNone && (families[f].flags & kGraphics)) graphics = f;
    if (compute == kNone && (families[f].flags & kCompute) && !(families[f].flags & kGraphics)) compute = f;
  }
  if (compute == kNone) compute = graphics;
}

inline Plan PlanQueues(const std::vector<Family>& families, const std::vector<Entry>& game, uint32_t extra_compute,
                       uint32_t extra_graphics) {
  Plan plan;
  plan.entries = game;
  uint32_t graphics = kNone, compute = kNone;
  DefaultFamilies(families, graphics, compute);
  if (graphics == kNone) return plan;
  plan.ok = true;
  // `extra` more queues in `family`, after the game's there. False when the family has no room.
  const auto add = [&](uint32_t family, uint32_t extra, uint32_t& first) -> bool {
    if (extra == 0) return true;
    for (auto& e : plan.entries) {
      if (e.family != family || e.flagged) continue;
      if (e.count + extra > families[family].count) return false;
      first = e.count;
      e.count += extra;
      return true;
    }
    if (extra > families[family].count) return false;
    first = 0;
    plan.entries.push_back(Entry{family, extra, false, -1});
    return true;
  };
  const auto place = [&](uint32_t need, uint32_t preferred, uint32_t extra, Placement& out) {
    out.family = preferred;
    if (extra == 0) return;
    std::vector<uint32_t> order{preferred};
    for (uint32_t f = 0; f < families.size(); ++f)
      if (f != preferred && (families[f].flags & need) == need) order.push_back(f);
    for (const uint32_t f : order)
      if (add(f, extra, out.index)) {
        out.family = f;
        return;
      }
    uint32_t used = 0;
    for (const auto& e : plan.entries)
      if (e.family == preferred && !e.flagged) used = (std::max)(used, e.count);
    out.index = used ? used - 1 : 0;
    out.shared = true;
  };
  place(kCompute, compute, extra_compute, plan.compute);
  place(kGraphics, graphics, extra_graphics, plan.graphics);
  return plan;
}

}  // namespace mfgunlock::xemfg::native::slvk::queues
