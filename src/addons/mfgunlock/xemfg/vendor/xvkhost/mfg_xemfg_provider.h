#pragma once
// vkprov.1: XeMFG as a DLSS-G (NGX frame generation) provider on Vulkan. MFGUnlocker exports
// MfgXeMFGProviderGetApi; the NVSDK_NGX_VULKAN_* entry points OptiScaler's RegisterProvider gets
// (xemfg_provider_inprocess.cpp), or a beta's mfgunlocker_xemfg.dll, forward here. XeFG's internal frame generator
// runs on the game's own Vulkan device through xvk, recording into the command buffer of each evaluation;
// Streamline's DLSS-G presents and paces the frames.
// ngxprov.1: the same provider on D3D12 (xess/d3d12_provider.cpp): XeFG records straight into the evaluation's
// command list on the game's device. The NVSDK_NGX_D3D12_* entry points forward to the D3D12 calls below.
//
// Version 1 stays append-only: new entries go at the end and callers check `size`.
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

struct NVSDK_NGX_Parameter;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

#define MFG_XEMFG_PROVIDER_API_VERSION 1
#define MFG_XEMFG_PROVIDER_GET_API "MfgXeMFGProviderGetApi"
#define MFG_XEMFG_PROVIDER_SHIM_NAME L"mfgunlocker_xemfg.dll"

typedef struct MfgXeMFGProviderApi
{
    uint32_t size;      // sizeof(MfgXeMFGProviderApi) of the running MFGUnlocker
    uint32_t version;   // MFG_XEMFG_PROVIDER_API_VERSION
    // Every call returns an NVSDK_NGX_Result.
    int32_t (*VulkanInit)(VkInstance instance, VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                          PFN_vkGetDeviceProcAddr gdpa);
    int32_t (*VulkanCreateFeature)(VkDevice device, VkCommandBuffer command, struct NVSDK_NGX_Parameter* parameters,
                                   void** feature);
    // One generated frame per call: DLSSG.MultiFrameIndex 1..DLSSG.MultiFrameCount into DLSSG.OutputInterpolated.
    int32_t (*VulkanEvaluate)(VkCommandBuffer command, void* feature, struct NVSDK_NGX_Parameter* parameters);
    int32_t (*ReleaseFeature)(void* feature);   // a Vulkan feature
    int32_t (*Shutdown)(void);                  // Vulkan
    // Generated frames per real frame the provider can make (DLSSG.MultiFrameCountMax); 0 once Refusal is set.
    uint32_t (*MaxGeneratedFrames)(void);

    // ---- ngxprov.1 (appended; a shim checks `size` before using them) ----
    // Nonzero when the provider can't run in this process, with the reason (NUL-terminated, cut to `capacity`).
    // Lock-free and loads nothing, so the shim can ask from its DllMain and refuse to load: OptiScaler then falls back
    // to another provider instead of choosing one whose every feature would fail.
    uint32_t (*Refusal)(char* reason, uint32_t capacity);
    int32_t (*D3D12Init)(struct ID3D12Device* device);
    int32_t (*D3D12CreateFeature)(struct ID3D12GraphicsCommandList* list, struct NVSDK_NGX_Parameter* parameters,
                                  void** feature);
    // One generated frame per call, as VulkanEvaluate.
    int32_t (*D3D12Evaluate)(struct ID3D12GraphicsCommandList* list, void* feature, struct NVSDK_NGX_Parameter* parameters);
    int32_t (*D3D12ReleaseFeature)(void* feature);
    int32_t (*D3D12Shutdown)(void);
} MfgXeMFGProviderApi;

// The fields a version 1 MFGUnlocker before ngxprov.1 has (vkprov.1-.4): Vulkan only, no Refusal.
#define MFG_XEMFG_PROVIDER_API_SIZE_VULKAN (2 * sizeof(uint32_t) + 6 * sizeof(void*))

typedef const MfgXeMFGProviderApi* (*PFN_MfgXeMFGProviderGetApi)(uint32_t version);
