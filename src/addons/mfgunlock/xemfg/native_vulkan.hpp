// SPDX-License-Identifier: MIT
#pragma once
// 1.7 (plan F): XeMFG on Vulkan games. dashdogy's Vulkan host
// (vendor/xvkhost/vk_host.cpp, MFG Unlocker v1.7.0, MIT) runs the validated
// libxess_fg/libxell pair on the game's VkDevice through the xvk layer. This
// file connects it to the addon: hooks on vulkan-1's exports (ReShade loads
// the addon inside vkCreateInstance, before the game's device exists), the
// DLSS Super Resolution inputs from NGX's Vulkan entry points, and the game's
// DLSS-G switch from Streamline.
#include <cstdint>
#include <string>

namespace mfgunlock::xemfg::native::vulkan {
// [XeFG] Vulkan (default 1) with the XeMFG backend selected.
bool Enabled() noexcept;
// [Diagnostics] VulkanFaults=1: access violations after a takeover or inside another RenoDX addon go to the log.
void InstallDiagnostics() noexcept;
// ReShade's create_device for Vulkan (before vkCreateInstance): installs the
// vulkan-1 hooks and raises the instance to Vulkan 1.3 (the program set is
// SPIR-V 1.6). Returns true when api_version was changed.
bool BeforeInstance(uint32_t& apiVersion) noexcept;
// A Vulkan device the host prepared exists: DXGI swap chains in this process
// belong to the Vulkan driver's presentation, not to a D3D12 game.
bool GameUsesVulkan() noexcept;
// The game created a Vulkan instance with the host enabled: the D3D12 input
// adapters (Streamline tags, constants, markers, sleep) pass straight on.
bool InstanceSeen() noexcept;
// The user's multiplier (2..6), live.
void SetFrames(uint32_t multiplier) noexcept;
// Streamline's slDLSSGSetOptions from the game: its DLSS-G switch.
void SetGameFrameGeneration(bool on) noexcept;
// NVSDK_NGX_VULKAN_* hooks once _nvngx.dll is mapped.
void DiscoverNgx() noexcept;
// The panel's view of the Vulkan host (vk_host ReadSnapshot).
struct PanelStatus {
  bool game{};        // a Vulkan game with the host enabled
  bool active{};      // a taken-over swap chain presents through XeFG
  uint32_t frames{};  // frames presented per base frame
  uint32_t baseFpsMilli{},outputFpsMilli{};
  uint64_t inputFrames{};
  bool dynamic{},gameLatency{};   // 1.7.1: Dynamic MFG runs; the game's sleep drove XeLL
  uint32_t dynamicTargetMilli{},fpsLimit{};
  std::string reason;
};
PanelStatus Status() noexcept;
}
