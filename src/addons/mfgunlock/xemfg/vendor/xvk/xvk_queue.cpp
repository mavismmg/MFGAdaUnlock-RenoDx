#include "xvk_internal.h"

namespace xvk {

// ---------------------------------------------------------------- CommandAllocator
CommandAllocator::CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type) : DeviceChild(device), type_(type) {}

CommandAllocator::~CommandAllocator()
{
    Device* device = device_;
    const VkCommandPool pool = pool_;
    if (pool) device->DeferDestroy([device, pool] { device->vk().vkDestroyCommandPool(device->vkDevice(), pool, nullptr); });
}

HRESULT CommandAllocator::Init()
{
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, device_->queueFamily()};
    return device_->vk().vkCreateCommandPool(device_->vkDevice(), &info, nullptr, &pool_) == VK_SUCCESS ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE CommandAllocator::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12CommandAllocator, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandAllocator>(this, riid, out);
}

HRESULT STDMETHODCALLTYPE CommandAllocator::Reset()
{
    std::lock_guard lock(mutex_);
    if (device_->vk().vkResetCommandPool(device_->vkDevice(), pool_, 0) != VK_SUCCESS) return E_FAIL;
    free_.insert(free_.end(), used_.begin(), used_.end());
    used_.clear();
    return S_OK;
}

VkCommandBuffer CommandAllocator::Allocate()
{
    std::lock_guard lock(mutex_);
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    if (!free_.empty()) {
        buffer = free_.back();
        free_.pop_back();
    } else {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (device_->vk().vkAllocateCommandBuffers(device_->vkDevice(), &info, &buffer) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    used_.push_back(buffer);
    return buffer;
}

// ---------------------------------------------------------------- QueryHeap
QueryHeap::~QueryHeap()
{
    Device* device = device_;
    const VkQueryPool pool = pool_;
    if (pool) device->DeferDestroy([device, pool] { device->vk().vkDestroyQueryPool(device->vkDevice(), pool, nullptr); });
}

HRESULT QueryHeap::Init()
{
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, desc_.Count};
    return device_->vk().vkCreateQueryPool(device_->vkDevice(), &info, nullptr, &pool_) == VK_SUCCESS ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE QueryHeap::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12QueryHeap, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12QueryHeap>(this, riid, out);
}

// ---------------------------------------------------------------- Fence
Fence::~Fence()
{
    Device* device = device_;
    const VkSemaphore semaphore = semaphore_;
    if (semaphore) device->DeferDestroy([device, semaphore] { device->vk().vkDestroySemaphore(device->vkDevice(), semaphore, nullptr); });
}

HRESULT Fence::Init(UINT64 initial)
{
    requested_.store(initial);
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, initial};
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
    return device_->vk().vkCreateSemaphore(device_->vkDevice(), &info, nullptr, &semaphore_) == VK_SUCCESS ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Fence::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12Fence, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Fence>(this, riid, out);
}

UINT64 STDMETHODCALLTYPE Fence::GetCompletedValue()
{
    uint64_t value = 0;
    device_->vk().vkGetSemaphoreCounterValue(device_->vkDevice(), semaphore_, &value);
    return value;
}

HRESULT STDMETHODCALLTYPE Fence::SetEventOnCompletion(UINT64 value, HANDLE event)
{
    if (GetCompletedValue() >= value) {
        if (event) SetEvent(event);
        return S_OK;
    }
    if (!event) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore_, &value};
        return device_->vk().vkWaitSemaphores(device_->vkDevice(), &wait, UINT64_MAX) == VK_SUCCESS ? S_OK : E_FAIL;
    }
    {
        static std::atomic<int> logged{0};
        if (logged++ < 12) Log("SetEventOnCompletion: fence %p value %llu event %p", static_cast<void*>(this), value, event);
    }
    device_->WaitAsync(semaphore_, value, event);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Fence::Signal(UINT64 value)
{
    if (!ClaimSignal(value)) {
        static std::atomic<int> logged{0};
        if (logged++ < 20) Log("CPU Signal skipped: fence %p value %llu (completed %llu)", static_cast<void*>(this), value, GetCompletedValue());
        if (value < GetCompletedValue()) Unimplemented("Fence::Signal to a lower value (timeline semaphores cannot go back)");
        return S_OK;
    }
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, semaphore_, value};
    return device_->vk().vkSignalSemaphore(device_->vkDevice(), &signal) == VK_SUCCESS ? S_OK : E_FAIL;
}

// ---------------------------------------------------------------- CommandQueue
HRESULT STDMETHODCALLTYPE CommandQueue::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12CommandQueue, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandQueue>(this, riid, out);
}

HRESULT CommandQueue::Submit(const std::vector<VkCommandBuffer>& buffers, VkSemaphore signal, uint64_t signalValue,
                             const VkSemaphoreSubmitInfo* extraWaits, uint32_t extraWaitCount,
                             const VkSemaphoreSubmitInfo* extraSignals, uint32_t extraSignalCount, VkFence fence, uint64_t* progress)
{
    if (FAILED(device_->GetDeviceRemovedReason())) return DXGI_ERROR_DEVICE_REMOVED;
    if (device_->recorded()) {
        // vkprov.1: the lists already recorded into the open recording, in order; nothing is submitted. Waits are
        // met by that order, and a signal completes at once (a CPU wait on it must not wait for a submission the
        // caller makes only after it returns). Objects stay alive until the recording's event (DeferDestroy).
        {
            std::lock_guard lock(pendingMutex_);
            pendingWaits_.clear();
        }
        if (signal) {
            uint64_t current = 0;
            device_->vk().vkGetSemaphoreCounterValue(device_->vkDevice(), signal, &current);
            if (signalValue > current) {
                VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, signal, signalValue};
                if (device_->vk().vkSignalSemaphore(device_->vkDevice(), &info) != VK_SUCCESS) return E_FAIL;
            }
        }
        if (progress) {
            std::lock_guard lock(slot_->mutex);
            *progress = slot_->submitted;
        }
        (void)buffers; (void)extraWaits; (void)extraWaitCount; (void)extraSignals; (void)extraSignalCount; (void)fence;
        device_->PollRecordings();
        return S_OK;
    }
    std::vector<VkSemaphoreSubmitInfo> waits;
    VkSemaphore init = VK_NULL_HANDLE;
    uint64_t initValue = 0;
    device_->InitWait(init, initValue);
    if (initValue) waits.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, init, initValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
    {
        std::lock_guard lock(pendingMutex_);
        for (const auto& [semaphore, value] : pendingWaits_)
            waits.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, semaphore, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
        pendingWaits_.clear();
    }
    waits.insert(waits.end(), extraWaits, extraWaits + extraWaitCount);
    std::vector<VkCommandBufferSubmitInfo> commands;
    for (VkCommandBuffer b : buffers) commands.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, b});
    std::vector<VkSemaphoreSubmitInfo> signals(extraSignals, extraSignals + extraSignalCount);
    if (signal) signals.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, signal, signalValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
    std::unique_lock lock(slot_->mutex);
    signals.push_back(device_->Progress(*slot_));
    if (progress) *progress = signals.back().value;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = uint32_t(waits.size());
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = uint32_t(commands.size());
    submit.pCommandBufferInfos = commands.data();
    submit.signalSemaphoreInfoCount = uint32_t(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();
    const VkResult result = device_->vk().vkQueueSubmit2(slot_->queue, 1, &submit, fence);
    lock.unlock();
    device_->Collect();
    if (result != VK_SUCCESS) {
        device_->Fault(("vkQueueSubmit2 " + std::to_string(int(result))).c_str());
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    return S_OK;
}

void STDMETHODCALLTYPE CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists)
{
    std::vector<VkCommandBuffer> buffers;
    for (UINT i = 0; i < count; ++i) {
        auto* list = Own<CommandList>(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (!list->closed() || list->failed()) {
            device_->Fault("ExecuteCommandLists: list not closed or recorded with errors");
            return;
        }
        buffers.push_back(list->buffer());
    }
    Submit(buffers, VK_NULL_HANDLE, 0, nullptr, 0, nullptr, 0, VK_NULL_HANDLE, nullptr);
}

HRESULT STDMETHODCALLTYPE CommandQueue::Signal(ID3D12Fence* fence, UINT64 value)
{
    if (!fence) return E_INVALIDARG;
    Fence* f = Own<Fence>(fence);
    if (!f->ClaimSignal(value)) {   // already at (or asked for) this value: nothing changes
        static std::atomic<int> logged{0};
        if (logged++ < 20) Log("queue Signal skipped: fence %p value %llu (completed %llu)", static_cast<void*>(f), value, f->GetCompletedValue());
        return S_OK;
    }
    return Submit({}, f->semaphore(), value);
}

HRESULT STDMETHODCALLTYPE CommandQueue::Wait(ID3D12Fence* fence, UINT64 value)
{
    if (!fence) return E_INVALIDARG;
    std::lock_guard lock(pendingMutex_);
    pendingWaits_.push_back({Own<Fence>(fence)->semaphore(), value});
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueue::GetTimestampFrequency(UINT64* frequency)
{
    if (!frequency) return E_INVALIDARG;
    const float period = device_->properties().limits.timestampPeriod;
    if (period <= 0) return E_FAIL;
    *frequency = UINT64(1e9 / double(period) + 0.5);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueue::GetClockCalibration(UINT64* gpu, UINT64* cpu)
{
    if (!gpu || !cpu) return E_INVALIDARG;
    const auto& vk = device_->vk();
    auto calibrate = vk.vkGetCalibratedTimestampsKHR ? vk.vkGetCalibratedTimestampsKHR : vk.vkGetCalibratedTimestampsEXT;
    if (!calibrate) XVK_UNIMPLEMENTED_HR("GetClockCalibration without calibrated timestamps");
    VkCalibratedTimestampInfoKHR infos[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR},
                                             {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR}};
    uint64_t values[2]{}, deviation = 0;
    if (calibrate(device_->vkDevice(), 2, infos, values, &deviation) != VK_SUCCESS) return E_FAIL;
    *gpu = values[0];
    *cpu = values[1];
    return S_OK;
}

} // namespace xvk
