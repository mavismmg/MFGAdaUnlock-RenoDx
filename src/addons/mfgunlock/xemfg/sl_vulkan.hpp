// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (prototype): NVIDIA's DLSS frame generation, and XMX inside it, for Vulkan games that call DLSS
// Super Resolution straight through NGX and have no Streamline (RDR2 in Vulkan). The Vulkan counterpart of
// sl_host.hpp: Streamline 2.14 in its manual hooking mode (ProgrammingGuideManualHooking.md, section 4.2/5.2):
// - slInit (Vulkan) when ReShade's layer loads, before the game creates its device;
// - the game's vkCreateDevice gets the extensions, features and queues DLSS-G, Reflex and PCL ask for
//   (slGetFeatureRequirements), then slSetVulkanInfo;
// - the swap chain and present calls go through sl.interposer's proxies (sl_hooks.h's mandatory list);
// - each DLSS SR evaluation (NGX Vulkan) tags depth and motion and sets the constants, as on D3D12.
// [DLSSG] AddFrameGeneration=1 with the DLSS-G backend, in a process where ReShade is the Vulkan layer.
#include <windows.h>
#include <vulkan/vulkan.h>
#include <memory>
#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace mfgunlock::xemfg::native::slvk {
bool Wanted() noexcept;
// Loads MFGUnlock-Streamline\sl.interposer.dll and calls slInit (Vulkan); once. True when Streamline runs.
bool Start() noexcept;
bool Active() noexcept;
// A call from one of Streamline's modules: the vulkan-1 hooks pass it straight on.
bool IsStreamlineCaller(const void* address) noexcept;

struct DeviceEdit;
struct DeviceEditDeleter { void operator()(DeviceEdit*) const noexcept; };
using DeviceEditPtr = std::unique_ptr<DeviceEdit, DeviceEditDeleter>;
// The game's device creation with Streamline's additions; null when nothing could be added.
DeviceEditPtr EditDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept;
const VkDeviceCreateInfo* Edited(DeviceEdit* edit, const VkDeviceCreateInfo* fallback) noexcept;
// Puts back the game's own feature structures (their bits were raised in place for the call).
void Restore(DeviceEdit* edit) noexcept;
void DeviceCreated(DeviceEdit* edit, VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept;

// The game's Win32 surfaces (native_vulkan.cpp's hooks). While Streamline is shown an existing surface,
// ShowingSurface() is that surface and the vkCreateWin32SurfaceKHR hook returns it.
void SurfaceCreated(VkSurfaceKHR surface, HWND window) noexcept;
void SurfaceDestroyed(VkSurfaceKHR surface) noexcept;
VkSurfaceKHR ShowingSurface() noexcept;
void ShowSurface(VkSurfaceKHR surface) noexcept;

// Streamline's proxies. Each returns false when it does not handle the call (Streamline not on that device).
bool CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*, VkResult&) noexcept;
bool DestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) noexcept;
bool GetSwapchainImages(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*, VkResult&) noexcept;
bool AcquireNextImage(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*, VkResult&) noexcept;
bool QueuePresent(VkQueue, const VkPresentInfoKHR*, VkResult&) noexcept;
bool DeviceWaitIdle(VkDevice, VkResult&) noexcept;

// NGX Vulkan (native_vulkan.cpp's hooks): DLSS SR's creation flags, and its evaluation's inputs.
void FeatureCreated(const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params) noexcept;
void AfterEvaluate(VkCommandBuffer command, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
                   uint32_t depthUsage, uint32_t motionUsage) noexcept;

// The panel's status, in sl_host.hpp's State values (0 off ... 7 generating).
int PanelState() noexcept;
// The support report's line.
std::string Describe();
}  // namespace mfgunlock::xemfg::native::slvk
