#include "xvk_internal.h"
#include "../cuda/cuda_layers.h"

namespace xvk {

namespace {

template <class T>
HRESULT Hand(T* object, HRESULT init, REFIID riid, void** out)
{
    if (FAILED(init)) { object->Release(); return init; }
    if (!out) { object->Release(); return S_FALSE; }
    const HRESULT hr = object->QueryInterface(riid, out);
    object->Release();
    return hr;
}

VkSamplerAddressMode Address(D3D12_TEXTURE_ADDRESS_MODE mode)
{
    switch (mode) {
    case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

} // namespace

// Builds a Vulkan sampler description from D3D12 filter/address fields (descriptor and static samplers).
VkSamplerCreateInfo SamplerInfo(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE u, D3D12_TEXTURE_ADDRESS_MODE v, D3D12_TEXTURE_ADDRESS_MODE w,
                                float bias, UINT anisotropy, D3D12_COMPARISON_FUNC comparison, VkBorderColor border, float minLod, float maxLod)
{
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const UINT f = UINT(filter);
    info.minFilter = ((f >> 4) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.magFilter = ((f >> 2) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = (f & 3) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (f & 0x40) { info.anisotropyEnable = VK_TRUE; info.maxAnisotropy = float(anisotropy); info.minFilter = info.magFilter = VK_FILTER_LINEAR; }
    const UINT reduction = (f >> 7) & 3;
    if (reduction == 1) { info.compareEnable = VK_TRUE; info.compareOp = VkCompareOp(UINT(comparison) - 1); }
    info.addressModeU = Address(u);
    info.addressModeV = Address(v);
    info.addressModeW = Address(w);
    info.mipLodBias = bias;
    info.borderColor = border;
    info.minLod = minLod;
    info.maxLod = maxLod;
    return info;
}

Device::~Device()
{
    {
        std::lock_guard lock(waitMutex_);
        stopping_ = true;
        if (wakeSemaphore_) {
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, wakeSemaphore_, ++wakeValue_};
            vk_.vkSignalSemaphore(device_, &signal);
        }
    }
    waitChanged_.notify_all();
    if (waiter_.joinable()) waiter_.join();
    for (auto& slot : slots_) {
        std::lock_guard lock(slot->mutex);
        if (slot->queue) vk_.vkQueueWaitIdle(slot->queue);
    }
    // Destroying one object can defer another (a placed resource releases its heap), so drain until empty.
    while (true) {
        std::vector<Grave> graves;
        {
            std::lock_guard lock(graveMutex_);
            graves.swap(graves_);
        }
        if (graves.empty()) break;
        for (auto& grave : graves) grave.destroy();
    }
    for (auto& slot : slots_)
        if (slot->timeline) vk_.vkDestroySemaphore(device_, slot->timeline, nullptr);
    if (vk_.vkDestroyEvent) {
        for (const Epoch& epoch : epochs_) vk_.vkDestroyEvent(device_, epoch.event, nullptr);
        for (VkEvent event : freeEvents_) vk_.vkDestroyEvent(device_, event, nullptr);
    }
    if (initPool_) vk_.vkDestroyCommandPool(device_, initPool_, nullptr);
    if (initSemaphore_) vk_.vkDestroySemaphore(device_, initSemaphore_, nullptr);
    if (wakeSemaphore_) vk_.vkDestroySemaphore(device_, wakeSemaphore_, nullptr);
    if (latencySemaphore_) vk_.vkDestroySemaphore(device_, latencySemaphore_, nullptr);
    for (auto& [key, sampler] : samplers_) vk_.vkDestroySampler(device_, sampler, nullptr);
}

VkSemaphore Device::LatencySemaphore()
{
    std::lock_guard lock(latencyMutex_);
    if (!latencySemaphore_) {
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
        if (vk_.vkCreateSemaphore(device_, &info, nullptr, &latencySemaphore_) != VK_SUCCESS) latencySemaphore_ = VK_NULL_HANDLE;
    }
    return latencySemaphore_;
}

void Device::NotifyOutOfBand()
{
    if (!vk_.vkQueueNotifyOutOfBandNV || recorded_ || outOfBand_.exchange(true)) return;
    for (auto& slot : slots_) {
        const bool present = slot.get() == PresentSlot();
        VkOutOfBandQueueTypeInfoNV type{VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV, nullptr,
                                        present ? VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV : VK_OUT_OF_BAND_QUEUE_TYPE_RENDER_NV};
        std::lock_guard lock(slot->mutex);
        vk_.vkQueueNotifyOutOfBandNV(slot->queue, &type);
    }
    Log("Reflex: %zu queues out of band (%zu render, 1 present)", slots_.size(), slots_.size() - 1);
}

HRESULT Device::Init(const DeviceDesc& desc)
{
    if (!desc.instance || !desc.physicalDevice || !desc.device || !desc.getInstanceProcAddr || !desc.programModule)
        return E_INVALIDARG;
    if (desc.recorded ? desc.queueCount != 0 : desc.queueCount < 2 || !desc.queues) return E_INVALIDARG;
    tensorDesc_ = desc;
    recorded_ = desc.recorded;
    instance_ = desc.instance;
    physical_ = desc.physicalDevice;
    device_ = desc.device;
    gipa_ = desc.getInstanceProcAddr;
#define XVK_LOAD_INSTANCE(name) vk_.name = reinterpret_cast<PFN_##name>(gipa_(instance_, #name));
    XVK_INSTANCE_FNS(XVK_LOAD_INSTANCE)
    XVK_OPTIONAL_INSTANCE_FNS(XVK_LOAD_INSTANCE)
#undef XVK_LOAD_INSTANCE
    if (!vk_.vkGetDeviceProcAddr) return E_FAIL;
    bool complete = true;
    auto resolve = [&](const char* name) {
        PFN_vkVoidFunction function = desc.deviceProcOverride ? desc.deviceProcOverride(desc.deviceProcOverrideUser, name) : nullptr;
        return function ? function : vk_.vkGetDeviceProcAddr(device_, name);
    };
#define XVK_LOAD_DEVICE(name)                                                              \
    vk_.name = reinterpret_cast<PFN_##name>(resolve(#name));                               \
    if (!vk_.name) { Log("missing device function %s", #name); complete = false; }
    XVK_DEVICE_FNS(XVK_LOAD_DEVICE)
#undef XVK_LOAD_DEVICE
#define XVK_LOAD_OPTIONAL(name) vk_.name = reinterpret_cast<PFN_##name>(resolve(#name));
    XVK_OPTIONAL_DEVICE_FNS(XVK_LOAD_OPTIONAL)
#undef XVK_LOAD_OPTIONAL
#define XVK_LOAD_LATENCY(name)                                                             \
    vk_.name = reinterpret_cast<PFN_##name>(desc.deviceProcOverride ? desc.deviceProcOverride(desc.deviceProcOverrideUser, #name) \
                                                                    : vk_.vkGetDeviceProcAddr(device_, #name));
    XVK_LATENCY_DEVICE_FNS(XVK_LOAD_LATENCY)
#undef XVK_LOAD_LATENCY
    if (!complete) return E_FAIL;
    presentIds_ = desc.presentIds;
    presentMetering_ = desc.presentMetering;   // MFGAdaUnlock 1.7
    Log("present metering %d", int(presentMetering_));
    Log("low latency: Reflex functions %d, out-of-band queues %d, present ids %d, Anti-Lag 2 %d", int(lowLatency2()),
        int(vk_.vkQueueNotifyOutOfBandNV != nullptr), int(presentIds_), int(vk_.vkAntiLagUpdateAMD != nullptr));

    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vk_.vkGetPhysicalDeviceProperties2(physical_, &properties);
    properties_ = properties.properties;
    if (!id.deviceLUIDValid) { Log("physical device has no LUID"); return E_FAIL; }
    memcpy(&luid_, id.deviceLUID, sizeof(LUID));
    vk_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

    if (recorded_) {
        // One slot without a VkQueue: its timeline is the progress of the recordings the GPU has run.
        if (!vk_.vkCreateEvent || !vk_.vkDestroyEvent || !vk_.vkCmdSetEvent || !vk_.vkGetEventStatus || !vk_.vkResetEvent) {
            Log("recorded device: event functions missing");
            return E_FAIL;
        }
        queueFamily_ = desc.recordedFamily;
        auto slot = std::make_unique<QueueSlot>();
        slot->family = queueFamily_;
        VkSemaphoreTypeCreateInfo progress{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo progressInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &progress};
        if (vk_.vkCreateSemaphore(device_, &progressInfo, nullptr, &slot->timeline) != VK_SUCCESS) return E_FAIL;
        slots_.push_back(std::move(slot));
    }
    else queueFamily_ = desc.queues[0].family;
    for (uint32_t i = 0; i < desc.queueCount; ++i) {
        if (desc.queues[i].family != queueFamily_) { Log("queues must share one family"); return E_INVALIDARG; }
        auto slot = std::make_unique<QueueSlot>();
        slot->family = queueFamily_;
        slot->index = i;
        vk_.vkGetDeviceQueue(device_, queueFamily_, desc.queues[i].index, &slot->queue);
        if (!slot->queue) return E_FAIL;
        VkSemaphoreTypeCreateInfo progress{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo progressInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &progress};
        if (vk_.vkCreateSemaphore(device_, &progressInfo, nullptr, &slot->timeline) != VK_SUCCESS) return E_FAIL;
        slots_.push_back(std::move(slot));
    }
    programs_.Load(desc.programModule);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queueFamily_};
    if (vk_.vkCreateCommandPool(device_, &pool, nullptr, &initPool_) != VK_SUCCESS) return E_FAIL;
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
    if (vk_.vkCreateSemaphore(device_, &semaphore, nullptr, &initSemaphore_) != VK_SUCCESS ||
        vk_.vkCreateSemaphore(device_, &semaphore, nullptr, &wakeSemaphore_) != VK_SUCCESS) return E_FAIL;
    waiter_ = std::thread([this] { WaiterThread(); });
    Log("device ready: %s, %u queues (family %u), %zu programs", properties_.deviceName, desc.queueCount, queueFamily_, programs_.Size());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12Device1, IUnknown, ID3D12Object, ID3D12Device, ID3D12Device1>(this, riid, out);
}

bool Device::FindMemoryType(uint32_t bits, D3D12_HEAP_TYPE heap, uint32_t& type) const
{
    struct Preference { VkMemoryPropertyFlags required, avoid; };
    std::vector<Preference> order;
    switch (heap) {
    case D3D12_HEAP_TYPE_UPLOAD:
        order = {{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
                 {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0}};
        break;
    case D3D12_HEAP_TYPE_READBACK:
        order = {{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0},
                 {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0}};
        break;
    default:
        order = {{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT}, {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0}};
        break;
    }
    for (const auto& p : order)
        for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            const auto flags = memory_.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (flags & p.required) == p.required && !(flags & p.avoid)) { type = i; return true; }
        }
    return false;
}

D3D12_GPU_VIRTUAL_ADDRESS Device::AllocateVa(uint64_t size, Resource* resource)
{
    std::lock_guard lock(vaMutex_);
    const D3D12_GPU_VIRTUAL_ADDRESS va = nextVa_;
    nextVa_ += AlignUp(size ? size : 1, kPlacementAlignment) + kPlacementAlignment;
    vas_[va] = {size, resource};
    return va;
}

void Device::FreeVa(D3D12_GPU_VIRTUAL_ADDRESS va)
{
    std::lock_guard lock(vaMutex_);
    vas_.erase(va);
}

Resource* Device::ResolveVa(D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t& offset)
{
    std::lock_guard lock(vaMutex_);
    auto it = vas_.upper_bound(va);
    if (it == vas_.begin()) return nullptr;
    --it;
    if (va - it->first >= it->second.first) return nullptr;
    offset = va - it->first;
    return it->second.second;
}

namespace
{
// A new image moves from UNDEFINED to GENERAL and is cleared, a new buffer is zeroed, then one full barrier.
void RecordInitialization(const VkFns& vk, VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkBuffer buffer)
{
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    if (image) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vk.vkCmdPipelineBarrier2(command, &dependency);
        const VkImageSubresourceRange range{aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue zero{};
            vk.vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        } else {
            VkClearDepthStencilValue zero{};
            vk.vkCmdClearDepthStencilImage(command, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
    }
    if (buffer) vk.vkCmdFillBuffer(command, buffer, 0, VK_WHOLE_SIZE, 0);
    dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vk.vkCmdPipelineBarrier2(command, &dependency);
}
}

void Device::InitializeResource(VkImage image, VkImageAspectFlags aspect, VkBuffer buffer)
{
    if (recorded_) {
        // vkprov.1: into the open recording, ahead of any use (lists record into the same command buffer after this);
        // before the first recording, at its start.
        std::lock_guard lock(recordMutex_);
        if (recording_) RecordInitialization(vk_, recording_, image, aspect, buffer);
        else pendingInit_.push_back([this, image, aspect, buffer](VkCommandBuffer command) {
            RecordInitialization(vk_, command, image, aspect, buffer);
        });
        return;
    }
    std::lock_guard lock(initMutex_);
    uint64_t completed = 0;
    vk_.vkGetSemaphoreCounterValue(device_, initSemaphore_, &completed);
    VkCommandBuffer command = VK_NULL_HANDLE;
    for (auto it = initBatches_.begin(); it != initBatches_.end(); ++it)
        if (it->value <= completed) { command = it->buffer; initBatches_.erase(it); break; }
    if (!command) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, initPool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (vk_.vkAllocateCommandBuffers(device_, &allocate, &command) != VK_SUCCESS) { Fault("init command buffer"); return; }
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk_.vkBeginCommandBuffer(command, &begin);
    RecordInitialization(vk_, command, image, aspect, buffer);
    vk_.vkEndCommandBuffer(command);
    const uint64_t value = ++initValue_;
    VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, command};
    QueueSlot& slot = *slots_[0];
    std::lock_guard queueLock(slot.mutex);
    VkSemaphoreSubmitInfo signals[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, initSemaphore_, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                        Progress(slot)};
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signals;
    if (vk_.vkQueueSubmit2(slot.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) { Fault("init submit"); return; }
    initBatches_.push_back({command, value});
}

void Device::DeferDestroy(std::function<void()> destroy)
{
    Grave grave{{}, std::move(destroy)};
    // vkprov.1: a recording still open ends as the next epoch, and its work may use the object.
    bool open = false;
    if (recorded_) {
        std::lock_guard lock(recordMutex_);
        open = recording_ != VK_NULL_HANDLE;
    }
    for (auto& slot : slots_) {
        std::lock_guard lock(slot->mutex);
        grave.targets.push_back(slot->submitted + (open ? 1 : 0));
    }
    {
        std::lock_guard lock(graveMutex_);
        graves_.push_back(std::move(grave));
    }
    Collect();
}

void Device::Collect()
{
    std::vector<uint64_t> done(slots_.size());
    for (size_t i = 0; i < slots_.size(); ++i) vk_.vkGetSemaphoreCounterValue(device_, slots_[i]->timeline, &done[i]);
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard lock(graveMutex_);
        for (auto it = graves_.begin(); it != graves_.end();) {
            bool finished = true;
            for (size_t i = 0; i < done.size() && finished; ++i) finished = done[i] >= it->targets[i];
            if (finished) { ready.push_back(std::move(it->destroy)); it = graves_.erase(it); }
            else ++it;
        }
    }
    for (auto& destroy : ready) destroy();
}

void Device::InitWait(VkSemaphore& semaphore, uint64_t& value)
{
    std::lock_guard lock(initMutex_);
    semaphore = initSemaphore_;
    value = initValue_;
}

bool Device::WaitIdle(uint64_t timeoutNs)
{
    if (recorded_) {
        const ULONGLONG deadline = GetTickCount64() + timeoutNs / 1000000;
        while (true) {
            PollRecordings();
            {
                std::lock_guard lock(recordMutex_);
                if (epochs_.empty()) return true;
            }
            if (GetTickCount64() >= deadline) return false;
            Sleep(1);
        }
    }
    std::vector<VkSemaphore> semaphores;
    std::vector<uint64_t> values;
    for (auto& slot : slots_) {
        std::lock_guard lock(slot->mutex);
        semaphores.push_back(slot->timeline);
        values.push_back(slot->submitted);
    }
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, uint32_t(semaphores.size()), semaphores.data(), values.data()};
    const VkResult result = vk_.vkWaitSemaphores(device_, &wait, timeoutNs);
    Collect();
    return result == VK_SUCCESS;
}

void Device::Fault(const char* what)
{
    Log("device fault: %s", what);
    removed_.store(DXGI_ERROR_DEVICE_HUNG);
}

VkCommandBuffer Device::RecordingBuffer()
{
    std::lock_guard lock(recordMutex_);
    return recording_;
}

bool Device::BeginRecording(VkCommandBuffer commandBuffer)
{
    if (!recorded_ || !commandBuffer) return false;
    PollRecordings();
    std::vector<std::function<void(VkCommandBuffer)>> pending;
    {
        std::lock_guard lock(recordMutex_);
        if (recording_) return false;
        recording_ = commandBuffer;
        pending.swap(pendingInit_);
    }
    for (auto& record : pending) record(commandBuffer);
    return true;
}

void Device::EndRecording()
{
    VkEvent event = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    {
        std::lock_guard lock(recordMutex_);
        command = recording_;
        recording_ = VK_NULL_HANDLE;
        if (!command) return;
        if (!freeEvents_.empty()) {
            event = freeEvents_.back();
            freeEvents_.pop_back();
        }
    }
    if (!event) {
        VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (vk_.vkCreateEvent(device_, &info, nullptr, &event) != VK_SUCCESS) { Fault("recording event"); return; }
    }
    // Set once everything recorded before it has run; the epoch's objects are released after that.
    vk_.vkCmdSetEvent(command, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    QueueSlot& slot = *slots_[0];
    uint64_t value = 0;
    {
        std::lock_guard lock(slot.mutex);
        value = ++slot.submitted;
    }
    std::lock_guard lock(recordMutex_);
    epochs_.push_back({event, value});
}

void Device::PollRecordings()
{
    if (!recorded_) return;
    uint64_t finished = 0;
    {
        std::lock_guard lock(recordMutex_);
        // In order: a recording the caller never submitted holds every later one back, which only delays destruction.
        while (!epochs_.empty() && vk_.vkGetEventStatus(device_, epochs_.front().event) == VK_EVENT_SET) {
            finished = epochs_.front().value;
            vk_.vkResetEvent(device_, epochs_.front().event);
            freeEvents_.push_back(epochs_.front().event);
            epochs_.erase(epochs_.begin());
        }
    }
    if (finished) {
        uint64_t current = 0;
        vk_.vkGetSemaphoreCounterValue(device_, slots_[0]->timeline, &current);
        if (finished > current) {
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, slots_[0]->timeline, finished};
            vk_.vkSignalSemaphore(device_, &signal);
        }
    }
    Collect();
}

void Device::WaitAsync(VkSemaphore semaphore, uint64_t value, HANDLE event)
{
    WaitAsync(semaphore, value, [event] { SetEvent(event); });
}

void Device::WaitAsync(VkSemaphore semaphore, uint64_t value, std::function<void()> done)
{
    {
        std::lock_guard lock(waitMutex_);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        waits_.push_back({semaphore, value, std::move(done), now.QuadPart, false});
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, wakeSemaphore_, ++wakeValue_};
        vk_.vkSignalSemaphore(device_, &signal);
    }
    waitChanged_.notify_all();
}

void Device::WaiterThread()
{
    std::vector<VkSemaphore> semaphores;
    std::vector<uint64_t> values;
    while (true) {
        {
            std::unique_lock lock(waitMutex_);
            waitChanged_.wait(lock, [&] { return stopping_ || !waits_.empty(); });
            if (stopping_) return;
            semaphores.clear();
            values.clear();
            for (const auto& w : waits_) { semaphores.push_back(w.semaphore); values.push_back(w.value); }
            semaphores.push_back(wakeSemaphore_);
            values.push_back(wakeValue_ + 1);
        }
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, VK_SEMAPHORE_WAIT_ANY_BIT, uint32_t(semaphores.size()),
                                 semaphores.data(), values.data()};
        if (vk_.vkWaitSemaphores(device_, &wait, 100'000'000ull) == VK_ERROR_DEVICE_LOST) Fault("vkWaitSemaphores: device lost");
        std::lock_guard lock(waitMutex_);
        for (auto it = waits_.begin(); it != waits_.end();) {
            uint64_t current = 0;
            vk_.vkGetSemaphoreCounterValue(device_, it->semaphore, &current);
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const double ms = double(now.QuadPart - it->since) * 1000.0 / double(frequency.QuadPart);
            if (current >= it->value) {
                if (ms > 500) Log("wait: semaphore %p value %llu completed after %.0f ms", static_cast<void*>(it->semaphore), it->value, ms);
                it->done();
                it = waits_.erase(it);
                continue;
            }
            if (ms > 500 && !it->reported) {
                it->reported = true;
                Log("wait: semaphore %p value %llu still pending after %.0f ms (counter %llu)", static_cast<void*>(it->semaphore), it->value, ms, current);
            }
            ++it;
        }
    }
}

VkSampler Device::Sampler(const VkSamplerCreateInfo& info)
{
    std::array<uint8_t, sizeof(VkSamplerCreateInfo)> key{};
    memcpy(key.data(), &info, sizeof(info));
    std::lock_guard lock(samplerMutex_);
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vk_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    samplers_[key] = sampler;
    return sampler;
}

void Device::SetBackbufferHint(const BackbufferHint& hint)
{
    std::lock_guard lock(hintMutex_);
    hint_ = hint;
}

bool Device::MatchBackbuffer(const D3D12_RESOURCE_DESC& desc, BackbufferHint& hint)
{
    std::lock_guard lock(hintMutex_);
    if (!hint_.width || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        desc.Width != hint_.width || desc.Height != hint_.height || desc.Format != hint_.format || desc.DepthOrArraySize != 1 ||
        desc.MipLevels > 1 || desc.SampleDesc.Count > 1)
        return false;
    hint = hint_;
    return true;
}

void Device::SetRetireHint(HWND window, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(hintMutex_);
    if (swapchain) retire_[window] = {swapchain};
    else retire_.erase(window);
}

VkSwapchainKHR Device::DropRetireHint(HWND window)
{
    VkSwapchainKHR orphan = VK_NULL_HANDLE, swapchain = VK_NULL_HANDLE;
    {
        std::lock_guard lock(hintMutex_);
        auto found = retire_.find(window);
        if (found == retire_.end() || found->second.claimed) return VK_NULL_HANDLE;
        (found->second.destroy ? orphan : swapchain) = found->second.swapchain;
        retire_.erase(found);
    }
    // Handed over by the game and never retired: nothing names it any more.
    if (orphan) vk_.vkDestroySwapchainKHR(device_, orphan, nullptr);
    return swapchain;
}

VkSwapchainKHR Device::ClaimRetireHint(HWND window)
{
    std::lock_guard lock(hintMutex_);
    auto found = retire_.find(window);
    if (found == retire_.end() || found->second.claimed) return VK_NULL_HANDLE;
    found->second.claimed = true;
    return found->second.swapchain;
}

void Device::FinishRetire(HWND window)
{
    VkSwapchainKHR orphan = VK_NULL_HANDLE;
    {
        std::lock_guard lock(hintMutex_);
        auto found = retire_.find(window);
        if (found == retire_.end() || !found->second.claimed) return;
        if (found->second.destroy) orphan = found->second.swapchain;
        retire_.erase(found);
    }
    // oldSwapchain is retired even when the creation failed.
    if (orphan) vk_.vkDestroySwapchainKHR(device_, orphan, nullptr);
}

bool Device::DeferRetiredDestroy(VkSwapchainKHR swapchain)
{
    std::lock_guard lock(hintMutex_);
    for (auto& [window, hint] : retire_)
        if (hint.swapchain == swapchain) {
            hint.destroy = true;
            return true;
        }
    return false;
}

bool Device::ImageSupported(VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags) const
{
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, nullptr, format, VK_IMAGE_TYPE_2D,
                                          VK_IMAGE_TILING_OPTIMAL, usage, flags};
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    return vk_.vkGetPhysicalDeviceImageFormatProperties2(physical_, &info, &properties) == VK_SUCCESS;
}

// ---------------------------------------------------------------- object creation
HRESULT STDMETHODCALLTYPE Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out)
{
    if (!desc) return E_INVALIDARG;
    if (desc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc->Type != D3D12_COMMAND_LIST_TYPE_COMPUTE && desc->Type != D3D12_COMMAND_LIST_TYPE_COPY)
        return E_INVALIDARG;
    // Each D3D12 queue gets its own VkQueue while there are enough: D3D12 queues wait on each other's fences, and
    // a wait queued ahead of the signal on the same VkQueue would never complete. The last slot presents; a slot
    // whose queue was released is used again.
    const size_t usable = slots_.size() - 1;
    QueueSlot* slot = slots_[0].get();
    for (size_t i = 0; i < usable; ++i)
        if (slots_[i]->users.load() < slot->users.load()) slot = slots_[i].get();
    if (slot->users.load()) Log("CreateCommandQueue: shares VkQueue %u with %u queue(s) (%zu available)", slot->index, slot->users.load(), usable);
    return Hand(new CommandQueue(this, *desc, slot), S_OK, riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out)
{
    auto* allocator = new CommandAllocator(this, type);
    return Hand(allocator, allocator->Init(), riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    if (!desc || !desc->CS.pShaderBytecode || !desc->CS.BytecodeLength) return E_INVALIDARG;
    const std::string sha = Sha256(desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
    const Program* program = programs_.Find(sha);
    if (!program) {
        NoteMissingProgram(sha, desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
        return E_INVALIDARG;
    }
    RootSignature* rootSignature = Own<RootSignature>(desc->pRootSignature);
    ID3D12RootSignature* embedded = nullptr;
    if (!rootSignature) {
        if (FAILED(CreateRootSignature(0, desc->CS.pShaderBytecode, desc->CS.BytecodeLength, IID_PPV_ARGS(&embedded)))) return E_INVALIDARG;
        rootSignature = Own<RootSignature>(embedded);
    }
    auto* pso = new PipelineState(this);
    pso->SetTensorLayer(xefg_cuda::LayerForSha256(sha));
    const HRESULT hr = pso->Init(rootSignature, *program);
    if (embedded) embedded->Release();
    return Hand(pso, hr, riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initial, REFIID riid, void** out)
{
    if (!allocator) return E_INVALIDARG;
    auto* list = new CommandList(this, type);
    HRESULT hr = list->Begin(Own<CommandAllocator>(allocator));
    if (SUCCEEDED(hr) && initial) list->SetPipelineState(initial);
    return Hand(list, hr, riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out)
{
    if (!desc || !desc->NumDescriptors) return E_INVALIDARG;
    return Hand(new DescriptorHeap(this, *desc), S_OK, riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateRootSignature(UINT, const void* blob, SIZE_T size, REFIID riid, void** out)
{
    if (!blob || !size) return E_INVALIDARG;
    RootLayout layout;
    std::string error;
    if (!ParseRootSignature(blob, size, layout, error)) {
        Log("CreateRootSignature: %s", error.c_str());
        return E_INVALIDARG;
    }
    return Hand(new RootSignature(this, std::move(layout)), S_OK, riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID riid, void** out)
{
    if (!heap || !desc) return E_INVALIDARG;
    auto* resource = new Resource(this, *desc, *heap, flags);
    return Hand(resource, resource->Init(nullptr, 0), riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** out)
{
    if (!desc || !desc->SizeInBytes) return E_INVALIDARG;
    auto* heap = new Heap(this, *desc);
    return Hand(heap, heap->Init(), riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreatePlacedResource(ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID riid, void** out)
{
    if (!heap || !desc) return E_INVALIDARG;
    auto* owner = Own<Heap>(heap);
    const D3D12_HEAP_DESC heapDesc = owner->GetDesc();
    auto* resource = new Resource(this, *desc, heapDesc.Properties, heapDesc.Flags);
    return Hand(resource, resource->Init(owner, offset), riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** out)
{
    if (flags & (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER)) XVK_UNIMPLEMENTED_HR("Device::CreateFence(shared)");
    auto* fence = new Fence(this);
    return Hand(fence, fence->Init(initial), riid, out);
}

HRESULT STDMETHODCALLTYPE Device::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** out)
{
    if (!desc) return E_INVALIDARG;
    if (desc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP) XVK_UNIMPLEMENTED_HR("Device::CreateQueryHeap(non-timestamp)");
    auto* heap = new QueryHeap(this, *desc);
    return Hand(heap, heap->Init(), riid, out);
}

// ---------------------------------------------------------------- feature queries
HRESULT STDMETHODCALLTYPE Device::CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size)
{
    if (!data) return E_INVALIDARG;
    switch (feature) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
        if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data);
        *o = {};
        o->DoublePrecisionFloatShaderOps = TRUE;
        o->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
        o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
        o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
        o->TypedUAVLoadAdditionalFormats = TRUE;
        o->MaxGPUVirtualAddressBitsPerResource = 40;
        o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
        if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1*>(data);
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
        vk_.vkGetPhysicalDeviceProperties2(physical_, &properties);
        *o = {};
        o->WaveOps = TRUE;
        o->WaveLaneCountMin = o->WaveLaneCountMax = subgroup.subgroupSize;
        o->TotalLaneCount = subgroup.subgroupSize * 64;
        o->ExpandedComputeResourceStates = TRUE;
        o->Int64ShaderOps = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
        if (size != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(data);
        if (o->HighestShaderModel > D3D_SHADER_MODEL_6_6) o->HighestShaderModel = D3D_SHADER_MODEL_6_6;
        return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
        if (size != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE*>(data);
        o->TileBasedRenderer = FALSE; o->UMA = FALSE; o->CacheCoherentUMA = FALSE;
        return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
        if (size != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1*>(data);
        o->TileBasedRenderer = FALSE; o->UMA = FALSE; o->CacheCoherentUMA = FALSE; o->IsolatedMMU = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
        if (size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data);
        o->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL(0);
        for (UINT i = 0; i < o->NumFeatureLevels; ++i)
            if (o->pFeatureLevelsRequested[i] <= D3D_FEATURE_LEVEL_12_1 && o->pFeatureLevelsRequested[i] > o->MaxSupportedFeatureLevel)
                o->MaxSupportedFeatureLevel = o->pFeatureLevelsRequested[i];
        return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
        if (size != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT*>(data);
        o->MaxGPUVirtualAddressBitsPerProcess = 47; o->MaxGPUVirtualAddressBitsPerResource = 40;
        return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
        if (size != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE*>(data);
        if (o->HighestVersion > D3D_ROOT_SIGNATURE_VERSION_1_1) o->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        return S_OK;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
        if (size != sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY*>(data);
        o->PriorityForTypeIsSupported = o->Priority <= D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
        if (size != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT)) return E_INVALIDARG;
        auto* o = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
        const FormatInfo info = Format(o->Format);
        o->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
        o->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
        if (info.format == VK_FORMAT_UNDEFINED) return E_FAIL;
        VkFormatProperties props{};
        vk_.vkGetPhysicalDeviceFormatProperties(physical_, info.format, &props);
        const auto f = props.optimalTilingFeatures;
        UINT s1 = D3D12_FORMAT_SUPPORT1_BUFFER;
        if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) s1 |= D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
        if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) s1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
        if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) s1 |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) s1 |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
        UINT s2 = 0;
        if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) s2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) s2 |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE;
        o->Support1 = D3D12_FORMAT_SUPPORT1(s1);
        o->Support2 = D3D12_FORMAT_SUPPORT2(s2);
        return S_OK;
    }
    default:
        Unimplemented(("CheckFeatureSupport(" + std::to_string(int(feature)) + ")").c_str());
        return E_INVALIDARG;
    }
}

// ---------------------------------------------------------------- allocation queries
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE Device::GetResourceAllocationInfo(UINT, UINT count, const D3D12_RESOURCE_DESC* descs)
{
    D3D12_RESOURCE_ALLOCATION_INFO result{0, 0};
    for (UINT i = 0; i < count; ++i) {
        const auto& desc = descs[i];
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            VkBufferCreateInfo info = BufferCreateInfo(desc);
            VkDeviceBufferMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, nullptr, &info};
            vk_.vkGetDeviceBufferMemoryRequirements(device_, &query, &requirements);
        } else {
            VkImageCreateInfo info{};
            FormatInfo format;
            if (!ImageCreateInfo(desc, info, format)) return {UINT64_MAX, 0};
            VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, nullptr, &info};
            vk_.vkGetDeviceImageMemoryRequirements(device_, &query, &requirements);
        }
        const uint64_t alignment = std::max<uint64_t>({requirements.memoryRequirements.alignment, desc.Alignment ? desc.Alignment : kPlacementAlignment});
        result.SizeInBytes = AlignUp(result.SizeInBytes, alignment) + AlignUp(requirements.memoryRequirements.size, alignment);
        result.Alignment = std::max(result.Alignment, alignment);
    }
    return result;
}

D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE Device::GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_CUSTOM};
    switch (type) {
    case D3D12_HEAP_TYPE_UPLOAD: properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0; break;
    case D3D12_HEAP_TYPE_READBACK: properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0; break;
    default: properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L1; break;
    }
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    return properties;
}

void STDMETHODCALLTYPE Device::GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* rowSizes, UINT64* total)
{
    if (!desc) return;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (layouts && count) layouts[0] = {base, {DXGI_FORMAT_UNKNOWN, UINT(desc->Width), 1, 1, UINT(AlignUp(desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT))}};
        if (rows && count) rows[0] = 1;
        if (rowSizes && count) rowSizes[0] = desc->Width;
        if (total) *total = desc->Width;
        return;
    }
    const FormatInfo format = Format(desc->Format, (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
    const UINT mips = desc->MipLevels ? desc->MipLevels : 1;
    const UINT layers = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc->DepthOrArraySize;
    uint64_t offset = 0, end = 0;
    for (UINT i = 0; i < count; ++i) {
        const UINT sub = first + i;
        const UINT mip = sub % mips;
        if (sub / mips >= layers) break;
        const UINT w = std::max<UINT>(1, UINT(desc->Width >> mip));
        const UINT h = std::max<UINT>(1, desc->Height >> mip);
        const UINT d = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max<UINT>(1, desc->DepthOrArraySize >> mip) : 1;
        const uint64_t rowSize = uint64_t(w) * format.bytes;
        const uint64_t pitch = AlignUp(rowSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        offset = AlignUp(end, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (layouts) layouts[i] = {base + offset, {desc->Format, w, h, d, UINT(pitch)}};
        if (rows) rows[i] = h;
        if (rowSizes) rowSizes[i] = rowSize;
        end = offset + pitch * (uint64_t(h) * d - 1) + rowSize;
    }
    if (total) *total = end;
}

// ---------------------------------------------------------------- descriptors
namespace {
Descriptor* At(D3D12_CPU_DESCRIPTOR_HANDLE handle) { return reinterpret_cast<Descriptor*>(handle.ptr); }

// View format against the resource's image: depth images keep their own format and pick an aspect.
void ViewFormat(Resource* resource, DXGI_FORMAT requested, VkFormat& format, VkImageAspectFlags& aspect)
{
    const FormatInfo image = Format(resource->desc().Format, (resource->desc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
    if (image.depth) {
        format = resource->imageFormat();
        const FormatInfo view = Format(requested == DXGI_FORMAT_UNKNOWN ? resource->desc().Format : requested, true);
        aspect = (view.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        return;
    }
    format = requested == DXGI_FORMAT_UNKNOWN ? resource->imageFormat() : Format(requested).format;
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
}
} // namespace

void STDMETHODCALLTYPE Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = At(dest);
    *d = {};
    if (!desc) return;
    uint64_t offset = 0;
    Resource* resource = ResolveVa(desc->BufferLocation, offset);
    if (!resource) { Log("CreateConstantBufferView: unknown GPU VA 0x%llx", desc->BufferLocation); return; }
    d->kind = DescriptorKind::Cbv;
    d->resource = resource;
    d->buffer = {resource->buffer(), offset, desc->SizeInBytes};
}

void STDMETHODCALLTYPE Device::CreateShaderResourceView(ID3D12Resource* resourceIn, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = At(dest);
    *d = {};
    if (!resourceIn) { Unimplemented("CreateShaderResourceView(null resource)"); return; }
    Resource* resource = Own<Resource>(resourceIn);
    d->resource = resource;
    if (desc && desc->Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) Unimplemented("SRV component mapping");
    if (resource->IsBuffer()) {
        if (!desc || desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER) { Log("buffer SRV without buffer desc"); return; }
        uint64_t stride = desc->Buffer.StructureByteStride;
        if (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) stride = 4;
        if (!stride) { Unimplemented("typed buffer SRV"); return; }
        d->stride = uint32_t(stride);
        d->kind = DescriptorKind::SrvBuffer;
        d->buffer = {resource->buffer(), desc->Buffer.FirstElement * stride, uint64_t(desc->Buffer.NumElements) * stride};
        return;
    }
    VkFormat format;
    VkImageAspectFlags aspect;
    ViewFormat(resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, format, aspect);
    const auto& rd = resource->desc();
    uint32_t baseMip = 0, mips = rd.MipLevels ? rd.MipLevels : 1, baseLayer = 0, layers = rd.DepthOrArraySize;
    VkImageViewType type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (desc) {
        switch (desc->ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            type = VK_IMAGE_VIEW_TYPE_2D; baseMip = desc->Texture2D.MostDetailedMip; layers = 1;
            mips = desc->Texture2D.MipLevels == UINT(-1) ? mips - baseMip : desc->Texture2D.MipLevels;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; baseMip = desc->Texture2DArray.MostDetailedMip;
            mips = desc->Texture2DArray.MipLevels == UINT(-1) ? mips - baseMip : desc->Texture2DArray.MipLevels;
            baseLayer = desc->Texture2DArray.FirstArraySlice; layers = desc->Texture2DArray.ArraySize;
            break;
        default: Unimplemented("SRV dimension"); return;
        }
    }
    VkImageView view = resource->View(format, type, aspect, baseMip, mips, baseLayer, layers, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!view) return;
    d->viewType = type;
    d->kind = DescriptorKind::SrvImage;
    d->image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    if (type == VK_IMAGE_VIEW_TYPE_2D)
        d->cudaArrayView = resource->View(format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, aspect, baseMip, mips, baseLayer, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
}

void STDMETHODCALLTYPE Device::CreateUnorderedAccessView(ID3D12Resource* resourceIn, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = At(dest);
    *d = {};
    if (!resourceIn) { Unimplemented("CreateUnorderedAccessView(null resource)"); return; }
    if (counter) { Unimplemented("UAV counter resource"); return; }
    Resource* resource = Own<Resource>(resourceIn);
    d->resource = resource;
    if (resource->IsBuffer()) {
        if (!desc || desc->ViewDimension != D3D12_UAV_DIMENSION_BUFFER) { Log("buffer UAV without buffer desc"); return; }
        uint64_t stride = desc->Buffer.StructureByteStride;
        if (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) stride = 4;
        if (!stride) { Unimplemented("typed buffer UAV"); return; }
        d->stride = uint32_t(stride);
        d->kind = DescriptorKind::UavBuffer;
        d->buffer = {resource->buffer(), desc->Buffer.FirstElement * stride, uint64_t(desc->Buffer.NumElements) * stride};
        return;
    }
    VkFormat format;
    VkImageAspectFlags aspect;
    ViewFormat(resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, format, aspect);
    const auto& rd = resource->desc();
    uint32_t mip = 0, baseLayer = 0, layers = rd.DepthOrArraySize;
    VkImageViewType type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (desc) {
        switch (desc->ViewDimension) {
        case D3D12_UAV_DIMENSION_TEXTURE2D: type = VK_IMAGE_VIEW_TYPE_2D; mip = desc->Texture2D.MipSlice; layers = 1; break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; mip = desc->Texture2DArray.MipSlice;
            baseLayer = desc->Texture2DArray.FirstArraySlice; layers = desc->Texture2DArray.ArraySize;
            break;
        default: Unimplemented("UAV dimension"); return;
        }
    }
    VkImageView view = resource->View(format, type, aspect, mip, 1, baseLayer, layers, VK_IMAGE_USAGE_STORAGE_BIT);
    if (!view) return;
    d->viewType = type;
    d->kind = DescriptorKind::UavImage;
    d->image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    if (type == VK_IMAGE_VIEW_TYPE_2D)
        d->cudaArrayView = resource->View(format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, aspect, mip, 1, baseLayer, 1, VK_IMAGE_USAGE_STORAGE_BIT);
}

void STDMETHODCALLTYPE Device::CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = At(dest);
    *d = {};
    d->kind = DescriptorKind::Rtv;
    d->resource = Own<Resource>(resource);
}

void STDMETHODCALLTYPE Device::CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = At(dest);
    *d = {};
    d->kind = DescriptorKind::Dsv;
    d->resource = Own<Resource>(resource);
}

VkSamplerCreateInfo SamplerInfo(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE u, D3D12_TEXTURE_ADDRESS_MODE v, D3D12_TEXTURE_ADDRESS_MODE w,
                                float bias, UINT anisotropy, D3D12_COMPARISON_FUNC comparison, VkBorderColor border, float minLod, float maxLod);

void STDMETHODCALLTYPE Device::CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = At(dest);
    *d = {};
    if (!desc) return;
    const bool white = desc->BorderColor[0] > 0.5f;
    const bool opaque = desc->BorderColor[3] > 0.5f;
    const VkBorderColor border = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : (opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    const VkSamplerCreateInfo info = SamplerInfo(desc->Filter, desc->AddressU, desc->AddressV, desc->AddressW, desc->MipLODBias, desc->MaxAnisotropy,
                                                 desc->ComparisonFunc, border, desc->MinLOD, desc->MaxLOD);
    d->kind = DescriptorKind::Sampler;
    d->image.sampler = Sampler(info);
}

void STDMETHODCALLTYPE Device::CopyDescriptors(UINT destRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts, const UINT* destSizes,
    UINT srcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE)
{
    UINT di = 0, dOff = 0, si = 0, sOff = 0;
    while (di < destRanges && si < srcRanges) {
        const UINT dSize = destSizes ? destSizes[di] : 1;
        const UINT sSize = srcSizes ? srcSizes[si] : 1;
        At(destStarts[di])[dOff] = At(srcStarts[si])[sOff];
        if (++dOff == dSize) { ++di; dOff = 0; }
        if (++sOff == sSize) { ++si; sOff = 0; }
    }
}

void STDMETHODCALLTYPE Device::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE)
{
    memmove(At(dest), At(src), size_t(count) * sizeof(Descriptor));
}

} // namespace xvk

// ---------------------------------------------------------------- host entry points
namespace xvk {

HRESULT CreateDevice(const DeviceDesc& desc, REFIID riid, void** out) noexcept
{
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    try {
        auto* device = new Device();
        const HRESULT hr = device->Init(desc);
        if (FAILED(hr)) { device->Release(); return hr; }
        const HRESULT qi = device->QueryInterface(riid, out);
        device->Release();
        return qi;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

// vkprov.1: the device and resources passed here are this layer's own (a recorded device the caller created).
bool BeginRecording(ID3D12Device* d3d, VkCommandBuffer commandBuffer) noexcept
{
    if (!d3d) return false;
    try { return Own<Device>(static_cast<ID3D12Device1*>(d3d))->BeginRecording(commandBuffer); }
    catch (...) { return false; }
}

void EndRecording(ID3D12Device* d3d) noexcept
{
    if (!d3d) return;
    try { Own<Device>(static_cast<ID3D12Device1*>(d3d))->EndRecording(); }
    catch (...) {}
}

bool WaitRecordedIdle(ID3D12Device* d3d, uint64_t timeoutNs) noexcept
{
    if (!d3d) return false;
    try { return Own<Device>(static_cast<ID3D12Device1*>(d3d))->WaitIdle(timeoutNs); }
    catch (...) { return false; }
}

VkImage ImageOf(ID3D12Resource* resource) noexcept
{
    if (!resource) return VK_NULL_HANDLE;
    auto* own = Own<Resource>(resource);
    return own->IsBuffer() ? VK_NULL_HANDLE : own->image();
}

bool InstallTensorCores(ID3D12Device* device, const void* fatbin, size_t bytes, const TensorOptions& options, std::string& reason) noexcept
{
    auto* own = Own<Device>(device);
    return own && own->InstallTensor({static_cast<const unsigned char*>(fatbin), bytes}, options, reason);
}
void SetTensorCoresEnabled(ID3D12Device* device, bool enabled) noexcept
{
    if (auto* own = Own<Device>(device)) own->tensor().Enable(enabled);
}
} // namespace xvk
