// SPDX-License-Identifier: MIT
#pragma once
// 1.10.1 (plan item 7): Add frame generation puts Streamline above ReShade
// (game -> Streamline -> ReShade -> driver). An addon that changes the swap
// chain's format in ReShade's create_swapchain event (RenoDX's HDR upgrade:
// B8G8R8A8 to R10G10B10A2 with HDR10) then changes only the real swap chain
// under Streamline, while DLSS-G's own buffers, the ones the game draws to,
// keep the game's format (Red Dead Redemption 2 in Vulkan: "cloneFakeBuffers
// format=44" against RenoDX's r10g10b10a2, then a crash).
//
// The addon loads before RenoDX, so its create_swapchain callback sees the
// game's request first; init_swapchain sees what was created. The hosts
// compare the two around their own swap chain creation and step aside when
// they differ.
#include <atomic>
#include <cstdint>
#include <string>

#include <include/reshade.hpp>

namespace mfgunlock::scguard {

inline std::atomic<uint32_t> g_requested{0};  // reshade::api::format, typeless
inline std::atomic<uint32_t> g_created{0};
inline std::atomic<uint64_t> g_created_seq{0};

inline uint32_t Typeless(reshade::api::format format) {
  return static_cast<uint32_t>(reshade::api::format_to_typeless(format));
}

inline bool OnCreateSwapchain(reshade::api::device_api, reshade::api::swapchain_desc& desc, void*) {
  g_requested.store(Typeless(desc.back_buffer.texture.format), std::memory_order_release);
  return false;
}
inline void OnInitSwapchain(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr || swapchain->get_back_buffer_count() == 0) return;
  const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_back_buffer(0));
  g_created.store(Typeless(desc.texture.format), std::memory_order_release);
  g_created_seq.fetch_add(1, std::memory_order_acq_rel);
}

// Taken before a host creates its swap chain through Streamline.
struct Watch {
  uint64_t seq = g_created_seq.load(std::memory_order_acquire);
  // True when a swap chain was created during the watch in another format than requested.
  bool Changed() const {
    if (g_created_seq.load(std::memory_order_acquire) == seq) return false;
    const uint32_t requested = g_requested.load(std::memory_order_acquire);
    const uint32_t created = g_created.load(std::memory_order_acquire);
    return requested != 0 && created != 0 && requested != created;
  }
  std::string Text() const {
    return "format " + std::to_string(g_requested.load()) + " requested, " + std::to_string(g_created.load()) +
           " created (reshade::api::format)";
  }
};

inline void Register() {
  reshade::register_event<reshade::addon_event::create_swapchain>(&OnCreateSwapchain);
}
inline void Unregister() {
  reshade::unregister_event<reshade::addon_event::create_swapchain>(&OnCreateSwapchain);
}

}  // namespace mfgunlock::scguard
