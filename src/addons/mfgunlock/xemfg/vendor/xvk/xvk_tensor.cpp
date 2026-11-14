#include "xvk_tensor.h"
#include "xvk_internal.h"
#include "../cuda/cuda_layers.h"
#include <algorithm>
#include <cstring>

namespace xvk
{
namespace
{
VkResult CreateModule(PFN_vkCreateCuModuleNVX fn, VkDevice device, const VkCuModuleCreateInfoNVX* info,
                      VkCuModuleNVX* module, unsigned long* code) noexcept
{
    __try { return fn(device, info, nullptr, module); }
    __except ((*code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { return VK_ERROR_UNKNOWN; }
}
VkResult CreateFunction(PFN_vkCreateCuFunctionNVX fn, VkDevice device, const VkCuFunctionCreateInfoNVX* info,
                        VkCuFunctionNVX* function, unsigned long* code) noexcept
{
    __try { return fn(device, info, nullptr, function); }
    __except ((*code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { return VK_ERROR_UNKNOWN; }
}
bool LaunchKernel(PFN_vkCmdCuLaunchKernelNVX fn, VkCommandBuffer buffer, const VkCuLaunchInfoNVX* info,
                  unsigned long* code) noexcept
{
    __try { fn(buffer, info); return true; }
    __except ((*code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Contains(std::span<const unsigned char> bytes, const char* name) noexcept
{
    const auto length = std::strlen(name) + 1;
    return std::search(bytes.begin(), bytes.end(), name, name + length) != bytes.end();
}
}

bool TensorRoute::ArchitectureAllowed(int major, int minor, const std::string& name) noexcept
{
    return xefg_cuda::SupportedArchitecture(major, minor, name);
}

TensorRoute::~TensorRoute()
{
    // After a driver exception no further private driver calls are safe.
    if (!device_ || faultCode_) return;
    for (auto function : functions_) if (function && destroyFunction_) destroyFunction_(device_, function, nullptr);
    if (module_ && destroyModule_) destroyModule_(device_, module_, nullptr);
    if (sampler_ && destroySampler_) destroySampler_(device_, sampler_, nullptr);
}

bool TensorRoute::Install(const DeviceDesc& desc, std::span<const unsigned char> fatbin,
                          const TensorOptions& options, std::string& reason) noexcept
{
    try
    {
        if (!options.texturing64 || options.elfModule || options.argumentParams)
        { reason = "unvalidated_v0_combination"; return false; }
        if (device_) { reason = "already_attempted"; return false; }
        if (!desc.tensorExtensions) { reason = "no_extensions"; return false; }
        if (!desc.device || !desc.physicalDevice || !desc.getInstanceProcAddr) { reason = "device_inputs"; return false; }
        const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(desc.getInstanceProcAddr(desc.instance, "vkGetDeviceProcAddr"));
        const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(desc.getInstanceProcAddr(desc.instance, "vkGetPhysicalDeviceProperties2"));
        if (!gdpa || !props) { reason = "instance_functions"; return false; }
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        props(desc.physicalDevice, &properties);
        if (properties.properties.vendorID != 0x10DE) { reason = "not_nvidia"; return false; }
        if (!id.deviceLUIDValid) { reason = "adapter_luid"; return false; }
        LUID luid{}; std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        int major{}, minor{}; std::string name;
        if (!xefg_cuda::AdapterCapability(luid, major, minor, name)) { reason = "compute_capability_unknown"; return false; }
        if (!ArchitectureAllowed(major, minor, name))
        { reason = "architecture_unsupported"; return false; }
        if (options.texturing64 && (desc.binaryImportSpec < 2 || desc.imageViewHandleSpec < 3))
        { reason = "texturing_spec_old"; return false; }
        device_ = desc.device;
        options_ = options;
#define LOAD(member, name) member = reinterpret_cast<PFN_##name>(gdpa(device_, #name))
        LOAD(createModule_, vkCreateCuModuleNVX); LOAD(createFunction_, vkCreateCuFunctionNVX);
        LOAD(destroyModule_, vkDestroyCuModuleNVX); LOAD(destroyFunction_, vkDestroyCuFunctionNVX);
        LOAD(launch_, vkCmdCuLaunchKernelNVX); LOAD(barrier_, vkCmdPipelineBarrier2);
        LOAD(imageHandle32_, vkGetImageViewHandleNVX); LOAD(imageHandle64_, vkGetImageViewHandle64NVX);
        LOAD(destroySampler_, vkDestroySampler);
#undef LOAD
        if (!createModule_ || !createFunction_ || !destroyModule_ || !destroyFunction_ || !launch_ || !barrier_
            || !gdpa(device_, options.texturing64 ? "vkGetImageViewHandle64NVX" : "vkGetImageViewHandleNVX"))
        { reason = "device_functions"; return false; }
        for (const auto kernel : xefg_cuda::kKernels)
            if (!Contains(fatbin, kernel)) { reason = "kernel_missing"; return false; }
        major_ = major; minor_ = minor;
        const auto image = fatbin; // V0: whole pinned fatbin, no ELF extraction
        if (image.empty()) { reason = "module_bytes"; return false; }
        moduleBytes_ = image.size();
        VkCuModuleTexturingModeCreateInfoNVX texturing{VK_STRUCTURE_TYPE_CU_MODULE_TEXTURING_MODE_CREATE_INFO_NVX, nullptr, VK_TRUE};
        VkCuModuleCreateInfoNVX info{VK_STRUCTURE_TYPE_CU_MODULE_CREATE_INFO_NVX,
            &texturing, image.size(), image.data()};
        unsigned long code{};
        moduleResult_ = CreateModule(createModule_, device_, &info, &module_, &code);
        faultCode_.store(code, std::memory_order_relaxed);
        if (moduleResult_ != VK_SUCCESS || !module_)
        { reason = faultCode_ ? "module_create_fault" : "module_create"; return false; }
        for (size_t i = 0; i < functions_.size(); ++i)
        {
            VkCuFunctionCreateInfoNVX function{VK_STRUCTURE_TYPE_CU_FUNCTION_CREATE_INFO_NVX, nullptr, module_, xefg_cuda::kKernels[i]};
            const auto result = CreateFunction(createFunction_, device_, &function, &functions_[i], &code);
            faultCode_.store(code, std::memory_order_relaxed);
            if (result != VK_SUCCESS || !functions_[i]) { reason = faultCode_ ? "function_create_fault" : "function_create"; return false; }
        }
        xefg_cuda::ReportVulkan(xefg_cuda::State::Active, major, minor, uint32_t(std::size(xefg_cuda::kLayers)), "");
        active_ = true;
        xefg_cuda::Report("cuda_route_vk", "\"state\":\"active\",\"reason\":\"\",\"compute_capability\":\"" + std::to_string(major) + "." + std::to_string(minor)
            + "\",\"texturing\":\"64\",\"module\":\"fatbin\",\"params\":\"extras\",\"kernels\":16,\"full_barriers\":" + (options.fullBarriers ? "true" : "false"));
        reason.clear();
        return true;
    }
    catch (...) { reason = "exception"; return false; }
}

bool TensorRoute::Launch(VkCommandBuffer buffer, uint8_t kernel, uint32_t x, uint32_t y, uint32_t z,
                         const XefgCudaParams& params) noexcept
{
    if (!active_ || !buffer || kernel >= functions_.size()) return false;
    size_t bytes = sizeof(params);
    const void* extras[] = {reinterpret_cast<const void*>(1), &params, reinterpret_cast<const void*>(2), &bytes, nullptr};
    VkCuLaunchInfoNVX info{VK_STRUCTURE_TYPE_CU_LAUNCH_INFO_NVX};
    info.function = functions_[kernel];
    info.gridDimX = x; info.gridDimY = y; info.gridDimZ = z;
    info.blockDimX = 128; info.blockDimY = 1; info.blockDimZ = 1;
    info.extraCount = 1; info.pExtras = extras; // V0: parameter-buffer extras
    unsigned long code{};
    if (!LaunchKernel(launch_, buffer, &info, &code)) { faultCode_.store(code, std::memory_order_relaxed); Fault("launch"); return false; }
    if (options_.fullBarriers)
    {
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &memory;
        barrier_(buffer, &dependency);
    }
    return true;
}
void TensorRoute::Fault(const char* where) noexcept
{
    active_.store(false, std::memory_order_release);
    if (faultReported_.exchange(true)) return;
    xefg_cuda::ReportVulkan(xefg_cuda::State::Unavailable, major_, minor_, 0, "driver_fault");
    try { xefg_cuda::Report("cuda_route_vk", std::string("\"state\":\"unavailable\",\"reason\":\"driver_fault\",\"call\":\"") + where
        + "\",\"code\":" + std::to_string(faultCode_.load(std::memory_order_relaxed))); } catch (...) {}
}

uint64_t TensorRoute::ImageHandle(VkImageView view, bool storage) noexcept
{
    if (!active() || !view) return 0;
    VkImageViewHandleInfoNVX info{VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX};
    info.imageView = view;
    info.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    info.sampler = VK_NULL_HANDLE;
    __try { return imageHandle64_(device_, &info); }
    __except ((faultCode_ = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { Fault("image_handle"); return 0; }
}

bool TensorRoute::TryLaunch(Device& device, VkCommandBuffer buffer, uint8_t id, const Descriptor* t0,
    const Descriptor* t1, const uint32_t* constants, uint32_t constantMask, UINT x, UINT y, UINT z) noexcept
{
    if (!active() || !xefg_cuda::LayerEnabled(id)) return false;
    const auto& layer = xefg_cuda::kLayers[id - 1];
    auto fallback = [&](const char* reason) {
        ++fallbacks_; xefg_cuda::CountVulkan(0, 1);
        try {
            std::lock_guard lock(reasonsMutex_);
            if (reasons_.insert(reason).second) xefg_cuda::Report("cuda_vk_fallback", std::string("\"reason\":\"") + reason + "\",\"program\":\"" + layer.info.program + "\"");
        } catch (...) {}
        return false;
    };
    try
    {
        if (!xefg_cuda::DispatchShapeOk(layer, x, y, z)) return fallback("dispatch_shape");
        if (!t0 || !t1 || (constantMask & layer.constants) != layer.constants) return fallback("bindings_incomplete");
        const auto output = layer.family == xefg_cuda::Family::Flow ? 1 : 2;
        const bool decoder = layer.family == xefg_cuda::Family::Decoder;
        auto cudaView = [](const Descriptor& d) {
            return d.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? d.image.imageView
                : d.viewType == VK_IMAGE_VIEW_TYPE_2D ? d.cudaArrayView : VK_NULL_HANDLE;
        };
        auto imageOk = [&](const Descriptor& d, DescriptorKind kind) {
            return d.kind == kind && d.resource && cudaView(d) != VK_NULL_HANDLE;
        };
        if (!imageOk(t0[0], DescriptorKind::SrvImage) || !imageOk(t0[output], DescriptorKind::UavImage)
            || (decoder && !imageOk(t0[1], DescriptorKind::SrvImage))) return fallback("view_type");
        if (!seen_[id - 1].exchange(true)) xefg_cuda::Report("cuda_layer_vk", std::string("\"program\":\"") + layer.info.program + "\",\"kernel\":\"" + layer.info.kernel + "\"");
        XefgCudaParams params{};
        params.input = t0[0].resource->CudaHandle(cudaView(t0[0]), false, *this);
        params.output = t0[output].resource->CudaHandle(cudaView(t0[output]), true, *this);
        if (decoder) params.skip = t0[1].resource->CudaHandle(cudaView(t0[1]), false, *this);
        if (!params.input || !params.output || (decoder && !params.skip)) return fallback("image_handle");
        uint64_t* handles[] = {&params.weights, &params.scale, &params.bias};
        for (size_t i = 0; i < 3; ++i)
        {
            const auto& d = t1[i];
            if (d.kind != DescriptorKind::SrvBuffer || !d.resource || d.stride != 16 || !d.buffer.range
                || d.buffer.range % 16 || d.buffer.range / 16 > device.properties().limits.maxImageDimension1D)
                return fallback("shadow_shape");
            std::string reason;
            *handles[i] = d.resource->TensorShadow(buffer, d.buffer.offset, d.buffer.range, *this, reason);
            if (!*handles[i]) return fallback(reason.c_str());
        }
        if (!shadowSeen_[id - 1].exchange(true)) xefg_cuda::Report("cuda_vk_shadow", std::string("\"program\":\"") + layer.info.program + "\",\"elements\":" + std::to_string(t1[0].buffer.range / 16) + ",\"heap\":\"default\"");
        uint32_t gx{}, gy{}; xefg_cuda::FillLaunch(layer, x, y, constants, params, gx, gy);
        if (!Launch(buffer, layer.kernel, gx, gy, layer.gridZ, params)) return fallback("launch");
        ++launches_; xefg_cuda::CountVulkan(1, 0);
        return true;
    }
    catch (...) { return fallback("exception"); }
}

}
