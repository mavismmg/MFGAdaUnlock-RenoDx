#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "xvk.h"
#include "xvk_tensor.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xvk {

// ------------------------------------------------------------------ logging
void Log(const char* format, ...);
void Unimplemented(const char* what);          // logged once per name
#define XVK_UNIMPLEMENTED_HR(name) do { ::xvk::Unimplemented(name); return E_NOTIMPL; } while (0)
#define XVK_UNIMPLEMENTED_VOID(name) do { ::xvk::Unimplemented(name); return; } while (0)

// ------------------------------------------------------------------ Vulkan entry points
#define XVK_INSTANCE_FNS(X)                                  \
    X(vkGetDeviceProcAddr)                                   \
    X(vkGetPhysicalDeviceProperties2)                        \
    X(vkGetPhysicalDeviceMemoryProperties)                   \
    X(vkGetPhysicalDeviceFormatProperties)                   \
    X(vkGetPhysicalDeviceImageFormatProperties2)             \
    X(vkGetPhysicalDeviceQueueFamilyProperties)
#define XVK_DEVICE_FNS(X)                                    \
    X(vkGetDeviceQueue)                                      \
    X(vkCreateBuffer) X(vkDestroyBuffer)                     \
    X(vkCreateImage) X(vkDestroyImage)                       \
    X(vkGetDeviceBufferMemoryRequirements)                   \
    X(vkGetDeviceImageMemoryRequirements)                    \
    X(vkGetBufferMemoryRequirements) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory)                      \
    X(vkBindBufferMemory) X(vkBindImageMemory)               \
    X(vkMapMemory) X(vkUnmapMemory)                          \
    X(vkCreateImageView) X(vkDestroyImageView)               \
    X(vkCreateSampler) X(vkDestroySampler)                   \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)     \
    X(vkCreateShaderModule) X(vkDestroyShaderModule)         \
    X(vkCreateComputePipelines) X(vkDestroyPipeline)         \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers)      \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer)            \
    X(vkCmdBindPipeline) X(vkCmdPushDescriptorSetKHR) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier2)                \
    X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdFillBuffer) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) X(vkCmdCopyQueryPoolResults) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults)               \
    X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkCreateSemaphore) X(vkDestroySemaphore)               \
    X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores) X(vkSignalSemaphore) \
    X(vkCmdBlitImage) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus)
#define XVK_OPTIONAL_INSTANCE_FNS(X)                         \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR)        \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                  \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)             \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                  \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define XVK_OPTIONAL_DEVICE_FNS(X)                           \
    X(vkCreateCuModuleNVX) X(vkCreateCuFunctionNVX) X(vkDestroyCuModuleNVX) X(vkDestroyCuFunctionNVX) \
    X(vkCmdCuLaunchKernelNVX) X(vkGetImageViewHandleNVX) X(vkGetImageViewHandle64NVX) \
    X(vkGetCalibratedTimestampsKHR)                          \
    X(vkGetCalibratedTimestampsEXT)                          \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkSetHdrMetadataEXT) \
    X(vkCreateEvent) X(vkDestroyEvent) X(vkCmdSetEvent) X(vkGetEventStatus) X(vkResetEvent)
// The host's low-latency outputs (NVIDIA Reflex, AMD Anti-Lag 2). With a deviceProcOverride they come from
// it alone: OptiScaler's vkGetDeviceProcAddr hands out its own Reflex and Anti-Lag input wrappers for these names.
#define XVK_LATENCY_DEVICE_FNS(X)                            \
    X(vkSetLatencySleepModeNV) X(vkLatencySleepNV) X(vkSetLatencyMarkerNV) X(vkQueueNotifyOutOfBandNV) \
    X(vkGetLatencyTimingsNV) X(vkAntiLagUpdateAMD)

struct VkFns
{
#define XVK_DECLARE(name) PFN_##name name = nullptr;
    XVK_INSTANCE_FNS(XVK_DECLARE)
    XVK_OPTIONAL_INSTANCE_FNS(XVK_DECLARE)
    XVK_DEVICE_FNS(XVK_DECLARE)
    XVK_OPTIONAL_DEVICE_FNS(XVK_DECLARE)
    XVK_LATENCY_DEVICE_FNS(XVK_DECLARE)
#undef XVK_DECLARE
};

// ------------------------------------------------------------------ COM helpers
template <class... I>
inline bool IsIid(REFIID riid) { return (... || (riid == __uuidof(I))); }

// QueryInterface for one interface chain: for the IIDs of I (tested in order) `self` is AddRef'd and comes back as Out.
template <class Out, class... I, class Self>
inline HRESULT QueryAs(Self* self, REFIID riid, void** out)
{
    if (!out) return E_POINTER;
    if (IsIid<I...>(riid)) {
        self->AddRef();
        *out = static_cast<Out*>(self);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

struct GuidLess
{
    bool operator()(const GUID& a, const GUID& b) const { return memcmp(&a, &b, sizeof(GUID)) < 0; }
};

class PrivateData
{
public:
    HRESULT Get(REFGUID guid, UINT* size, void* data);
    HRESULT Set(REFGUID guid, UINT size, const void* data);
    HRESULT SetInterface(REFGUID guid, const IUnknown* object);
    ~PrivateData();
private:
    std::mutex mutex_;
    std::map<GUID, std::vector<uint8_t>, GuidLess> data_;
    std::map<GUID, IUnknown*, GuidLess> interfaces_;
};

// Reference counting and ID3D12Object for one interface chain. Objects that are D3D12 device
// children keep their device alive.
template <class Base>
class D3D12Object : public Base
{
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override { return private_.Get(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override { return private_.Set(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override { return private_.SetInterface(guid, object); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    virtual ~D3D12Object() = default;
protected:
    std::atomic<ULONG> refs_{1};
    PrivateData private_;
};

class Device;

template <class Base>
class DeviceChild : public D3D12Object<Base>
{
public:
    explicit DeviceChild(Device* device);
    ~DeviceChild() override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** out) override;
protected:
    Device* device_;
};

// ------------------------------------------------------------------ formats
struct FormatInfo
{
    VkFormat format = VK_FORMAT_UNDEFINED;   // concrete format used for images and views
    uint32_t bytes = 0;                      // bytes per texel (block)
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool depth = false;
};
FormatInfo Format(DXGI_FORMAT format, bool depthResource = false);

// ------------------------------------------------------------------ root signatures (RTS0)
struct RootRange
{
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    uint32_t baseRegister;
    uint32_t count;
    uint32_t space;
    uint32_t offset;   // descriptors from the table start (APPEND resolved)
};
struct RootParameter
{
    D3D12_ROOT_PARAMETER_TYPE type;
    std::vector<RootRange> ranges;     // tables
    uint32_t shaderRegister = 0;       // constants / root descriptors
    uint32_t space = 0;
    uint32_t num32 = 0;                // constants
};
struct StaticSampler
{
    D3D12_STATIC_SAMPLER_DESC desc;
};
struct RootLayout
{
    std::vector<RootParameter> parameters;
    std::vector<StaticSampler> samplers;
};
// Parses a serialized root signature: a DXBC/DXIL container with an RTS0 part, or a bare RTS0 blob.
bool ParseRootSignature(const void* data, size_t size, RootLayout& out, std::string& error);

// ------------------------------------------------------------------ program set
struct ProgramBinding
{
    uint32_t binding;
    VkDescriptorType type;
};
struct Program
{
    std::string sha256;
    std::vector<uint32_t> spirv;
    std::vector<ProgramBinding> bindings;
    uint32_t pushConstantBytes = 0;
};
// The embedded set (RCDATA): a program's SPIR-V is read and its SHA-256 verified on first use.
class ProgramSet
{
public:
    void Load(HMODULE module);
    const Program* Find(const std::string& sha256);
    size_t Size() const { return index_.size(); }
private:
    HMODULE module_ = nullptr;
    std::mutex mutex_;
    std::unordered_map<std::string, size_t> index_;              // DXIL SHA-256 -> embedded row
    std::unordered_map<std::string, std::unique_ptr<Program>> loaded_;
};
std::string Sha256(const void* data, size_t size);
// Logs (once per program) and counts a pipeline that failed closed for a missing program.
void NoteMissingProgram(const std::string& sha256, const void* dxil, size_t bytes);

// Binding contract (BINDINGS.md)
constexpr uint32_t kSrvBase = 0, kUavBase = 64, kCbvBase = 128, kSamplerBase = 192;

// ------------------------------------------------------------------ descriptors
enum class DescriptorKind : uint32_t { None, SrvBuffer, SrvImage, UavBuffer, UavImage, Cbv, Sampler, Rtv, Dsv };
class Resource;
struct Descriptor
{
    DescriptorKind kind = DescriptorKind::None;
    Resource* resource = nullptr;     // not owned
    VkDescriptorImageInfo image{};    // view (GENERAL) or sampler
    VkDescriptorBufferInfo buffer{};
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    VkImageView cudaArrayView = VK_NULL_HANDLE; // 2D twin as a one-layer array; owned/cached by resource
    uint32_t stride = 0;
};

// ------------------------------------------------------------------ resource descriptions
bool ImageCreateInfo(const D3D12_RESOURCE_DESC& desc, VkImageCreateInfo& info, FormatInfo& format);
VkBufferCreateInfo BufferCreateInfo(const D3D12_RESOURCE_DESC& desc);
constexpr uint64_t kPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
inline uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// ------------------------------------------------------------------ objects
class Heap;

class Resource final : public DeviceChild<ID3D12Resource>
{
public:
    Resource(Device* device, const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags);
    ~Resource() override;
    HRESULT Init(Heap* placedHeap, UINT64 placedOffset);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* readRange, void** data) override;
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE* writtenRange) override;
#if defined(_MSC_VER) || !defined(_WIN32)
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
#endif
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return va_; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) override { XVK_UNIMPLEMENTED_HR("Resource::WriteToSubresource"); }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) override { XVK_UNIMPLEMENTED_HR("Resource::ReadFromSubresource"); }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override;

    bool IsBuffer() const { return desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
    VkBuffer buffer() const { return buffer_; }
    VkImage image() const { return image_; }
    VkFormat imageFormat() const { return imageFormat_; }
    const D3D12_RESOURCE_DESC& desc() const { return desc_; }
    // Image view for the given D3D12 view parameters (cached per resource).
    VkImageView View(VkFormat format, VkImageViewType type, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mips,
                     uint32_t baseLayer, uint32_t layers, VkImageUsageFlags usage);
    uint64_t size() const { return size_; }
    void NoteWrite() noexcept { writeGeneration_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t CudaHandle(VkImageView view, bool storage, TensorRoute& route);
    uint64_t TensorShadow(VkCommandBuffer, uint64_t offset, uint64_t range, TensorRoute&, std::string& reason);

private:
    D3D12_RESOURCE_DESC desc_;
    D3D12_HEAP_PROPERTIES heapProperties_;
    D3D12_HEAP_FLAGS heapFlags_;
    Heap* heap_ = nullptr;             // placed: referenced heap
    VkDeviceMemory ownMemory_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    uint64_t size_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS va_ = 0;
    uint8_t* mapped_ = nullptr;
    std::mutex viewMutex_;
    std::map<std::array<uint32_t, 8>, VkImageView> views_;
    struct Shadow { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; uint64_t handle{}, generation{}; bool initialized{}; };
    std::map<std::pair<uint64_t, uint64_t>, Shadow> shadows_;
    std::map<std::pair<VkImageView, bool>, uint64_t> cudaHandles_;
    std::atomic<uint64_t> writeGeneration_{1};
};

class Heap final : public DeviceChild<ID3D12Heap>
{
public:
    Heap(Device* device, const D3D12_HEAP_DESC& desc);
    ~Heap() override;
    HRESULT Init();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
#if defined(_MSC_VER) || !defined(_WIN32)
    D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
#endif
    VkDeviceMemory memory() const { return memory_; }
    uint32_t memoryType() const { return memoryType_; }
    uint8_t* mapped() const { return mapped_; }
private:
    D3D12_HEAP_DESC desc_;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    uint32_t memoryType_ = 0;
    uint8_t* mapped_ = nullptr;
};

class DescriptorHeap final : public DeviceChild<ID3D12DescriptorHeap>
{
public:
    DescriptorHeap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC& desc);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
#if defined(_MSC_VER) || !defined(_WIN32)
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override
    {
        return {reinterpret_cast<SIZE_T>(descriptors_.data())};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override
    {
        return {(desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? reinterpret_cast<UINT64>(descriptors_.data()) : 0};
    }
#endif
private:
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    std::vector<Descriptor> descriptors_;
};

class RootSignature final : public DeviceChild<ID3D12RootSignature>
{
public:
    RootSignature(Device* device, RootLayout layout) : DeviceChild(device), layout_(std::move(layout)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    const RootLayout& layout() const { return layout_; }
private:
    RootLayout layout_;
};

struct BoundRange
{
    uint32_t parameter;
    uint32_t offset;           // descriptor offset in the table
    uint32_t binding;
    VkDescriptorType type;
};

class PipelineState final : public DeviceChild<ID3D12PipelineState>
{
public:
    PipelineState(Device* device) : DeviceChild(device) {}
    ~PipelineState() override;
    HRESULT Init(RootSignature* rootSignature, const Program& program);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob**) override { XVK_UNIMPLEMENTED_HR("PipelineState::GetCachedBlob"); }

    void SetTensorLayer(uint8_t layer) noexcept { tensorLayer_ = layer; }
    uint8_t tensorLayer() const noexcept { return tensorLayer_; }
    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    const std::vector<BoundRange>& ranges() const { return ranges_; }
    const std::vector<VkSampler>& staticSamplers() const { return samplers_; }
    const std::vector<uint32_t>& staticBindings() const { return staticBindings_; }
    int constantsParameter() const { return constantsParameter_; }
    uint32_t pushConstantBytes() const { return pushConstantBytes_; }
private:
    uint8_t tensorLayer_{};
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    std::vector<VkSampler> samplers_;       // owned by the device sampler cache
    std::vector<uint32_t> staticBindings_;
    std::vector<BoundRange> ranges_;
    int constantsParameter_ = -1;
    uint32_t pushConstantBytes_ = 0;
    std::string program_;
};

class CommandAllocator final : public DeviceChild<ID3D12CommandAllocator>
{
public:
    CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type);
    ~CommandAllocator() override;
    HRESULT Init();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    VkCommandBuffer Allocate();
private:
    D3D12_COMMAND_LIST_TYPE type_;
    std::mutex mutex_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> free_;
    std::vector<VkCommandBuffer> used_;
};

class QueryHeap final : public DeviceChild<ID3D12QueryHeap>
{
public:
    QueryHeap(Device* device, const D3D12_QUERY_HEAP_DESC& desc) : DeviceChild(device), desc_(desc) {}
    ~QueryHeap() override;
    HRESULT Init();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    VkQueryPool pool() const { return pool_; }
private:
    D3D12_QUERY_HEAP_DESC desc_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
};

class Fence final : public DeviceChild<ID3D12Fence>
{
public:
    Fence(Device* device) : DeviceChild(device) {}
    ~Fence() override;
    HRESULT Init(UINT64 initial);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;
    VkSemaphore semaphore() const { return semaphore_; }
    // D3D12 fences accept a signal of the value they already hold; Vulkan timelines must increase.
    // Returns false when the signal must be skipped (value not above the highest one requested).
    bool ClaimSignal(UINT64 value)
    {
        UINT64 last = requested_.load();
        while (value > last)
            if (requested_.compare_exchange_weak(last, value)) return true;
        return false;
    }
private:
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    std::atomic<UINT64> requested_{0};
};

struct QueueSlot
{
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    std::mutex mutex;          // VkQueue is externally synchronized
    VkSemaphore timeline = VK_NULL_HANDLE;   // every submission on this queue signals ++submitted
    uint64_t submitted = 0;
    uint32_t index = 0;                      // position in the device's slot list
    std::atomic<uint32_t> users{0};          // live D3D12 queues on this VkQueue
};

class CommandList;

class CommandQueue final : public DeviceChild<ID3D12CommandQueue>
{
public:
    CommandQueue(Device* device, const D3D12_COMMAND_QUEUE_DESC& desc, QueueSlot* slot) : DeviceChild(device), desc_(desc), slot_(slot) { ++slot_->users; }
    ~CommandQueue() override { --slot_->users; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
        ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS) override
    { XVK_UNIMPLEMENTED_VOID("CommandQueue::UpdateTileMappings"); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*,
        const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS) override
    { XVK_UNIMPLEMENTED_VOID("CommandQueue::CopyTileMappings"); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override;
#if defined(_MSC_VER) || !defined(_WIN32)
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
#endif
    QueueSlot* slot() const { return slot_; }
    // The swap chain's staging copy, ordered after earlier work on this queue.
    HRESULT SubmitStageCopy(VkCommandBuffer command, const VkSemaphoreSubmitInfo* signals, uint32_t count, VkFence fence, uint64_t* progress)
    {
        return Submit({command}, VK_NULL_HANDLE, 0, nullptr, 0, signals, count, fence, progress);
    }
    // Host work on this queue (the Vulkan host's acquire signal): ordered after earlier work and after every
    // D3D12 Wait issued on this queue so far, like any submission.
    HRESULT SubmitHost(VkCommandBuffer command, const VkSemaphoreSubmitInfo* waits, uint32_t waitCount,
                       const VkSemaphoreSubmitInfo* signals, uint32_t signalCount, VkFence fence)
    {
        std::vector<VkCommandBuffer> buffers;
        if (command) buffers.push_back(command);
        return Submit(buffers, VK_NULL_HANDLE, 0, waits, waitCount, signals, signalCount, fence, nullptr);
    }
private:
    // Queue waits apply to the next submission on this queue (D3D12 Wait blocks all later work).
    HRESULT Submit(const std::vector<VkCommandBuffer>& buffers, VkSemaphore signal, uint64_t signalValue,
                   const VkSemaphoreSubmitInfo* extraWaits = nullptr, uint32_t extraWaitCount = 0,
                   const VkSemaphoreSubmitInfo* extraSignals = nullptr, uint32_t extraSignalCount = 0, VkFence fence = VK_NULL_HANDLE,
                   uint64_t* progress = nullptr);
    D3D12_COMMAND_QUEUE_DESC desc_;
    QueueSlot* slot_;
    std::mutex pendingMutex_;
    std::vector<std::pair<VkSemaphore, uint64_t>> pendingWaits_;
};

class CommandList final : public DeviceChild<ID3D12GraphicsCommandList>
{
public:
    CommandList(Device* device, D3D12_COMMAND_LIST_TYPE type) : DeviceChild(device), type_(type) {}
    ~CommandList() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type_; }
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pso) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT, UINT) override { XVK_UNIMPLEMENTED_VOID("CommandList::DrawInstanced"); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override { XVK_UNIMPLEMENTED_VOID("CommandList::DrawIndexedInstanced"); }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 bytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* dst, ID3D12Resource* src) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Resource*,
        UINT64, D3D12_TILE_COPY_FLAGS) override { XVK_UNIMPLEMENTED_VOID("CommandList::CopyTiles"); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::ResolveSubresource"); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY) override { XVK_UNIMPLEMENTED_VOID("CommandList::IASetPrimitiveTopology"); }
    void STDMETHODCALLTYPE RSSetViewports(UINT, const D3D12_VIEWPORT*) override { XVK_UNIMPLEMENTED_VOID("CommandList::RSSetViewports"); }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT, const D3D12_RECT*) override { XVK_UNIMPLEMENTED_VOID("CommandList::RSSetScissorRects"); }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT[4]) override { XVK_UNIMPLEMENTED_VOID("CommandList::OMSetBlendFactor"); }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT) override { XVK_UNIMPLEMENTED_VOID("CommandList::OMSetStencilRef"); }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pso) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override { XVK_UNIMPLEMENTED_VOID("CommandList::ExecuteBundle"); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) override {}
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* rootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature*) override { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRootSignature"); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE base) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRootDescriptorTable"); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT parameter, UINT value, UINT offset) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT, UINT, UINT) override { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRoot32BitConstant"); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT parameter, UINT count, const void* data, UINT offset) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT, UINT, const void*, UINT) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRoot32BitConstants"); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetComputeRootConstantBufferView"); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRootConstantBufferView"); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetComputeRootShaderResourceView"); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRootShaderResourceView"); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetComputeRootUnorderedAccessView"); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::SetGraphicsRootUnorderedAccessView"); }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW*) override { XVK_UNIMPLEMENTED_VOID("CommandList::IASetIndexBuffer"); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*) override { XVK_UNIMPLEMENTED_VOID("CommandList::IASetVertexBuffers"); }
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override { XVK_UNIMPLEMENTED_VOID("CommandList::SOSetTargets"); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::OMSetRenderTargets"); }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::ClearDepthStencilView"); }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::ClearRenderTargetView"); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource* resource,
        const UINT values[4], UINT rects, const D3D12_RECT* rectList) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource* resource,
        const FLOAT values[4], UINT rects, const D3D12_RECT* rectList) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*, const D3D12_DISCARD_REGION*) override {}
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override { XVK_UNIMPLEMENTED_VOID("CommandList::BeginQuery"); }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* dst,
        UINT64 offset) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) override { XVK_UNIMPLEMENTED_VOID("CommandList::SetPredication"); }
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) override
    { XVK_UNIMPLEMENTED_VOID("CommandList::ExecuteIndirect"); }

    HRESULT Begin(CommandAllocator* allocator);
    VkCommandBuffer buffer() const { return buffer_; }
    bool closed() const { return !recording_; }
    bool failed() const { return failed_; }

private:
    void Fail(const char* what);
    void Barrier();
    void ClearUav(Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE cpu, const VkClearColorValue& value, uint32_t fillValue);
    D3D12_COMMAND_LIST_TYPE type_;
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
    bool recording_ = false;
    bool failed_ = false;
    PipelineState* pso_ = nullptr;
    RootSignature* rootSignature_ = nullptr;
    VkPipeline boundPipeline_ = VK_NULL_HANDLE;
    std::array<const Descriptor*, 64> tables_{};
    std::array<std::array<uint32_t, 64>, 16> constants_{};
    std::array<uint32_t, 16> constantMasks_{};
};

// ------------------------------------------------------------------ device
class Device final : public D3D12Object<ID3D12Device1>
{
public:
    Device() = default;
    TensorRoute& tensor() noexcept { return tensor_; }
    bool InstallTensor(std::span<const unsigned char> fatbin, const TensorOptions& options, std::string& reason) noexcept
    { return tensor_.Install(tensorDesc_, fatbin, options, reason); }
    ~Device() override;
    HRESULT Init(const DeviceDesc& desc);

    // IUnknown / ID3D12Device
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**) override
    { XVK_UNIMPLEMENTED_HR("Device::CreateGraphicsPipelineState"); }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initial, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override { return sizeof(Descriptor); }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void* blob, SIZE_T size, REFIID riid, void** out) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT destRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts, const UINT* destSizes,
        UINT srcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src,
        D3D12_DESCRIPTOR_HEAP_TYPE type) override;
#if defined(_MSC_VER) || !defined(_WIN32)
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visibleMask, UINT count, const D3D12_RESOURCE_DESC* descs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE type) override;
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override
    { XVK_UNIMPLEMENTED_HR("Device::CreateReservedResource"); }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*) override
    { XVK_UNIMPLEMENTED_HR("Device::CreateSharedHandle"); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void**) override { XVK_UNIMPLEMENTED_HR("Device::OpenSharedHandle"); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) override { XVK_UNIMPLEMENTED_HR("Device::OpenSharedHandleByName"); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return removed_.load(); }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* rowSizes, UINT64* total) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** out) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**) override
    { XVK_UNIMPLEMENTED_HR("Device::CreateCommandSignature"); }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*, UINT,
        D3D12_SUBRESOURCE_TILING*) override { XVK_UNIMPLEMENTED_VOID("Device::GetResourceTiling"); }
#if defined(_MSC_VER) || !defined(_WIN32)
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return luid_; }
#endif
    // ID3D12Device1
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void*, SIZE_T, REFIID, void**) override { XVK_UNIMPLEMENTED_HR("Device::CreatePipelineLibrary"); }
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const*, const UINT64*, UINT, D3D12_MULTIPLE_FENCE_WAIT_FLAGS, HANDLE) override
    { XVK_UNIMPLEMENTED_HR("Device::SetEventOnMultipleFenceCompletion"); }
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT, ID3D12Pageable* const*, const D3D12_RESIDENCY_PRIORITY*) override { return S_OK; }

    // layer internals
    const VkFns& vk() const { return vk_; }
    VkDevice vkDevice() const { return device_; }
    VkPhysicalDevice physical() const { return physical_; }
    uint32_t queueFamily() const { return queueFamily_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    bool FindMemoryType(uint32_t bits, D3D12_HEAP_TYPE heap, uint32_t& type) const;
    D3D12_GPU_VIRTUAL_ADDRESS AllocateVa(uint64_t size, Resource* resource);
    void FreeVa(D3D12_GPU_VIRTUAL_ADDRESS va);
    Resource* ResolveVa(D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t& offset);
    // New images move from UNDEFINED to GENERAL and new resources are zeroed (as D3D12 does) on the
    // init queue; every user submission waits for the last initialization.
    void InitializeResource(VkImage image, VkImageAspectFlags aspect, VkBuffer buffer);
    // Every user submission waits for the last image initialization.
    void InitWait(VkSemaphore& semaphore, uint64_t& value);
    void Fault(const char* what);   // device-removed style failure
    // Waits (bounded) until every queue finished the work submitted so far; false on timeout or device loss.
    bool WaitIdle(uint64_t timeoutNs);
    // Runs `destroy` once every queue has finished the work submitted before this call.
    void DeferDestroy(std::function<void()> destroy);
    void Collect();
    // vkprov.1 (DeviceDesc::recorded): the command buffer lists record into while a recording is open (null
    // otherwise), and the recording's bounds (xvk.h BeginRecording / EndRecording).
    bool recorded() const { return recorded_; }
    VkCommandBuffer RecordingBuffer();
    bool BeginRecording(VkCommandBuffer commandBuffer);
    void EndRecording();
    // Marks recordings the GPU has run as finished on the progress timeline (their events are set), then collects.
    void PollRecordings();
    // Adds this queue's progress signal to a submission (caller holds slot.mutex).
    VkSemaphoreSubmitInfo Progress(QueueSlot& slot)
    {
        return {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, slot.timeline, ++slot.submitted, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }
    // Fence waits that signal Win32 events.
    void WaitAsync(VkSemaphore semaphore, uint64_t value, HANDLE event);
    void WaitAsync(VkSemaphore semaphore, uint64_t value, std::function<void()> done);
    PFN_vkGetInstanceProcAddr gipa() const { return gipa_; }
    VkInstance instance() const { return instance_; }
    // Samplers are cached by their create info (descriptor samplers and static samplers).
    VkSampler Sampler(const VkSamplerCreateInfo& info);
    // Swap chains present from their own thread on this queue; D3D12 queues never map to it (when there are two or more).
    QueueSlot* PresentSlot() const { return slots_.back().get(); }
    // VK_NV_low_latency2's functions came with the device (its swap chains can take low latency mode), and
    // VK_KHR_present_id's presentId feature is on (Reflex's present markers name the real frame's present).
    bool lowLatency2() const { return vk_.vkSetLatencySleepModeNV && vk_.vkLatencySleepNV && vk_.vkSetLatencyMarkerNV; }
    bool presentIds() const { return presentIds_; }
    bool presentMetering() const { return presentMetering_; }   // MFGAdaUnlock 1.7
    // Reflex's sleep signal (a timeline semaphore, created on first use; null when it could not be), and the value the
    // next sleep signals (never reused: a sleep that timed out still signals its own).
    VkSemaphore LatencySemaphore();
    uint64_t NextLatencyValue() { return ++latencyValue_; }
    // Marks the layer's queues out of band for Reflex, once: XeFG's work as render, the present queue as present.
    void NotifyOutOfBand();
    // The images XeFG creates as the application's back buffers become the game's swap-chain images, so they
    // need the game's image format (an sRGB swap chain keeps sRGB semantics for the game's copies and blits)
    // and the game's swap-chain usage on top of what D3D12 asked for.
    struct BackbufferHint
    {
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        VkFormat imageFormat = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
    };
    void SetBackbufferHint(const BackbufferHint& hint);
    bool MatchBackbuffer(const D3D12_RESOURCE_DESC& desc, BackbufferHint& hint);
    // Whether this device can create an image with this format, usage and flags (optimal tiling, 2D).
    bool ImageSupported(VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags) const;
    // The game's own swap chain on a window. The first layer chain there names it as oldSwapchain when that chain's
    // VkSwapchainKHR is created, at its first present: until XeFG presents, the game's chain is not retired, so a
    // failed XeFG init leaves the game a working window. A game that destroys the chain before then hands it over
    // (DeferRetiredDestroy), and the layer destroys it once it is retired (FinishRetire).
    void SetRetireHint(HWND window, VkSwapchainKHR swapchain);
    // Removes a hint no layer chain claimed: returns the chain (still the game's), or null when it was claimed
    // (then retired) or already handed over and destroyed here.
    VkSwapchainKHR DropRetireHint(HWND window);
    VkSwapchainKHR ClaimRetireHint(HWND window);   // the layer chain's creation; FinishRetire after it
    void FinishRetire(HWND window);
    bool DeferRetiredDestroy(VkSwapchainKHR swapchain);   // true: the layer destroys it after retiring it

private:
    void WaiterThread();
    TensorRoute tensor_;
    DeviceDesc tensorDesc_;
    VkFns vk_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    LUID luid_{};
    uint32_t queueFamily_ = 0;
    std::vector<std::unique_ptr<QueueSlot>> slots_;
    std::atomic<uint32_t> nextSlot_{0};
    ProgramSet programs_;
    bool presentIds_ = false;
    bool presentMetering_ = false;   // MFGAdaUnlock 1.7
    std::mutex latencyMutex_;                       // latencySemaphore_'s creation
    VkSemaphore latencySemaphore_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> latencyValue_{0};
    std::atomic<bool> outOfBand_{false};
    std::atomic<HRESULT> removed_{S_OK};
    std::mutex hintMutex_;
    BackbufferHint hint_;
    struct RetireHint { VkSwapchainKHR swapchain = VK_NULL_HANDLE; bool claimed = false, destroy = false; };
    std::map<HWND, RetireHint> retire_;

    std::mutex vaMutex_;
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, std::pair<uint64_t, Resource*>> vas_;
    D3D12_GPU_VIRTUAL_ADDRESS nextVa_ = 0x100000000000ull;

    std::mutex initMutex_;
    VkCommandPool initPool_ = VK_NULL_HANDLE;
    VkSemaphore initSemaphore_ = VK_NULL_HANDLE;
    uint64_t initValue_ = 0;
    struct InitBatch { VkCommandBuffer buffer; uint64_t value; };
    std::vector<InitBatch> initBatches_;

    struct Grave { std::vector<uint64_t> targets; std::function<void()> destroy; };
    std::mutex graveMutex_;
    std::vector<Grave> graves_;

    // vkprov.1: recorded mode. One queue-less slot carries the progress timeline; each ended recording is an epoch
    // whose event, set by the GPU at the end of the caller's command buffer, moves the timeline to its value.
    bool recorded_ = false;
    std::mutex recordMutex_;
    VkCommandBuffer recording_ = VK_NULL_HANDLE;
    struct Epoch { VkEvent event; uint64_t value; };
    std::vector<Epoch> epochs_;
    std::vector<VkEvent> freeEvents_;
    std::vector<std::function<void(VkCommandBuffer)>> pendingInit_;   // initializations before the first recording

    std::mutex samplerMutex_;
    std::map<std::array<uint8_t, sizeof(VkSamplerCreateInfo)>, VkSampler> samplers_;

    std::mutex waitMutex_;
    std::condition_variable waitChanged_;
    struct PendingWait { VkSemaphore semaphore; uint64_t value; std::function<void()> done; int64_t since = 0; bool reported = false; };
    std::vector<PendingWait> waits_;
    VkSemaphore wakeSemaphore_ = VK_NULL_HANDLE;
    uint64_t wakeValue_ = 0;
    bool stopping_ = false;
    std::thread waiter_;
};

// Downcast helpers: objects handed to the layer must be the layer's own (fail closed otherwise).
template <class T, class I>
T* Own(I* object) { return static_cast<T*>(object); }

template <class Base>
DeviceChild<Base>::DeviceChild(Device* device) : device_(device) { device_->AddRef(); }
template <class Base>
DeviceChild<Base>::~DeviceChild() { device_->Release(); }
template <class Base>
HRESULT STDMETHODCALLTYPE DeviceChild<Base>::GetDevice(REFIID riid, void** out) { return device_->QueryInterface(riid, out); }

} // namespace xvk
