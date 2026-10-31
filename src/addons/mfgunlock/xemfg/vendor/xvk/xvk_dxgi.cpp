// DXGI on Vulkan: a flip-model swap chain that presents through a VkSwapchainKHR, and the factory XeFG
// reaches through IDXGISwapChain::GetParent (it recreates its own chain on the same window there).
#include "xvk_internal.h"
#include "xvk_output_frame.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <intrin.h>
#include <chrono>
#include <shared_mutex>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace xvk {

namespace {

struct Surface
{
    Device* device = nullptr;
    HWND window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool owned = true;                          // false: the game's surface, registered by the host
    VkSwapchainKHR current = VK_NULL_HANDLE;   // the live swap chain on this surface (retired ones are not listed)
    bool broken = false;                        // a layer chain's VkSwapchainKHR could not be created here
    VkFullScreenExclusiveEXT exclusive = VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;   // vkswapext.1: the game's mode
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;                // mailbox.1: the game's present mode
    VkSwapchainKHR latencyChain = VK_NULL_HANDLE;   // vkreflex.1: the live chain with low latency mode (gLatencyLock)
    std::atomic<uint64_t> frameBase{0};             // vkreflex.1: the host's id of XeFG's frame 0 (SetFrameBase)
    ~Surface()
    {
        if (surface && owned) device->vk().vkDestroySurfaceKHR(device->instance(), surface, nullptr);
    }
};

std::mutex gSurfaceMutex;
std::atomic<uint64_t> gPresents{0};
std::map<HWND, std::weak_ptr<Surface>> gSurfaces;
std::map<HWND, std::shared_ptr<Surface>> gRegistered;   // the game's surfaces (gSurfaceMutex)

// vkreflex.1: Reflex on the layer's swap chains. A surface's latencyChain is published and withdrawn under gLatencyLock
// held exclusively; Reflex calls on it (sleep mode, sleep, markers from the game's threads) hold it shared, so none
// reaches a destroyed chain. gReflexOn mirrors gReflex.on for the present thread's markers.
std::shared_mutex gLatencyLock;
ReflexMode gReflex;
std::atomic<bool> gReflexOn{false};
std::atomic<uint32_t> gMeteringBatch{0};   // MFGAdaUnlock 1.7: SetPresentMeteringBatch

void ApplySleepMode(Device* device, VkSwapchainKHR chain, const ReflexMode& mode)
{
    VkLatencySleepModeInfoNV info{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    info.lowLatencyMode = mode.lowLatency ? VK_TRUE : VK_FALSE;
    info.lowLatencyBoost = mode.boost ? VK_TRUE : VK_FALSE;
    info.minimumIntervalUs = mode.minimumIntervalUs;
    const VkResult result = device->vk().vkSetLatencySleepModeNV(device->vkDevice(), chain, &info);
    if (result != VK_SUCCESS) Log("vkSetLatencySleepModeNV %d", int(result));
}

// The driver's own reports of the chain's last frames (it measures them for a low-latency chain only), as medians: from
// the frame's simulation start to its GPU end, the GPU's render time and the OS render queue's time. Caller holds
// gLatencyLock shared.
void LogLatencyTimings(Device* device, VkSwapchainKHR chain)
{
    if (!device->vk().vkGetLatencyTimingsNV) return;
    std::array<VkLatencyTimingsFrameReportNV, 64> reports{};
    for (auto& report : reports) report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    VkGetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV, nullptr, uint32_t(reports.size()), reports.data()};
    device->vk().vkGetLatencyTimingsNV(device->vkDevice(), chain, &info);
    std::vector<double> total, gpu, queue;
    for (uint32_t i = 0; i < info.timingCount && i < reports.size(); ++i) {
        const auto& r = reports[i];
        if (r.simStartTimeUs && r.gpuRenderEndTimeUs > r.simStartTimeUs) total.push_back(double(r.gpuRenderEndTimeUs - r.simStartTimeUs) / 1000.0);
        if (r.gpuRenderStartTimeUs && r.gpuRenderEndTimeUs > r.gpuRenderStartTimeUs) gpu.push_back(double(r.gpuRenderEndTimeUs - r.gpuRenderStartTimeUs) / 1000.0);
        if (r.osRenderQueueStartTimeUs && r.osRenderQueueEndTimeUs > r.osRenderQueueStartTimeUs)
            queue.push_back(double(r.osRenderQueueEndTimeUs - r.osRenderQueueStartTimeUs) / 1000.0);
    }
    auto median = [](std::vector<double>& v) {
        if (v.empty()) return -1.0;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    Log("Reflex frame reports %u (last present id %llu): simulation start to GPU end %.2f ms, GPU %.2f ms, render queue %.2f ms (medians)",
        info.timingCount, info.timingCount ? reports[std::min<uint32_t>(info.timingCount, uint32_t(reports.size())) - 1].presentID : 0ull,
        median(total), median(gpu), median(queue));
}

// The output frame the calling thread presents (OutputFrameScope around XeFG's internal present).
thread_local uint64_t tOutputFrame = 0;
thread_local bool tOutputGenerated = false;

std::shared_ptr<Surface> SurfaceFor(Device* device, HWND window)
{
    std::lock_guard lock(gSurfaceMutex);
    if (auto registered = gRegistered.find(window); registered != gRegistered.end() && registered->second->device == device)
        return registered->second;
    if (auto existing = gSurfaces[window].lock()) return existing;
    if (!device->vk().vkCreateWin32SurfaceKHR) { Log("VK_KHR_win32_surface is not enabled on the instance"); return nullptr; }
    auto surface = std::make_shared<Surface>();
    surface->device = device;
    surface->window = window;
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, GetModuleHandleW(nullptr), window};
    if (device->vk().vkCreateWin32SurfaceKHR(device->instance(), &info, nullptr, &surface->surface) != VK_SUCCESS) return nullptr;
    gSurfaces[window] = surface;
    return surface;
}

// Real DXGI output that contains the window (XeFG reads its display modes for pacing).
HRESULT ContainingOutput(HWND window, IDXGIOutput** out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return DXGI_ERROR_NOT_FOUND;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) { *out = output.Detach(); return S_OK; }
        }
    }
    return DXGI_ERROR_NOT_FOUND;
}

struct SurfaceChoice { VkFormat format; VkColorSpaceKHR colorSpace; };

std::vector<SurfaceChoice> Candidates(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE space)
{
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        if (space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
            return {{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}};
        return {{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        if (space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            return {{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT}};
        return {{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    case DXGI_FORMAT_B8G8R8A8_UNORM:   // the same channel order keeps the present-thread copy a plain copy, not a blit
        return {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    default:
        return {{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    }
}

// stallguard.1 (Enshrouded on an RX 6900 XT, 22 Oct: presentation stopped at the first generated frame, the process kept
// running and the log said nothing): the present thread's waits run in kStallStepNs steps. One that passes a step is
// logged; one that passes kStallLimitMs while the window is not minimised returns `stalled`, and the caller marks the
// surface broken (its frames are dropped, the host gives the window back to the game for the rest of the session).
constexpr uint64_t kStallStepNs = 1'000'000'000ull;
constexpr uint32_t kStallLimitMs = 10000;
template <class Wait>
VkResult BoundedWait(const char* what, HWND window, Wait wait, bool& stalled)
{
    stalled = false;
    uint32_t waited = 0;
    for (;;) {
        const VkResult result = wait(kStallStepNs);
        if (result != VK_TIMEOUT && result != VK_NOT_READY) {
            if (waited) Log("present thread: %s done after more than %u ms (result %d)", what, waited, int(result));
            return result;
        }
        if (window && IsIconic(window)) continue;   // a minimised window may keep the presentation engine's images
        waited += uint32_t(kStallStepNs / 1'000'000ull);
        if (waited == uint32_t(kStallStepNs / 1'000'000ull)) Log("present thread: %s still waiting after %u ms", what, waited);
        if (waited >= kStallLimitMs) {
            stalled = true;
            Log("present thread: %s stalled for %u ms, the window goes back to the game", what, waited);
            return VK_TIMEOUT;
        }
    }
}

} // namespace

class Factory;

class SwapChain final : public IDXGISwapChain4
{
public:
    SwapChain(Device* device, CommandQueue* queue, Factory* factory, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen);
    ~SwapChain();
    HRESULT Init();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsIid<IUnknown, IDXGIObject, IDXGIDeviceSubObject, IDXGISwapChain, IDXGISwapChain1, IDXGISwapChain2, IDXGISwapChain3, IDXGISwapChain4>(riid)) {
            AddRef();
            *out = static_cast<IDXGISwapChain4*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override { return private_.Set(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override { return private_.SetInterface(guid, object); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override { return private_.Get(guid, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override { return queue_->QueryInterface(riid, device); }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override { return Present1(sync, flags, nullptr); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput*) override
    {
        if (fullscreen) XVK_UNIMPLEMENTED_HR("SwapChain::SetFullscreenState(TRUE)");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override
    {
        if (fullscreen) *fullscreen = FALSE;
        if (target) *target = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return output ? ContainingOutput(window_, output) : E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return DXGI_ERROR_FRAME_STATISTICS_DISJOINT; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override
    {
        if (!count) return E_INVALIDARG;
        *count = UINT(presentCount_);
        return S_OK;
    }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override
    {
        if (!desc) return E_INVALIDARG;
        *desc = desc_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override
    {
        if (!desc) return E_INVALIDARG;
        *desc = fullscreen_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* window) override
    {
        if (!window) return E_INVALIDARG;
        *window = window_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void**) override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override
    {
        if (output) *output = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override
    {
        if (color) *color = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return rotation == DXGI_MODE_ROTATION_IDENTITY ? S_OK : DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override
    {
        if (rotation) *rotation = DXGI_MODE_ROTATION_IDENTITY;
        return S_OK;
    }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT, UINT) override { XVK_UNIMPLEMENTED_HR("SwapChain::SetSourceSize"); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override
    {
        if (width) *width = desc_.Width;
        if (height) *height = desc_.Height;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override
    {
        if (!latency) return E_INVALIDARG;
        *latency = maxLatency_;
        return S_OK;
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*) override { XVK_UNIMPLEMENTED_HR("SwapChain::SetMatrixTransform"); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F*) override { return DXGI_ERROR_INVALID_CALL; }
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return current_; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* support) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT*, IUnknown* const*) override
    {
        return ResizeBuffers(count, width, height, format, flags);
    }
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) override;

private:
    struct Frame
    {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };
    HRESULT CreateSwapchain(VkSwapchainKHR old);
    HRESULT CreateFirstSwapchain();
    HRESULT CreateBackbuffers();
    void DestroySwapchain();
    void WaitIdle();

    std::atomic<ULONG> refs_{1};
    PrivateData private_;
    Device* device_;
    CommandQueue* queue_;
    Factory* factory_;
    HWND window_;
    DXGI_SWAP_CHAIN_DESC1 desc_;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_{};
    DXGI_COLOR_SPACE_TYPE colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::shared_ptr<Surface> surface_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> renderDone_;
    std::vector<Frame> frames_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;   // presentCount_ after each present's copy
    std::vector<Resource*> backbuffers_;
    std::mutex presentMutex_;
    UINT current_ = 0;
    uint64_t presentCount_ = 0;
    UINT maxLatency_ = 1;
    HANDLE latency_ = nullptr;
    bool hasHdr_ = false;
    VkHdrMetadataEXT hdr_{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
#if MFG_UNLOCK_XVK_CAPTURE
    // Diagnostic builds only: XVK_CAPTURE=<dir> (+ XVK_CAPTURE_FIRST/COUNT) writes presented frames (4-byte formats)
    // as raw present-<n>.bin.
    std::wstring captureDir_;
    uint64_t captureFirst_ = 1, captureCount_ = 0;
    VkBuffer captureBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory captureMemory_ = VK_NULL_HANDLE;
    uint8_t* captureMapped_ = nullptr;
#endif
    // CPU time spent in Present1 steps (QPC ticks): fence wait, acquire, record+submit, enqueue, total; and
    // vkQueuePresentKHR on the present thread.
    std::array<int64_t, 6> presentTicks_{};
    uint64_t timedPresents_ = 0;
    // Flip-discard emulation. Present1 copies the backbuffer into a staging ring (XeFG's queue) and releases the
    // frame-latency waitable when that copy completes; the present thread owns the VkSwapchainKHR and presents
    // the newest staged frame, dropping older ones as DXGI flip-discard does when the display falls behind.
    // (vkQueuePresentKHR can block 0.5-0.9 ms per call windowed on 617.42, and tying XeFG's waitable to the
    // presentation engine starved it into XeFG's 1 s timeout under load, 19-20 Oct.)
    // staging slot, timeline_ value of its copy, and whether it was presented with a sync interval (V-Sync: shown in
    // order, never dropped) or without (flip-discard: the newest wins); vkreflex.1: the host's frame it belongs to (0:
    // unknown) and whether it is a generated one.
    struct QueuedPresent { uint32_t staged; uint64_t value; bool sync; uint64_t frame; bool generated; };
    static constexpr uint32_t kStaged = 3;
    std::vector<Resource*> staged_;
    bool recreating_ = false;      // the present thread is remaking the chain: Present1 drops its frame (queueMutex_)
    uint32_t present1Active_ = 0;  // Present1 calls past that check (queueMutex_)
    std::array<bool, kStaged> stagedBusy_{};       // the present thread is copying out of it (queueMutex_)
    std::array<uint64_t, kStaged> stagedRead_{};   // presentTimeline_ value after which it may be rewritten (queueMutex_)
    std::vector<Frame> threadFrames_;               // present-thread ring (its own pool: pools are per thread)
    VkCommandPool threadPool_ = VK_NULL_HANDLE;
    VkSemaphore presentTimeline_ = VK_NULL_HANDLE;
    uint64_t presentTimelineValue_ = 0;
    uint64_t dropped_ = 0;
    void PresentThread();
    void WaitThreadIdle();
    void DrainPresents();
    std::thread presenter_;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<QueuedPresent> queued_;
    bool presenting_ = false;
    bool stopPresenter_ = false;
    std::atomic<bool> outOfDate_{false};
    bool noArea_ = false;   // the window has no area, so no chain is made for it
    bool WindowHasNoArea();
    bool broken_ = false;                      // present thread: the first VkSwapchainKHR failed; frames are dropped
    VkSwapchainKHR owned_ = VK_NULL_HANDLE;   // swapchain_ as published to the surface (gSurfaceMutex): the present
                                              // thread can recreate swapchain_ while Present1 checks for retirement
    // vkreflex.1: swapchain_ has low latency mode (Reflex), and the last present id it was given (ids only increase);
    // the present the present thread took last, and whether it already remade the chain for low latency mode.
    static constexpr uint64_t kLatencyAfterPresents = 240;
    bool latencyMode_ = false;
    uint64_t lastPresentId_ = 0;
    uint32_t meteringFrame_ = 0;   // MFGAdaUnlock 1.7: the base frame of the batch metering last opened
    uint32_t meteringSent_ = 0;    // and the batch size the driver was last given (1: metering off)
    uint32_t loggedFrames_ = 0;
    std::atomic<uint64_t> shownPresents_{0};
    bool latencyUpgraded_ = false;
    uint64_t frameBase_ = 0;   // the surface's frameBase when this chain was made
};

class Factory final : public IDXGIFactory4
{
public:
    explicit Factory(Device* device) : device_(device)
    {
        device_->AddRef();
        CreateDXGIFactory2(0, IID_PPV_ARGS(&real_));
    }
    ~Factory() { device_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsIid<IUnknown, IDXGIObject, IDXGIFactory, IDXGIFactory1, IDXGIFactory2, IDXGIFactory3, IDXGIFactory4>(riid)) {
            AddRef();
            *out = static_cast<IDXGIFactory4*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override { return private_.Set(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override { return private_.SetInterface(guid, object); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override { return private_.Get(guid, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** adapter) override { return real_->EnumAdapters(index, adapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override { return real_->MakeWindowAssociation(window, flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* window) override { return real_->GetWindowAssociation(window); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**) override { XVK_UNIMPLEMENTED_HR("Factory::CreateSwapChain"); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter**) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override { return real_->EnumAdapters1(index, adapter); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return TRUE; }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput*, IDXGISwapChain1** out) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**) override
    { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE, LUID*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE, DWORD*) override { return DXGI_ERROR_UNSUPPORTED; }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE, DWORD*) override { return DXGI_ERROR_UNSUPPORTED; }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**) override
    { return DXGI_ERROR_UNSUPPORTED; }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return 0; }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) override { return real_->EnumAdapterByLuid(luid, riid, adapter); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID, void**) override { return DXGI_ERROR_UNSUPPORTED; }

private:
    std::atomic<ULONG> refs_{1};
    PrivateData private_;
    Device* device_;
    ComPtr<IDXGIFactory4> real_;
};

// ---------------------------------------------------------------- SwapChain
SwapChain::SwapChain(Device* device, CommandQueue* queue, Factory* factory, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen)
    : device_(device), queue_(queue), factory_(factory), window_(window), desc_(desc)
{
    device_->AddRef();
    queue_->AddRef();
    factory_->AddRef();
    fullscreen_.Windowed = TRUE;
    if (fullscreen) fullscreen_ = *fullscreen;
    if (!desc_.Width || !desc_.Height) {
        RECT rect{};
        GetClientRect(window_, &rect);
        if (!desc_.Width) desc_.Width = UINT(std::max<LONG>(1, rect.right - rect.left));
        if (!desc_.Height) desc_.Height = UINT(std::max<LONG>(1, rect.bottom - rect.top));
    }
}

SwapChain::~SwapChain()
{
    if (timedPresents_) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        auto us = [&](int i) { return double(presentTicks_[i]) * 1e6 / double(frequency.QuadPart) / double(timedPresents_); };
        Log("Present1 CPU us per call over %llu presents: fence wait %.1f, staging wait %.1f, record+submit %.1f, enqueue %.1f, total %.1f; "
            "present thread vkQueuePresentKHR %.1f; frames dropped (flip-discard) %llu", timedPresents_, us(0), us(1), us(2), us(3), us(4),
            us(5), dropped_);
    }
    {
        std::lock_guard lock(queueMutex_);
        stopPresenter_ = true;
    }
    queueChanged_.notify_all();
    if (presenter_.joinable()) presenter_.join();
    WaitIdle();
    {
        // Presentation still waits on renderDone_ and holds the images: drain the present queue before the chain goes.
        QueueSlot* slot = device_->PresentSlot();
        std::lock_guard lock(slot->mutex);
        device_->vk().vkQueueWaitIdle(slot->queue);
    }
    for (Resource* r : backbuffers_) r->Release();
    for (Resource* r : staged_) r->Release();
    {
        // Before the handle goes: a later chain on this window must not name a destroyed swap chain as oldSwapchain
        // (XeFG's init retry after a failed first attempt did, and the driver faulted).
        std::lock_guard lock(gSurfaceMutex);
        if (surface_ && surface_->current == owned_) surface_->current = VK_NULL_HANDLE;
    }
    DestroySwapchain();
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    for (auto* ring : {&frames_, &threadFrames_})
        for (auto& f : *ring) {
            if (f.acquire) vk.vkDestroySemaphore(device, f.acquire, nullptr);
            if (f.fence) vk.vkDestroyFence(device, f.fence, nullptr);
        }
    if (pool_) vk.vkDestroyCommandPool(device, pool_, nullptr);
    if (threadPool_) vk.vkDestroyCommandPool(device, threadPool_, nullptr);
    if (timeline_) vk.vkDestroySemaphore(device, timeline_, nullptr);
    if (presentTimeline_) vk.vkDestroySemaphore(device, presentTimeline_, nullptr);
#if MFG_UNLOCK_XVK_CAPTURE
    if (captureBuffer_) vk.vkDestroyBuffer(device, captureBuffer_, nullptr);
    if (captureMemory_) vk.vkFreeMemory(device, captureMemory_, nullptr);
#endif
    if (latency_) CloseHandle(latency_);
    surface_.reset();
    factory_->Release();
    queue_->Release();
    device_->Release();
}

HRESULT SwapChain::Init()
{
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    if (!vk.vkCreateSwapchainKHR) { Log("VK_KHR_swapchain is not enabled on the device"); return DXGI_ERROR_UNSUPPORTED; }
    if (desc_.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc_.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) return DXGI_ERROR_INVALID_CALL;
    surface_ = SurfaceFor(device_, window_);
    if (!surface_) return E_FAIL;
    frameBase_ = surface_->frameBase.load();
    VkBool32 supported = VK_FALSE;
    vk.vkGetPhysicalDeviceSurfaceSupportKHR(device_->physical(), queue_->slot()->family, surface_->surface, &supported);
    if (!supported) { Log("queue family cannot present to this window"); return DXGI_ERROR_UNSUPPORTED; }
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, device_->queueFamily()};
    if (vk.vkCreateCommandPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) return E_OUTOFMEMORY;
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
    if (vk.vkCreateSemaphore(device, &semaphore, nullptr, &timeline_) != VK_SUCCESS ||
        vk.vkCreateSemaphore(device, &semaphore, nullptr, &presentTimeline_) != VK_SUCCESS ||
        vk.vkCreateCommandPool(device, &pool, nullptr, &threadPool_) != VK_SUCCESS) return E_OUTOFMEMORY;
#if MFG_UNLOCK_XVK_CAPTURE
    {
        wchar_t text[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"XVK_CAPTURE", text, MAX_PATH) && Format(desc_.Format).bytes == 4) {
            captureDir_ = text;
            captureCount_ = 64;
            if (GetEnvironmentVariableW(L"XVK_CAPTURE_FIRST", text, MAX_PATH)) captureFirst_ = _wtoi64(text);
            if (GetEnvironmentVariableW(L"XVK_CAPTURE_COUNT", text, MAX_PATH)) captureCount_ = _wtoi64(text);
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, VkDeviceSize(desc_.Width) * desc_.Height * 4,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
            VkMemoryRequirements requirements{};
            uint32_t type = 0;
            void* host = nullptr;
            if (vk.vkCreateBuffer(device, &info, nullptr, &captureBuffer_) != VK_SUCCESS) return E_OUTOFMEMORY;
            vk.vkGetBufferMemoryRequirements(device, captureBuffer_, &requirements);
            if (!device_->FindMemoryType(requirements.memoryTypeBits, D3D12_HEAP_TYPE_READBACK, type)) return E_OUTOFMEMORY;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
            if (vk.vkAllocateMemory(device, &allocate, nullptr, &captureMemory_) != VK_SUCCESS ||
                vk.vkBindBufferMemory(device, captureBuffer_, captureMemory_, 0) != VK_SUCCESS ||
                vk.vkMapMemory(device, captureMemory_, 0, VK_WHOLE_SIZE, 0, &host) != VK_SUCCESS) return E_OUTOFMEMORY;
            captureMapped_ = static_cast<uint8_t*>(host);
            Log("capturing presents %llu..%llu (%ux%u, DXGI format %d)", captureFirst_, captureFirst_ + captureCount_ - 1, desc_.Width,
                desc_.Height, int(desc_.Format));
        }
    }
#endif
    // The VkSwapchainKHR itself comes with the first present (CreateFirstSwapchain): XeFG creates its chain early
    // in its init and can still fail after it, and a chain created here would already have retired the game's.
    UINT support = 0;
    CheckColorSpaceSupport(colorSpace_, &support);
    if (!support) { Log("no surface format for DXGI format %d", int(desc_.Format)); return DXGI_ERROR_UNSUPPORTED; }
    frames_.resize(4);
    for (auto& f : frames_) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkSemaphoreCreateInfo binary{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        if (vk.vkAllocateCommandBuffers(device, &allocate, &f.command) != VK_SUCCESS ||
            vk.vkCreateSemaphore(device, &binary, nullptr, &f.acquire) != VK_SUCCESS ||
            vk.vkCreateFence(device, &fence, nullptr, &f.fence) != VK_SUCCESS) return E_OUTOFMEMORY;
    }
    HRESULT hr = CreateBackbuffers();
    if (FAILED(hr)) return hr;
    presenter_ = std::thread([this] { PresentThread(); });
    return S_OK;
}

// Present thread, first frame: the swap chain on the window, retiring the live layer chain there or the game's own.
HRESULT SwapChain::CreateFirstSwapchain()
{
    if (WindowHasNoArea()) return S_FALSE;   // before the game's chain is claimed for retiring
    VkSwapchainKHR old = VK_NULL_HANDLE;
    {
        std::lock_guard lock(gSurfaceMutex);
        old = surface_->current;
    }
    const VkSwapchainKHR game = old ? VK_NULL_HANDLE : device_->ClaimRetireHint(window_);
    const HRESULT hr = CreateSwapchain(old ? old : game);
    if (game) device_->FinishRetire(window_);
    if (FAILED(hr)) {
        broken_ = true;
        std::lock_guard lock(gSurfaceMutex);
        surface_->broken = true;
    }
    return hr;
}

void SwapChain::PresentThread()
{
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    QueueSlot* slot = device_->PresentSlot();
    uint64_t frameIndex = 0;
    while (true) {
        QueuedPresent item{};
        {
            std::unique_lock lock(queueMutex_);
            queueChanged_.wait(lock, [&] { return stopPresenter_ || !queued_.empty(); });
            if (queued_.empty()) return;
            if (queued_.front().sync) {
                item = queued_.front();   // V-Sync: every frame, in order
                queued_.pop_front();
            } else {
                item = queued_.back();    // newest wins (flip-discard)
                dropped_ += queued_.size() - 1;
                queued_.clear();
            }
            stagedBusy_[item.staged] = true;
            presenting_ = true;
        }
        shownPresents_ = item.value;
        if (broken_ || (!swapchain_ && (FAILED(CreateFirstSwapchain()) || !swapchain_))) {
            // No swap chain on the window: the frame is dropped (the host sees the surface broken and gives the
            // window back to the game).
            {
                std::lock_guard lock(queueMutex_);
                stagedBusy_[item.staged] = false;
                ++dropped_;
                presenting_ = false;
            }
            queueChanged_.notify_all();
            continue;
        }
        Frame& frame = threadFrames_[frameIndex++ % threadFrames_.size()];
        bool stalled = false;
        uint64_t readValue = 0;
        // MFGAdaUnlock 1.7.1: remaking the chain frees its images in the driver and holds a driver lock meanwhile, and that
        // can wait for GPU work that needs XeFG to go on (alt-tab froze Indiana Jones here). So Present1 neither waits for
        // this slot nor enters the driver until the chain is made: running calls finish first, later ones drop their frame.
        auto beginRecreate = [&] {
            std::unique_lock lock(queueMutex_);
            if (readValue) stagedRead_[item.staged] = readValue;
            stagedBusy_[item.staged] = false;
            recreating_ = true;
            queueChanged_.notify_all();
            queueChanged_.wait(lock, [&] { return !present1Active_; });
        };
        auto endRecreate = [&] {
            std::lock_guard lock(queueMutex_);
            recreating_ = false;
        };
        BoundedWait("copy fence", window_, [&](uint64_t ns) { return vk.vkWaitForFences(device, 1, &frame.fence, VK_TRUE, ns); }, stalled);
        uint32_t image = 0;
        auto acquire = [&](uint64_t ns) { return vk.vkAcquireNextImageKHR(device, swapchain_, ns, frame.acquire, VK_NULL_HANDLE, &image); };
        VkResult result = stalled ? VK_TIMEOUT : BoundedWait("vkAcquireNextImageKHR", window_, acquire, stalled);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // The slot may be rewritten from here on, so this frame is dropped; the next one acquires on the new chain.
            beginRecreate();
            WaitThreadIdle();
            CreateSwapchain(swapchain_);
            endRecreate();
        }
        if (stalled) {
            // stallguard.1: the frame is dropped and so is every later one (broken_), as when no swap chain could be made.
            broken_ = true;
            {
                std::lock_guard lock(gSurfaceMutex);
                surface_->broken = true;
            }
            {
                std::lock_guard lock(queueMutex_);
                stagedBusy_[item.staged] = false;
                ++dropped_;
                presenting_ = false;
            }
            queueChanged_.notify_all();
            continue;
        }
        LARGE_INTEGER start{}, end{};
        QueryPerformanceCounter(&start);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            vk.vkResetFences(device, 1, &frame.fence);
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vk.vkBeginCommandBuffer(frame.command, &begin);
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images_[image];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vk.vkCmdPipelineBarrier2(frame.command, &dependency);
            Resource* source = staged_[item.staged];
            const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            const uint32_t width = std::min(extent_.width, desc_.Width), height = std::min(extent_.height, desc_.Height);
            if (source->imageFormat() == format_) {
                VkImageCopy region{layers, {}, layers, {}, {width, height, 1}};
                vk.vkCmdCopyImage(frame.command, source->image(), VK_IMAGE_LAYOUT_GENERAL, images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &region);
            } else {
                VkImageBlit blit{layers, {{0, 0, 0}, {int32_t(width), int32_t(height), 1}}, layers,
                                 {{0, 0, 0}, {int32_t(width), int32_t(height), 1}}};
                vk.vkCmdBlitImage(frame.command, source->image(), VK_IMAGE_LAYOUT_GENERAL, images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &blit, VK_FILTER_NEAREST);
            }
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vk.vkCmdPipelineBarrier2(frame.command, &dependency);
            vk.vkEndCommandBuffer(frame.command);

            VkSemaphore init = VK_NULL_HANDLE;
            uint64_t initValue = 0;
            device_->InitWait(init, initValue);
            const VkSemaphoreSubmitInfo waits[3] = {
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, frame.acquire, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT},
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, item.value, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT},
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, init, initValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
            readValue = ++presentTimelineValue_;
            VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, frame.command};
            std::lock_guard queueLock(slot->mutex);
            const VkSemaphoreSubmitInfo signals[3] = {
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, renderDone_[image], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, presentTimeline_, readValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                device_->Progress(*slot)};
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit.waitSemaphoreInfoCount = initValue ? 3 : 2;
            submit.pWaitSemaphoreInfos = waits;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &command;
            submit.signalSemaphoreInfoCount = 3;
            submit.pSignalSemaphoreInfos = signals;
            if (vk.vkQueueSubmit2(slot->queue, 1, &submit, frame.fence) != VK_SUCCESS) {
                device_->Fault("present thread submit");
            } else {
                VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores = &renderDone_[image];
                present.swapchainCount = 1;
                present.pSwapchains = &swapchain_;
                present.pImageIndices = &image;
                // vkreflex.1: Reflex counts every output as an out-of-band present of its batch's frame (the game's
                // own present markers come from the host); the real frame also names its present by the frame's id.
                // The present thread owns swapchain_, so these need no gLatencyLock.
                const bool marked = latencyMode_ && item.frame && gReflexOn.load(std::memory_order_relaxed);
                uint64_t presentId = 0;
                VkPresentIdKHR ids{VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr, 1, &presentId};
                if (marked && !item.generated && device_->presentIds() && item.frame > lastPresentId_) {
                    presentId = lastPresentId_ = item.frame;
                    present.pNext = &ids;
                }
                // MFGAdaUnlock 1.7: the driver's present metering. A batch is the outputs of one base frame (the low 32 bits
                // of XeFG's frame id are the host's frame); its first present names the batch size, and a batch of 1
                // turns the metering off once generation stops.
                VkSetPresentConfigNV metering{VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV, nullptr, 0, 0};
                if (device_->presentMetering() && item.frame) {
                    const uint32_t batch = gMeteringBatch.load(std::memory_order_relaxed);
                    const uint32_t base = uint32_t(item.frame);
                    const uint32_t wanted = batch >= 2 ? batch : 1;
                    if (base != meteringFrame_ && (wanted >= 2 || meteringSent_ >= 2)) {
                        metering.numFramesPerBatch = wanted;
                        metering.pNext = present.pNext;
                        present.pNext = &metering;
                        if (wanted != meteringSent_) Log("present metering: %u frames per batch", wanted);
                        meteringSent_ = wanted;
                    }
                    meteringFrame_ = base;
                }
                VkSetLatencyMarkerInfoNV marker{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, nullptr, item.frame,
                                                VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV};
                if (marked) vk.vkSetLatencyMarkerNV(device, swapchain_, &marker);
                result = vk.vkQueuePresentKHR(slot->queue, &present);
                if (marked) {
                    marker.marker = VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV;
                    vk.vkSetLatencyMarkerNV(device, swapchain_, &marker);
                }
                if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ++gPresents;
                else if (result == VK_ERROR_OUT_OF_DATE_KHR) outOfDate_ = true;
                else Log("vkQueuePresentKHR %d", int(result));
            }
        } else {
            if (!noArea_) Log("vkAcquireNextImageKHR %d (frame dropped)", int(result));
        }
        if (outOfDate_.exchange(false)) {
            // Recreate before clearing presenting_, so ResizeBuffers (which drains first) never races it.
            beginRecreate();
            WaitThreadIdle();
            CreateSwapchain(swapchain_);
            endRecreate();
        } else if (swapchain_ && !latencyMode_ && !latencyUpgraded_ && gReflexOn.load(std::memory_order_relaxed) &&
                   device_->lowLatency2() && item.value >= kLatencyAfterPresents) {
            // vkreflex.1: a chain made before Reflex was on, or in its first presents, takes low latency mode now (once).
            latencyUpgraded_ = true;
            Log("Reflex: the swap chain takes low latency mode after %llu presents", item.value);
            beginRecreate();
            WaitThreadIdle();
            CreateSwapchain(swapchain_);
            endRecreate();
        }
        QueryPerformanceCounter(&end);
        {
            std::lock_guard lock(queueMutex_);
            if (readValue) stagedRead_[item.staged] = readValue;
            stagedBusy_[item.staged] = false;
            presentTicks_[5] += end.QuadPart - start.QuadPart;
            presenting_ = false;
        }
        queueChanged_.notify_all();
    }
}

void SwapChain::DrainPresents()
{
    std::unique_lock lock(queueMutex_);
    queueChanged_.wait(lock, [&] { return queued_.empty() && !presenting_; });
}

// MFGAdaUnlock 1.7.1: a minimized window has a 0x0 extent, which a swap chain may not have, and every chain made is a
// chance of the driver hang in beginRecreate. The old chain stays and frames are dropped until the window has area.
bool SwapChain::WindowHasNoArea()
{
    VkSurfaceCapabilitiesKHR caps{};
    device_->vk().vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), surface_->surface, &caps);
    noArea_ = !caps.currentExtent.width || !caps.currentExtent.height;
    return noArea_;
}

HRESULT SwapChain::CreateSwapchain(VkSwapchainKHR old)
{
    if (WindowHasNoArea()) return S_FALSE;
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    const VkPhysicalDevice physical = device_->physical();
    VkSurfaceCapabilitiesKHR caps{};
    vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_->surface, &caps);
    uint32_t count = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_->surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_->surface, &count, formats.data());
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED};
    for (const auto& c : Candidates(desc_.Format, colorSpace_)) {
        for (const auto& f : formats)
            if (f.format == c.format && f.colorSpace == c.colorSpace) { chosen = f; break; }
        if (chosen.format != VK_FORMAT_UNDEFINED) break;
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) { Log("no surface format for DXGI format %d", int(desc_.Format)); return DXGI_ERROR_UNSUPPORTED; }
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_->surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_->surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    // XeFG presents with sync interval 0 and ALLOW_TEARING; tearing-capable chains take IMMEDIATE. mailbox.1: a game
    // that asked for MAILBOX (no tearing, no V-Sync wait: RDR2) keeps it, it got IMMEDIATE before (22 Oct).
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
        mode = surface_->presentMode == VK_PRESENT_MODE_MAILBOX_KHR && has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
             : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
             : (has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR);
    extent_ = {desc_.Width, desc_.Height};
    if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
    // Two images beyond the DXGI buffer count: XeFG waits on the frame-latency waitable before every present,
    // and we release it when the present copy completes, which waits for a free image. With only the DXGI count,
    // the presentation engine holding images starved the waitable into XeFG's 1 s timeout (19-20 Oct).
    uint32_t images = std::max(caps.minImageCount, desc_.BufferCount) + 2;
    if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_->surface;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    // vkswapext.1: the game's full-screen-exclusive mode on its registered surface (only set when its device enabled
    // VK_EXT_full_screen_exclusive).
    VkSurfaceFullScreenExclusiveInfoEXT exclusive{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    if (surface_->exclusive != VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT) {
        exclusive.fullScreenExclusive = surface_->exclusive;
        info.pNext = &exclusive;
    }
    // vkreflex.1: low latency mode while the host's output is Reflex and the device has VK_NV_low_latency2, from the
    // chain's kLatencyAfterPresents-th present on: a game recreates its swap chain several times a second while it starts
    // (No Man's Sky, 23 Oct: the driver faulted in dxgi.dll making the first low-latency chain in that churn), and the
    // present thread upgrades the chain made before then.
    VkSwapchainLatencyCreateInfoNV latency{VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV, info.pNext, VK_TRUE};
    const bool lowLatency = gReflexOn.load() && device_->lowLatency2() && shownPresents_.load() >= kLatencyAfterPresents;
    if (lowLatency) info.pNext = &latency;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult result = vk.vkCreateSwapchainKHR(device, &info, nullptr, &created);
    if (result != VK_SUCCESS) { Log("vkCreateSwapchainKHR %d", int(result)); return E_FAIL; }
    if (old == swapchain_) DestroySwapchain();
    swapchain_ = created;
    latencyMode_ = lowLatency;
    lastPresentId_ = 0;
    format_ = chosen.format;
    {
        std::lock_guard lock(gSurfaceMutex);
        surface_->current = swapchain_;
        owned_ = swapchain_;
    }
    {
        // The window's Reflex calls go to this chain from now on (none when it has no low latency mode).
        std::unique_lock lock(gLatencyLock);
        surface_->latencyChain = latencyMode_ ? swapchain_ : VK_NULL_HANDLE;
        if (latencyMode_) ApplySleepMode(device_, swapchain_, gReflex);
    }
    vk.vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr);
    images_.resize(count);
    vk.vkGetSwapchainImagesKHR(device, swapchain_, &count, images_.data());
    VkSemaphoreCreateInfo binary{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderDone_.resize(count);
    for (auto& s : renderDone_) vk.vkCreateSemaphore(device, &binary, nullptr, &s);
    if (threadFrames_.empty()) {
        threadFrames_.resize(count + 1);
        for (auto& f : threadFrames_) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, threadPool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            vk.vkAllocateCommandBuffers(device, &allocate, &f.command);
            vk.vkCreateSemaphore(device, &binary, nullptr, &f.acquire);
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
            vk.vkCreateFence(device, &fence, nullptr, &f.fence);
        }
    }
    Log("swap chain %ux%u: %u images, format %d, color space %d, present mode %d, low latency %d", extent_.width, extent_.height, count,
        int(format_), int(chosen.colorSpace), int(mode), int(latencyMode_));
    if (hasHdr_ && vk.vkSetHdrMetadataEXT) vk.vkSetHdrMetadataEXT(device, 1, &swapchain_, &hdr_);
    return S_OK;
}

void SwapChain::DestroySwapchain()
{
    const auto& vk = device_->vk();
    for (auto& s : renderDone_) vk.vkDestroySemaphore(device_->vkDevice(), s, nullptr);
    renderDone_.clear();
    if (swapchain_) {
        {
            // vkreflex.1: no Reflex call may still name it (they hold gLatencyLock shared).
            std::unique_lock lock(gLatencyLock);
            if (surface_ && surface_->latencyChain == swapchain_) surface_->latencyChain = VK_NULL_HANDLE;
        }
        vk.vkDestroySwapchainKHR(device_->vkDevice(), swapchain_, nullptr);
    }
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
}

HRESULT SwapChain::CreateBackbuffers()
{
    for (Resource* r : backbuffers_) r->Release();
    for (Resource* r : staged_) r->Release();
    backbuffers_.clear();
    staged_.clear();
    for (UINT i = 0; i < desc_.BufferCount + kStaged; ++i) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, desc_.Width, desc_.Height, 1, 1, desc_.Format, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* resource = nullptr;
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                           IID_PPV_ARGS(&resource));
        if (FAILED(hr)) return hr;
        (i < desc_.BufferCount ? backbuffers_ : staged_).push_back(Own<Resource>(resource));
    }
    stagedBusy_.fill(false);
    stagedRead_.fill(0);
    current_ = 0;
    return S_OK;
}

void SwapChain::WaitIdle()
{
    const auto& vk = device_->vk();
    for (auto& f : frames_)
        if (f.fence) vk.vkWaitForFences(device_->vkDevice(), 1, &f.fence, VK_TRUE, UINT64_MAX);
    WaitThreadIdle();
}

void SwapChain::WaitThreadIdle()
{
    const auto& vk = device_->vk();
    for (auto& f : threadFrames_)
        if (f.fence) vk.vkWaitForFences(device_->vkDevice(), 1, &f.fence, VK_TRUE, UINT64_MAX);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetParent(REFIID riid, void** parent) { return factory_->QueryInterface(riid, parent); }

HRESULT STDMETHODCALLTYPE SwapChain::GetBuffer(UINT index, REFIID riid, void** surface)
{
    if (index >= backbuffers_.size()) return DXGI_ERROR_INVALID_CALL;
    return backbuffers_[index]->QueryInterface(riid, surface);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* desc)
{
    if (!desc) return E_INVALIDARG;
    *desc = {};
    desc->BufferDesc = {desc_.Width, desc_.Height, fullscreen_.RefreshRate, desc_.Format, fullscreen_.ScanlineOrdering, fullscreen_.Scaling};
    desc->SampleDesc = desc_.SampleDesc;
    desc->BufferUsage = desc_.BufferUsage;
    desc->BufferCount = desc_.BufferCount;
    desc->OutputWindow = window_;
    desc->Windowed = fullscreen_.Windowed;
    desc->SwapEffect = desc_.SwapEffect;
    desc->Flags = desc_.Flags;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    std::lock_guard lock(presentMutex_);
    DrainPresents();
    WaitIdle();
    if (count) desc_.BufferCount = count;
    if (format != DXGI_FORMAT_UNKNOWN) desc_.Format = format;
    desc_.Flags = flags;
    RECT rect{};
    GetClientRect(window_, &rect);
    desc_.Width = width ? width : UINT(std::max<LONG>(1, rect.right - rect.left));
    desc_.Height = height ? height : UINT(std::max<LONG>(1, rect.bottom - rect.top));
    // Before the first present the new size waits for CreateFirstSwapchain.
    HRESULT hr = swapchain_ ? CreateSwapchain(swapchain_) : S_OK;
    if (FAILED(hr)) return hr;
    return CreateBackbuffers();
}

HRESULT STDMETHODCALLTYPE SwapChain::SetMaximumFrameLatency(UINT latency)
{
    if (!latency || latency > DXGI_MAX_SWAP_CHAIN_BUFFERS) return DXGI_ERROR_INVALID_CALL;
    if (latency_ && latency > maxLatency_) ReleaseSemaphore(latency_, LONG(latency - maxLatency_), nullptr);
    Log("SetMaximumFrameLatency(%u) (was %u, waitable %s)", latency, maxLatency_, latency_ ? "created" : "not created");
    maxLatency_ = latency;
    return S_OK;
}

HANDLE STDMETHODCALLTYPE SwapChain::GetFrameLatencyWaitableObject()
{
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) return nullptr;
    if (!latency_) latency_ = CreateSemaphoreW(nullptr, LONG(maxLatency_), DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
    Log("GetFrameLatencyWaitableObject (initial count %u) from %p", maxLatency_, _ReturnAddress());
    HANDLE copy = nullptr;
    DuplicateHandle(GetCurrentProcess(), latency_, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    Log("GetFrameLatencyWaitableObject returns %p (semaphore %p)", copy, latency_);
    return copy;
}

HRESULT STDMETHODCALLTYPE SwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* support)
{
    if (!support) return E_INVALIDARG;
    *support = 0;
    uint32_t count = 0;
    const auto& vk = device_->vk();
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical(), surface_->surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical(), surface_->surface, &count, formats.data());
    for (const auto& c : Candidates(desc_.Format, space))
        for (const auto& f : formats)
            if (f.format == c.format && f.colorSpace == c.colorSpace) *support = DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE space)
{
    UINT support = 0;
    CheckColorSpaceSupport(space, &support);
    if (!support) return DXGI_ERROR_UNSUPPORTED;
    if (space == colorSpace_) return S_OK;
    std::lock_guard lock(presentMutex_);
    DrainPresents();
    WaitIdle();
    colorSpace_ = space;
    return swapchain_ ? CreateSwapchain(swapchain_) : S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* data)
{
    if (type == DXGI_HDR_METADATA_TYPE_NONE) return S_OK;
    if (type != DXGI_HDR_METADATA_TYPE_HDR10 || size != sizeof(DXGI_HDR_METADATA_HDR10) || !data) return DXGI_ERROR_INVALID_CALL;
    const auto& source = *static_cast<const DXGI_HDR_METADATA_HDR10*>(data);
    auto point = [](const UINT16 xy[2]) { return VkXYColorEXT{float(xy[0]) / 50000.0f, float(xy[1]) / 50000.0f}; };
    std::lock_guard lock(presentMutex_);
    hdr_ = {VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    hdr_.displayPrimaryRed = point(source.RedPrimary);
    hdr_.displayPrimaryGreen = point(source.GreenPrimary);
    hdr_.displayPrimaryBlue = point(source.BluePrimary);
    hdr_.whitePoint = point(source.WhitePoint);
    hdr_.maxLuminance = float(source.MaxMasteringLuminance);
    hdr_.minLuminance = float(source.MinMasteringLuminance) / 10000.0f;
    hdr_.maxContentLightLevel = float(source.MaxContentLightLevel);
    hdr_.maxFrameAverageLightLevel = float(source.MaxFrameAverageLightLevel);
    hasHdr_ = true;
    const auto& vk = device_->vk();
    if (vk.vkSetHdrMetadataEXT && swapchain_) vk.vkSetHdrMetadataEXT(device_->vkDevice(), 1, &swapchain_, &hdr_);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*)
{
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    std::lock_guard lock(presentMutex_);
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    {
        std::lock_guard surfaceLock(gSurfaceMutex);
        if (surface_->current != owned_) return DXGI_ERROR_INVALID_CALL;   // retired by a newer chain on this window
    }
    // MFGAdaUnlock 1.7.1: see beginRecreate in PresentThread.
    auto dropFrame = [&] {
        if (latency_) ReleaseSemaphore(latency_, 1, nullptr);
        current_ = (current_ + 1) % desc_.BufferCount;
        return S_OK;
    };
    {
        std::lock_guard queueLock(queueMutex_);
        if (recreating_) {
            ++dropped_;
            return dropFrame();
        }
        ++present1Active_;
    }
    struct Leave {
        SwapChain* self;
        ~Leave()
        {
            {
                std::lock_guard queueLock(self->queueMutex_);
                --self->present1Active_;
            }
            self->queueChanged_.notify_all();
        }
    } leave{this};
    LARGE_INTEGER t0{}, t1{}, t2{}, t3{}, t4{};
    QueryPerformanceCounter(&t0);
    Frame& frame = frames_[presentCount_ % frames_.size()];
    // stallguard.1: Present1's waits are logged once when they pass kStallStepNs (they still wait).
    for (bool logged = false; vk.vkWaitForFences(device, 1, &frame.fence, VK_TRUE, kStallStepNs) == VK_TIMEOUT;)
        if (!std::exchange(logged, true)) Log("Present1: staging copy fence of present %llu still pending after 1 s", presentCount_ + 1);
    QueryPerformanceCounter(&t1);
    const uint64_t presentValue = presentCount_ + 1;
    const uint32_t slot = uint32_t(presentCount_ % kStaged);
    uint64_t readValue = 0;
    {
        // Without a sync interval, a staged frame not yet taken by the present thread is replaced (flip-discard), and
        // Present1 waits only while the present thread is copying out of this slot. With one, the queued frame is
        // shown first: Present1 waits until the present thread took it, which throttles the caller like DXGI does.
        std::unique_lock queueLock(queueMutex_);
        auto queuedHere = [&] {
            return std::any_of(queued_.begin(), queued_.end(), [&](const QueuedPresent& q) { return q.staged == slot && q.sync; });
        };
        for (bool logged = false; !queueChanged_.wait_for(queueLock, std::chrono::seconds(1), [&] { return stopPresenter_ || recreating_ || !queuedHere(); });)
            if (!std::exchange(logged, true)) Log("Present1: V-Sync frame in slot %u not taken by the present thread after 1 s", slot);
        if (recreating_ && queuedHere()) {
            ++dropped_;
            return dropFrame();
        }
        for (auto it = queued_.begin(); it != queued_.end();) {
            if (it->staged == slot) { it = queued_.erase(it); ++dropped_; }
            else ++it;
        }
        for (bool logged = false; !queueChanged_.wait_for(queueLock, std::chrono::seconds(1), [&] { return !stagedBusy_[slot]; });)
            if (!std::exchange(logged, true))
                Log("Present1: staging slot %u still read by the present thread after 1 s (presenting %d, queued %zu)", slot,
                    int(presenting_), queued_.size());
        readValue = stagedRead_[slot];
    }
    if (readValue) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &presentTimeline_, &readValue};
        for (bool logged = false; vk.vkWaitSemaphores(device, &wait, kStallStepNs) == VK_TIMEOUT;)
            if (!std::exchange(logged, true)) Log("Present1: present copy %llu of slot %u still pending after 1 s", readValue, slot);
    }
    vk.vkResetFences(device, 1, &frame.fence);
    QueryPerformanceCounter(&t2);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(frame.command, &begin);
    // Earlier submissions on this queue wrote the backbuffer: make those writes visible to the copy.
    VkMemoryBarrier2 written{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    written.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    written.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    written.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    written.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &written;
    vk.vkCmdPipelineBarrier2(frame.command, &dependency);
    Resource* source = backbuffers_[current_];
#if MFG_UNLOCK_XVK_CAPTURE
    const bool capture = captureMapped_ && presentValue >= captureFirst_ && presentValue < captureFirst_ + captureCount_;
    if (capture) {
        VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {desc_.Width, desc_.Height, 1}};
        vk.vkCmdCopyImageToBuffer(frame.command, source->image(), VK_IMAGE_LAYOUT_GENERAL, captureBuffer_, 1, &region);
    }
#endif
    const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageCopy region{layers, {}, layers, {}, {desc_.Width, desc_.Height, 1}};
    vk.vkCmdCopyImage(frame.command, source->image(), VK_IMAGE_LAYOUT_GENERAL, staged_[slot]->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    vk.vkEndCommandBuffer(frame.command);

    const VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, presentValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    HRESULT hr = queue_->SubmitStageCopy(frame.command, &signal, 1, frame.fence, nullptr);
    if (FAILED(hr)) return hr;
#if MFG_UNLOCK_XVK_CAPTURE
    if (capture) {
        vk.vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
        wchar_t name[64];
        swprintf_s(name, L"\\present-%06llu.bin", presentValue);
        FILE* file = nullptr;
        if (_wfopen_s(&file, (captureDir_ + name).c_str(), L"wb") == 0 && file) {
            fwrite(captureMapped_, 1, size_t(desc_.Width) * desc_.Height * 4, file);
            fclose(file);
        }
    }
#endif
    QueryPerformanceCounter(&t3);
    // The host's frame id: XeFG's own frame number (low 32 bits, from 0 per context) on the chain's base.
    const uint64_t frameId = tOutputFrame ? frameBase_ + (tOutputFrame & 0xFFFFFFFFull) : 0;
    if (tOutputFrame && loggedFrames_ < 8) {
        ++loggedFrames_;
        Log("Present1 %llu: XeFG frame %llu (host frame %llu), %s", presentValue, tOutputFrame, frameId,
            tOutputGenerated ? "generated" : "real");
    }
    {
        std::lock_guard queueLock(queueMutex_);
        queued_.push_back({slot, presentValue, sync > 0, frameId, tOutputGenerated});
    }
    queueChanged_.notify_all();
    presentCount_ = presentValue;
    if (latency_) {
        // The frame is retired once it is staged (DXGI retires a flip-discard frame when shown or replaced).
        HANDLE semaphore = latency_;
        device_->WaitAsync(timeline_, presentValue, [semaphore] { ReleaseSemaphore(semaphore, 1, nullptr); });
    }
    current_ = (current_ + 1) % desc_.BufferCount;
    QueryPerformanceCounter(&t4);
    presentTicks_[0] += t1.QuadPart - t0.QuadPart;
    presentTicks_[1] += t2.QuadPart - t1.QuadPart;
    presentTicks_[2] += t3.QuadPart - t2.QuadPart;
    presentTicks_[3] += t4.QuadPart - t3.QuadPart;
    presentTicks_[4] += t4.QuadPart - t0.QuadPart;
    ++timedPresents_;
    return S_OK;
}

// ---------------------------------------------------------------- Factory
HRESULT STDMETHODCALLTYPE Factory::CreateSwapChainForHwnd(IUnknown* deviceOrQueue, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput*, IDXGISwapChain1** out)
{
    if (!deviceOrQueue || !window || !desc || !out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(deviceOrQueue->QueryInterface(IID_PPV_ARGS(&queue)))) return DXGI_ERROR_INVALID_CALL;
    auto* chain = new SwapChain(device_, Own<CommandQueue>(queue), this, window, *desc, fullscreen);
    queue->Release();
    const HRESULT hr = chain->Init();
    if (FAILED(hr)) { chain->Release(); return hr; }
    *out = chain;
    return S_OK;
}

} // namespace xvk

namespace xvk {

uint64_t PresentCount() noexcept { return gPresents.load(); }

HRESULT CreateFactory(ID3D12Device* device, REFIID riid, void** out) noexcept
{
    if (!device || !out) return E_INVALIDARG;
    try {
        auto* factory = new Factory(Own<Device>(static_cast<ID3D12Device1*>(device)));
        const HRESULT hr = factory->QueryInterface(riid, out);
        factory->Release();
        return hr;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

void RegisterSurface(ID3D12Device* device, HWND window, VkSurfaceKHR surface, VkFullScreenExclusiveEXT exclusive,
                     VkPresentModeKHR presentMode) noexcept
{
    try {
        auto entry = std::make_shared<Surface>();
        entry->device = Own<Device>(static_cast<ID3D12Device1*>(device));
        entry->window = window;
        entry->surface = surface;
        entry->owned = false;
        entry->exclusive = exclusive;
        entry->presentMode = presentMode;
        std::lock_guard lock(gSurfaceMutex);
        gRegistered[window] = std::move(entry);
    } catch (...) {}
}

void ForgetSurface(HWND window) noexcept
{
    std::shared_ptr<Surface> entry;
    std::lock_guard lock(gSurfaceMutex);
    if (auto found = gRegistered.find(window); found != gRegistered.end()) {
        entry = std::move(found->second);
        gRegistered.erase(found);
    }
}

bool SurfaceBroken(HWND window) noexcept
{
    std::lock_guard lock(gSurfaceMutex);
    const auto found = gRegistered.find(window);
    return found != gRegistered.end() && found->second->broken;
}

// ---------------------------------------------------------------- vkreflex.1: the host's low-latency outputs
void SetPresentMeteringBatch(uint32_t frames) noexcept
{
    gMeteringBatch.store(frames, std::memory_order_relaxed);
}

void SetOutputFrame(uint64_t frame, bool generated) noexcept
{
    tOutputFrame = frame;
    tOutputGenerated = generated;
}

void ClearOutputFrame() noexcept
{
    tOutputFrame = 0;
    tOutputGenerated = false;
}

void SetFrameBase(HWND window, uint64_t base) noexcept
{
    std::lock_guard lock(gSurfaceMutex);
    if (const auto found = gRegistered.find(window); found != gRegistered.end()) found->second->frameBase = base;
}

void SetReflexMode(ID3D12Device* d3d, const ReflexMode& mode) noexcept
{
    try {
        auto* device = Own<Device>(static_cast<ID3D12Device1*>(d3d));
        std::vector<std::shared_ptr<Surface>> surfaces;
        {
            std::lock_guard lock(gSurfaceMutex);
            for (const auto& [window, surface] : gRegistered)
                if (surface->device == device) surfaces.push_back(surface);
        }
        {
            std::unique_lock lock(gLatencyLock);
            if (mode == gReflex) return;
            gReflex = mode;
            gReflexOn = mode.on;
            for (const auto& surface : surfaces)
                if (surface->latencyChain) ApplySleepMode(device, surface->latencyChain, mode);
        }
        if (mode.on && device->lowLatency2()) device->NotifyOutOfBand();
        Log("Reflex: on %d, low latency %d, boost %d, minimum interval %u us (device functions %d)", int(mode.on), int(mode.lowLatency),
            int(mode.boost), mode.minimumIntervalUs, int(device->lowLatency2()));
    } catch (...) {}
}

bool ReflexSleep(HWND window, uint64_t timeoutNs) noexcept
{
    try {
        std::shared_ptr<Surface> surface;
        {
            std::lock_guard lock(gSurfaceMutex);
            const auto found = gRegistered.find(window);
            if (found == gRegistered.end()) return false;
            surface = found->second;
        }
        Device* device = surface->device;
        const auto& vk = device->vk();
        VkSemaphore semaphore = VK_NULL_HANDLE;
        uint64_t value = 0;
        {
            std::shared_lock lock(gLatencyLock);
            if (!gReflex.on || !surface->latencyChain || !(semaphore = device->LatencySemaphore())) return false;
            value = device->NextLatencyValue();   // the driver signals the semaphore to it when the sleep ends
            const VkLatencySleepInfoNV sleep{VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, nullptr, semaphore, value};
            const VkResult result = vk.vkLatencySleepNV(device->vkDevice(), surface->latencyChain, &sleep);
            if (result != VK_SUCCESS) {
                static std::atomic<int> logged{0};
                if (logged++ < 8) Log("vkLatencySleepNV %d", int(result));
                return false;
            }
            if ((value % 600) == 0) LogLatencyTimings(device, surface->latencyChain);
        }
        const VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore, &value};
        if (vk.vkWaitSemaphores(device->vkDevice(), &wait, timeoutNs) == VK_TIMEOUT) {
            static std::atomic<int> logged{0};
            if (logged++ < 8) Log("Reflex sleep to %llu not done within %llu ms", value, timeoutNs / 1'000'000ull);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void ReflexMarker(HWND window, uint64_t frame, VkLatencyMarkerNV marker) noexcept
{
    try {
        std::shared_ptr<Surface> surface;
        {
            std::lock_guard lock(gSurfaceMutex);
            const auto found = gRegistered.find(window);
            if (found == gRegistered.end()) return;
            surface = found->second;
        }
        std::shared_lock lock(gLatencyLock);
        if (!gReflex.on || !surface->latencyChain) return;
        const VkSetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, nullptr, frame, marker};
        surface->device->vk().vkSetLatencyMarkerNV(surface->device->vkDevice(), surface->latencyChain, &info);
    } catch (...) {}
}

bool AntiLagUpdate(ID3D12Device* d3d, bool on, uint32_t maxFps, VkAntiLagStageAMD stage, uint64_t frame) noexcept
{
    try {
        auto* device = Own<Device>(static_cast<ID3D12Device1*>(d3d));
        if (!device->vk().vkAntiLagUpdateAMD) return false;
        const VkAntiLagPresentationInfoAMD presentation{VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD, nullptr, stage, frame};
        const VkAntiLagDataAMD data{VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, nullptr, on ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD,
                                    maxFps, &presentation};
        device->vk().vkAntiLagUpdateAMD(device->vkDevice(), &data);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace xvk
