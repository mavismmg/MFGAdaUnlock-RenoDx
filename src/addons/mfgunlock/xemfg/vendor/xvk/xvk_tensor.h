#pragma once
#include "xvk.h"
#include "../cuda/xefg_cuda_params.h"
#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <span>
#include <string>

namespace xvk
{
// V0 first proves that the exact shipping kernel image can be imported. Installation owns
// no queue, context or submission; callers retain the device until this object is destroyed.
class Device;
struct Descriptor;
class TensorRoute
{
public:
    TensorRoute() = default;
    static bool ArchitectureAllowed(int major, int minor, const std::string& name) noexcept;
    ~TensorRoute();
    TensorRoute(const TensorRoute&) = delete;
    TensorRoute& operator=(const TensorRoute&) = delete;
    bool Install(const DeviceDesc&, std::span<const unsigned char> fatbin, const TensorOptions&, std::string& reason) noexcept;
    bool Launch(VkCommandBuffer, uint8_t kernel, uint32_t x, uint32_t y, uint32_t z, const XefgCudaParams&) noexcept;
    bool TryLaunch(Device&, VkCommandBuffer, uint8_t layer, const Descriptor* t0, const Descriptor* t1,
                   const uint32_t* constants, uint32_t constantMask, UINT x, UINT y, UINT z) noexcept;
    uint64_t ImageHandle(VkImageView, bool storage) noexcept;
    void Enable(bool on) noexcept { enabled_ = on; }
    bool active() const noexcept { return active_ && enabled_; }
    VkResult moduleResult() const noexcept { return moduleResult_; }
    unsigned long faultCode() const noexcept { return faultCode_.load(std::memory_order_relaxed); }
    size_t moduleBytes() const noexcept { return moduleBytes_; }
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCuModuleNVX module_ = VK_NULL_HANDLE;
    std::array<VkCuFunctionNVX, 16> functions_{};
    PFN_vkCreateCuModuleNVX createModule_{};
    PFN_vkCreateCuFunctionNVX createFunction_{};
    PFN_vkDestroyCuModuleNVX destroyModule_{};
    PFN_vkDestroyCuFunctionNVX destroyFunction_{};
    PFN_vkCmdCuLaunchKernelNVX launch_{};
    PFN_vkCmdPipelineBarrier2 barrier_{};
    TensorOptions options_{};
    VkResult moduleResult_ = VK_NOT_READY;
    std::atomic<unsigned long> faultCode_{};
    std::atomic<bool> faultReported_{};
    int major_{}, minor_{};
    void Fault(const char* where) noexcept;
    size_t moduleBytes_{};
    std::atomic<bool> active_{};
    std::atomic<bool> enabled_{true};
    VkSampler sampler_{};
    PFN_vkGetImageViewHandleNVX imageHandle32_{};
    PFN_vkGetImageViewHandle64NVX imageHandle64_{};
    PFN_vkDestroySampler destroySampler_{};
    std::atomic<uint64_t> launches_{}, fallbacks_{};
    std::array<std::atomic<bool>, 17> seen_{}, shadowSeen_{};
    std::mutex reasonsMutex_;
    std::set<std::string> reasons_;
};
}
