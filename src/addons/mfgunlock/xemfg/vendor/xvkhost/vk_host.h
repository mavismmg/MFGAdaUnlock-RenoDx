#pragma once
// vkxefg.1 (V1.6.2): XeMFG on Vulkan games. RTXMFG is the XeFG host here: it runs the player's own libxess_fg and
// libxell on the game's VkDevice through the xvk D3D12-on-Vulkan layer (xess/vulkan/xvk.h), takes over the
// game's swap chain (the game renders into XeFG's back buffers, XeFG presents through a VkSwapchainKHR it owns)
// and feeds XeFG the game's DLSS Super Resolution inputs.
//
// The overlay's vulkan-1 resolver wrappers (overlay_vulkan.cpp) call in here. Only Vulkan 1.0 types cross this
// header: overlay_vulkan.cpp builds with the module's pinned Vulkan 1.2 headers, the host with 1.4 ones.
// Everything fails closed: a device, swap chain or frame that does not meet every requirement stays native.
#include <Windows.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <string>

struct NVSDK_NGX_Parameter;

namespace xess_vulkan
{
// The user's choice (control file; patcher.cpp passes every revision): enabled, and the frame count (2..6).
// `harness`: the harness builds' xessVulkan key (MFG_UNLOCK_XEFG_VK_STANDALONE only); `standalone`: xemfgsa.1's
// XeMFG standalone switch, the product's.
void Configure(bool harness, bool standalone, uint32_t frames) noexcept;
// xemfgsa.5: XeMFG standalone's FPS limit (the menu's, presented frames per second; 0 off). Without OptiScaler's
// latency output a standalone chain applies it as XeLL's minimum interval between the game's frames.
void SetStandaloneFpsLimit(uint32_t fps) noexcept;
// xemfgsa.6: XeMFG standalone's Dynamic MFG (xess/xemfg_dynamic.h), as xess_d3d12::SetDynamic. Applies live.
void SetStandaloneDynamic(bool enabled, uint32_t targetFps, int rounding, bool allow1x, bool targetAsCap) noexcept;
// The game's own DLSS Frame Generation (Streamline DLSS-G options) is on. Harness: the host takes no chain over and
// gives a taken-over one back, so two frame generators never stack. XeMFG standalone (xemfgsa.1): the host replaces
// it (patcher.cpp holds DLSS-G at Off) and the game's switch turns XeMFG's generation on and off.
void SetGameFrameGeneration(bool on) noexcept;
// xemfgsa.1: a chain presents through XeFG under XeMFG standalone (the game's DLSS-G is then held at Off).
bool ReplacesFrameGeneration() noexcept;

// vkCreateInstance: the SPIR-V program set is SPIR-V 1.6, so the application's Vulkan version is raised to 1.3
// when the loader offers it and the host is enabled. `storage` keeps the edited structures alive for the call.
struct InstanceEdit
{
    VkInstanceCreateInfo info{};
    VkApplicationInfo application{};
};
const VkInstanceCreateInfo* PrepareInstance(const VkInstanceCreateInfo* info, PFN_vkGetInstanceProcAddr loader,
    InstanceEdit& storage) noexcept;
void InstanceCreated(VkInstance instance, const VkInstanceCreateInfo* used, PFN_vkGetInstanceProcAddr loader) noexcept;
void InstanceDestroyed(VkInstance instance) noexcept;

// vkCreateDevice: adds the layer's extensions, features and queues to the game's device when everything is
// supported (otherwise the game's create info is used unchanged and the device stays native).
class DeviceEdit;
struct DeviceEditDeleter { void operator()(DeviceEdit*) const noexcept; };
using DeviceEditPtr = std::unique_ptr<DeviceEdit, DeviceEditDeleter>;
DeviceEditPtr PrepareDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept;
const VkDeviceCreateInfo* EditedInfo(const DeviceEdit* edit, const VkDeviceCreateInfo* original) noexcept;
void DeviceCreated(DeviceEditPtr edit, VkDevice device) noexcept;
void DeviceDestroying(VkDevice device) noexcept;
// The game's vkDeviceWaitIdle on a device with the layer's queues: those queues are the host's, not the game's to
// synchronize, so the host waits for each of the game's queues and for the layer's own progress instead.
bool DeviceWaitIdle(VkDevice device, VkResult& result) noexcept;
// A queue the game obtained (vkGetDeviceQueue/2): the host refuses to touch queues it did not see.
void QueueSeen(VkDevice device, uint32_t family, uint32_t index, VkQueue queue) noexcept;

void SurfaceCreated(VkInstance instance, VkSurfaceKHR surface, HWND window) noexcept;
void SurfaceDestroyed(VkSurfaceKHR surface) noexcept;

// Swap-chain entry points. Each returns true when the call was the host's (result in `result`); false leaves it
// to the caller's original path. The created handle of a taken-over swap chain is the host's own, and every
// wrapper in overlay_vulkan.cpp routes it here.
bool CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* output, PFN_vkCreateSwapchainKHR original, VkResult& result) noexcept;
bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) noexcept;
bool GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images, VkResult& result) noexcept;
bool AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
    uint32_t* index, VkResult& result) noexcept;
bool SetHdrMetadata(VkDevice device, VkSwapchainKHR swapchain, const void* metadata) noexcept;
bool OwnsSwapchain(VkSwapchainKHR swapchain) noexcept;
// The resolver wrapped one of the entry points a taken-over handle reaches (vkCreateSwapchainKHR,
// vkQueuePresentKHR, vkDestroySwapchainKHR, vkGetSwapchainImagesKHR, vkAcquireNextImageKHR). No chain is taken
// over until the game resolved all five through RTXMFG's wrappers.
void EntryWrapped(const char* name) noexcept;
// MFGAdaUnlock 1.7: the DLSS input copy into the game's command buffers (on by default; off counts the inputs only).
void SetInputCopy(int mode) noexcept;
// MFGAdaUnlock 1.7: tag the copied inputs to XeFG (on by default; off presents the game's frames only).
void SetGenerate(bool enabled) noexcept;
// MFGAdaUnlock 1.7: NVIDIA's present metering on the layer's swap chain (VK_NV_present_metering), on by default.
void SetPresentMetering(bool enabled) noexcept;
// MFGAdaUnlock 1.7.1: [XeFG] VulkanGameLatency. The game's vkLatencySleepNV and simulation markers on a taken-over chain
// drive XeLL (off by default: the frame sleeps at its acquisition and its markers are all stamped there).
void SetGameLatency(bool enabled) noexcept;
// True when the game's sleep was taken as this frame's; it then skips the sleep at the acquisition.
bool GameLatencySleep(VkSwapchainKHR swapchain) noexcept;
void GameLatencyMarker(VkSwapchainKHR swapchain, uint32_t marker) noexcept;
// MFGAdaUnlock 1.7: the view the input copy samples for a DLSS input image (depth: its depth aspect alone).
using InputViewFn = VkImageView (*)(VkImage image, VkFormat format, bool depth) noexcept;
void SetInputView(InputViewFn view) noexcept;
// vkQueuePresentKHR on a taken-over swap chain (after the overlay drew into the game's image).
bool QueuePresent(VkQueue queue, const VkPresentInfoKHR* info, VkResult& result) noexcept;
// vkQueuePresentKHR on a native swap chain returned `result`: when the host wants to take it over (or give a taken
// over one back), the game is told the chain is out of date, so it recreates it through CreateSwapchain.
VkResult AfterNativePresent(VkQueue queue, const VkPresentInfoKHR* info, VkResult result) noexcept;

// The DLSS Super Resolution inputs of the frame being recorded (vk_inputs.cpp).
struct DlssInputs
{
    VkImage depth = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkImage motion = VK_NULL_HANDLE;
    VkImageView motionView = VK_NULL_HANDLE;
    VkFormat motionFormat = VK_FORMAT_UNDEFINED;
    uint32_t renderWidth = 0, renderHeight = 0;   // the rendered area
    uint32_t motionWidth = 0, motionHeight = 0;   // the motion vectors' area in the game's image (render or output size)
    uint32_t outputWidth = 0, outputHeight = 0;
    float jitterX = 0, jitterY = 0;
    float motionScaleX = 1, motionScaleY = 1;
    bool reset = false;
    bool depthInverted = false;
    bool motionJittered = false;
    bool motionLowRes = true;
    bool flagsKnown = false;   // the create flags came from the feature's own parameters
    // The layouts the views are sampled in (SHADER_READ_ONLY_OPTIMAL for DLSS's own inputs).
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout motionLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Camera and frame time when the provider knows them (OptiScaler); 0 = a generic projection, measured time.
    float cameraNear = 0, cameraFar = 0, cameraVFov = 0;
    float frameTimeMs = 0;
};
// Records the copy of the inputs into the host's own images on the game's command buffer, before DLSS reads
// them. Both copies are at render size: XeFG requires depth and motion vectors at one size, so output-size motion
// vectors are resampled and their scale converted to render pixels (XeFG's high-res flag is never used).
// Returns false (nothing recorded) unless a taken-over swap chain is waiting for this device's inputs.
bool RecordDlssInputs(VkCommandBuffer command, const DlssInputs& inputs) noexcept;
// A DLSS Super Resolution (or Ray Reconstruction) Vulkan evaluation is about to run on this command buffer: reads
// its depth, motion vectors and constants from the parameter map (NGX's own names) and records the copy. Called by
// the NGX runtime route's Vulkan Evaluate thunks (ampere_backend.cpp) before the real Evaluate.
bool CaptureDlssEvaluate(void* command, const NVSDK_NGX_Parameter* parameters) noexcept;

// Status line fields for the runtime log and the menu.
struct Snapshot
{
    bool enabled = false;
    bool deviceReady = false;
    bool active = false;          // a taken-over swap chain presents through XeFG
    bool vulkanGame = false;      // the game created a Vulkan window surface
    bool restartNeeded = false;   // enabled, but the game's device was created while the host was off
    bool gameFrameGeneration = false;
    bool hosted = false;          // OptiScaler's XeFG output owns a device (vkxefg.2)
    uint32_t frames = 0;          // XeFG's current count of presented frames per base frame (generated + 1)
    uint64_t basePresents = 0;
    uint64_t vulkanPresents = 0;  // the layer's own presents (all frames)
    uint64_t inputFrames = 0;
    uint64_t missingPrograms = 0;
    uint32_t latencyMode = 0;     // vkreflex.1: the low-latency mode the last frame slept in (MfgXeFGVkLatencyMode)
    bool latencyLimited = false;  // and whether it applied the host's FPS limit
    // xemfgsa.5: the game's and the layer's frame rates (FPS x 1000), the sample's age (UINT64_MAX before the first) and
    // XeMFG standalone's FPS limit.
    uint32_t baseFpsMilli = 0, outputFpsMilli = 0;
    uint64_t fpsAgeMs = UINT64_MAX;
    uint32_t fpsLimit = 0;
    // xemfgsa.6: Dynamic MFG runs (frames is its count, 1 = generation off), its target (FPS x 1000) and its cap mode.
    bool dynamic = false, dynamicCap = false;
    uint32_t dynamicTargetMilli = 0;
    bool gameLatency = false;     // MFGAdaUnlock 1.7.1: the game's own sleep drove XeLL on the last frame
    std::string state = "off";
    std::string reason;
    // alpha.15 (B4-12), after a failed search: the RTXMFG-XeSS folder beside MFG Unlocker, the game's EXE folder and
    // the first near-miss name (UTF-8), as xess_d3d12::Snapshot.
    std::string runtimeSearch, runtimeGame, runtimeMisnamed;
};
Snapshot ReadSnapshot() noexcept;

// OptiScaler-hosted XeFG (its XeFG_Vk output, through mfg_xefg_vk.h). OptiScaler routes the game's device and
// swap-chain calls here and provides the upscaler's inputs; RTXMFG's own Vulkan wrappers do not take anything over.
namespace hosted
{
using OriginalProc = PFN_vkVoidFunction (VKAPI_PTR*)(void* user, const char* name);
DeviceEditPtr PrepareDevice(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, uint32_t instanceApiVersion,
                            VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept;
bool DeviceCreated(DeviceEditPtr edit, VkDevice device, OriginalProc originals, void* user) noexcept;
VkResult CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, HWND window, bool invertedDepth,
                         VkSwapchainKHR* output) noexcept;
VkResult RecordFrame(VkSwapchainKHR swapchain, VkCommandBuffer command, const DlssInputs& inputs) noexcept;
VkResult WaitForPresent(VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) noexcept;
void SetEnabled(VkSwapchainKHR swapchain, bool enabled) noexcept;
// XeFG's debug view: its interpolated frames tagged (applied at the next present).
void SetDebugView(VkSwapchainKHR swapchain, bool enabled) noexcept;
bool SetInterpolatedFrames(VkSwapchainKHR swapchain, uint32_t count) noexcept;
constexpr uint32_t kMaxInterpolatedFrames = 5;
bool OwnsSwapchain(VkSwapchainKHR swapchain) noexcept;
// true when the handle was a host chain (released) or the game's old chain the host retires itself.
bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) noexcept;
VkResult GetSwapchainImages(VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) noexcept;
VkResult AcquireNextImage(VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) noexcept;
VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) noexcept;
void SetHdrMetadata(VkSwapchainKHR swapchain, const VkHdrMetadataEXT* metadata) noexcept;
// vkReleaseSwapchainImagesKHR/EXT (swapchain_maintenance1) on a hosted chain.
VkResult ReleaseSwapchainImages(VkSwapchainKHR swapchain, uint32_t count, const uint32_t* indices) noexcept;
// vkswapext.1: the swap-chain entry points the host routes (MFG_XEFG_VK_ROUTE_* in mfg_xefg_vk.h); device extensions
// whose entry points are all routed no longer keep a device native.
void SetHostRoutes(uint32_t routes) noexcept;
// vkreflex.1: the host's low-latency output (MfgXeFGVkLatency in mfg_xefg_vk.h).
void SetLatency(uint32_t mode, bool lowLatency, bool boost, float outputFpsLimit) noexcept;
}
}
