#pragma once
// vkprov.1: XeMFG as a Vulkan DLSS-G provider (mfg_xemfg_provider.h). Each evaluation works as d3d12_provider.h
// describes, recording into the evaluation's command buffer (xvk recorded device); nothing is submitted here.
// ngxprov.1: the API table and its export live in xess/ngx_provider.cpp, shared with the D3D12 provider.
#include "mfg_xemfg_provider.h"

namespace xess_vulkan_provider
{
// The API's Vulkan entries; every call returns an NVSDK_NGX_Result.
int32_t VulkanInit(VkInstance instance, VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                   PFN_vkGetDeviceProcAddr gdpa);
int32_t VulkanCreateFeature(VkDevice device, VkCommandBuffer command, NVSDK_NGX_Parameter* parameters, void** feature);
int32_t VulkanEvaluate(VkCommandBuffer command, void* feature, NVSDK_NGX_Parameter* parameters);
int32_t ReleaseFeature(void* feature);
int32_t Shutdown();

// The provider's shim sits in OptiScaler's custom DLSSG folder (<OptiScaler folder>\custom\<name>\dlssg, also under
// its OptiScaler subfolder). Checked once; the Vulkan host then prepares instances and devices for the provider.
// ngxprov.1: also logs which DLL OptiScaler loads from each such folder (ngx_provider::OptiscalerDlssgPick).
bool ShimInstalled() noexcept;
}
