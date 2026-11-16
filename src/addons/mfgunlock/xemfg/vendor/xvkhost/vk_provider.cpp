// MFGAdaUnlock 1.10.0: from dashdogy/MFG-Unlocker 128d081 (MIT, ../LICENSE.dashdogy.txt); its OptiScaler
// shim detection at the end is replaced (see there).
// vkprov.1: XeMFG as a Vulkan DLSS-G provider (vk_provider.h, mfg_xemfg_provider.h).
#include "vk_provider.h"
#include "vk_host.h"
#include "xvk_internal.h"
#include "xefg_internal.h"
#include "vk_input_copy_spv.h"
#include "xess/xess_mod.h"
#include "xess/cuda/cuda_route.h"
#include "xess/standalone_policy.h"
#include "xess/xemfg_host_common.h"
#include "xess/ngx_provider.h"
#include "xess/ngx_provider_policy.h"
#include "optiscaler_host.h"
#include "single_module.h"
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xess_vulkan_provider
{
bool gUiComposition = false;
void SetUiComposition(bool on) noexcept { gUiComposition = on; }

namespace
{
using Microsoft::WRL::ComPtr;
using xemfg_host::Escape, xemfg_host::Narrow, xemfg_host::Log;

constexpr uint32_t kMaxGenerated = xess_vulkan::hosted::kMaxInterpolatedFrames;   // 5: 6x
static_assert(kMaxGenerated == ngx_provider::kMaxGenerated);
constexpr VkImageLayout kGameLayout = VK_IMAGE_LAYOUT_GENERAL;   // prototype: the evaluation's images, as read and written

#define MFG_PROVIDER_FNS(X)                                                                                        \
    X(vkCmdCopyImage) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) X(vkCmdPushDescriptorSetKHR) X(vkCmdPushConstants)  \
    X(vkCmdDispatch) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule)              \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkGetQueryPoolResults)

struct Device
{
    std::mutex mutex;
    bool initialized = false;   // VulkanInit ran (and named a device)
    bool ready = false;         // the xvk device, XeFG's entry points and the copy pipelines exist
    std::string reason = "not_initialized";
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    uint32_t family = 0;
    ComPtr<ID3D12Device> d3d;   // xvk recorded device on `device`
    xefg_internal::Api api;
#define MFG_FIELD(name) PFN_##name name = nullptr;
    MFG_PROVIDER_FNS(MFG_FIELD)
#undef MFG_FIELD
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipelines[3] = {};   // input_copy: 0 r32f (depth), 1 rg16f, 2 rg32f (motion vectors)
    // pluginprov.2: GPU timing (ngx_provider::GpuTiming), three timestamps per evaluation in a pool kept for the
    // process (as the device is). No pool: untimed.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    uint64_t timestampFrequency = 0;                                  // ticks per second
    std::array<bool, ngx_provider::kTimingRecords> recordPending{};  // written by an evaluation, not yet read
    std::array<bool, ngx_provider::kTimingRecords> recordBatchStart{};
    uint32_t nextRecord = 0;
    int32_t record = -1;              // the evaluation's, while it records
    bool batchStartRecord = false, inputsStamped = false;
    ngx_provider::GpuTiming timing;
};
// The game owns VkDevice and destroys it before CRT teardown. Keep the recorded device
// alive for process lifetime: its destructor (and TensorRoute's) must not call a dead VkDevice.
Device& gDevice = *new Device;

struct Slot   // one real frame's inputs, kept for the next batch as its previous frame
{
    ComPtr<ID3D12Resource> backbuffer, hudless, ui, depth, motion;
    VkImageView depthView = VK_NULL_HANDLE, motionView = VK_NULL_HANDLE;
    bool hasHudless = false, hasUi = false;
    float projection[16]{};
    float jitterX = 0, jitterY = 0, motionScaleX = 0, motionScaleY = 0;
};

struct Feature
{
    xefg_internal::Handle context = nullptr;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    bool listOpen = false;
    uint32_t width = 0, height = 0;           // back buffer
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // back buffer (UNORM view of an sRGB format)
    uint32_t depthWidth = 0, depthHeight = 0;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN, hudlessFormat = DXGI_FORMAT_UNKNOWN, uiFormat = DXGI_FORMAT_UNKNOWN;
    bool inverted = false;
    std::array<Slot, 2> slots;
    uint32_t current = 0;
    bool havePrevious = false;
    std::array<ComPtr<ID3D12Resource>, kMaxGenerated> outputs;
    uint32_t lastIndex = 0;                   // the previous evaluation's DLSSG.MultiFrameIndex
    uint32_t batchCount = 0;
    bool batchReady = false;
    bool cameraLogged = false, realFormatLogged = false;
    uint64_t batches = 0, failures = 0, evaluations = 0, realCopies = 0;
    std::string fault;
    // pluginprov.2: the batch's XeFG call, kept for its later evaluations, which each record their own output
    // (ngx_provider::ExecuteRange). XeFG takes the same output array for every call of a pair.
    xefg_internal::ExecuteParams exec{};
    std::array<ID3D12Resource*, kMaxGenerated> execOutputs{};
    std::array<xefg_internal::OutputBase, kMaxGenerated> execBases{};
    uint32_t generatedThrough = 0;            // the batch's outputs recorded so far
    uint64_t paused = 0;                      // evaluations with DLSSG.EnableInterp 0
};

int32_t Ngx(NVSDK_NGX_Result result) { return static_cast<int32_t>(result); }

DXGI_FORMAT ToDxgi(VkFormat format, bool& srgb)
{
    srgb = false;
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: srgb = true; [[fallthrough]];
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: srgb = true; [[fallthrough]];
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

const NVSDK_NGX_Resource_VK* ImageParameter(NVSDK_NGX_Parameter* parameters, const char* name)
{
    void* value = nullptr;
    if (NVSDK_NGX_FAILED(parameters->Get(name, &value)) || !value) return nullptr;
    const auto* resource = static_cast<const NVSDK_NGX_Resource_VK*>(value);
    if (resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || !resource->Resource.ImageViewInfo.Image ||
        !resource->Resource.ImageViewInfo.Width || !resource->Resource.ImageViewInfo.Height) return nullptr;
    return resource;
}

template <class T> T Number(NVSDK_NGX_Parameter* parameters, const char* name, T fallback)
{
    T value{};
    return NVSDK_NGX_SUCCEED(parameters->Get(name, &value)) ? value : fallback;
}

void FullBarrier(const Device& d, VkCommandBuffer command)
{
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    d.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0,
                           nullptr, 0, nullptr);
}

void CopyColor(const Device& d, VkCommandBuffer command, VkImage source, VkImage target, uint32_t width, uint32_t height)
{
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    d.vkCmdCopyImage(command, source, kGameLayout, target, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

// target width x height from the source's top-left sourceWidth x sourceHeight (point-sampled when they differ).
void RecordSampledCopy(const Device& d, VkCommandBuffer command, VkPipeline pipeline, VkImageView source, VkImageView target,
                       uint32_t width, uint32_t height, uint32_t sourceWidth, uint32_t sourceHeight)
{
    d.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDescriptorImageInfo sourceInfo{d.sampler, source, kGameLayout};
    VkDescriptorImageInfo targetInfo{VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sourceInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &targetInfo}};
    d.vkCmdPushDescriptorSetKHR(command, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipelineLayout, 0, 2, writes);
    const uint32_t size[4] = {width, height, sourceWidth, sourceHeight};
    d.vkCmdPushConstants(command, d.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    d.vkCmdDispatch(command, (width + 7) / 8, (height + 7) / 8, 1);
}

bool CreatePipelines(Device& d)
{
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr,
                                            VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 2, bindings};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    if (d.vkCreateSampler(d.device, &samplerInfo, nullptr, &d.sampler) != VK_SUCCESS ||
        d.vkCreateDescriptorSetLayout(d.device, &setInfo, nullptr, &d.setLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &d.setLayout, 1, &push};
    if (d.vkCreatePipelineLayout(d.device, &layoutInfo, nullptr, &d.pipelineLayout) != VK_SUCCESS) return false;
    using namespace xess_vulkan::input_copy;
    const std::pair<const uint32_t*, size_t> code[3] = {{shader0, sizeof(shader0)}, {shader1, sizeof(shader1)}, {shader2, sizeof(shader2)}};
    for (int i = 0; i < 3; ++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code[i].second, code[i].first};
        VkShaderModule module = VK_NULL_HANDLE;
        if (d.vkCreateShaderModule(d.device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        pipelineInfo.layout = d.pipelineLayout;
        const VkResult created = d.vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &d.pipelines[i]);
        d.vkDestroyShaderModule(d.device, module, nullptr);
        if (created != VK_SUCCESS) return false;
    }
    return true;
}

bool PrepareDevice(Device& d);

// ---------------------------------------------------------------- pluginprov.2: GPU timing (ngx_provider::GpuTiming)
// The timestamp pool, when the recorded queue family writes timestamps; untimed otherwise. Under gDevice.mutex.
void PrepareTiming(Device& d)
{
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceProperties"));
    const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        d.gipa(d.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    VkPhysicalDeviceProperties device{};
    std::vector<VkQueueFamilyProperties> list;
    if (properties && families) {
        properties(d.physical, &device);
        uint32_t count = 0;
        families(d.physical, &count, nullptr);
        list.resize(count);
        if (count) families(d.physical, &count, list.data());
        list.resize(count);
    }
    const bool stamps = d.family < list.size() && list[d.family].timestampValidBits && device.limits.timestampPeriod > 0.0f;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = ngx_provider::kTimingRecords * 3;
    if (stamps && d.vkCreateQueryPool(d.device, &info, nullptr, &d.queryPool) == VK_SUCCESS)
        d.timestampFrequency = uint64_t(1e9 / double(device.limits.timestampPeriod) + 0.5);
    else
        d.queryPool = VK_NULL_HANDLE;
    Log("vk_provider_timing", "\"frequency\":" + std::to_string(d.timestampFrequency) + ",\"available\":" +
        (d.queryPool ? "true" : "false"));
}

// The record's stamps when the GPU has written them, into d.timing; a record not finished by its next turn is dropped.
void ConsumeRecord(Device& d, uint32_t record)
{
    if (!d.recordPending[record]) return;
    d.recordPending[record] = false;
    uint64_t stamps[3]{};
    if (d.vkGetQueryPoolResults(d.device, d.queryPool, record * 3, 3, sizeof(stamps), stamps, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    d.timing.Add({stamps[0], stamps[1], stamps[2]}, d.recordBatchStart[record], d.timestampFrequency);
}

// Stamp 0 opens the evaluation's record (reading its previous evaluation first), 1 marks the inputs copied (once; stamp 2
// writes it when nothing did), 2 closes the record.
void Stamp(Device& d, VkCommandBuffer command, uint32_t which)
{
    if (which == 0) {
        d.record = -1;
        d.batchStartRecord = false;
        d.inputsStamped = false;
        if (!d.queryPool) return;
        const uint32_t record = d.nextRecord++ % ngx_provider::kTimingRecords;
        ConsumeRecord(d, record);
        d.vkCmdResetQueryPool(command, d.queryPool, record * 3, 3);
        d.record = int32_t(record);
    }
    if (d.record < 0 || !d.queryPool) return;
    const uint32_t base = uint32_t(d.record) * 3;
    if (which == 1) {
        if (d.inputsStamped) return;
        d.inputsStamped = true;
    }
    if (which == 2 && !d.inputsStamped) d.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.queryPool, base + 1);
    d.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.queryPool, base + which);
    if (which == 2) {
        d.recordPending[size_t(d.record)] = true;
        d.recordBatchStart[size_t(d.record)] = d.batchStartRecord;
        d.record = -1;
    }
}

// The averages since the last log line, for the batch line; empty before any timed batch.
std::string TimingFields(Device& d)
{
    if (!d.timing.Batches()) return {};
    char text[160]{};
    std::snprintf(text, sizeof(text), ",\"gpu_ms\":{\"inputs\":%.3f,\"total\":%.3f,\"batches\":%llu}",
                  d.timing.InputsMs(d.timestampFrequency), d.timing.TotalMs(d.timestampFrequency),
                  static_cast<unsigned long long>(d.timing.Batches()));
    d.timing.Reset();
    return text;
}

// The xvk device, XeFG's internal entry points on the route's pair (unlocked, ported model) and the copy pipelines.
// Under gDevice.mutex. ngxprov.1: a refusal is final for the device and, the game having one device, for the
// process: it is noted for the shim's Refusal and MaxGeneratedFrames.
bool EnsureDevice(Device& d)
{
    if (d.ready) return true;
    if (!d.initialized) { d.reason = "not_initialized"; return false; }
    if (d.reason != "not_initialized" && d.reason != "pending") return false;
    if (PrepareDevice(d)) return true;
    ngx_provider::NoteRefusal("vk_" + d.reason);
    return false;
}

bool PrepareDevice(Device& d)
{
    xess_vulkan::ProviderDevice record;
    if (!xess_vulkan::FindProviderDevice(d.device, record)) { d.reason = "device_not_prepared"; return false; }
    d.physical = record.physical;
    d.instance = record.instance;
    d.gipa = record.gipa;
    d.family = record.family;
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(d.gipa(d.instance, "vkGetDeviceProcAddr"));
    if (!gdpa) { d.reason = "device_functions"; return false; }
    bool complete = true;
#define MFG_LOAD(name) d.name = reinterpret_cast<PFN_##name>(gdpa(d.device, #name)); complete &= d.name != nullptr;
    MFG_PROVIDER_FNS(MFG_LOAD)
#undef MFG_LOAD
    if (!complete) { d.reason = "device_functions"; return false; }
    std::string why;
    const HMODULE fg = xess_vulkan::ProviderRuntime(why);
    if (!fg) { d.reason = "runtime_" + why; return false; }
    xvk::DeviceDesc desc;
    desc.instance = d.instance;
    desc.physicalDevice = d.physical;
    desc.device = d.device;
    desc.getInstanceProcAddr = d.gipa;
    desc.programModule = single_module::Self();
    desc.recorded = true;
    desc.recordedFamily = d.family;
    desc.tensorExtensions = record.tensorExtensions; desc.binaryImportSpec = record.binaryImportSpec; desc.imageViewHandleSpec = record.imageViewHandleSpec;
    if (FAILED(xvk::CreateDevice(desc, IID_PPV_ARGS(&d.d3d))) || !d.d3d) { d.reason = "xvk_device"; return false; }
    if (!xess_mod::PrepareProvider(d.d3d.Get(), why)) { d.reason = "route_" + why; return false; }
    xess_vulkan::InstallConfiguredTensorCores(d.d3d.Get());
    if (!xefg_internal::Resolve(fg, d.api, why)) { d.reason = "xefg_" + why; return false; }
    if (!CreatePipelines(d)) { d.reason = "copy_pipelines"; return false; }
    PrepareTiming(d);
    d.ready = true;
    d.reason.clear();
    Log("vk_provider_ready", "\"family\":" + std::to_string(d.family) + ",\"xefg\":\"" + d.api.family + "\",\"generated\":" +
        std::to_string(kMaxGenerated) + ",\"model\":\"" + xess_mod::ProviderModelName() + "\",\"outputs_per_evaluation\":" +
        (ngx_provider::WholeBatch() ? "\"all\"" : "1"));
    return true;
}

HRESULT CreateTexture(ID3D12Device* d3d, uint32_t width, uint32_t height, DXGI_FORMAT format, ComPtr<ID3D12Resource>& out)
{
    out.Reset();
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    return d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        nullptr, IID_PPV_ARGS(&out));
}

VkImageView StorageView(ID3D12Resource* resource, VkFormat format)
{
    return xvk::Own<xvk::Resource>(resource)->View(format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                                    VK_IMAGE_USAGE_STORAGE_BIT);
}

struct Inputs
{
    const NVSDK_NGX_Resource_VK* backbuffer = nullptr;
    const NVSDK_NGX_Resource_VK* depth = nullptr;
    const NVSDK_NGX_Resource_VK* motion = nullptr;
    const NVSDK_NGX_Resource_VK* hudless = nullptr;
    const NVSDK_NGX_Resource_VK* ui = nullptr;
};

// The images of one feature: back buffer size and format, depth size (motion vectors are resampled to it). Destroys
// and recreates XeFG's context when any of them changes. Inside the recording (XeFG creates and uploads at Init).
bool EnsureFeature(Device& d, Feature& f, const Inputs& in, bool inverted)
{
    bool srgb = false;
    const auto& bb = in.backbuffer->Resource.ImageViewInfo;
    const DXGI_FORMAT format = ToDxgi(bb.Format, srgb);
    if (format == DXGI_FORMAT_UNKNOWN) { f.fault = "backbuffer_format_" + std::to_string(int(bb.Format)); return false; }
    const auto& depth = in.depth->Resource.ImageViewInfo;
    const auto& motion = in.motion->Resource.ImageViewInfo;
    const DXGI_FORMAT motionFormat = (motion.Format == VK_FORMAT_R32G32_SFLOAT || motion.Format == VK_FORMAT_R32G32B32A32_SFLOAT)
                                         ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN, uiFormat = DXGI_FORMAT_UNKNOWN;
    bool ignored = false;
    if (in.hudless) hudlessFormat = ToDxgi(in.hudless->Resource.ImageViewInfo.Format, ignored);
    if (in.ui) uiFormat = ToDxgi(in.ui->Resource.ImageViewInfo.Format, ignored);
    const bool same = f.context && f.width == bb.Width && f.height == bb.Height && f.format == format && f.depthWidth == depth.Width &&
                      f.depthHeight == depth.Height && f.motionFormat == motionFormat && f.hudlessFormat == hudlessFormat &&
                      f.uiFormat == uiFormat && f.inverted == inverted;
    if (same) return true;
    if (f.context) {
        d.api.Destroy(f.context);
        f.context = nullptr;
    }
    f.havePrevious = false;
    f.width = bb.Width;
    f.height = bb.Height;
    f.format = format;
    f.depthWidth = depth.Width;
    f.depthHeight = depth.Height;
    f.motionFormat = motionFormat;
    f.hudlessFormat = hudlessFormat;
    f.uiFormat = uiFormat;
    f.inverted = inverted;
    ID3D12Device* d3d = d.d3d.Get();
    for (Slot& slot : f.slots) {
        slot = {};
        if (FAILED(CreateTexture(d3d, f.width, f.height, format, slot.backbuffer)) ||
            FAILED(CreateTexture(d3d, f.depthWidth, f.depthHeight, DXGI_FORMAT_R32_FLOAT, slot.depth)) ||
            FAILED(CreateTexture(d3d, f.depthWidth, f.depthHeight, motionFormat, slot.motion)) ||
            (hudlessFormat && FAILED(CreateTexture(d3d, in.hudless->Resource.ImageViewInfo.Width, in.hudless->Resource.ImageViewInfo.Height,
                                                   hudlessFormat, slot.hudless))) ||
            (uiFormat && FAILED(CreateTexture(d3d, in.ui->Resource.ImageViewInfo.Width, in.ui->Resource.ImageViewInfo.Height, uiFormat, slot.ui))))
        { f.fault = "input_images"; return false; }
        slot.depthView = StorageView(slot.depth.Get(), VK_FORMAT_R32_SFLOAT);
        slot.motionView = StorageView(slot.motion.Get(), motionFormat == DXGI_FORMAT_R32G32_FLOAT ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R16G16_SFLOAT);
        if (!slot.depthView || !slot.motionView) { f.fault = "input_views"; return false; }
    }
    for (auto& output : f.outputs)
        if (FAILED(CreateTexture(d3d, f.width, f.height, format, output))) { f.fault = "output_images"; return false; }
    if (!f.allocator && FAILED(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
    { f.fault = "allocator"; return false; }
    xefg_internal::Handle context = nullptr;
    int32_t result = d.api.CreateContext(d3d, &context);
    if (result != 0 || !context) { f.fault = "create_context_" + std::to_string(result); return false; }
    xefg_internal::InitParams init{};
    init.width = f.width;
    init.height = f.height;
    init.format = format;
    init.flags = inverted ? 1u : 0u;   // XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH; motion vectors in pixels (scale below)
    init.maxInterpolatedFrames = kMaxGenerated;
    init.creationNodeMask = init.visibleNodeMask = 1;
    init.uiMode = 0;   // AUTO: from the inputs given (HUD-less color, UI texture)
    result = d.api.Init(context, init);
    if (result != 0) {
        d.api.Destroy(context);
        f.fault = "init_" + std::to_string(result);
        return false;
    }
    f.context = context;
    Log("vk_provider_feature", "\"width\":" + std::to_string(f.width) + ",\"height\":" + std::to_string(f.height) +
        ",\"format\":" + std::to_string(int(format)) + ",\"srgb\":" + (srgb ? "true" : "false") + ",\"depth\":[" +
        std::to_string(f.depthWidth) + "," + std::to_string(f.depthHeight) + "],\"motion_format\":" + std::to_string(int(motionFormat)) +
        ",\"hudless\":" + (hudlessFormat ? "true" : "false") + ",\"ui\":" + (uiFormat ? "true" : "false") +
        ",\"inverted\":" + (inverted ? "true" : "false"));
    return true;
}

xefg_internal::Resource Describe(ID3D12Resource* resource, uint32_t width, uint32_t height)
{
    return {resource, 0, 0, width, height};
}

void DescribeFrame(const Feature& f, const Slot& slot, xefg_internal::Frame& frame)
{
    frame = {};
    frame.backbuffer = Describe(slot.backbuffer.Get(), f.width, f.height);
    frame.depth = Describe(slot.depth.Get(), f.depthWidth, f.depthHeight);
    frame.motionVectors = Describe(slot.motion.Get(), f.depthWidth, f.depthHeight);
    if (slot.hasHudless) frame.hudless = Describe(slot.hudless.Get(), f.width, f.height);
    if (slot.hasUi) frame.ui = Describe(slot.ui.Get(), f.width, f.height);
    for (int i = 0; i < 16; ++i) frame.viewMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // identity: no camera matrices given
    std::memcpy(frame.projectionMatrix, slot.projection, sizeof(frame.projectionMatrix));
    frame.outputWidth = float(f.width);
    frame.outputHeight = float(f.height);
    frame.jitterX = slot.jitterX;
    frame.jitterY = slot.jitterY;
    frame.motionVectorScaleX = slot.motionScaleX;
    frame.motionVectorScaleY = slot.motionScaleY;
}

// pluginprov.2: XeFG records the outputs `range` names (from output 0 also the pair's shared work) with the batch's call,
// into the open recording.
bool RecordOutputs(Device& d, Feature& f, VkCommandBuffer command, const ngx_provider::OutputRange& range)
{
    f.exec.firstOutput = range.first;
    f.exec.outputCount = range.count;
    ID3D12Device* d3d = d.d3d.Get();
    if (!f.list) {
        if (FAILED(d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.Get(), nullptr, IID_PPV_ARGS(&f.list))))
        { f.fault = "command_list"; return false; }
    } else if (FAILED(f.allocator->Reset()) || FAILED(f.list->Reset(f.allocator.Get(), nullptr))) { f.fault = "command_list_reset"; return false; }
    const int32_t result = d.api.Execute(f.context, f.list.Get(), f.exec);
    f.list->Close();
    FullBarrier(d, command);
    if (result != 0) { f.fault = "execute_" + std::to_string(result); return false; }
    f.generatedThrough = range.through;
    return true;
}

// The batch's first evaluation: inputs into the current slot, XeFG's call for the batch, then the outputs `range` names
// (ngx_provider::ExecuteRange). Inside the recording.
bool GenerateBatch(Device& d, Feature& f, VkCommandBuffer command, NVSDK_NGX_Parameter* parameters, uint32_t count,
                   const ngx_provider::OutputRange& range)
{
    Inputs in;
    in.backbuffer = ImageParameter(parameters, "DLSSG.Backbuffer");
    in.depth = ImageParameter(parameters, "DLSSG.Depth");
    in.motion = ImageParameter(parameters, "DLSSG.MVecs");
    in.hudless = ImageParameter(parameters, "DLSSG.HUDLess");
    in.ui = ImageParameter(parameters, "DLSSG.UI");
    if (!in.backbuffer || !in.depth || !in.motion) { f.fault = "inputs_missing"; return false; }
    const bool inverted = Number<int>(parameters, "DLSSG.DepthInverted", 0) != 0;
    if (!EnsureFeature(d, f, in, inverted)) return false;
    const bool reset = Number<int>(parameters, "DLSSG.Reset", 0) != 0;
    Slot& slot = f.slots[f.current];
    // The frame's inputs.
    FullBarrier(d, command);
    const auto& bb = in.backbuffer->Resource.ImageViewInfo;
    CopyColor(d, command, bb.Image, xvk::ImageOf(slot.backbuffer.Get()), f.width, f.height);
    slot.hasHudless = in.hudless && slot.hudless;
    if (slot.hasHudless)
        CopyColor(d, command, in.hudless->Resource.ImageViewInfo.Image, xvk::ImageOf(slot.hudless.Get()),
                  std::min(in.hudless->Resource.ImageViewInfo.Width, f.width), std::min(in.hudless->Resource.ImageViewInfo.Height, f.height));
    slot.hasUi = in.ui && slot.ui;
    if (slot.hasUi)
        CopyColor(d, command, in.ui->Resource.ImageViewInfo.Image, xvk::ImageOf(slot.ui.Get()),
                  std::min(in.ui->Resource.ImageViewInfo.Width, f.width), std::min(in.ui->Resource.ImageViewInfo.Height, f.height));
    const auto& depth = in.depth->Resource.ImageViewInfo;
    const auto& motion = in.motion->Resource.ImageViewInfo;
    RecordSampledCopy(d, command, d.pipelines[0], depth.ImageView, slot.depthView, f.depthWidth, f.depthHeight, depth.Width, depth.Height);
    RecordSampledCopy(d, command, d.pipelines[f.motionFormat == DXGI_FORMAT_R32G32_FLOAT ? 2 : 1], motion.ImageView, slot.motionView,
                      f.depthWidth, f.depthHeight, motion.Width, motion.Height);
    FullBarrier(d, command);
    d.batchStartRecord = true;
    Stamp(d, command, 1);
    // The frame's constants. vkprov.4: DLSSG.MvecScale is already a pixel scale of the motion vectors' extent (not
    // Streamline's normalized one), carried to the depth size they are resampled to. vkprov.3: XeFG's left-handed
    // projection from DLSS-G's near, far and field of view, as the XeMFG hosts build it; DLSSG.CameraViewToClip is
    // OptiScaler's right-handed one, with which XeFG reconstructs depth the wrong way round.
    const float mvecX = Number<float>(parameters, "DLSSG.MvecScaleX", 1.0f);
    const float mvecY = Number<float>(parameters, "DLSSG.MvecScaleY", 1.0f);
    const auto [scaleX, scaleY] = xemfg_standalone::NgxDlssgMotionScale(mvecX, mvecY, motion.Width, motion.Height,
                                                                        f.depthWidth, f.depthHeight);
    slot.motionScaleX = scaleX;
    slot.motionScaleY = scaleY;
    slot.jitterX = Number<float>(parameters, "DLSSG.JitterOffsetX", 0.0f);
    slot.jitterY = Number<float>(parameters, "DLSSG.JitterOffsetY", 0.0f);
    const float aspect = Number<float>(parameters, "DLSSG.CameraAspectRatio", float(f.width) / float(f.height));
    const float cameraNear = Number<float>(parameters, "DLSSG.CameraNear", 0.0f);
    const float cameraFar = Number<float>(parameters, "DLSSG.CameraFar", 0.0f);
    const float fov = Number<float>(parameters, "DLSSG.CameraFOV", 0.0f);
    xemfg_standalone::Projection(aspect, cameraNear, cameraFar, fov, inverted, slot.projection);
    if (!f.cameraLogged) {
        f.cameraLogged = true;
        Log("vk_provider_camera", "\"aspect\":" + std::to_string(aspect) + ",\"near\":" + std::to_string(cameraNear) +
            ",\"far\":" + std::to_string(cameraFar) + ",\"fov\":" + std::to_string(fov) + ",\"inverted\":" +
            (inverted ? "true" : "false") + ",\"mvec_param\":[" + std::to_string(mvecX) + "," + std::to_string(mvecY) + "]" +
            ",\"mvecs\":[" + std::to_string(motion.Width) + "," + std::to_string(motion.Height) + "]" +
            ",\"mvec_scale\":[" + std::to_string(scaleX) + "," + std::to_string(scaleY) + "]" +
            ",\"output_real\":" + (ImageParameter(parameters, "DLSSG.OutputReal") ? "true" : "false"));
    }
    const Slot& previous = f.havePrevious && !reset ? f.slots[f.current ^ 1] : slot;
    // The batch's call, kept for its later evaluations (the slots it names stay until the next batch).
    f.exec = {};
    DescribeFrame(f, previous, f.exec.previous);
    DescribeFrame(f, slot, f.exec.current);
    f.execOutputs = {};
    f.execBases = {};
    for (uint32_t i = 0; i < count; ++i) f.execOutputs[i] = f.outputs[i].Get();
    f.exec.outputs = f.execOutputs.data();
    f.exec.outputBases = f.execBases.data();
    f.exec.interpolatedFrames = uint8_t(count);
    // MFG Unlock: XeFG's UI composition left the generated frames uninterpolated (DOOM: The Dark Ages, KCD2 in D3D12);
    // opt-in only ([DLSSG] XmxUiComposition=1).
    f.exec.uiComposition = gUiComposition && (slot.hasHudless || slot.hasUi) ? 1u : 0u;
    f.current ^= 1;
    f.havePrevious = true;
    return RecordOutputs(d, f, command, range);
}
}

// ---------------------------------------------------------------- the API's Vulkan entries (xess/ngx_provider.cpp)
int32_t VulkanInit(VkInstance instance, VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr)
{
    try {
        std::lock_guard lock(gDevice.mutex);
        if (gDevice.initialized && gDevice.device != device) return Ngx(NVSDK_NGX_Result_FAIL_PlatformError);   // one device
        gDevice.initialized = device != VK_NULL_HANDLE;
        gDevice.device = device;
        gDevice.instance = instance;
        gDevice.physical = physical;
        if (gDevice.reason == "not_initialized") gDevice.reason = "pending";
        Log("vk_provider_init", std::string("\"device\":") + (device ? "true" : "false"));
        return Ngx(device ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t VulkanCreateFeature(VkDevice device, VkCommandBuffer, NVSDK_NGX_Parameter*, void** feature)
{
    if (!feature) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    *feature = nullptr;
    try {
        std::lock_guard lock(gDevice.mutex);
        if (device && gDevice.device && device != gDevice.device) return Ngx(NVSDK_NGX_Result_FAIL_PlatformError);
        if (!EnsureDevice(gDevice)) {
            Log("vk_provider_refused", "\"reason\":\"" + Escape(gDevice.reason) + "\"");
            return Ngx(NVSDK_NGX_Result_FAIL_FeatureNotSupported);
        }
        *feature = new Feature();   // XeFG's context comes with the first evaluation, once the inputs are known
        return Ngx(NVSDK_NGX_Result_Success);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t VulkanEvaluate(VkCommandBuffer command, void* handle, NVSDK_NGX_Parameter* parameters)
{
    if (!command || !handle || !parameters) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    Feature& f = *static_cast<Feature*>(handle);
    try {
        std::lock_guard lock(gDevice.mutex);
        Device& d = gDevice;
        if (!d.ready) return Ngx(NVSDK_NGX_Result_FAIL_NotInitialized);
        const uint32_t count = std::clamp<uint32_t>(Number<unsigned int>(parameters, "DLSSG.MultiFrameCount", 1u), 1u, kMaxGenerated);
        const uint32_t index = std::clamp<uint32_t>(Number<unsigned int>(parameters, "DLSSG.MultiFrameIndex", 1u), 1u, count);
        const NVSDK_NGX_Resource_VK* output = ImageParameter(parameters, "DLSSG.OutputInterpolated");
        if (!output) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
        const NVSDK_NGX_Resource_VK* real = ImageParameter(parameters, "DLSSG.OutputReal");
        const NVSDK_NGX_Resource_VK* backbuffer = ImageParameter(parameters, "DLSSG.Backbuffer");
        if (!xvk::BeginRecording(d.d3d.Get(), command)) return Ngx(NVSDK_NGX_Result_FAIL_PlatformError);
        ++f.evaluations;
        Stamp(d, command, 0);
        // vkprov.3: batches start as ngx_provider::NewBatch says. pluginprov.2: DLSSG.EnableInterp 0 pauses generation
        // for this frame (a menu, a loading screen): no XeFG work, the next batch starts without a previous frame. Each
        // later evaluation of a batch records its own output.
        const bool interpolate = Number<int>(parameters, "DLSSG.EnableInterp", 1) != 0;
        if (!interpolate) {
            f.batchReady = false;
            f.havePrevious = false;
            f.generatedThrough = 0;
            ++f.paused;
        } else if (ngx_provider::NewBatch(index, f.lastIndex, count, f.batchCount, f.batches)) {
            f.batchCount = count;
            f.fault.clear();
            f.generatedThrough = 0;
            f.batchReady = GenerateBatch(d, f, command, parameters, count,
                                         ngx_provider::ExecuteRange(true, index, count, ngx_provider::WholeBatch()));
            ++f.batches;
            xess_mod::NoteProviderBatch(f.batchReady);
            if (!f.batchReady && ++f.failures <= 16)
                Log("vk_provider_batch_failed", "\"reason\":\"" + Escape(f.fault) + "\",\"index\":" + std::to_string(index) +
                    ",\"count\":" + std::to_string(count));
            else if (f.batches == 1 || f.batches % 600 == 0)
                Log("vk_provider_batch", "\"batches\":" + std::to_string(f.batches) + ",\"count\":" + std::to_string(count) +
                    ",\"evaluations\":" + std::to_string(f.evaluations) + ",\"real\":" + std::to_string(f.realCopies) +
                    ",\"failures\":" + std::to_string(f.failures) + ",\"paused\":" + std::to_string(f.paused) +
                    ",\"cuda_launches\":" + std::to_string(xefg_cuda::Snapshot().launches) + ",\"cuda_fallbacks\":" + std::to_string(xefg_cuda::Snapshot().fallbacks) +
                    TimingFields(d));
        } else if (f.batchReady && index > f.generatedThrough) {
            const auto range = ngx_provider::ExecuteRange(false, index, f.batchCount, ngx_provider::WholeBatch());
            if (range.record && f.context) {
                Stamp(d, command, 1);
                if (!RecordOutputs(d, f, command, range) && ++f.failures <= 16)
                    Log("vk_provider_output_failed", "\"reason\":\"" + Escape(f.fault) + "\",\"index\":" + std::to_string(index) +
                        ",\"count\":" + std::to_string(f.batchCount));
            }
        }
        f.lastIndex = index;
        const bool copied = f.batchReady && index <= f.generatedThrough && index <= f.batchCount && f.outputs[index - 1];
        if (copied) {
            const auto& out = output->Resource.ImageViewInfo;
            FullBarrier(d, command);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {std::min(out.Width, f.width), std::min(out.Height, f.height), 1};
            d.vkCmdCopyImage(command, xvk::ImageOf(f.outputs[index - 1].Get()), VK_IMAGE_LAYOUT_GENERAL, out.Image, kGameLayout, 1, &region);
            FullBarrier(d, command);
        } else if (!interpolate && backbuffer && output->Resource.ImageViewInfo.Image != backbuffer->Resource.ImageViewInfo.Image &&
                   output->Resource.ImageViewInfo.Format == backbuffer->Resource.ImageViewInfo.Format) {
            // pluginprov.2: paused: the real frame takes the output's place, never its stale content.
            const auto& source = backbuffer->Resource.ImageViewInfo;
            const auto& target = output->Resource.ImageViewInfo;
            FullBarrier(d, command);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {std::min(source.Width, target.Width), std::min(source.Height, target.Height), 1};
            d.vkCmdCopyImage(command, source.Image, kGameLayout, target.Image, kGameLayout, 1, &region);
            FullBarrier(d, command);
        }
        // vkprov.3: Streamline presents DLSSG.OutputReal as the real frame after the generated ones; left unwritten,
        // the real frame on screen is a stale one.
        if (real && backbuffer && real->Resource.ImageViewInfo.Image != backbuffer->Resource.ImageViewInfo.Image &&
            real->Resource.ImageViewInfo.Format != backbuffer->Resource.ImageViewInfo.Format && !f.realFormatLogged) {
            f.realFormatLogged = true;
            Log("vk_provider_real_skipped", "\"backbuffer_format\":" + std::to_string(int(backbuffer->Resource.ImageViewInfo.Format)) +
                ",\"real_format\":" + std::to_string(int(real->Resource.ImageViewInfo.Format)));
        }
        if (real && backbuffer && real->Resource.ImageViewInfo.Image != backbuffer->Resource.ImageViewInfo.Image &&
            real->Resource.ImageViewInfo.Format == backbuffer->Resource.ImageViewInfo.Format) {
            const auto& source = backbuffer->Resource.ImageViewInfo;
            const auto& target = real->Resource.ImageViewInfo;
            FullBarrier(d, command);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {std::min(source.Width, target.Width), std::min(source.Height, target.Height), 1};
            d.vkCmdCopyImage(command, source.Image, kGameLayout, target.Image, kGameLayout, 1, &region);
            FullBarrier(d, command);
            ++f.realCopies;
        }
        Stamp(d, command, 2);
        xvk::EndRecording(d.d3d.Get());
        return Ngx(copied || !interpolate ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail);
    } catch (...) {
        xvk::EndRecording(gDevice.d3d.Get());
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t ReleaseFeature(void* handle)
{
    if (!handle) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    auto* f = static_cast<Feature*>(handle);
    try {
        std::lock_guard lock(gDevice.mutex);
        // xvk keeps the released images and XeFG's objects until the GPU has run the recordings that used them.
        if (f->context) gDevice.api.Destroy(f->context);
        f->context = nullptr;
        delete f;
        return Ngx(NVSDK_NGX_Result_Success);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t Shutdown()
{
    try {
        std::lock_guard lock(gDevice.mutex);
        if (gDevice.d3d) xvk::WaitRecordedIdle(gDevice.d3d.Get(), 2'000'000'000ull);
        return Ngx(NVSDK_NGX_Result_Success);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

// MFGAdaUnlock 1.10.0: upstream's OptiScaler shim detection (FileExportsNgxInit, RegisteredAndSelected and the
// custom DLSSG folder scan) is not taken: this build never runs under OptiScaler. XMX on Vulkan reaches the entries
// above from the addon's own NGX Vulkan hooks (xemfg/native_vulkan.cpp), never through a shim.
bool ShimInstalled() noexcept
{
    return false;
}
}
