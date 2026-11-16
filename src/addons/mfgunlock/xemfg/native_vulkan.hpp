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
// 1.10.0 (plan XMX/Vulkan/VRAM, item 3): XMX on Vulkan, with the DLSS-G backend selected and
// [DLSSG] XmxGenerator=1 and XmxVulkan=1. dashdogy's Vulkan DLSS-G provider (vendor/xvkhost/vk_provider.cpp,
// MIT) replaces NVIDIA's frame generation feature: XeFG's ported model records into each evaluation's command
// buffer and Streamline's DLSS-G still presents and paces. NVIDIA's feature is never created. The host only
// adds what the provider needs to the game's device (its features-only edit); it takes no swap chain.
bool XmxProviderWanted() noexcept;
// 1.10.0 (prototype, sl_vulkan.hpp): Streamline hosted for Vulkan games with DLSS SR only.
bool StreamlineHostWanted() noexcept;
bool BeginStreamlineHost(uint32_t& apiVersion) noexcept;
std::string StreamlineHostReport() noexcept;
// The panel's status of that host, in slhost::State values (0 when it is not running).
int StreamlineHostState() noexcept;
// 1.10.0 (plan XMX Vulkan pacing): the real-frame interval XMX's provider can hold (us, 0 none), the frame's GPU cost
// it comes from, and whether the pacing runs ([DLSSG] XmxVulkanPacing, on by default).
uint32_t XmxPacingIntervalUs() noexcept;
uint32_t XmxPacingCostUs() noexcept;
bool XmxPacingEnabled() noexcept;
// XMX on Vulkan chosen in the INI (default on with XMX); XmxProviderWanted adds "this is a Vulkan game".
bool XmxProviderSelected() noexcept;
// Set once in DllMain: ReShade was loaded as the Vulkan layer (not as dxgi/d3d12/... in a D3D game).
void SetVulkanLayerHost(bool layer) noexcept;
bool VulkanLayerHost() noexcept;
void UninstallForUnload() noexcept;
// ReShade's create_device for Vulkan in that mode: the vulkan-1 hooks and the Vulkan 1.3 instance.
bool BeginXmxProvider(uint32_t& apiVersion) noexcept;
// For the support report: "", or what the provider did (created, refused and why, evaluations).
std::string XmxProviderReport() noexcept;
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
