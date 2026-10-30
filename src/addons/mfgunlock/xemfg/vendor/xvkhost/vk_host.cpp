// vkxefg.1 (V1.6.2): XeMFG on Vulkan games, with RTXMFG as the XeFG host (vk_host.h).
//
// Frame flow on a taken-over swap chain (one family F; the layer owns its extra queues on F):
//  acquire  waits on XeFG's frame-latency waitable, takes the proxy's current back buffer and signals the game's
//           semaphore/fence from the layer's application queue, after every wait XeFG queued there (the D3D12
//           rule that the application's next rendering into a buffer waits for XeFG to release it).
//  inputs   the game's DLSS depth and motion vectors are copied on its own command buffer into a ring of the
//           host's images (RecordDlssInputs), before DLSS reads them.
//  present  on the game's queue: wait for the game's semaphores, move the image to GENERAL and signal the bridge
//           timeline; the application queue waits for it, the frame's inputs and constants are tagged, and the
//           proxy presents. XeFG renders the generated frames and presents through the layer's VkSwapchainKHR.
#include "vk_host.h"
#include "../../vulkan_policy.hpp"
#include "mfg_xefg_vk.h"
#include "xvk_internal.h"
#include "vk_input_copy_spv.h"
#include "xess/xess_mod.h"
#include "xess/standalone_policy.h"
#include "xess/runtime_search.h"
#include "xess/xemfg_dynamic.h"
#include "xess/xemfg_window.h"
#include "single_module.h"
#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>
#include <xefg_swapchain_debug.h>
#include <xell.h>
#include <xell_d3d12.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <unordered_map>

namespace xess_vulkan
{
namespace
{
using Microsoft::WRL::ComPtr;

std::string Escape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

void Log(const char* stage, const std::string& fields) noexcept
{
    xess_mod::WriteLog(stage, fields);
}

int64_t Now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double Milliseconds(int64_t ticks) noexcept
{
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    return double(ticks) * 1000.0 / frequency;
}

std::atomic<bool> gEnabled{false};
std::atomic<bool> gStandalone{false};            // xemfgsa.1: the product's XeMFG standalone switch (replaces DLSS-G)
std::atomic<bool> gGameFrameGeneration{false};   // SetGameFrameGeneration
std::atomic<bool> gDlssgSeen{false};             // the game drives Streamline's DLSS-G (its switch gates XeMFG)
std::atomic<bool> gSurfaceSeen{false};           // a Vulkan window surface (the menu shows the host's settings)
std::atomic<bool> gDeviceWhileOff{false};        // a device created while the host was off (on applies at restart)
std::atomic<uint32_t> gFrames{2};
constexpr const char* kHostedEntries[] = {"vkCreateSwapchainKHR", "vkQueuePresentKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR",
                                          "vkAcquireNextImageKHR"};
std::atomic<uint32_t> gWrappedEntries{0};
std::atomic<int> gAddonInputCopy{1};   // MFGAdaUnlock 1.7: SetInputCopy (0 off, 1 copy, 2 no dispatch, 3 slots only)
std::atomic<bool> gAddonGenerate{true};    // MFGAdaUnlock 1.7: SetGenerate
std::atomic<bool> gAddonPresentMetering{true};   // MFGAdaUnlock 1.7: SetPresentMetering
std::atomic<InputViewFn> gAddonInputView{nullptr};   // MFGAdaUnlock 1.7: SetInputView
std::atomic<bool> gAddonGameLatency{false};   // MFGAdaUnlock 1.7.1: SetGameLatency
std::atomic<bool> gGameLatencyUsed{false};

// ---------------------------------------------------------------- runtime (the player's libxess_fg + libxell)
struct Runtime
{
    std::once_flag once;
    bool ready = false;
    std::string reason = "not_loaded";
    HMODULE fg = nullptr, ll = nullptr;
    std::wstring folder;
    xess_runtime_search::Result search;   // alpha.15 (B4-12): the folders searched and why the pair was not found
#define MFG_XEFG_FUNCTIONS(X) \
    X(xefgSwapChainD3D12CreateContext) X(xefgSwapChainSetLatencyReduction) X(xefgSwapChainD3D12InitFromSwapChainDesc) \
    X(xefgSwapChainD3D12GetSwapChainPtr) X(xefgSwapChainSetNumInterpolatedFrames) X(xefgSwapChainSetEnabled) \
    X(xefgSwapChainD3D12TagFrameResource) X(xefgSwapChainTagFrameConstants) X(xefgSwapChainSetPresentId) \
    X(xefgSwapChainGetLastPresentStatus) X(xefgSwapChainDestroy) X(xefgSwapChainSetLoggingCallback) X(xefgSwapChainD3D12GetProperties) \
    X(xefgSwapChainEnableDebugFeature)
#define MFG_XELL_FUNCTIONS(X) \
    X(xellD3D12CreateContext) X(xellSetSleepMode) X(xellSleep) X(xellAddMarkerData) X(xellDestroyContext)
#define MFG_FIELD(name) decltype(&::name) name = nullptr;
    MFG_XEFG_FUNCTIONS(MFG_FIELD)
    MFG_XELL_FUNCTIONS(MFG_FIELD)
#undef MFG_FIELD
};
Runtime gRuntime;
std::atomic<bool> gRuntimeTried{false};

// vkxefg.2: OptiScaler sends libxell's exports to its XeLL input, which would take the host's context for the game's.
// The hosting device's originals give OptiScaler's real XeLL functions instead (set before the layer loads the
// runtime).
std::atomic<hosted::OriginalProc> gHostOriginals{nullptr};
std::atomic<void*> gHostOriginalsUser{nullptr};

// Never bundled: the pair comes from the player's install (beside RTXMFG, an RTXMFG-XeSS folder, or the folder
// OptiScaler's package unpacks it to). It must pass the same discovery and instruction-layout checks as any
// OptiScaler pair before a single export is called.
Runtime& LoadRuntime()
{
    std::call_once(gRuntime.once, [] {
        Runtime& rt = gRuntime;
        xess_runtime_search::Bases bases;
        // alpha.15 (B4-12): vk_runtime on every outcome, with the folders searched (the early returns below skipped it
        // for every failure).
        struct Report {
            Runtime& rt;
            const xess_runtime_search::Bases& bases;
            ~Report() {
                try {
                    std::string folder;
                    for (wchar_t c : rt.folder) folder += c < 0x80 ? char(c) : '?';
                    Log("vk_runtime", std::string("\"ready\":") + (rt.ready ? "true" : "false") + ",\"folder\":\"" + Escape(folder) +
                        "\"" + ",\"xellFromHost\":" + (gHostOriginals.load() ? "true" : "false") +
                        (rt.reason.empty() ? "" : ",\"reason\":\"" + rt.reason + "\"") +
                        (rt.search.folders.empty() ? "" : "," + xess_runtime_search::Json(bases, rt.search)));
                } catch (...) {}
                gRuntimeTried = true;
            }
        } report{rt, bases};
        try {
            HMODULE activeFg = nullptr, activeLl = nullptr;
            if (xess_mod::ActiveSdkPair(activeFg, activeLl)) {
                // vkxefg.2: the route is already active on the pair OptiScaler loaded (the one OptiScaler keeps
                // as the real XeLL; any other libxell goes to its XeLL input entirely).
                wchar_t modulePath[MAX_PATH]{};
                const DWORD n = GetModuleFileNameW(activeFg, modulePath, MAX_PATH);
                rt.folder = n && n < MAX_PATH ? std::filesystem::path(modulePath).parent_path().wstring() : L"route";
                rt.fg = activeFg;
                rt.ll = activeLl;
            } else {
                // The module's folder first, then its RTXMFG-XeSS and OptiScaler folders, then the game's (runtime_search.h).
                bases = xess_runtime_search::ReadBases(xess_runtime_search::ModulePath(single_module::Self()),
                    xess_runtime_search::ModulePath(nullptr));
                rt.search = xess_runtime_search::Find(bases, xess_runtime_search::Order::eVulkan);
                if (rt.search.found != SIZE_MAX) {
                    const auto& folder = rt.search.folders[rt.search.found].path;
                    const auto fgPath = folder / xess_runtime_search::kFg, llPath = folder / xess_runtime_search::kLl;
                    rt.folder = folder.wstring();
                    rt.ll = LoadLibraryExW(llPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                    rt.fg = rt.ll ? LoadLibraryExW(fgPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
                }
            }
            if (rt.folder.empty()) { rt.reason = rt.search.reason.empty() ? "runtime_not_found" : rt.search.reason; return; }
            if (!rt.fg || !rt.ll) { rt.reason = "runtime_load_failed"; return; }
            if (!xess_mod::AdoptVulkanHostPair(rt.fg, rt.ll)) { rt.reason = "runtime_route_rejected"; return; }
            bool complete = true;
#define MFG_RESOLVE_FG(name) rt.name = reinterpret_cast<decltype(rt.name)>(GetProcAddress(rt.fg, #name)); complete &= rt.name != nullptr;
#define MFG_RESOLVE_LL(name) rt.name = reinterpret_cast<decltype(rt.name)>(GetProcAddress(rt.ll, #name)); complete &= rt.name != nullptr;
            MFG_XEFG_FUNCTIONS(MFG_RESOLVE_FG)
            MFG_XELL_FUNCTIONS(MFG_RESOLVE_LL)
#undef MFG_RESOLVE_FG
#undef MFG_RESOLVE_LL
            if (!complete) { rt.reason = "runtime_exports_missing"; return; }
            if (auto originals = gHostOriginals.load()) {
                void* user = gHostOriginalsUser.load();
#define MFG_HOST_LL(name) if (auto real = originals(user, #name)) rt.name = reinterpret_cast<decltype(rt.name)>(real); \
                          else if (xess_mod::HostRedirectedExport(rt.ll, #name)) complete = false;
                MFG_XELL_FUNCTIONS(MFG_HOST_LL)
#undef MFG_HOST_LL
                if (!complete) { rt.reason = "runtime_xell_redirected"; return; }
            }
            rt.ready = true;
            rt.reason.clear();
        } catch (...) {
            rt.reason = "runtime_exception";
        }
    });
    return gRuntime;
}

// XeFG presenting without its frame generation initialized (its init retry can succeed for the swap chain alone):
// counted here, and QueuePresent gives the game its chain back.
std::atomic<uint64_t> gGenerationUninitialized{0};

void XefgLog(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    if (message && strstr(message, "Execute called before initialization")) ++gGenerationUninitialized;
    static std::atomic<int> lines{0};
    if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING && lines.fetch_add(1) < 64)
        Log("vk_xefg_message", "\"level\":" + std::to_string(int(level)) + ",\"text\":\"" + Escape(message ? message : "") + "\"");
}

// ---------------------------------------------------------------- registries
struct InstanceRecord
{
    uint32_t apiVersion = VK_API_VERSION_1_0;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
};
struct SurfaceRecord
{
    VkInstance instance = VK_NULL_HANDLE;
    HWND window = nullptr;
};

#define MFG_HOST_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) \
    X(vkWaitSemaphores) X(vkCmdBindPipeline) X(vkCmdPushDescriptorSetKHR) X(vkCmdPushConstants) X(vkCmdDispatch) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateSampler) X(vkDestroySampler)

struct DeviceFns
{
#define MFG_FIELD(name) PFN_##name name = nullptr;
    MFG_HOST_DEVICE_FNS(MFG_FIELD)
#undef MFG_FIELD
};

constexpr uint32_t kInputSlots = 5;
struct InputSlot
{
    uint64_t frame = 0;                 // the base frame (1-based present count) whose inputs this slot holds
    DlssInputs inputs;
    ComPtr<ID3D12Resource> depth, motion;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;   // render size, both images
    VkImageView depthView = VK_NULL_HANDLE, motionView = VK_NULL_HANDLE;   // owned by the xvk resources
    bool deferred = false;   // MFGAdaUnlock 1.7: copied at the frame's present (DeferredCopy), not in DLSS's command buffer
};

struct FakeChain;

struct HostDevice
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    DeviceFns vk;
    uint32_t family = 0, firstQueue = 0, queueCount = 0;
    bool calibrated = false;
    bool presentIds = false;            // vkreflex.1: VK_KHR_present_id and its presentId feature added (Reflex)
    bool presentMetering = false;       // MFGAdaUnlock 1.7: VK_NV_present_metering and its feature added
    uint32_t vendor = 0;
    std::mutex mutex;                   // layer creation, inputs, bridge
    xvk::ReflexMode reflex;             // vkreflex.1: the Reflex mode last given to the layer (game thread, chain mutex)
    // vkreflex.1: frame ids run on across the game's swap-chain recreations. XeLL's context and the driver's Reflex state
    // belong to the device, and an id that starts again at 1 is older than the ones they already saw (No Man's Sky, 23 Oct:
    // the driver's Reflex frame reports stopped at the recreated chain).
    std::atomic<uint32_t> lastFrameId{0};
    ComPtr<ID3D12Device> d3d;
    xvk::Device* layer = nullptr;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D12CommandQueue> appQueue;
    ComPtr<ID3D12Fence> bridge;
    uint64_t bridgeValue = 0;
    xell_context_handle_t xell = nullptr;
    uint32_t xellIntervalUs = 0;        // xemfgsa.5: the XeLL minimum interval the standalone FPS limit last set
    std::string failure;                // the layer could not be created: native from now on
    // inputs
    uint64_t inputFrames = 0;
    int64_t lastInputTick = 0;
    DlssInputs lastInputs{};
    std::array<InputSlot, kInputSlots> slots{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> pipelines{};
    bool copyFailed = false;
    // MFGAdaUnlock 1.7: the deferred input copies (own command buffers on the game's queue, ordered by the bridge value).
    VkCommandPool copyPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 8> copyCommands{};
    std::array<uint64_t, 8> copyValues{};
    uint32_t copyNext = 0;
    std::weak_ptr<FakeChain> active;
    // OptiScaler-hosted (namespace hosted): its unhooked device functions, used for the layer's swap chain calls.
    bool hosted = false;
    hosted::OriginalProc originals = nullptr;
    void* originalsUser = nullptr;
};

struct NativeChain
{
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR info{};
    HWND window = nullptr;
    uint64_t presents = 0;
    bool requested = false;            // the game was told the chain is out of date to take it over
    uint64_t requestedAt = 0;
};

struct FakeChain
{
    std::shared_ptr<HostDevice> owner;
    std::mutex mutex;
    VkSwapchainCreateInfoKHR info{};
    HWND window = nullptr;
    bool vsync = false;
    uint32_t initFlags = 0;
    xefg_swapchain_handle_t fg = nullptr;
    ComPtr<IDXGISwapChain4> proxy;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<VkImage> images;
    HANDLE latency = nullptr;
    // swapchain_maintenance1: a released acquisition kept its frame-latency count, the next acquisition uses it
    bool latencyHeld = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> toPresent, toGeneral;
    // presentqueue.1: the game presents from a queue of another family of the device (idTech's out-of-band present
    // queue): the image goes to the host family with an ownership transfer, released on the presenting queue
    // (`release`, a pool of that family) and acquired on the application queue (`acquireForeign`, `pool`).
    uint32_t foreignFamily = UINT32_MAX;
    VkCommandPool foreignPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> release, acquireForeign;
    int acquired = -1;
    uint32_t frameId = 0;
    uint64_t presents = 0;
    int64_t lastPresent = 0;
    bool enabledApplied = false;
    uint32_t framesApplied = 0;
    bool broken = false;
    bool retired = false;
    uint64_t missingPrograms = 0;   // the layer's count of programs without SPIR-V when XeFG initialized
    uint64_t uninitialized = 0;     // gGenerationUninitialized then
    uint64_t lastBridge = 0;
    xefg_swapchain_present_status_t status{};
    // OptiScaler-hosted chains: its enable switch and frame count, and the game's present ids (present wait).
    bool hosted = false;
    bool userEnabled = true;
    uint32_t targetFrames = 4;
    // OptiScaler's XeFG Debug View (XeFG tags its interpolated frames)
    bool debugView = false;
    bool debugViewApplied = false;
    uint64_t lastPresentId = 0;
    std::condition_variable_any presented;
    // vkreflex.1: the low-latency mode this frame slept in (MfgXeFGVkLatencyMode) and its Anti-Lag 2 limit; counts per
    // mode for the log.
    uint32_t latencyUsed = MFG_XEFG_VK_LATENCY_XELL;
    uint32_t antiLagMaxFps = 0;
    uint32_t latencyLogged = UINT32_MAX;
    std::array<uint64_t, 3> latencyFrames{};
    // xemfgsa.6: Dynamic MFG on a standalone chain: its controllers, 1X (generation off, the count kept), whether it ran
    // last frame, what the game waited for at its last acquisition (the low-latency sleep, XeFG's frame-latency
    // waitable) and present, and the window's refresh rate (looked up every 2 s).
    xemfg_standalone::DynamicFrames dynamic;
    bool dynamicOff = false, dynamicRan = false;
    double sleepMs = 0.0, waitableMs = 0.0, lastPresentMs = 0.0;
    uint32_t refreshHz = 0;
    uint64_t refreshCheckedMs = 0, dynamicLogMs = 0;
    // MFGAdaUnlock 1.7.1: the acquisition holds the chain's mutex through its sleep and XeFG's waitable. A DLSS evaluation
    // on another thread meanwhile reads the frame from here instead of giving the frame's inputs up.
    std::atomic<bool> acquireWaiting{false};
    std::atomic<uint64_t> presentsDone{0};
    struct AcquireWait
    {
        FakeChain& chain;
        explicit AcquireWait(FakeChain& c) : chain(c) { chain.acquireWaiting = true; }
        ~AcquireWait() { chain.acquireWaiting = false; }
    };
    // MFGAdaUnlock 1.7.1: the frame the game's own sleep was for, and which of its simulation markers XeLL already has.
    // Under gameMarkers; a marker arriving once the frame is acquired is not this frame's to give.
    std::mutex gameMarkers;
    uint32_t gameSleptId = 0;
    bool gameMarkersOpen = false, gameSimStart = false, gameSimEnd = false;
};

std::mutex gMutex;
std::unordered_map<VkInstance, InstanceRecord> gInstances;
std::unordered_map<VkSurfaceKHR, SurfaceRecord> gSurfaces;
std::unordered_map<VkDevice, std::shared_ptr<HostDevice>> gDevices;
std::unordered_map<VkQueue, std::pair<VkDevice, uint32_t>> gQueues;
std::unordered_map<VkSwapchainKHR, std::shared_ptr<FakeChain>> gFakes;
std::unordered_map<VkSwapchainKHR, NativeChain> gNatives;
std::atomic<uint64_t> gBasePresents{0};
std::string gLastReason;

// vkreflex.1: the host's low-latency output (hosted::SetLatency), and the modes it asked for before PrepareDevice (their
// device extensions are added there), one bit per MfgXeFGVkLatencyMode.
std::mutex gLatencyMutex;
MfgXeFGVkLatency gLatency{sizeof(MfgXeFGVkLatency), MFG_XEFG_VK_LATENCY_XELL};
std::atomic<uint32_t> gLatencyRequested{0};
// The mode the last frame slept in, and whether that mode applied the FPS limit (the status for the host's limiter).
std::atomic<uint32_t> gLatencyUsed{MFG_XEFG_VK_LATENCY_XELL};
std::atomic<bool> gLatencyLimited{false};
// xemfgsa.5: XeMFG standalone's FPS limit from the menu (presented frames per second; 0 off), and the game's and the
// layer's frame rates for the menu.
std::atomic<uint32_t> gStandaloneFpsLimit{0};
std::mutex gFpsMutex;
xemfg_standalone::FpsWindow gFps;
// xemfgsa.6: XeMFG standalone's Dynamic MFG settings (the most frames and the FPS limit are filled in per frame).
std::mutex gDynamicMutex;
xemfg_standalone::DynamicSettings gDynamic;

void NoteReason(const std::string& reason)
{
    std::lock_guard lock(gMutex);
    if (reason == gLastReason) return;
    gLastReason = reason;
    Log("vk_host_state", "\"reason\":\"" + Escape(reason) + "\"");
}

std::shared_ptr<HostDevice> FindDevice(VkDevice device)
{
    std::lock_guard lock(gMutex);
    auto found = gDevices.find(device);
    return found == gDevices.end() ? nullptr : found->second;
}

std::shared_ptr<FakeChain> FindFake(VkSwapchainKHR swapchain)
{
    std::lock_guard lock(gMutex);
    auto found = gFakes.find(swapchain);
    return found == gFakes.end() ? nullptr : found->second;
}

// ---------------------------------------------------------------- formats
struct FormatChoice
{
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    VkFormat image = VK_FORMAT_UNDEFINED;
    DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
};

bool ChooseFormat(VkFormat format, VkColorSpaceKHR space, FormatChoice& out)
{
    out.image = format;
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: out.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: out.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: out.dxgi = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: out.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    default: return false;
    }
    if (space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) out.space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    else if (space == VK_COLOR_SPACE_HDR10_ST2084_EXT && format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) out.space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    else if (space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && format == VK_FORMAT_R16G16B16A16_SFLOAT) out.space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    else return false;
    return true;
}

template <class T>
T* FindStruct(const void* next, VkStructureType type)
{
    for (unsigned links = 0; next && links < 64; ++links) {
        auto* base = static_cast<const VkBaseInStructure*>(next);
        if (base->sType == type) return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(base));
        next = base->pNext;
    }
    return nullptr;
}

bool PNextHas(const void* next, VkStructureType type)
{
    for (unsigned links = 0; next && links < 64; ++links) {
        auto* base = static_cast<const VkBaseInStructure*>(next);
        if (base->sType == type) return true;
        next = base->pNext;
    }
    return false;
}

// ---------------------------------------------------------------- the layer device (lazy, per VkDevice)
bool EnsureLayer(HostDevice& host, std::string& reason)
{
    if (!host.failure.empty()) { reason = host.failure; return false; }   // also a layer that failed later (QueuePresent)
    if (host.layer) return true;
    if (!xess_mod::Self()) { reason = "xess_route_not_started"; return false; }   // not latched: the backend starts it
    Runtime& rt = LoadRuntime();
    if (!rt.ready) { reason = rt.reason; return false; }
    try {
        std::vector<xvk::QueueId> queues;
        for (uint32_t i = 0; i < host.queueCount; ++i) queues.push_back({host.family, host.firstQueue + i});
        xvk::DeviceDesc desc;
        desc.instance = host.instance;
        desc.physicalDevice = host.physical;
        desc.device = host.device;
        desc.getInstanceProcAddr = host.gipa;
        desc.queueCount = uint32_t(queues.size());
        desc.queues = queues.data();
        desc.programModule = single_module::Self();
        desc.calibratedTimestamps = host.calibrated;
        desc.presentIds = host.presentIds;
        desc.presentMetering = host.presentMetering;
        desc.deviceProcOverride = host.originals;   // OptiScaler-hosted: its unhooked swap chain and present entries
        desc.deviceProcOverrideUser = host.originalsUser;
        HRESULT hr = xvk::CreateDevice(desc, IID_PPV_ARGS(&host.d3d));
        if (FAILED(hr)) throw std::runtime_error("layer_device_" + std::to_string(hr));
        host.layer = static_cast<xvk::Device*>(static_cast<ID3D12Device1*>(host.d3d.Get()));
        xess_mod::SetVulkanHostDevice(host.d3d.Get());
        if (FAILED(xvk::CreateFactory(host.d3d.Get(), IID_PPV_ARGS(&host.factory)))) throw std::runtime_error("layer_factory");
        D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if (FAILED(host.d3d->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&host.appQueue)))) throw std::runtime_error("layer_queue");
        if (FAILED(host.d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&host.bridge)))) throw std::runtime_error("layer_fence");
        xess_mod::VulkanHostCallScope scope;
        if (rt.xellD3D12CreateContext(host.d3d.Get(), &host.xell) != XELL_RESULT_SUCCESS || !host.xell) throw std::runtime_error("xell_create");
        // XeFG generates frames only with XeLL's low-latency mode on (it logs "Latency reduction is not
        // enabled" and presents the real frame alone otherwise); the host sleeps in XeLL at every acquisition.
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = 1;
        if (rt.xellSetSleepMode(host.xell, &sleep) != XELL_RESULT_SUCCESS) throw std::runtime_error("xell_sleep_mode");
        Log("vk_layer_ready", "\"family\":" + std::to_string(host.family) + ",\"queues\":" + std::to_string(host.queueCount) +
            ",\"programs\":" + std::to_string(xvk::EmbeddedProgramCount(single_module::Self())));
        return true;
    } catch (const std::exception& error) {
        host.failure = error.what();
    } catch (...) {
        host.failure = "layer_exception";
    }
    if (host.xell && rt.xellDestroyContext) { xess_mod::VulkanHostCallScope scope; rt.xellDestroyContext(host.xell); }
    host.xell = nullptr;
    host.bridge.Reset();
    host.appQueue.Reset();
    host.factory.Reset();
    host.layer = nullptr;
    host.d3d.Reset();
    reason = host.failure;
    Log("vk_layer_rejected", "\"reason\":\"" + Escape(host.failure) + "\"");
    return false;
}

// ---------------------------------------------------------------- takeover policy
bool InputsRecent(const HostDevice& host)
{
    return host.inputFrames >= 2 && host.lastInputTick && Milliseconds(Now() - host.lastInputTick) < 2000.0;
}

bool Eligible(HostDevice& host, const VkSwapchainCreateInfoKHR& info, HWND window, std::string& reason)
{
    FormatChoice format;
    if (!gEnabled.load()) reason = "disabled";
    else if (gGameFrameGeneration.load() && !gStandalone.load()) reason = "game_frame_generation";
    else if (gWrappedEntries.load() != (1u << std::size(kHostedEntries)) - 1) reason = "swapchain_entries_not_wrapped";
    else if (!InputsRecent(host)) reason = "no_dlss_inputs";
    else if (!window || !IsWindow(window)) reason = "surface_window_unknown";
    else if (info.imageArrayLayers != 1 || (info.flags & ~VkSwapchainCreateFlagsKHR(VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)))
        reason = "swapchain_flags";
    else if (!ChooseFormat(info.imageFormat, info.imageColorSpace, format)) reason = "swapchain_format";
    else if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT) reason = "swapchain_concurrent";
    else if (!info.imageExtent.width || !info.imageExtent.height) reason = "swapchain_extent";
    // MFGAdaUnlock 1.7: the addon answers the mode commands on the host's chain (native_vulkan.cpp) and CreateFake gives
    // the layer's chain the game's own mode; removing it instead let the driver pick (RDR2: a frozen picture in focus).
    else if (!gStandalone.load() && PNextHas(info.pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT)) reason = "fullscreen_exclusive";
    else return true;
    return false;
}

// ---------------------------------------------------------------- fake chains
void RecordTransition(const DeviceFns& vk, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                      uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
{
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    vk.vkBeginCommandBuffer(command, &begin);
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vk.vkCmdPipelineBarrier2(command, &dependency);
    vk.vkEndCommandBuffer(command);
}

void ReleaseFake(FakeChain& chain)
{
    Runtime& rt = gRuntime;
    HostDevice& host = *chain.owner;
    {
        xess_mod::VulkanHostCallScope scope;
        if (chain.fg && rt.xefgSwapChainSetEnabled) rt.xefgSwapChainSetEnabled(chain.fg, 0);
        chain.proxy.Reset();
        chain.buffers.clear();
        if (chain.fg && rt.xefgSwapChainDestroy) {
            const auto result = rt.xefgSwapChainDestroy(chain.fg);
            Log("vk_xefg_destroy", "\"result\":" + std::to_string(int(result)));
        }
        chain.fg = nullptr;
    }
    if (chain.latency) CloseHandle(chain.latency);
    chain.latency = nullptr;
    // The transitions may still run on the game's queue (bridge) or the application queue.
    if (host.layer) {
        if (chain.lastBridge) {
            VkSemaphore semaphore = xvk::Own<xvk::Fence>(host.bridge.Get())->semaphore();
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore, &chain.lastBridge};
            host.vk.vkWaitSemaphores(host.device, &wait, 2'000'000'000ull);
        }
        host.layer->WaitIdle(2'000'000'000ull);
        // Gone before its first present: the game's chain was never retired (or, handed over, is destroyed now).
        if (chain.window) host.layer->DropRetireHint(chain.window);
    }
    if (chain.pool) host.vk.vkDestroyCommandPool(host.device, chain.pool, nullptr);
    chain.pool = VK_NULL_HANDLE;
    if (chain.foreignPool) host.vk.vkDestroyCommandPool(host.device, chain.foreignPool, nullptr);
    chain.foreignPool = VK_NULL_HANDLE;
    chain.foreignFamily = UINT32_MAX;
    chain.release.clear();
    chain.acquireForeign.clear();
    chain.toPresent.clear();
    chain.toGeneral.clear();
    chain.images.clear();
    if (chain.window) xvk::ForgetSurface(chain.window);
    chain.retired = true;
}

// presentqueue.1: a present from `family` (not the host family): per image, a release from that family to the host
// family on the presenting queue (PRESENT_SRC to GENERAL) and the matching acquire on the application queue. Made
// at the first such present; a chain serves one presenting family.
bool EnsureForeignFamily(FakeChain& chain, uint32_t family)
{
    HostDevice& host = *chain.owner;
    if (chain.foreignFamily == family) return true;
    if (chain.foreignFamily != UINT32_MAX || chain.images.empty()) return false;
    const uint32_t count = uint32_t(chain.images.size());
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
    if (host.vk.vkCreateCommandPool(host.device, &poolInfo, nullptr, &chain.foreignPool) != VK_SUCCESS) {
        chain.foreignPool = VK_NULL_HANDLE;
        return false;
    }
    chain.release.resize(count);
    chain.acquireForeign.resize(count);
    VkCommandBufferAllocateInfo releaseInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, chain.foreignPool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, count};
    VkCommandBufferAllocateInfo acquireInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, chain.pool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, count};
    if (host.vk.vkAllocateCommandBuffers(host.device, &releaseInfo, chain.release.data()) != VK_SUCCESS ||
        host.vk.vkAllocateCommandBuffers(host.device, &acquireInfo, chain.acquireForeign.data()) != VK_SUCCESS) {
        host.vk.vkDestroyCommandPool(host.device, chain.foreignPool, nullptr);   // frees its buffers
        chain.foreignPool = VK_NULL_HANDLE;
        chain.release.clear();
        chain.acquireForeign.clear();   // any allocated from `pool` are freed with it
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        RecordTransition(host.vk, chain.release[i], chain.images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
                         family, host.family);
        RecordTransition(host.vk, chain.acquireForeign[i], chain.images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
                         family, host.family);
    }
    chain.foreignFamily = family;
    return true;
}

// `retired`: the game's chain `retire` was named as oldSwapchain by a layer chain, which retires it even when that
// creation or XeFG's init failed afterwards; a fail-closed native chain must then not name it again.
std::shared_ptr<FakeChain> CreateFake(const std::shared_ptr<HostDevice>& hostPtr, const VkSwapchainCreateInfoKHR& info, HWND window,
                                      VkSwapchainKHR retire, bool invertedDepth, std::string& reason, bool& retired)
{
    retired = false;
    HostDevice& host = *hostPtr;
    Runtime& rt = gRuntime;
    auto chain = std::make_shared<FakeChain>();
    chain->owner = hostPtr;
    chain->frameId = host.lastFrameId.load();
    chain->info = info;
    chain->info.pNext = nullptr;
    chain->info.pQueueFamilyIndices = nullptr;
    chain->info.queueFamilyIndexCount = 0;
    chain->window = window;
    // vkswapext.1: FIFO_LATEST_READY (VK_KHR/EXT_present_mode_fifo_latest_ready) is V-Sync too.
    constexpr auto kFifoLatestReady = static_cast<VkPresentModeKHR>(1000361000);
    chain->vsync = info.presentMode == VK_PRESENT_MODE_FIFO_KHR || info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ||
                   info.presentMode == kFifoLatestReady;
    FormatChoice format;
    ChooseFormat(info.imageFormat, info.imageColorSpace, format);
    const VkImageUsageFlags layerUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!host.layer->ImageSupported(format.image, info.imageUsage | layerUsage,
                                    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)) {
        reason = "swapchain_usage_unsupported";
        return nullptr;
    }
    VkSurfaceKHR surface = info.surface;
    // vkswapext.1: the game's full-screen-exclusive mode (VK_EXT_full_screen_exclusive, hosted::CreateSwapchain admits
    // it only when the host routes the mode calls) applies to the real swap chain on this surface. The game's
    // acquire/release calls on our handle are answered by the host, so application-controlled becomes allowed.
    VkFullScreenExclusiveEXT exclusive = VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
    if (auto* fse = FindStruct<VkSurfaceFullScreenExclusiveInfoEXT>(info.pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT))
        exclusive = fse->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
                                                                                                   : fse->fullScreenExclusive;
    // The game's surface: XeFG's swap chain takes it over, retiring the game's own chain on it.
    xvk::RegisterSurface(host.d3d.Get(), window, surface, exclusive, info.presentMode);
    // vkreflex.1: XeFG's frame 0 on this chain is the host's next frame (Reflex names each output by it).
    xvk::SetFrameBase(window, uint64_t(chain->frameId) + 1);
    if (retire) host.layer->SetRetireHint(window, retire);
    // Never HIGH_RES_MV: the host's motion-vector copy is at render size (XeFG wants depth and motion vectors at one
    // size). Never JITTERED_MV: with it XeFG fails its init with "Invalid internal state" on the MFG port (seen
    // 21 Oct, before it requests the port's splat programs), so a game's jittered vectors are taken as they are.
    chain->initFlags = invertedDepth ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH : 0;
    const UINT bufferCount = std::clamp<UINT>(info.minImageCount, 2, 8);
    host.layer->SetBackbufferHint({info.imageExtent.width, info.imageExtent.height, format.dxgi, format.image, info.imageUsage});
    auto fail = [&](const std::string& why) {
        reason = why;
        host.layer->SetBackbufferHint({});
        // A hint still waiting was never used (and must not outlive this attempt). The layer names it only at its
        // first present, so it cannot have been taken here; checked anyway.
        retired = retire && !host.layer->DropRetireHint(window);
        ReleaseFake(*chain);
        return std::shared_ptr<FakeChain>();
    };
    // XeFG without one of its programs runs at 1x ("Execute called before initialization") even when its init
    // retry succeeds: a program the embedded set lacks keeps the game's own chain.
    const uint64_t missingBefore = xvk::MissingProgramCount();
    {
        xess_mod::VulkanHostCallScope scope;
        if (rt.xefgSwapChainD3D12CreateContext(host.d3d.Get(), &chain->fg) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !chain->fg)
            return fail("xefg_create");
        rt.xefgSwapChainSetLoggingCallback(chain->fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, XefgLog, nullptr);
        if (rt.xefgSwapChainSetLatencyReduction(chain->fg, host.xell) != XEFG_SWAPCHAIN_RESULT_SUCCESS) return fail("xefg_latency");
        xefg_swapchain_d3d12_init_params_t params{};
        params.initFlags = chain->initFlags;
        params.maxInterpolatedFrames = 5;
        params.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = info.imageExtent.width;
        desc.Height = info.imageExtent.height;
        desc.Format = format.dxgi;
        desc.SampleDesc = {1, 0};
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        desc.BufferCount = bufferCount;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (chain->vsync ? 0 : DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        auto result = rt.xefgSwapChainD3D12InitFromSwapChainDesc(chain->fg, window, &desc, nullptr, host.appQueue.Get(), host.factory.Get(), &params);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS && xvk::MissingProgramCount() == missingBefore) {
            // Retry without the 5-frame allocation (an unlock the route refused leaves XeFG's stock limit).
            params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
            result = rt.xefgSwapChainD3D12InitFromSwapChainDesc(chain->fg, window, &desc, nullptr, host.appQueue.Get(), host.factory.Get(), &params);
        }
        if (xvk::MissingProgramCount() != missingBefore) {
            // presentqueue.1: the set lacks one of the device's programs for good; the game keeps its chains
            // instead of every recreation running XeFG's init again (an RDR2 log spent 26 s in the first one).
            // Both callers hold host.mutex.
            host.failure = "programs_missing";
            return fail("programs_missing");
        }
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) return fail("xefg_init_" + std::to_string(int(result)));
        chain->missingPrograms = missingBefore;
        chain->uninitialized = gGenerationUninitialized.load();
        if (rt.xefgSwapChainD3D12GetSwapChainPtr(chain->fg, IID_PPV_ARGS(&chain->proxy)) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !chain->proxy)
            return fail("xefg_proxy");
    }
    host.layer->SetBackbufferHint({});
    if (format.space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 && FAILED(chain->proxy->SetColorSpace1(format.space)))
        return fail("color_space");
    chain->latency = chain->proxy->GetFrameLatencyWaitableObject();
    if (!chain->latency) return fail("latency_waitable");
    for (UINT i = 0; i < bufferCount; ++i) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(chain->proxy->GetBuffer(i, IID_PPV_ARGS(&buffer))) || !buffer) return fail("proxy_buffer");
        auto* resource = xvk::Own<xvk::Resource>(buffer.Get());
        if (!resource->image()) return fail("proxy_buffer_image");
        chain->images.push_back(resource->image());
        chain->buffers.push_back(std::move(buffer));
    }
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, host.family};
    if (host.vk.vkCreateCommandPool(host.device, &poolInfo, nullptr, &chain->pool) != VK_SUCCESS) return fail("command_pool");
    chain->toPresent.resize(bufferCount);
    chain->toGeneral.resize(bufferCount);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, chain->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         bufferCount};
    if (host.vk.vkAllocateCommandBuffers(host.device, &allocate, chain->toPresent.data()) != VK_SUCCESS ||
        host.vk.vkAllocateCommandBuffers(host.device, &allocate, chain->toGeneral.data()) != VK_SUCCESS)
        return fail("command_buffers");
    for (UINT i = 0; i < bufferCount; ++i) {
        RecordTransition(host.vk, chain->toPresent[i], chain->images[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        RecordTransition(host.vk, chain->toGeneral[i], chain->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL);
    }
    {
        xess_mod::VulkanHostCallScope scope;
        const uint32_t frames = std::clamp<uint32_t>(gFrames.load(), 2, 6);
        rt.xefgSwapChainSetNumInterpolatedFrames(chain->fg, frames - 1);
        chain->framesApplied = frames;
        rt.xefgSwapChainSetEnabled(chain->fg, 0);
        chain->enabledApplied = false;
    }
    chain->lastPresent = Now();
    Log("vk_takeover", "\"width\":" + std::to_string(info.imageExtent.width) + ",\"height\":" + std::to_string(info.imageExtent.height) +
        ",\"format\":" + std::to_string(int(info.imageFormat)) + ",\"color_space\":" + std::to_string(int(info.imageColorSpace)) +
        ",\"images\":" + std::to_string(bufferCount) + ",\"vsync\":" + (chain->vsync ? "true" : "false") +
        ",\"init_flags\":" + std::to_string(chain->initFlags) + ",\"frames\":" + std::to_string(chain->framesApplied));
    return chain;
}

// XeFG's camera from DLSS Super Resolution inputs (they carry no matrices): identity view and a left-handed
// perspective projection over the output aspect, XMMatrixPerspectiveFovLH's layout as OptiScaler's D3D12 XeFG
// output builds it. The provider's camera when it has one (OptiScaler), else 60 degrees vertical, near 0.1,
// far 10000; near and far swapped for inverted depth, an infinite far plane taken to its limit.
void Projection(float aspect, const DlssInputs& in, float out[16])
{
    const bool known = in.cameraNear > 0 && in.cameraFar > 0 && in.cameraVFov > 0.0001f && in.cameraNear != in.cameraFar;
    float nearPlane = known ? in.cameraNear : 0.1f, farPlane = known ? in.cameraFar : 10000.0f;
    if (in.depthInverted) std::swap(nearPlane, farPlane);
    const float y = 1.0f / std::tan(0.5f * (known ? in.cameraVFov : 1.0471976f));
    const float x = y / (aspect > 0 ? aspect : 1.0f);
    float depth = 0, offset = 0;
    if (std::isinf(farPlane)) { depth = 1.0f; offset = -nearPlane; }
    else if (std::isinf(nearPlane)) { depth = 0.0f; offset = farPlane; }
    else { depth = farPlane / (farPlane - nearPlane); offset = -nearPlane * depth; }
    const float m[16] = {x, 0, 0, 0, 0, y, 0, 0, 0, 0, depth, 1, 0, 0, offset, 0};
    memcpy(out, m, sizeof(m));
}

InputSlot* SlotFor(HostDevice& host, uint64_t frame)
{
    for (auto& slot : host.slots)
        if (slot.frame == frame && slot.depth && slot.motion) return &slot;
    return nullptr;
}
} // namespace

void SetInputCopy(int mode) noexcept
{
    gAddonInputCopy = mode;
}

void SetGenerate(bool enabled) noexcept
{
    gAddonGenerate = enabled;
}

void SetPresentMetering(bool enabled) noexcept
{
    gAddonPresentMetering = enabled;
}

void SetInputView(InputViewFn view) noexcept
{
    gAddonInputView = view;
}

void EntryWrapped(const char* name) noexcept
{
    for (size_t i = 0; i < std::size(kHostedEntries); ++i)
        if (name && !strcmp(name, kHostedEntries[i])) gWrappedEntries.fetch_or(1u << i);
}

// ---------------------------------------------------------------- configuration
void SetGameFrameGeneration(bool on) noexcept
{
    gDlssgSeen = true;
    if (gGameFrameGeneration.exchange(on) != on) Log("vk_host_game_fg", std::string("\"on\":") + (on ? "true" : "false"));
}

void Configure(bool harness, bool standalone, uint32_t frames) noexcept
{
#if !MFG_UNLOCK_XEFG_VK_STANDALONE
    // The harness key's takeover stays the harness build's (MFG_UNLOCK_XEFG_VK_STANDALONE); the product takes chains
    // over only under XeMFG standalone (xemfgsa.1). Under OptiScaler its XeFG_Vk output hosts XeFG (namespace hosted).
    harness = false;
#endif
    const bool enabled = harness || standalone;
    gStandalone = standalone;
    const bool was = gEnabled.exchange(enabled);
    const uint32_t previous = gFrames.exchange(std::clamp<uint32_t>(frames, 2, 6));
    if (was != enabled || previous != gFrames.load())
        Log("vk_host_config", std::string("\"enabled\":") + (enabled ? "true" : "false") + ",\"standalone\":" +
            (standalone ? "true" : "false") + ",\"frames\":" + std::to_string(gFrames.load()));
}

void SetStandaloneFpsLimit(uint32_t fps) noexcept
{
    if (gStandaloneFpsLimit.exchange(fps) != fps) Log("vk_fps_limit_config", "\"fps\":" + std::to_string(fps));
}

void SetStandaloneDynamic(bool enabled, uint32_t targetFps, int rounding, bool allow1x, bool targetAsCap) noexcept
{
    try {
        std::lock_guard lock(gDynamicMutex);
        auto next = gDynamic;
        next.enabled = enabled;
        next.targetFps = std::min<uint32_t>(targetFps, 1000);
        next.rounding = std::clamp(rounding, 0, 2);
        next.allow1x = allow1x;
        next.targetAsCap = targetAsCap;
        if (next == gDynamic) return;
        gDynamic = next;
        Log("vk_dynamic_config", std::string("\"enabled\":") + (enabled ? "true" : "false") + ",\"target\":" +
            std::to_string(next.targetFps) + ",\"rounding\":" + std::to_string(next.rounding) + ",\"allow_1x\":" +
            (allow1x ? "true" : "false") + ",\"target_as_cap\":" + (targetAsCap ? "true" : "false"));
    } catch (...) {}
}

bool ReplacesFrameGeneration() noexcept
{
    if (!gStandalone.load() || !gEnabled.load()) return false;
    try {
        std::lock_guard lock(gMutex);
        for (const auto& [handle, chain] : gFakes) if (!chain->hosted && !chain->broken) return true;
    } catch (...) {}
    return false;
}

// ---------------------------------------------------------------- instance
const VkInstanceCreateInfo* PrepareInstance(const VkInstanceCreateInfo* info, PFN_vkGetInstanceProcAddr loader,
                                            InstanceEdit& storage) noexcept
{
    if (!info || !loader || !gEnabled.load()) return info;
    try {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(loader(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        if (!enumerate || enumerate(&loaderVersion) != VK_SUCCESS || loaderVersion < xvk::kRequiredApiVersion) return info;
        const uint32_t requested = info->pApplicationInfo && info->pApplicationInfo->apiVersion ? info->pApplicationInfo->apiVersion
                                                                                               : VK_API_VERSION_1_0;
        if (requested >= xvk::kRequiredApiVersion) return info;
        storage.info = *info;
        if (info->pApplicationInfo) storage.application = *info->pApplicationInfo;
        else storage.application = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        storage.application.apiVersion = xvk::kRequiredApiVersion;
        storage.info.pApplicationInfo = &storage.application;
        Log("vk_instance_version", "\"requested\":\"" + std::to_string(VK_API_VERSION_MAJOR(requested)) + "." +
            std::to_string(VK_API_VERSION_MINOR(requested)) + "\",\"used\":\"1.3\"");
        return &storage.info;
    } catch (...) {
        return info;
    }
}

void InstanceCreated(VkInstance instance, const VkInstanceCreateInfo* used, PFN_vkGetInstanceProcAddr loader) noexcept
{
    if (!instance || !used || !loader) return;
    try {
        InstanceRecord record;
        record.gipa = loader;
        record.apiVersion = used->pApplicationInfo && used->pApplicationInfo->apiVersion ? used->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        std::lock_guard lock(gMutex);
        if (gInstances.size() < 16) gInstances[instance] = record;
    } catch (...) {}
}

void InstanceDestroyed(VkInstance instance) noexcept
{
    try {
        std::lock_guard lock(gMutex);
        gInstances.erase(instance);
        std::erase_if(gSurfaces, [&](const auto& row) { return row.second.instance == instance; });
    } catch (...) {}
}

void SurfaceCreated(VkInstance instance, VkSurfaceKHR surface, HWND window) noexcept
{
    gSurfaceSeen = true;
    try {
        std::lock_guard lock(gMutex);
        if (gSurfaces.size() < 64) gSurfaces[surface] = {instance, window};
    } catch (...) {}
}

void SurfaceDestroyed(VkSurfaceKHR surface) noexcept
{
    try {
        std::lock_guard lock(gMutex);
        gSurfaces.erase(surface);
    } catch (...) {}
}

// ---------------------------------------------------------------- device preparation
class DeviceEdit
{
public:
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDeviceCreateInfo info{};
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<std::vector<float>> priorities;
    std::vector<const char*> extensions;
    VkPhysicalDeviceFeatures features10{};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceMaintenance4Features maintenance4{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures dot{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDeviceAntiLagFeaturesAMD antiLag{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
    uint32_t family = 0, firstQueue = 0, queueCount = 0;
    bool calibrated = false;
    bool presentIds = false;   // vkreflex.1
    bool presentMetering = false;   // MFGAdaUnlock 1.7
    VkPhysicalDevicePresentMeteringFeaturesNV metering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV};
    uint32_t vendor = 0;
    bool hosted = false;
    std::vector<std::pair<uint32_t, uint32_t>> gameQueues;   // the game's own queues: family, count (flags 0)
};

void DeviceEditDeleter::operator()(DeviceEdit* edit) const noexcept { delete edit; }

namespace {
// Device extensions that pass swap-chain handles through entry points the host does not emulate. A device that
// enables one stays native.
constexpr const char* kSwapchainExtensionsNotEmulated[] = {
    "VK_KHR_present_wait", "VK_KHR_present_wait2", "VK_KHR_present_id", "VK_KHR_present_id2", "VK_NV_low_latency2",
    "VK_GOOGLE_display_timing", "VK_KHR_shared_presentable_image", "VK_EXT_display_control", "VK_EXT_swapchain_maintenance1",
    "VK_KHR_swapchain_maintenance1", "VK_AMD_display_native_hdr", "VK_KHR_display_swapchain", "VK_KHR_device_group",
    "VK_EXT_full_screen_exclusive", "VK_EXT_present_timing"};

// OptiScaler-hosted: present wait and present ids are served on the host's chains (hosted::WaitForPresent, ids read
// at QueuePresent), and OptiScaler keeps VK_NV_low_latency2 calls on them away from the driver (its Reflex input
// signals the game's sleep semaphore, its Reflex output skips FG swap chains; XeLL runs in the engine), and
// swapchain_maintenance1's present fences and image release are served on the host's chains (QueuePresentImpl,
// hosted::ReleaseSwapchainImages; per-present mode changes are ignored), so a game using them can still be hosted.
// MFGAdaUnlock 1.7 (plan F): served on standalone chains by the addon's wrappers (see PrepareDeviceImpl).
constexpr const char* kEmulatedByAddon[] = {"VK_KHR_present_wait", "VK_KHR_present_id", "VK_NV_low_latency2",
                                            "VK_EXT_full_screen_exclusive"};
constexpr const char* kEmulatedWhenHosted[] = {"VK_KHR_present_wait", "VK_KHR_present_wait2", "VK_KHR_present_id", "VK_KHR_present_id2",
                                               "VK_NV_low_latency2", "VK_KHR_swapchain_maintenance1", "VK_EXT_swapchain_maintenance1"};

// vkswapext.1: extensions whose swap-chain entry points the host answers for its chains once it declares the routes
// (MfgXeFGVkApi::SetHostRoutes); before that (older OptiScaler builds) they keep the device native as before.
// VK_KHR_display_swapchain needs no route (vkCreateSharedSwapchainsKHR makes the driver's own chains). Swap-chain
// options none of these can serve are refused per chain (hosted::CreateSwapchain). VK_EXT_present_timing stays native.
struct RoutedExtension
{
    const char* name;
    uint32_t routes;
};
constexpr RoutedExtension kRoutedWhenHosted[] = {
    {"VK_KHR_device_group", MFG_XEFG_VK_ROUTE_ACQUIRE_NEXT_IMAGE_2},
    {"VK_EXT_full_screen_exclusive", MFG_XEFG_VK_ROUTE_FULL_SCREEN_EXCLUSIVE},
    {"VK_GOOGLE_display_timing", MFG_XEFG_VK_ROUTE_DISPLAY_TIMING},
    {"VK_AMD_display_native_hdr", MFG_XEFG_VK_ROUTE_LOCAL_DIMMING},
    {"VK_KHR_shared_presentable_image", MFG_XEFG_VK_ROUTE_SWAPCHAIN_STATUS},
    {"VK_EXT_display_control", MFG_XEFG_VK_ROUTE_SWAPCHAIN_COUNTER},
    {"VK_KHR_display_swapchain", 0}};
std::atomic<bool> gHostRoutesDeclared{false};
std::atomic<uint32_t> gHostRoutes{0};

bool RoutedByHost(const char* name)
{
    if (!gHostRoutesDeclared.load()) return false;
    const uint32_t routes = gHostRoutes.load();
    return std::any_of(std::begin(kRoutedWhenHosted), std::end(kRoutedWhenHosted),
                       [&](const RoutedExtension& e) { return !strcmp(e.name, name) && (routes & e.routes) == e.routes; });
}

thread_local std::string tLastRefusal;   // read by hosted::PrepareDevice (Refuse can run under gMutex)

std::string Refuse(const char* reason)
{
    Log("vk_device_native", std::string("\"reason\":\"") + reason + "\"");
    tLastRefusal = reason;
    return reason;
}

struct HostedInstance
{
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    uint32_t apiVersion = 0;
};

DeviceEditPtr PrepareDeviceImpl(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const HostedInstance* hosted) noexcept;
} // namespace

DeviceEditPtr PrepareDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept
{
    if (!physical || !info) return nullptr;
    if (!gEnabled.load()) {
        gDeviceWhileOff = true;
        return nullptr;
    }
    return PrepareDeviceImpl(physical, info, nullptr);
}

namespace {
DeviceEditPtr PrepareDeviceImpl(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const HostedInstance* hosted) noexcept
{
    try {
        auto edit = DeviceEditPtr(new DeviceEdit);
        edit->hosted = hosted != nullptr;
        if (hosted) {
            // OptiScaler names the instance and the API version it was created with.
            if (hosted->apiVersion < xvk::kRequiredApiVersion) { Refuse("instance_api_version"); return nullptr; }
            edit->instance = hosted->instance;
            edit->gipa = hosted->gipa;
        }
        // The instance that enumerated this physical device, and its API version.
        else {
            std::lock_guard lock(gMutex);
            for (const auto& [instance, record] : gInstances) {
                auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(record.gipa(instance, "vkEnumeratePhysicalDevices"));
                uint32_t count = 0;
                if (!enumerate || enumerate(instance, &count, nullptr) != VK_SUCCESS || !count || count > 64) continue;
                std::vector<VkPhysicalDevice> list(count);
                if (enumerate(instance, &count, list.data()) != VK_SUCCESS) continue;
                if (std::find(list.begin(), list.end(), physical) == list.end()) continue;
                if (record.apiVersion < xvk::kRequiredApiVersion) { Refuse("instance_api_version"); return nullptr; }
                edit->instance = instance;
                edit->gipa = record.gipa;
                break;
            }
        }
        if (!edit->instance) { Refuse("instance_unknown"); return nullptr; }
        auto gipa = edit->gipa;
        auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(edit->instance, "vkGetPhysicalDeviceProperties2"));
        auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(edit->instance, "vkGetPhysicalDeviceFeatures2"));
        auto familiesFn = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(edit->instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        auto extensionsFn = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(edit->instance, "vkEnumerateDeviceExtensionProperties"));
        auto presentation = reinterpret_cast<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(
            gipa(edit->instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR"));
        if (!properties2 || !features2 || !familiesFn || !extensionsFn || !presentation) { Refuse("instance_functions"); return nullptr; }
        VkPhysicalDeviceVulkan11Properties v11Properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &v11Properties};
        properties2(physical, &properties);
        edit->vendor = properties.properties.vendorID;
        if (properties.properties.apiVersion < xvk::kRequiredApiVersion) { Refuse("device_api_version"); return nullptr; }
        // XeFG's inverted-depth motion splat reduces with wave min/max (subgroup arithmetic in compute).
        constexpr VkSubgroupFeatureFlags kSubgroupOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        if (!(v11Properties.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            (v11Properties.subgroupSupportedOperations & kSubgroupOps) != kSubgroupOps) { Refuse("subgroup_operations"); return nullptr; }
        if (edit->vendor == 0x8086) { Refuse("intel_adapter"); return nullptr; }
        // Features the layer and the program set need.
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        features2(physical, &supported);
        if (!v12.timelineSemaphore || !v13.synchronization2 || !v13.maintenance4 || !v13.shaderIntegerDotProduct ||
            !supported.features.shaderStorageImageExtendedFormats) { Refuse("device_features"); return nullptr; }
        uint32_t count = 0;
        extensionsFn(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        extensionsFn(physical, nullptr, &count, available.data());
        auto offered = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const auto& e) { return !strcmp(e.extensionName, name); });
        };
        auto enabled = [&](const char* name) {
            for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                if (info->ppEnabledExtensionNames[i] && !strcmp(info->ppEnabledExtensionNames[i], name)) return true;
            return false;
        };
        {
            std::string names;
            for (uint32_t i = 0; i < info->enabledExtensionCount && i < 256; ++i)
                if (info->ppEnabledExtensionNames[i])
                    names += (names.empty() ? "\"" : ",\"") + Escape(info->ppEnabledExtensionNames[i]) + "\"";
            Log("vk_device_extensions", std::string("\"hosted\":") + (hosted ? "true" : "false") + ",\"enabled\":[" + names + "]");
        }
        if (!enabled(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { Refuse("no_swapchain_extension"); return nullptr; }
        for (const char* name : kSwapchainExtensionsNotEmulated) {
            // MFGAdaUnlock 1.7 (plan F): the addon's vulkan-1 wrappers (native_vulkan.cpp) answer present ids, present
            // wait and VK_NV_low_latency2 (Streamline's Reflex) on the host's own chains.
            const bool emulated = (hosted && (std::any_of(std::begin(kEmulatedWhenHosted), std::end(kEmulatedWhenHosted),
                                                          [&](const char* e) { return !strcmp(e, name); }) ||
                                              RoutedByHost(name))) ||
                                  (!hosted && std::any_of(std::begin(kEmulatedByAddon), std::end(kEmulatedByAddon),
                                                          [&](const char* e) { return !strcmp(e, name); }));
            if (enabled(name) && !emulated) {
                Log("vk_device_native", std::string("\"reason\":\"swapchain_extension\",\"extension\":\"") + name + "\"");
                tLastRefusal = std::string("swapchain_extension_") + name;
                return nullptr;
            }
        }
        if (!offered(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) { Refuse("push_descriptor"); return nullptr; }
        // The game's graphics family that can present gets the layer's queues.
        familiesFn(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        familiesFn(physical, &count, families.data());
        const VkDeviceQueueCreateInfo* target = nullptr;
        for (uint32_t i = 0; i < info->queueCreateInfoCount && !target; ++i) {
            const auto& q = info->pQueueCreateInfos[i];
            if (q.queueFamilyIndex < families.size() && (families[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (families[q.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT) && !q.flags &&
                presentation(physical, q.queueFamilyIndex))
                target = &q;
        }
        if (!target) { Refuse("no_graphics_present_family"); return nullptr; }
        const uint32_t available_ = families[target->queueFamilyIndex].queueCount;
        if (available_ < target->queueCount + 4) { Refuse("family_queue_count"); return nullptr; }
        edit->family = target->queueFamilyIndex;
        edit->firstQueue = target->queueCount;
        edit->queueCount = std::min<uint32_t>(5, available_ - target->queueCount);
        // Queues: the game's (the host knows them without its vkGetDeviceQueue wrapper), then the layer's.
        for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
            if (!info->pQueueCreateInfos[i].flags)
                edit->gameQueues.emplace_back(info->pQueueCreateInfos[i].queueFamilyIndex, info->pQueueCreateInfos[i].queueCount);
        edit->queues.assign(info->pQueueCreateInfos, info->pQueueCreateInfos + info->queueCreateInfoCount);
        edit->priorities.resize(edit->queues.size());
        for (size_t i = 0; i < edit->queues.size(); ++i) {
            auto& q = edit->queues[i];
            if (&info->pQueueCreateInfos[i] != target) continue;
            edit->priorities[i].assign(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);
            edit->priorities[i].resize(q.queueCount + edit->queueCount, 1.0f);
            q.queueCount += edit->queueCount;
            q.pQueuePriorities = edit->priorities[i].data();
        }
        // Extensions
        edit->extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
        if (!enabled(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) edit->extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        if (enabled(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) || enabled(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
            edit->calibrated = true;
        else if (offered(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) { edit->extensions.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME); edit->calibrated = true; }
        else if (offered(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) { edit->extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME); edit->calibrated = true; }
        // vkreflex.1: the host's low-latency output asked for Reflex or Anti-Lag 2 before this call. Their extensions
        // and features go on the device where the GPU has them (the host's own additions find them enabled).
        const uint32_t latency = hosted ? gLatencyRequested.load() : 0;
        bool reflexFeatures = false, antiLagFeatures = false;
        if ((latency & (1u << MFG_XEFG_VK_LATENCY_REFLEX)) && offered(VK_NV_LOW_LATENCY_2_EXTENSION_NAME) &&
            offered(VK_KHR_PRESENT_ID_EXTENSION_NAME)) {
            VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &presentId};
            features2(physical, &query);
            for (const char* name : {VK_NV_LOW_LATENCY_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME})
                if (!enabled(name)) edit->extensions.push_back(name);
            reflexFeatures = presentId.presentId == VK_TRUE;
        }
        if ((latency & (1u << MFG_XEFG_VK_LATENCY_ANTILAG2)) && offered(VK_AMD_ANTI_LAG_EXTENSION_NAME)) {
            VkPhysicalDeviceAntiLagFeaturesAMD antiLag{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &antiLag};
            features2(physical, &query);
            if (antiLag.antiLag) {
                if (!enabled(VK_AMD_ANTI_LAG_EXTENSION_NAME)) edit->extensions.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
                antiLagFeatures = true;
            }
        }
        // Features: set the bits in the game's own structures where it passes them (its values are only raised),
        // add the individual structures it does not.
        edit->info = *info;
        edit->info.queueCreateInfoCount = uint32_t(edit->queues.size());
        edit->info.pQueueCreateInfos = edit->queues.data();
        edit->info.enabledExtensionCount = uint32_t(edit->extensions.size());
        edit->info.ppEnabledExtensionNames = edit->extensions.data();
        const void* chain = info->pNext;
        if (auto* f2 = FindStruct<VkPhysicalDeviceFeatures2>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))
            f2->features.shaderStorageImageExtendedFormats = VK_TRUE;
        else {
            if (info->pEnabledFeatures) edit->features10 = *info->pEnabledFeatures;
            edit->features10.shaderStorageImageExtendedFormats = VK_TRUE;
            edit->info.pEnabledFeatures = &edit->features10;
        }
        void* head = const_cast<void*>(info->pNext);
        auto prepend = [&](auto& structure) {
            structure.pNext = head;
            head = &structure;
        };
        if (auto* f12 = FindStruct<VkPhysicalDeviceVulkan12Features>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))
            f12->timelineSemaphore = VK_TRUE;
        else if (auto* t = FindStruct<VkPhysicalDeviceTimelineSemaphoreFeatures>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES))
            t->timelineSemaphore = VK_TRUE;
        else { edit->timeline.timelineSemaphore = VK_TRUE; prepend(edit->timeline); }
        if (auto* f13 = FindStruct<VkPhysicalDeviceVulkan13Features>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
            f13->synchronization2 = VK_TRUE;
            f13->maintenance4 = VK_TRUE;
            f13->shaderIntegerDotProduct = VK_TRUE;
        } else {
            if (auto* s = FindStruct<VkPhysicalDeviceSynchronization2Features>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES))
                s->synchronization2 = VK_TRUE;
            else { edit->sync2.synchronization2 = VK_TRUE; prepend(edit->sync2); }
            if (auto* m = FindStruct<VkPhysicalDeviceMaintenance4Features>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES))
                m->maintenance4 = VK_TRUE;
            else { edit->maintenance4.maintenance4 = VK_TRUE; prepend(edit->maintenance4); }
            if (auto* d = FindStruct<VkPhysicalDeviceShaderIntegerDotProductFeatures>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES))
                d->shaderIntegerDotProduct = VK_TRUE;
            else { edit->dot.shaderIntegerDotProduct = VK_TRUE; prepend(edit->dot); }
        }
        if (reflexFeatures) {
            if (auto* p = FindStruct<VkPhysicalDevicePresentIdFeaturesKHR>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR))
                p->presentId = VK_TRUE;
            else { edit->presentId.presentId = VK_TRUE; prepend(edit->presentId); }
            edit->presentIds = true;
        }
        // MFGAdaUnlock 1.7: NVIDIA's present metering for the layer's swap chain, where the driver offers it.
        if (gAddonPresentMetering.load() && offered(VK_NV_PRESENT_METERING_EXTENSION_NAME)) {
            VkPhysicalDevicePresentMeteringFeaturesNV metering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV};
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &metering};
            features2(physical, &query);
            if (metering.presentMetering) {
                if (!enabled(VK_NV_PRESENT_METERING_EXTENSION_NAME)) edit->extensions.push_back(VK_NV_PRESENT_METERING_EXTENSION_NAME);
                if (auto* m = FindStruct<VkPhysicalDevicePresentMeteringFeaturesNV>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV))
                    m->presentMetering = VK_TRUE;
                else { edit->metering.presentMetering = VK_TRUE; prepend(edit->metering); }
                edit->presentMetering = true;
            }
        }
        if (antiLagFeatures) {
            if (auto* a = FindStruct<VkPhysicalDeviceAntiLagFeaturesAMD>(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD))
                a->antiLag = VK_TRUE;
            else { edit->antiLag.antiLag = VK_TRUE; prepend(edit->antiLag); }
        }
        edit->info.pNext = head;
        edit->physical = physical;
        // presentqueue.1: the game's queues (family, count, create flags, family flags, presentation support).
        std::string gameQueues;
        for (uint32_t i = 0; i < info->queueCreateInfoCount && i < 16; ++i) {
            const auto& q = info->pQueueCreateInfos[i];
            const bool known = q.queueFamilyIndex < families.size();
            gameQueues += std::string(i ? "," : "") + "[" + std::to_string(q.queueFamilyIndex) + "," + std::to_string(q.queueCount) + "," +
                          std::to_string(q.flags) + "," + std::to_string(known ? families[q.queueFamilyIndex].queueFlags : 0) + "," +
                          (known && presentation(physical, q.queueFamilyIndex) ? "true" : "false") + "]";
        }
        Log("vk_device_prepared", "\"family\":" + std::to_string(edit->family) + ",\"first_queue\":" + std::to_string(edit->firstQueue) +
            ",\"queues\":" + std::to_string(edit->queueCount) + ",\"vendor\":" + std::to_string(edit->vendor) +
            ",\"calibrated\":" + (edit->calibrated ? "true" : "false") + ",\"game_queues\":[" + gameQueues + "]" +
            ",\"latency_requested\":" + std::to_string(latency) + ",\"present_ids\":" + (edit->presentIds ? "true" : "false") +
            ",\"anti_lag\":" + (antiLagFeatures ? "true" : "false") + ",\"present_metering\":" + (edit->presentMetering ? "true" : "false"));
        return edit;
    } catch (...) {
        Refuse("prepare_exception");
        return nullptr;
    }
}
} // namespace

const VkDeviceCreateInfo* EditedInfo(const DeviceEdit* edit, const VkDeviceCreateInfo* original) noexcept
{
    return edit ? &edit->info : original;
}

namespace {
bool DeviceCreatedImpl(DeviceEditPtr edit, VkDevice device, hosted::OriginalProc originals, void* user) noexcept;
}

void DeviceCreated(DeviceEditPtr edit, VkDevice device) noexcept
{
    DeviceCreatedImpl(std::move(edit), device, nullptr, nullptr);
}

namespace {
bool DeviceCreatedImpl(DeviceEditPtr edit, VkDevice device, hosted::OriginalProc originals, void* user) noexcept
{
    if (!edit || !device) return false;
    try {
        auto host = std::make_shared<HostDevice>();
        host->hosted = edit->hosted;
        host->originals = originals;
        host->originalsUser = user;
        if (edit->hosted && originals) {
            gHostOriginalsUser.store(user);
            gHostOriginals.store(originals);
        }
        host->device = device;
        host->physical = edit->physical;
        host->instance = edit->instance;
        host->gipa = edit->gipa;
        host->family = edit->family;
        host->firstQueue = edit->firstQueue;
        host->queueCount = edit->queueCount;
        host->calibrated = edit->calibrated;
        host->presentIds = edit->presentIds;
        host->presentMetering = edit->presentMetering;
        host->vendor = edit->vendor;
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(edit->gipa(edit->instance, "vkGetDeviceProcAddr"));
        bool complete = gdpa != nullptr;
#define MFG_LOAD(name) host->vk.name = complete ? reinterpret_cast<PFN_##name>(gdpa(device, #name)) : nullptr; complete &= host->vk.name != nullptr;
        MFG_HOST_DEVICE_FNS(MFG_LOAD)
#undef MFG_LOAD
        if (!complete) { Refuse("device_functions"); return false; }
        std::lock_guard lock(gMutex);
        for (const auto& [family, count] : edit->gameQueues)
            for (uint32_t i = 0; i < count && gQueues.size() < 256; ++i) {
                VkQueue queue = VK_NULL_HANDLE;
                host->vk.vkGetDeviceQueue(device, family, i, &queue);
                if (queue) gQueues[queue] = {device, family};
            }
        gDevices[device] = std::move(host);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

void DeviceDestroying(VkDevice device) noexcept
{
    try {
        std::shared_ptr<HostDevice> host;
        std::vector<std::shared_ptr<FakeChain>> chains;
        {
            std::lock_guard lock(gMutex);
            auto found = gDevices.find(device);
            if (found == gDevices.end()) return;
            host = found->second;
            gDevices.erase(found);
            for (auto it = gFakes.begin(); it != gFakes.end();) {
                if (it->second->owner == host) { chains.push_back(it->second); it = gFakes.erase(it); }
                else ++it;
            }
            std::erase_if(gNatives, [&](const auto& row) { return row.second.device == device; });
            std::erase_if(gQueues, [&](const auto& row) { return row.second.first == device; });
        }
        for (auto& chain : chains) {
            std::lock_guard lock(chain->mutex);
            if (!chain->retired) ReleaseFake(*chain);
        }
        std::lock_guard lock(host->mutex);
        if (host->layer) host->layer->WaitIdle(2'000'000'000ull);
        for (auto& slot : host->slots) { slot.depth.Reset(); slot.motion.Reset(); }
        if (host->copyPool) host->vk.vkDestroyCommandPool(device, host->copyPool, nullptr);   // MFGAdaUnlock 1.7
        for (VkPipeline pipeline : host->pipelines) if (pipeline) host->vk.vkDestroyPipeline(device, pipeline, nullptr);
        if (host->pipelineLayout) host->vk.vkDestroyPipelineLayout(device, host->pipelineLayout, nullptr);
        if (host->setLayout) host->vk.vkDestroyDescriptorSetLayout(device, host->setLayout, nullptr);
        if (host->sampler) host->vk.vkDestroySampler(device, host->sampler, nullptr);
        if (host->xell) { xess_mod::VulkanHostCallScope scope; gRuntime.xellDestroyContext(host->xell); }
        host->xell = nullptr;
        host->bridge.Reset();
        host->appQueue.Reset();
        host->factory.Reset();
        if (host->d3d) xess_mod::SetVulkanHostDevice(nullptr);
        host->layer = nullptr;
        host->d3d.Reset();
    } catch (...) {}
}

bool DeviceWaitIdle(VkDevice device, VkResult& result) noexcept
{
    try {
        auto host = FindDevice(device);
        if (!host) return false;
        ComPtr<ID3D12Device> layer;
        {
            std::lock_guard lock(host->mutex);
            layer = host->d3d;
        }
        if (!layer) return false;   // no layer queue was ever used: the driver's own wait applies
        std::vector<VkQueue> queues;
        {
            std::lock_guard lock(gMutex);
            for (const auto& [queue, owner] : gQueues) if (owner.first == device) queues.push_back(queue);
        }
        result = VK_SUCCESS;
        for (VkQueue queue : queues) {
            const VkResult waited = host->vk.vkQueueWaitIdle(queue);
            if (waited == VK_ERROR_DEVICE_LOST) result = waited;
        }
        if (!static_cast<xvk::Device*>(static_cast<ID3D12Device1*>(layer.Get()))->WaitIdle(2'000'000'000ull))
            Log("vk_device_wait_idle", "\"layer_idle\":false");
        return true;
    } catch (...) {
        return false;
    }
}

void QueueSeen(VkDevice device, uint32_t family, uint32_t, VkQueue queue) noexcept
{
    try {
        std::lock_guard lock(gMutex);
        if (gDevices.contains(device) && gQueues.size() < 256) gQueues[queue] = {device, family};
    } catch (...) {}
}

// ---------------------------------------------------------------- swap chains
bool OwnsSwapchain(VkSwapchainKHR swapchain) noexcept
{
    try {
        std::lock_guard lock(gMutex);
        auto found = gFakes.find(swapchain);
        return found != gFakes.end() && !found->second->hosted;   // hosted chains are OptiScaler's to route
    } catch (...) {
        return false;
    }
}

namespace {
// A call on a hosted device or chain: RTXMFG's own wrappers pass it on, it reaches the host through OptiScaler.
bool HostedHandle(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(gMutex);
    if (auto found = gFakes.find(swapchain); found != gFakes.end()) return found->second->hosted;
    auto host = gDevices.find(device);
    return host != gDevices.end() && host->second->hosted;
}
}

bool CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator,
                     VkSwapchainKHR* output, PFN_vkCreateSwapchainKHR original, VkResult& result) noexcept
{
    if (!info || !output || !original) return false;
    try {
        auto host = FindDevice(device);
        if (!host || host->hosted) return false;   // OptiScaler routes a hosted device's chains (namespace hosted)
        HWND window = nullptr;
        {
            std::lock_guard lock(gMutex);
            if (auto surface = gSurfaces.find(info->surface); surface != gSurfaces.end()) window = surface->second.window;
        }
        std::shared_ptr<FakeChain> previous = info->oldSwapchain ? FindFake(info->oldSwapchain) : nullptr;
        std::string reason;
        bool take = Eligible(*host, *info, window, reason);
        if (take) {
            std::lock_guard lock(host->mutex);
            take = EnsureLayer(*host, reason);
        }
        if (previous) {
            // The game recreates a taken-over chain (resize, or the host gave it back): XeFG's chain goes first,
            // so the window is free for the next one.
            std::lock_guard lock(previous->mutex);
            if (!previous->retired) ReleaseFake(*previous);
        }
        if (!take) {
            NoteReason(reason);
            VkSwapchainCreateInfoKHR native = *info;
            if (previous) native.oldSwapchain = VK_NULL_HANDLE;
            result = original(device, &native, allocator, output);
            if (result == VK_SUCCESS && *output) {
                NativeChain record;
                record.device = device;
                record.info = *info;
                record.info.pNext = nullptr;
                record.window = window;
                std::lock_guard lock(gMutex);
                if (gNatives.size() < 64) gNatives[*output] = record;
            }
            return true;
        }
        std::shared_ptr<FakeChain> chain;
        bool retired = false;
        {
            std::lock_guard lock(host->mutex);
            chain = CreateFake(host, *info, window, previous ? VK_NULL_HANDLE : info->oldSwapchain, host->lastInputs.depthInverted, reason,
                               retired);
        }
        if (!chain) {
            NoteReason(reason);
            Log("vk_takeover_failed", "\"reason\":\"" + Escape(reason) + "\",\"game_chain_retired\":" + (retired ? "true" : "false"));
            // Fail closed: the game gets a native chain.
            VkSwapchainCreateInfoKHR native = *info;
            if (previous || retired) native.oldSwapchain = VK_NULL_HANDLE;
            result = original(device, &native, allocator, output);
            return true;
        }
        const auto handle = reinterpret_cast<VkSwapchainKHR>(chain.get());
        {
            std::lock_guard lock(gMutex);
            gFakes[handle] = chain;
            if (info->oldSwapchain) gNatives.erase(info->oldSwapchain);
            host->active = chain;
        }
        NoteReason("active");
        *output = handle;
        result = VK_SUCCESS;
        return true;
    } catch (...) {
        return false;
    }
}

bool DestroySwapchainImpl(VkDevice device, VkSwapchainKHR swapchain) noexcept
{
    try {
        std::shared_ptr<FakeChain> chain;
        {
            std::lock_guard lock(gMutex);
            gNatives.erase(swapchain);
            auto found = gFakes.find(swapchain);
            if (found != gFakes.end()) {
                chain = found->second;
                gFakes.erase(found);
            }
        }
        if (!chain) {
            // The game's old chain, destroyed right after the takeover returned: the layer chain names it as
            // oldSwapchain at its first present, so it stays alive until then.
            auto host = FindDevice(device);
            if (!host) return false;
            ComPtr<ID3D12Device> layer;
            {
                std::lock_guard lock(host->mutex);
                layer = host->d3d;
            }
            return layer && static_cast<xvk::Device*>(static_cast<ID3D12Device1*>(layer.Get()))->DeferRetiredDestroy(swapchain);
        }
        std::lock_guard lock(chain->mutex);
        if (!chain->retired) ReleaseFake(*chain);
        return true;
    } catch (...) {
        return true;
    }
}

bool GetSwapchainImagesImpl(VkSwapchainKHR swapchain, uint32_t* count, VkImage* images, VkResult& result) noexcept
{
    auto chain = FindFake(swapchain);
    if (!chain) return false;
    std::lock_guard lock(chain->mutex);
    const uint32_t available = uint32_t(chain->images.size());
    if (!count) { result = VK_ERROR_INITIALIZATION_FAILED; return true; }
    if (!images) { *count = available; result = VK_SUCCESS; return true; }
    const uint32_t copied = std::min(*count, available);
    std::copy_n(chain->images.begin(), copied, images);
    *count = copied;
    result = copied < available ? VK_INCOMPLETE : VK_SUCCESS;
    return true;
}

// ---------------------------------------------------------------- vkreflex.1: the host's low-latency output
constexpr uint64_t kLatencySleepTimeoutNs = 500'000'000ull;
constexpr const char* kLatencyNames[] = {"xell", "reflex", "antilag2"};

// The game's next frame sleeps in the host's Reflex (on the layer's chain) or Anti-Lag 2 (on the device), otherwise in
// XeLL: also before the layer's first chain exists, without the driver's functions, or when the host asks for XeLL.
// XeLL's markers stay in every mode (XeFG paces from them). The FPS limit counts presented frames; the sleep paces
// base frames, so it is divided by the frames XeFG presents per base frame.
void LatencySleep(HostDevice& host, FakeChain& chain, uint32_t frameId)
{
    MfgXeFGVkLatency latency;
    {
        std::lock_guard lock(gLatencyMutex);
        latency = gLatency;
    }
    const uint32_t perBase = chain.enabledApplied ? std::max<uint32_t>(chain.framesApplied, 1) : 1;
    // xemfgsa.5: without OptiScaler's latency output, XeMFG standalone's limit (the menu's FPS limit); xemfgsa.6: or
    // Dynamic MFG's target with "Use target as cap".
    const double capFps = chain.dynamic.CapFps();
    const uint32_t standaloneFps = chain.hosted || !gStandalone.load() ? 0
        : capFps > 0.0 ? uint32_t(capFps + 0.5) : gStandaloneFpsLimit.load();
    const float limit = latency.outputFpsLimit > 0.0f ? latency.outputFpsLimit : float(standaloneFps);
    xvk::ReflexMode reflex;
    reflex.on = latency.mode == MFG_XEFG_VK_LATENCY_REFLEX;
    if (reflex.on) {
        reflex.lowLatency = latency.lowLatency != 0;
        reflex.boost = latency.boost != 0;
        reflex.minimumIntervalUs = limit > 0.0f ? uint32_t(std::lround(1e6 * perBase / limit)) : 0;
    }
    if (!(reflex == host.reflex)) {
        xvk::SetReflexMode(host.d3d.Get(), reflex);
        host.reflex = reflex;
    }
    uint32_t used = MFG_XEFG_VK_LATENCY_XELL;
    if (reflex.on && xvk::ReflexSleep(chain.window, kLatencySleepTimeoutNs)) {
        used = MFG_XEFG_VK_LATENCY_REFLEX;
    } else if (latency.mode == MFG_XEFG_VK_LATENCY_ANTILAG2) {
        chain.antiLagMaxFps = limit > 0.0f ? uint32_t(limit / float(perBase)) : 0;
        if (xvk::AntiLagUpdate(host.d3d.Get(), latency.lowLatency != 0, chain.antiLagMaxFps, VK_ANTI_LAG_STAGE_INPUT_AMD, frameId))
            used = MFG_XEFG_VK_LATENCY_ANTILAG2;
    }
    bool xellLimited = false;
    if (used == MFG_XEFG_VK_LATENCY_XELL) {
        xess_mod::VulkanHostCallScope scope;
        // xemfgsa.5: a standalone chain's FPS limit as XeLL's minimum interval, which XeLL holds for the frames XeFG
        // presents (OptiScaler's chains keep their own XeLL setup).
        if (!chain.hosted) {
            const uint32_t interval = xemfg_standalone::LimitIntervalUs(standaloneFps);
            if (interval != host.xellIntervalUs) {
                xell_sleep_params_t sleep{};
                sleep.bLowLatencyMode = 1;
                sleep.minimumIntervalUs = interval;
                const auto result = gRuntime.xellSetSleepMode(host.xell, &sleep);
                if (result == XELL_RESULT_SUCCESS) host.xellIntervalUs = interval;
                Log("vk_fps_limit", "\"fps\":" + std::to_string(standaloneFps) + ",\"per_base\":" + std::to_string(perBase) +
                    ",\"interval_us\":" + std::to_string(interval) + ",\"result\":" + std::to_string(int(result)));
            }
            xellLimited = host.xellIntervalUs != 0;
        }
        gRuntime.xellSleep(host.xell, frameId);
    }
    chain.latencyUsed = used;
    ++chain.latencyFrames[used];
    gLatencyUsed = used;
    gLatencyLimited = (used != MFG_XEFG_VK_LATENCY_XELL && limit > 0.0f) || xellLimited;
    if (used != chain.latencyLogged || (frameId % 1200) == 0) {
        chain.latencyLogged = used;
        Log("vk_latency", std::string("\"frame\":") + std::to_string(frameId) + ",\"requested\":\"" +
            kLatencyNames[std::min<uint32_t>(latency.mode, 2)] + "\",\"used\":\"" + kLatencyNames[used] + "\",\"low_latency\":" +
            std::to_string(latency.lowLatency) + ",\"boost\":" + std::to_string(latency.boost) + ",\"fps_limit\":" +
            std::to_string(limit) + ",\"per_base\":" + std::to_string(perBase) + ",\"reflex_interval_us\":" +
            std::to_string(reflex.minimumIntervalUs) + ",\"anti_lag_max_fps\":" + std::to_string(chain.antiLagMaxFps) +
            ",\"frames\":[" + std::to_string(chain.latencyFrames[0]) + "," + std::to_string(chain.latencyFrames[1]) + "," +
            std::to_string(chain.latencyFrames[2]) + "]");
    }
}

bool AcquireNextImageImpl(VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index,
                      VkResult& result) noexcept
{
    auto chain = FindFake(swapchain);
    if (!chain) return false;
    try {
        std::lock_guard lock(chain->mutex);
        HostDevice& host = *chain->owner;
        if (chain->retired || chain->broken || !chain->proxy) { result = VK_ERROR_OUT_OF_DATE_KHR; return true; }
        if (chain->acquired >= 0) {
            // One back buffer at a time (DXGI's model): a second acquisition before presenting gives the chain back.
            // MFGAdaUnlock 1.7.1: for the session, as presentqueue.1: the game would acquire ahead again on a new chain.
            chain->broken = true;
            {
                std::lock_guard hostLock(host.mutex);
                host.failure = "second_acquire";
            }
            NoteReason("second_acquire");
            Log("vk_takeover_released", "\"reason\":\"second_acquire\"");
            result = VK_ERROR_OUT_OF_DATE_KHR;
            return true;
        }
        const uint32_t frameId = chain->frameId + 1;
        // The frame start's sleep (XeLL, or the host's Reflex / Anti-Lag 2: vkreflex.1), then XeFG's frame-latency
        // waitable. xemfgsa.6: both timed, as waits Dynamic MFG reads.
        const int64_t sleepStart = Now();
        FakeChain::AcquireWait waiting{*chain};
        bool gameSlept = false;   // MFGAdaUnlock 1.7.1
        {
            std::lock_guard markers(chain->gameMarkers);
            gameSlept = chain->gameSleptId == frameId;
        }
        gGameLatencyUsed = gameSlept;
        if (!gameSlept) LatencySleep(host, *chain, frameId);
        const int64_t waitStart = Now();
        chain->sleepMs = Milliseconds(waitStart - sleepStart);
        const DWORD wait = timeout == UINT64_MAX ? 2000 : DWORD(std::min<uint64_t>(timeout / 1'000'000ull, 2000));
        struct WaitTimer { FakeChain& chain; int64_t start; ~WaitTimer() { chain.waitableMs = Milliseconds(Now() - start); } }
            waitTimer{*chain, waitStart};
        if (chain->latencyHeld) chain->latencyHeld = false;   // a released acquisition's count (maintenance1)
        else if (WaitForSingleObject(chain->latency, wait) == WAIT_TIMEOUT) {
            if (timeout != UINT64_MAX) { result = timeout ? VK_TIMEOUT : VK_NOT_READY; return true; }
            static std::atomic<int> logged{0};
            if (logged++ < 8) Log("vk_latency_wait_timeout", "\"frame\":" + std::to_string(chain->frameId));
        }
        const UINT current = chain->proxy->GetCurrentBackBufferIndex();
        if (current >= chain->images.size()) { chain->broken = true; result = VK_ERROR_OUT_OF_DATE_KHR; return true; }
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, semaphore, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const HRESULT hr = xvk::Own<xvk::CommandQueue>(host.appQueue.Get())->SubmitHost(chain->toPresent[current], nullptr, 0,
                                                                                         semaphore ? &signal : nullptr, semaphore ? 1 : 0, fence);
        if (FAILED(hr)) { chain->broken = true; result = VK_ERROR_DEVICE_LOST; return true; }
        chain->acquired = int(current);
        *index = current;
        chain->frameId = frameId;
        host.lastFrameId = frameId;
        {
            xess_mod::VulkanHostCallScope scope;
            bool simStart = false, simEnd = false;   // MFGAdaUnlock 1.7.1: those the game already gave
            {
                std::lock_guard markers(chain->gameMarkers);
                if (chain->gameSleptId == frameId) { simStart = chain->gameSimStart; simEnd = chain->gameSimEnd; }
                chain->gameMarkersOpen = false;
            }
            if (!simStart) gRuntime.xellAddMarkerData(host.xell, chain->frameId, XELL_SIMULATION_START);
            if (!simEnd) gRuntime.xellAddMarkerData(host.xell, chain->frameId, XELL_SIMULATION_END);
            gRuntime.xellAddMarkerData(host.xell, chain->frameId, XELL_RENDERSUBMIT_START);
        }
        if (chain->latencyUsed == MFG_XEFG_VK_LATENCY_REFLEX)
            for (VkLatencyMarkerNV marker : {VK_LATENCY_MARKER_SIMULATION_START_NV, VK_LATENCY_MARKER_SIMULATION_END_NV,
                                             VK_LATENCY_MARKER_RENDERSUBMIT_START_NV})
                xvk::ReflexMarker(chain->window, chain->frameId, marker);
        result = VK_SUCCESS;
        return true;
    } catch (...) {
        result = VK_ERROR_OUT_OF_DATE_KHR;
        return true;
    }
}

// MFGAdaUnlock 1.7.1: only between frames (the previous one presented, this one not acquired): XeLL's sleep may wait for
// the previous frame's present, which needs this chain's mutex. Any other order leaves the sleep to the acquisition, so
// each frame sleeps exactly once and XeLL's ids stay consecutive.
bool GameLatencySleepImpl(VkSwapchainKHR swapchain) noexcept
{
    if (!gAddonGameLatency.load()) return false;
    auto chain = FindFake(swapchain);
    if (!chain) return false;
    try {
        std::lock_guard lock(chain->mutex);
        if (chain->retired || chain->broken || !chain->proxy || chain->acquired >= 0) return false;
        const uint32_t frameId = chain->frameId + 1;
        {
            std::lock_guard markers(chain->gameMarkers);
            if (chain->gameSleptId == frameId) return true;
        }
        {
            FakeChain::AcquireWait waiting{*chain};
            LatencySleep(*chain->owner, *chain, frameId);
        }
        std::lock_guard markers(chain->gameMarkers);
        chain->gameSleptId = frameId;
        chain->gameMarkersOpen = true;
        chain->gameSimStart = chain->gameSimEnd = false;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) Log("vk_game_latency", "\"frame\":" + std::to_string(frameId));
        return true;
    } catch (...) {
        return false;
    }
}

void GameLatencyMarkerImpl(VkSwapchainKHR swapchain, uint32_t marker) noexcept
{
    if (!gAddonGameLatency.load()) return;
    auto chain = FindFake(swapchain);
    if (!chain) return;
    try {
        std::lock_guard markers(chain->gameMarkers);
        if (!chain->gameMarkersOpen) return;
        xess_mod::VulkanHostCallScope scope;
        auto& xell = chain->owner->xell;
        if (marker == VK_LATENCY_MARKER_SIMULATION_START_NV && !chain->gameSimStart) {
            gRuntime.xellAddMarkerData(xell, chain->gameSleptId, XELL_SIMULATION_START);
            chain->gameSimStart = true;
        } else if (marker == VK_LATENCY_MARKER_SIMULATION_END_NV && chain->gameSimStart && !chain->gameSimEnd) {
            gRuntime.xellAddMarkerData(xell, chain->gameSleptId, XELL_SIMULATION_END);
            chain->gameSimEnd = true;
        }
    } catch (...) {}
}

bool SetHdrMetadataImpl(VkSwapchainKHR swapchain, const void* metadata) noexcept
{
    auto chain = FindFake(swapchain);
    if (!chain) return false;
    try {
        const auto& source = *static_cast<const VkHdrMetadataEXT*>(metadata);
        DXGI_HDR_METADATA_HDR10 hdr{};
        auto point = [](const VkXYColorEXT& c, UINT16 out[2]) {
            out[0] = UINT16(std::clamp(c.x, 0.0f, 1.0f) * 50000.0f);
            out[1] = UINT16(std::clamp(c.y, 0.0f, 1.0f) * 50000.0f);
        };
        point(source.displayPrimaryRed, hdr.RedPrimary);
        point(source.displayPrimaryGreen, hdr.GreenPrimary);
        point(source.displayPrimaryBlue, hdr.BluePrimary);
        point(source.whitePoint, hdr.WhitePoint);
        hdr.MaxMasteringLuminance = UINT(source.maxLuminance);
        hdr.MinMasteringLuminance = UINT(source.minLuminance * 10000.0f);
        hdr.MaxContentLightLevel = UINT16(source.maxContentLightLevel);
        hdr.MaxFrameAverageLightLevel = UINT16(source.maxFrameAverageLightLevel);
        std::lock_guard lock(chain->mutex);
        if (chain->proxy) chain->proxy->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr);
    } catch (...) {}
    return true;
}

namespace { VkCommandBuffer DeferredCopy(HostDevice& host, InputSlot& slot); }   // MFGAdaUnlock 1.7

bool QueuePresentImpl(VkQueue queue, const VkPresentInfoKHR* info, VkResult& result) noexcept
{
    if (!info || !info->swapchainCount || !info->pSwapchains || !info->pImageIndices) return false;
    auto chain = FindFake(info->pSwapchains[0]);
    if (!chain) {
        for (uint32_t i = 1; i < info->swapchainCount; ++i)
            if (OwnsSwapchain(info->pSwapchains[i])) { result = VK_ERROR_OUT_OF_DATE_KHR; return true; }
        return false;
    }
    // VK_KHR/EXT_swapchain_maintenance1: the game's present fence, signaled once its wait semaphores are consumed.
    VkFence presentFence = VK_NULL_HANDLE;
    if (auto* fences = FindStruct<VkSwapchainPresentFenceInfoKHR>(info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR);
        fences && fences->swapchainCount && fences->pFences)
        presentFence = fences->pFences[0];
    bool waitsConsumed = false;   // the bridge batch took the game's semaphores
    auto finish = [&](VkResult value) {
        if (!waitsConsumed && (presentFence || info->waitSemaphoreCount)) {
            // Like a rejected present: its semaphore waits still execute (presentqueue.1: also without a present
            // fence, so the game's binary semaphores are unsignaled again) and its fence is signaled after them.
            std::vector<VkSemaphoreSubmitInfo> waits;
            for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
                waits.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, info->pWaitSemaphores[i], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit.waitSemaphoreInfoCount = uint32_t(waits.size());
            submit.pWaitSemaphoreInfos = waits.data();
            chain->owner->vk.vkQueueSubmit2(queue, 1, &submit, presentFence);
            presentFence = VK_NULL_HANDLE;
            waitsConsumed = true;
        }
        result = value;
        if (info->pResults) std::fill_n(info->pResults, info->swapchainCount, value);
        return true;
    };
    if (info->swapchainCount != 1) return finish(VK_ERROR_OUT_OF_DATE_KHR);
    try {
        std::lock_guard lock(chain->mutex);
        HostDevice& host = *chain->owner;
        Runtime& rt = gRuntime;
        const uint32_t index = info->pImageIndices[0];
        if (chain->retired || chain->broken || chain->acquired < 0 || index != uint32_t(chain->acquired)) {
            chain->broken = true;
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        }
        // presentqueue.1: any queue of the device may present (idTech presents from an out-of-band queue of another
        // family); one the host does not know keeps the game's own chains for the rest of the session. Before, both
        // only released the chain, which the game recreated and the host took over again at once: a loop with no
        // frame on screen (Doom: The Dark Ages, 22 Oct).
        uint32_t presentFamily = UINT32_MAX;
        bool knownQueue = false;
        {
            std::lock_guard queues(gMutex);
            auto found = gQueues.find(queue);
            knownQueue = found != gQueues.end() && found->second.first == host.device;
            if (knownQueue) presentFamily = found->second.second;
        }
        const bool foreign = knownQueue && presentFamily != host.family;
        if (!knownQueue || (foreign && !EnsureForeignFamily(*chain, presentFamily))) {
            const char* fault = !knownQueue ? "present_queue_unknown" : "present_queue_family";
            chain->broken = true;
            {
                std::lock_guard hostLock(host.mutex);
                host.failure = fault;
            }
            NoteReason(fault);
            char handle[32];
            snprintf(handle, sizeof(handle), "%p", static_cast<void*>(queue));
            Log("vk_takeover_released", std::string("\"reason\":\"") + fault + "\",\"queue\":\"" + handle + "\",\"family\":" +
                (knownQueue ? std::to_string(presentFamily) : std::string("null")) + ",\"host_family\":" + std::to_string(host.family) +
                ",\"chain_family\":" + (chain->foreignFamily == UINT32_MAX ? std::string("null") : std::to_string(chain->foreignFamily)));
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        }
        if (foreign && chain->presents == 0)
            Log("vk_present_queue", "\"family\":" + std::to_string(presentFamily) + ",\"host_family\":" + std::to_string(host.family) +
                ",\"ownership_transfer\":true");
        // Game queue: its semaphores, the image to GENERAL, then the bridge value the application queue waits for.
        uint64_t bridgeValue = 0;
        VkCommandBuffer inputCopy = VK_NULL_HANDLE;   // MFGAdaUnlock 1.7: the frame's deferred input copy
        {
            std::lock_guard bridgeLock(host.mutex);
            bridgeValue = ++host.bridgeValue;
            if (InputSlot* slot = SlotFor(host, chain->presents + 1); slot && slot->deferred) {
                slot->deferred = false;
                inputCopy = foreign ? VK_NULL_HANDLE : DeferredCopy(host, *slot);
                if (inputCopy) host.copyValues[(host.copyNext + host.copyCommands.size() - 1) % host.copyCommands.size()] = bridgeValue;
                else slot->frame = 0;   // not tagged: XeFG presents this frame without generated ones
            }
        }
        auto* bridge = xvk::Own<xvk::Fence>(host.bridge.Get());
        bridge->ClaimSignal(bridgeValue);
        std::vector<VkSemaphoreSubmitInfo> waits;
        for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
            waits.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, info->pWaitSemaphores[i], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, bridge->semaphore(), bridgeValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        // Another family presents: its queue releases the image to the host family (the application queue acquires
        // it below).
        VkCommandBufferSubmitInfo commands[2] = {
            {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, inputCopy},
            {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, foreign ? chain->release[index] : chain->toGeneral[index]}};
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = uint32_t(waits.size());
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = inputCopy ? 2 : 1;
        submit.pCommandBufferInfos = inputCopy ? commands : commands + 1;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        // The bridge batch consumes the game's semaphores: the present fence (maintenance1) is signaled with it.
        const VkResult submitted = host.vk.vkQueueSubmit2(queue, 1, &submit, presentFence);
        presentFence = VK_NULL_HANDLE;
        waitsConsumed = true;
        if (submitted != VK_SUCCESS) {
            chain->broken = true;
            Log("vk_takeover_released", "\"reason\":\"bridge_submit\",\"result\":" + std::to_string(int(submitted)));
            return finish(submitted == VK_ERROR_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DATE_KHR);
        }
        chain->lastBridge = bridgeValue;
        host.appQueue->Wait(host.bridge.Get(), bridgeValue);
        if (foreign &&
            FAILED(xvk::Own<xvk::CommandQueue>(host.appQueue.Get())->SubmitHost(chain->acquireForeign[index], nullptr, 0, nullptr, 0, VK_NULL_HANDLE))) {
            chain->broken = true;
            Log("vk_takeover_released", "\"reason\":\"acquire_submit\"");
            return finish(VK_ERROR_DEVICE_LOST);
        }
        chain->acquired = -1;
        const uint64_t baseFrame = chain->presents + 1;
        const uint32_t frameId = chain->frameId;
        const int64_t now = Now();
        const float frameMs = float(Milliseconds(now - chain->lastPresent));
        chain->lastPresent = now;
        bool tagged = false;
        {
            xess_mod::VulkanHostCallScope scope;
            InputSlot* slot = nullptr;
            {
                std::lock_guard inputs(host.mutex);
                slot = SlotFor(host, baseFrame);
            }
            if (slot && gAddonGenerate.load()) {   // MFGAdaUnlock 1.7: field isolation ([XeFG] VulkanGenerate=0)
                const DlssInputs& in = slot->inputs;
                xefg_swapchain_d3d12_resource_data_t depth{};
                depth.type = XEFG_SWAPCHAIN_RES_DEPTH;
                depth.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
                depth.resourceSize = {in.renderWidth, in.renderHeight};
                depth.pResource = slot->depth.Get();
                depth.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                xefg_swapchain_d3d12_resource_data_t motion = depth;
                motion.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
                motion.resourceSize = {in.renderWidth, in.renderHeight};   // the host's copy (RecordDlssInputs)
                motion.pResource = slot->motion.Get();
                xefg_swapchain_frame_constant_data_t constants{};
                const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                memcpy(constants.viewMatrix, identity, sizeof(identity));
                Projection(float(chain->info.imageExtent.width) / float(chain->info.imageExtent.height), in, constants.projectionMatrix);
                constants.jitterOffsetX = in.jitterX;
                constants.jitterOffsetY = in.jitterY;
                // Output-size motion vectors were resampled to render size: their pixels become render pixels.
                constants.motionVectorScaleX = in.motionScaleX * float(in.renderWidth) / float(in.motionWidth);
                constants.motionVectorScaleY = in.motionScaleY * float(in.renderHeight) / float(in.motionHeight);
                constants.resetHistory = in.reset ? 1 : 0;
                constants.frameRenderTime = in.frameTimeMs > 0 ? in.frameTimeMs : frameMs;
                tagged = rt.xefgSwapChainD3D12TagFrameResource(chain->fg, nullptr, frameId, &depth) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
                         rt.xefgSwapChainD3D12TagFrameResource(chain->fg, nullptr, frameId, &motion) == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
                         rt.xefgSwapChainTagFrameConstants(chain->fg, frameId, &constants) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
            }
            // Hosted chains: OptiScaler's FG switch and frame count (0 interpolated frames = off). XeMFG standalone
            // (xemfgsa.1): the game's own DLSS-G switch, when it drives one.
            const bool gameSwitch = chain->hosted || !gStandalone.load() || !gDlssgSeen.load() || gGameFrameGeneration.load();
            const bool canGenerate = tagged && gameSwitch && (!chain->hosted || (chain->userEnabled && chain->targetFrames >= 2));
            uint32_t frames = std::clamp<uint32_t>(chain->hosted ? chain->targetFrames : gFrames.load(), 2, 6);
            // xemfgsa.6: XeMFG standalone's Dynamic MFG picks this frame's count, up to the frames set (1 = generation
            // off; hosted chains take OptiScaler's own Dynamic MFG count above). As on the D3D12 host.
            xemfg_standalone::DynamicSettings dynamic;
            {
                std::lock_guard dynamicLock(gDynamicMutex);
                dynamic = gDynamic;
            }
            bool dynamicOff = false;
            if (!chain->hosted && gStandalone.load() && canGenerate && dynamic.enabled) {
                if (!chain->dynamicRan) chain->dynamic.Restart();
                const uint64_t tick = GetTickCount64();
                if (!chain->refreshCheckedMs || tick - chain->refreshCheckedMs >= 2000) {
                    chain->refreshCheckedMs = tick;
                    chain->refreshHz = xemfg_standalone::WindowRefreshHz(chain->window);
                }
                xemfg_standalone::DynamicFrames::Frame frame;
                frame.nowMs = Milliseconds(now);
                frame.frameMs = frameMs;
                frame.sleepMs = chain->sleepMs;
                frame.presentMs = chain->waitableMs + chain->lastPresentMs;
                frame.refreshFps = double(chain->refreshHz);
                frame.focus = xemfg_standalone::WindowFocus(chain->window);
                frame.frames = chain->dynamicOff ? 1 : chain->framesApplied ? chain->framesApplied : frames;
                dynamic.maxFrames = frames;
                dynamic.limitFps = gStandaloneFpsLimit.load();
                const uint32_t wanted = chain->dynamic.Update(frame, dynamic);
                if (chain->dynamic.Changed() || tick - chain->dynamicLogMs >= 5000) {
                    chain->dynamicLogMs = tick;
                    Log("xemfg_dynamic", "\"api\":\"vulkan\"," + chain->dynamic.Describe(frame.frames, wanted));
                }
                dynamicOff = wanted <= 1;
                if (!dynamicOff) frames = wanted;
                chain->dynamicRan = true;
            } else {
                if (!dynamic.enabled && chain->dynamic.Active()) chain->dynamic.Reset();
                chain->dynamicRan = false;
            }
            const bool enabled = canGenerate && !dynamicOff;
            // MFGAdaUnlock 1.7: the driver's present metering takes this frame's batch (not under V-Sync, as on D3D12).
            xvk::SetPresentMeteringBatch(enabled && !chain->vsync ? frames : 0);
            if (enabled != chain->enabledApplied) {
                rt.xefgSwapChainSetEnabled(chain->fg, enabled ? 1 : 0);
                chain->enabledApplied = enabled;
            }
            // Hosted 1X (Dynamic MFG turned generation off) keeps XeFG's count: nothing changes when it comes back.
            const bool keepCount = (chain->hosted && chain->targetFrames < 2) || dynamicOff;
            if (!keepCount && frames != chain->framesApplied) {
                if (rt.xefgSwapChainSetNumInterpolatedFrames(chain->fg, frames - 1) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                    chain->framesApplied = frames;
            }
            chain->dynamicOff = dynamicOff;
            if (chain->debugView != chain->debugViewApplied &&
                rt.xefgSwapChainEnableDebugFeature(chain->fg, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,
                                                   chain->debugView ? 1 : 0, nullptr) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                chain->debugViewApplied = chain->debugView;
                // presentqueue.1: in the log, as XeFG's purple bars were taken for a broken output (RDR2, 22 Oct).
                Log("vk_debug_view", std::string("\"enabled\":") + (chain->debugView ? "true" : "false"));
            }
            rt.xefgSwapChainSetPresentId(chain->fg, frameId);
            rt.xellAddMarkerData(host.xell, frameId, XELL_RENDERSUBMIT_END);
            rt.xellAddMarkerData(host.xell, frameId, XELL_PRESENT_START);
        }
        // vkreflex.1: the host's output sees the game's present of this frame (XeFG's outputs are its out-of-band presents).
        if (chain->latencyUsed == MFG_XEFG_VK_LATENCY_REFLEX) {
            xvk::ReflexMarker(chain->window, frameId, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
            xvk::ReflexMarker(chain->window, frameId, VK_LATENCY_MARKER_PRESENT_START_NV);
        } else if (chain->latencyUsed == MFG_XEFG_VK_LATENCY_ANTILAG2) {
            MfgXeFGVkLatency latency;
            {
                std::lock_guard latencyLock(gLatencyMutex);
                latency = gLatency;
            }
            xvk::AntiLagUpdate(host.d3d.Get(), latency.lowLatency != 0, chain->antiLagMaxFps, VK_ANTI_LAG_STAGE_PRESENT_AMD, frameId);
        }
        const int64_t presentStart = Now();
        const HRESULT presented = chain->proxy->Present(chain->vsync ? 1 : 0, chain->vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        chain->lastPresentMs = Milliseconds(Now() - presentStart);   // xemfgsa.6: a wait Dynamic MFG reads
        if (chain->latencyUsed == MFG_XEFG_VK_LATENCY_REFLEX) xvk::ReflexMarker(chain->window, frameId, VK_LATENCY_MARKER_PRESENT_END_NV);
        {
            xess_mod::VulkanHostCallScope scope;
            rt.xellAddMarkerData(host.xell, frameId, XELL_PRESENT_END);
            if ((chain->presents & 63) == 0) rt.xefgSwapChainGetLastPresentStatus(chain->fg, &chain->status);
        }
        ++chain->presents;
        chain->presentsDone = chain->presents;   // MFGAdaUnlock 1.7.1
        ++gBasePresents;
        {
            std::lock_guard fpsLock(gFpsMutex);   // xemfgsa.5
            gFps.Add(GetTickCount64(), gBasePresents.load(), xvk::PresentCount());
        }
        // The game's present id (VK_KHR_present_id/present_id2): its present wait on this chain returns once the
        // frame went to XeFG.
        uint64_t presentId = 0;
        if (auto* ids = FindStruct<VkPresentIdKHR>(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR); ids && ids->swapchainCount && ids->pPresentIds)
            presentId = ids->pPresentIds[0];
        if (auto* ids = FindStruct<VkPresentId2KHR>(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR); ids && ids->swapchainCount && ids->pPresentIds)
            presentId = ids->pPresentIds[0];
        if (presentId > chain->lastPresentId) {
            chain->lastPresentId = presentId;
            chain->presented.notify_all();
        }
        if (FAILED(presented)) {
            chain->broken = true;
            Log("vk_takeover_released", "\"reason\":\"proxy_present\",\"hresult\":" + std::to_string(presented));
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        }
        if (chain->presents == 1 || (chain->presents % 600) == 0)
            Log("vk_present", "\"presents\":" + std::to_string(chain->presents) + ",\"tagged\":" + (tagged ? "true" : "false") +
                ",\"frames_presented\":" + std::to_string(chain->status.framesPresented) + ",\"fg_result\":" +
                std::to_string(int(chain->status.frameGenResult)) + ",\"vulkan_presents\":" + std::to_string(xvk::PresentCount()));
        const char* fault = xvk::MissingProgramCount() != chain->missingPrograms ? "programs_missing"   // a later program
                          : gGenerationUninitialized.load() != chain->uninitialized ? "xefg_not_initialized" : nullptr;
        if (fault) {
            // XeFG stopped generating (or never started): the game gets its chain back for the rest of the session.
            chain->broken = true;
            {
                std::lock_guard hostLock(host.mutex);
                host.failure = fault;
            }
            NoteReason(fault);
            Log("vk_takeover_released", std::string("\"reason\":\"") + fault + "\"");
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        }
        if (xvk::SurfaceBroken(chain->window)) {
            // The layer could not create its swap chain on the window (its frames are dropped): give the window back
            // to the game for the rest of the session.
            chain->broken = true;
            {
                std::lock_guard hostLock(host.mutex);
                host.failure = "layer_swapchain";
            }
            NoteReason("layer_swapchain");
            Log("vk_takeover_released", "\"reason\":\"layer_swapchain\"");
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        }
        // Give the chain back when the user turned the host off, and recreate on a window size change.
        RECT client{};
        const bool resized = chain->window && GetClientRect(chain->window, &client) && client.right > 0 && client.bottom > 0 &&
                             (uint32_t(client.right) != chain->info.imageExtent.width || uint32_t(client.bottom) != chain->info.imageExtent.height);
        if ((!chain->hosted && (!gEnabled.load() || (gGameFrameGeneration.load() && !gStandalone.load()))) || resized)
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        return finish(VK_SUCCESS);
    } catch (...) {
        chain->broken = true;
        return finish(VK_ERROR_OUT_OF_DATE_KHR);
    }
}

// RTXMFG's own swap-chain wrappers (standalone takeover): hosted devices and chains are passed on.
bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) noexcept
{
    try {
        if (HostedHandle(device, swapchain)) return false;
    } catch (...) {}
    return DestroySwapchainImpl(device, swapchain);
}

bool GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images, VkResult& result) noexcept
{
    try {
        if (HostedHandle(device, swapchain)) return false;
    } catch (...) {}
    return GetSwapchainImagesImpl(swapchain, count, images, result);
}

bool AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index,
                      VkResult& result) noexcept
{
    try {
        if (HostedHandle(device, swapchain)) return false;
    } catch (...) {}
    return AcquireNextImageImpl(swapchain, timeout, semaphore, fence, index, result);
}

bool SetHdrMetadata(VkDevice device, VkSwapchainKHR swapchain, const void* metadata) noexcept
{
    try {
        if (HostedHandle(device, swapchain)) return false;
    } catch (...) {}
    return SetHdrMetadataImpl(swapchain, metadata);
}

bool QueuePresent(VkQueue queue, const VkPresentInfoKHR* info, VkResult& result) noexcept
{
    try {
        if (info && info->swapchainCount && info->pSwapchains && HostedHandle(VK_NULL_HANDLE, info->pSwapchains[0])) return false;
    } catch (...) {}
    return QueuePresentImpl(queue, info, result);
}

VkResult AfterNativePresent(VkQueue queue, const VkPresentInfoKHR* info, VkResult result) noexcept
{
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) || !info || info->swapchainCount != 1 || !gEnabled.load() ||
        (gGameFrameGeneration.load() && !gStandalone.load()))
        return result;
    try {
        std::lock_guard lock(gMutex);
        auto native = gNatives.find(info->pSwapchains[0]);
        if (native == gNatives.end()) return result;
        NativeChain& record = native->second;
        ++record.presents;
        auto device = gDevices.find(record.device);
        if (device == gDevices.end()) return result;
        HostDevice& host = *device->second;
        if (!host.failure.empty() || !InputsRecent(host)) return result;
        if (record.requested) {
            // Once per chain: a game that ignores out-of-date keeps its native chain.
            if (record.presents == record.requestedAt + 600) Log("vk_takeover_ignored", "\"presents\":" + std::to_string(record.presents));
            return result;
        }
        record.requested = true;
        record.requestedAt = record.presents;
        Log("vk_takeover_request", "\"presents\":" + std::to_string(record.presents));
        return VK_ERROR_OUT_OF_DATE_KHR;
    } catch (...) {
        return result;
    }
}

// ---------------------------------------------------------------- inputs
namespace {
bool EnsureCopyPipelines(HostDevice& host)
{
    if (host.pipelines[0]) return true;
    if (host.copyFailed) return false;
    auto& vk = host.vk;
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr,
                                            VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 2, bindings};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    bool ok = vk.vkCreateSampler(host.device, &samplerInfo, nullptr, &host.sampler) == VK_SUCCESS &&
              vk.vkCreateDescriptorSetLayout(host.device, &setInfo, nullptr, &host.setLayout) == VK_SUCCESS;
    if (ok) {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &host.setLayout, 1, &push};
        ok = vk.vkCreatePipelineLayout(host.device, &layoutInfo, nullptr, &host.pipelineLayout) == VK_SUCCESS;
    }
    const std::pair<const uint32_t*, size_t> code[3] = {{input_copy::shader0, sizeof(input_copy::shader0)},
                                                        {input_copy::shader1, sizeof(input_copy::shader1)},
                                                        {input_copy::shader2, sizeof(input_copy::shader2)}};
    for (int i = 0; ok && i < 3; ++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code[i].second, code[i].first};
        VkShaderModule module = VK_NULL_HANDLE;
        ok = vk.vkCreateShaderModule(host.device, &moduleInfo, nullptr, &module) == VK_SUCCESS;
        if (!ok) break;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        pipelineInfo.layout = host.pipelineLayout;
        ok = vk.vkCreateComputePipelines(host.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &host.pipelines[i]) == VK_SUCCESS;
        vk.vkDestroyShaderModule(host.device, module, nullptr);
    }
    if (!ok) {
        host.copyFailed = true;
        Log("vk_input_copy_rejected", "\"reason\":\"pipeline\"");
    }
    return ok;
}

bool PrepareSlot(HostDevice& host, InputSlot& slot, const DlssInputs& in)
{
    const DXGI_FORMAT motionFormat = (in.motionFormat == VK_FORMAT_R32G32_SFLOAT || in.motionFormat == VK_FORMAT_R32G32B32A32_SFLOAT)
                                         ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
    if (slot.depth && slot.motion && slot.width == in.renderWidth && slot.height == in.renderHeight && slot.motionFormat == motionFormat)
        return true;
    slot.depth.Reset();
    slot.motion.Reset();
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, in.renderWidth, in.renderHeight, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(host.d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.depth))))
        return false;
    desc.Format = motionFormat;   // render size too (RecordDlssInputs)
    if (FAILED(host.d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.motion))))
        return false;
    auto* depth = xvk::Own<xvk::Resource>(slot.depth.Get());
    auto* motion = xvk::Own<xvk::Resource>(slot.motion.Get());
    slot.depthView = depth->View(VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, VK_IMAGE_USAGE_STORAGE_BIT);
    slot.motionView = motion->View(motionFormat == DXGI_FORMAT_R32G32_FLOAT ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R16G16_SFLOAT,
                                   VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, VK_IMAGE_USAGE_STORAGE_BIT);
    if (!slot.depthView || !slot.motionView) return false;
    // The layer initializes new images on its own queue; the game's command buffer must not use them earlier.
    VkSemaphore init = VK_NULL_HANDLE;
    uint64_t value = 0;
    host.layer->InitWait(init, value);
    if (value) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &init, &value};
        if (host.vk.vkWaitSemaphores(host.device, &wait, 1'000'000'000ull) != VK_SUCCESS) return false;
    }
    slot.width = in.renderWidth;
    slot.height = in.renderHeight;
    slot.motionFormat = motionFormat;
    return true;
}

// target width x height from the source's top-left sourceWidth x sourceHeight (point-sampled when they differ).
void RecordCopy(HostDevice& host, VkCommandBuffer command, VkPipeline pipeline, VkImageView source, VkImageLayout sourceLayout,
                VkImageView target, uint32_t width, uint32_t height, uint32_t sourceWidth, uint32_t sourceHeight)
{
    auto& vk = host.vk;
    vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDescriptorImageInfo sourceInfo{host.sampler, source, sourceLayout};
    VkDescriptorImageInfo targetInfo{VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sourceInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &targetInfo}};
    vk.vkCmdPushDescriptorSetKHR(command, VK_PIPELINE_BIND_POINT_COMPUTE, host.pipelineLayout, 0, 2, writes);
    const uint32_t size[4] = {width, height, sourceWidth, sourceHeight};
    vk.vkCmdPushConstants(command, host.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vk.vkCmdDispatch(command, (width + 7) / 8, (height + 7) / 8, 1);
}

// The frame's inputs into the host's ring on `command`, for the chain's next present.
bool RecordInputsFor(const std::shared_ptr<HostDevice>& hostPtr, const std::shared_ptr<FakeChain>& chain, VkCommandBuffer command,
                     const DlssInputs& in)
{
    {
        HostDevice& host = *hostPtr;
        std::lock_guard lock(host.mutex);
        const bool first = !host.inputFrames;
        ++host.inputFrames;
        host.lastInputTick = Now();
        host.lastInputs = in;
        if (first)
            Log("vk_inputs_first", "\"render\":\"" + std::to_string(in.renderWidth) + "x" + std::to_string(in.renderHeight) + "\",\"motion\":\"" +
                std::to_string(in.motionWidth) + "x" + std::to_string(in.motionHeight) + "\",\"output\":\"" + std::to_string(in.outputWidth) +
                "x" + std::to_string(in.outputHeight) + "\",\"depth_format\":" + std::to_string(int(in.depthFormat)) + ",\"motion_format\":" +
                std::to_string(int(in.motionFormat)) + ",\"inverted\":" + (in.depthInverted ? "true" : "false") + ",\"low_res_mv\":" +
                (in.motionLowRes ? "true" : "false") + ",\"jittered_mv\":" + (in.motionJittered ? "true" : "false") + ",\"flags_known\":" +
                (in.flagsKnown ? "true" : "false"));
        if (!chain || !host.layer || !EnsureCopyPipelines(host)) return false;
        const int copyMode = gAddonInputCopy.load();   // MFGAdaUnlock 1.7: field isolation ([XeFG] VulkanInputCopy)
        if (!copyMode) return false;
        uint64_t frame = 0;
        {
            // The base frame being recorded is the one the next present of the chain shows.
            std::unique_lock chainLock(chain->mutex, std::try_to_lock);
            if (chainLock.owns_lock()) frame = chain->presents + 1;
            else if (chain->acquireWaiting.load()) frame = chain->presentsDone.load() + 1;   // MFGAdaUnlock 1.7.1
            else {
                static std::atomic<uint32_t> logged{0};
                if (logged++ < 8) Log("vk_input_skipped", "\"presents\":" + std::to_string(chain->presentsDone.load()));
                return false;
            }
        }
        InputSlot& slot = host.slots[frame % kInputSlots];
        if (!PrepareSlot(host, slot, in)) {
            slot.frame = 0;
            return false;
        }
        if (copyMode == 3) { slot.frame = 0; return false; }
        // MFGAdaUnlock 1.7: by default the copy runs at the frame's present, on the host's own command buffer. Recorded
        // here, our compute pipeline, push descriptors and constants replaced the game's compute state in the middle of
        // its command buffer (The Great Circle does not bind it again after DLSS: the GPU hung). 6 records it here.
        if (copyMode == 1) {
            slot.inputs = in;
            slot.frame = frame;
            slot.deferred = true;
            return true;
        }
        slot.deferred = false;
        auto& vk = host.vk;
        // Earlier readers of the slot (XeFG, several frames back) and the image's own GENERAL layout.
        VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        before.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &before;
        vk.vkCmdPipelineBarrier2(command, &dependency);
        if (copyMode != 2 && copyMode != 4) {
        RecordCopy(host, command, host.pipelines[0], in.depthView, in.depthLayout, slot.depthView, in.renderWidth, in.renderHeight,
                   in.renderWidth, in.renderHeight);
        }
        if (copyMode != 2 && copyMode != 5) {
        RecordCopy(host, command, host.pipelines[slot.motionFormat == DXGI_FORMAT_R32G32_FLOAT ? 2 : 1], in.motionView, in.motionLayout,
                   slot.motionView, in.renderWidth, in.renderHeight, in.motionWidth, in.motionHeight);
        }
        VkMemoryBarrier2 after{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        after.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        after.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        dependency.pMemoryBarriers = &after;
        vk.vkCmdPipelineBarrier2(command, &dependency);
        slot.inputs = in;
        slot.frame = frame;
        return true;
    }
}
// MFGAdaUnlock 1.7: the deferred copy of `slot` (host.mutex held), submitted before the bridge's own command on the
// presenting queue, after the game's semaphores: the frame's inputs are complete and the game's state is untouched.
VkCommandBuffer DeferredCopy(HostDevice& host, InputSlot& slot)
{
    auto& vk = host.vk;
    if (!host.copyPool) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         host.family};
        if (vk.vkCreateCommandPool(host.device, &poolInfo, nullptr, &host.copyPool) != VK_SUCCESS) { host.copyPool = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, host.copyPool,
                                             VK_COMMAND_BUFFER_LEVEL_PRIMARY, uint32_t(host.copyCommands.size())};
        if (vk.vkAllocateCommandBuffers(host.device, &allocate, host.copyCommands.data()) != VK_SUCCESS) {
            vk.vkDestroyCommandPool(host.device, host.copyPool, nullptr);
            host.copyPool = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
    }
    const uint32_t index = host.copyNext;
    if (host.copyValues[index]) {
        // The bridge value of its last submission: done once the bridge passed it.
        VkSemaphore semaphore = xvk::Own<xvk::Fence>(host.bridge.Get())->semaphore();
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore, &host.copyValues[index]};
        if (vk.vkWaitSemaphores(host.device, &wait, 100'000'000ull) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    host.copyNext = (index + 1) % uint32_t(host.copyCommands.size());
    const VkCommandBuffer command = host.copyCommands[index];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk.vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
    const DlssInputs& in = slot.inputs;
    VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    before.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    before.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &before;
    vk.vkCmdPipelineBarrier2(command, &dependency);
    RecordCopy(host, command, host.pipelines[0], in.depthView, in.depthLayout, slot.depthView, in.renderWidth, in.renderHeight,
               in.renderWidth, in.renderHeight);
    RecordCopy(host, command, host.pipelines[slot.motionFormat == DXGI_FORMAT_R32G32_FLOAT ? 2 : 1], in.motionView, in.motionLayout,
               slot.motionView, in.renderWidth, in.renderHeight, in.motionWidth, in.motionHeight);
    VkMemoryBarrier2 after{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    after.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    after.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    dependency.pMemoryBarriers = &after;
    vk.vkCmdPipelineBarrier2(command, &dependency);
    if (vk.vkEndCommandBuffer(command) != VK_SUCCESS) return VK_NULL_HANDLE;
    return command;
}
} // namespace

bool RecordDlssInputs(VkCommandBuffer command, const DlssInputs& in) noexcept
{
    if (!command || !in.depthView || !in.motionView || !in.renderWidth || !in.renderHeight || !in.motionWidth || !in.motionHeight ||
        !gEnabled.load())
        return false;
    try {
        // The device: the one usable device (DLSS evaluations carry only a command buffer).
        std::shared_ptr<HostDevice> hostPtr;
        std::shared_ptr<FakeChain> chain;
        {
            std::lock_guard lock(gMutex);
            // MFGAdaUnlock 1.7.1: a game may keep another device alive; the command buffer's dispatch table names its own.
            if (gDevices.size() == 1) hostPtr = gDevices.begin()->second;
            else for (const auto& [device, candidate] : gDevices)
                if (mfgunlock::xemfg::native::vulkan::policy::DispatchKey(device) ==
                    mfgunlock::xemfg::native::vulkan::policy::DispatchKey(command)) hostPtr = candidate;
            if (!hostPtr) return false;
            if (hostPtr->hosted) return false;   // OptiScaler provides that device's inputs (hosted::RecordFrame)
            chain = hostPtr->active.lock();
            if (chain && !gFakes.contains(reinterpret_cast<VkSwapchainKHR>(chain.get()))) chain.reset();
        }
        return RecordInputsFor(hostPtr, chain, command, in);
    } catch (...) {
        return false;
    }
}

void SetGameLatency(bool enabled) noexcept { gAddonGameLatency = enabled; }
bool GameLatencySleep(VkSwapchainKHR swapchain) noexcept { return GameLatencySleepImpl(swapchain); }
void GameLatencyMarker(VkSwapchainKHR swapchain, uint32_t marker) noexcept { GameLatencyMarkerImpl(swapchain, marker); }

bool CaptureDlssEvaluate(void* command, const NVSDK_NGX_Parameter* parameters) noexcept
{
    if (!command || !parameters || !gEnabled.load()) return false;
    try {
        void* depthValue = nullptr;
        void* motionValue = nullptr;
        if (NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_Depth, &depthValue)) || !depthValue ||
            NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motionValue)) || !motionValue)
            return false;
        const auto* depth = static_cast<const NVSDK_NGX_Resource_VK*>(depthValue);
        const auto* motion = static_cast<const NVSDK_NGX_Resource_VK*>(motionValue);
        if (depth->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || motion->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return false;
        DlssInputs in;
        in.depth = depth->Resource.ImageViewInfo.Image;
        in.depthView = depth->Resource.ImageViewInfo.ImageView;
        in.depthFormat = depth->Resource.ImageViewInfo.Format;
        in.motion = motion->Resource.ImageViewInfo.Image;
        in.motionView = motion->Resource.ImageViewInfo.ImageView;
        in.motionFormat = motion->Resource.ImageViewInfo.Format;
        // MFGAdaUnlock 1.7: with the addon's own single-aspect views (below) a depth+stencil view is fine (Red Dead
        // Redemption 2 passes its D32S8 depth that way); the game's own view is sampled only with one aspect.
        if (!gAddonInputView.load() && depth->Resource.ImageViewInfo.SubresourceRange.aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT &&
            depth->Resource.ImageViewInfo.SubresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
            return false;
        // MFGAdaUnlock 1.7: the game's views may span depth and stencil (a D24S8 view declared COLOR to NGX); the copy
        // samples single-aspect views of the same images.
        if (const auto view = gAddonInputView.load()) {
            in.depthView = view(in.depth, in.depthFormat, true);
            in.motionView = view(in.motion, in.motionFormat, false);
            if (!in.depthView || !in.motionView) return false;
        }
        unsigned int value = 0;
        int flags = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags))) {
            in.flagsKnown = true;
            in.depthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
            in.motionJittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
            in.motionLowRes = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        }
        // The rendered area: the subrect when the game passes one, else the feature's render size, else the depth.
        uint32_t width = 0, height = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &value))) width = value;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &value))) height = value;
        if (!width || !height) {
            if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Width, &value))) width = value;
            if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Height, &value))) height = value;
        }
        if (!width || !height) {
            width = depth->Resource.ImageViewInfo.Width;
            height = depth->Resource.ImageViewInfo.Height;
        }
        in.renderWidth = std::min(width, depth->Resource.ImageViewInfo.Width);
        in.renderHeight = std::min(height, depth->Resource.ImageViewInfo.Height);
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_OutWidth, &value))) in.outputWidth = value;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_OutHeight, &value))) in.outputHeight = value;
        if (in.motionLowRes || !in.outputWidth || !in.outputHeight) {
            in.motionWidth = std::min(in.renderWidth, motion->Resource.ImageViewInfo.Width);
            in.motionHeight = std::min(in.renderHeight, motion->Resource.ImageViewInfo.Height);
        } else {
            in.motionWidth = std::min(in.outputWidth, motion->Resource.ImageViewInfo.Width);
            in.motionHeight = std::min(in.outputHeight, motion->Resource.ImageViewInfo.Height);
        }
        float number = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &number))) in.jitterX = number;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &number))) in.jitterY = number;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &number)) && number != 0) in.motionScaleX = number;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &number)) && number != 0) in.motionScaleY = number;
        int reset = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Reset, &reset))) in.reset = reset != 0;
        return RecordDlssInputs(static_cast<VkCommandBuffer>(command), in);
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------- status
Snapshot ReadSnapshot() noexcept
{
    Snapshot snapshot;
    try {
        snapshot.enabled = gEnabled.load();
        snapshot.vulkanGame = gSurfaceSeen.load();
        snapshot.gameFrameGeneration = gGameFrameGeneration.load();
        snapshot.basePresents = gBasePresents.load();
        snapshot.vulkanPresents = xvk::PresentCount();
        snapshot.missingPrograms = xvk::MissingProgramCount();
        snapshot.latencyMode = gLatencyUsed.load();
        snapshot.latencyLimited = gLatencyLimited.load();
        snapshot.fpsLimit = gStandaloneFpsLimit.load();
        {
            std::lock_guard fpsLock(gFpsMutex);   // xemfgsa.5
            snapshot.baseFpsMilli = gFps.baseMilli;
            snapshot.outputFpsMilli = gFps.outputMilli;
            snapshot.fpsAgeMs = gFps.sampleMs ? GetTickCount64() - gFps.sampleMs : UINT64_MAX;
        }
        std::lock_guard lock(gMutex);
        snapshot.deviceReady = !gDevices.empty();
        for (const auto& [handle, chain] : gFakes) {
            snapshot.active = true;
            snapshot.frames = chain->dynamicOff ? 1 : chain->framesApplied;   // xemfgsa.6: Dynamic MFG's 1X
            snapshot.dynamic = chain->dynamicRan;
            snapshot.dynamicCap = chain->dynamicRan && chain->dynamic.CapActive();
            snapshot.dynamicTargetMilli = chain->dynamicRan ? uint32_t(chain->dynamic.TargetFps() * 1000.0 + 0.5) : 0;
        }
        for (const auto& [handle, host] : gDevices) {
            snapshot.inputFrames += host->inputFrames;
            snapshot.hosted = snapshot.hosted || host->hosted;
        }
        snapshot.restartNeeded = snapshot.enabled && !snapshot.deviceReady && gDeviceWhileOff.load();
        snapshot.state = snapshot.active ? "active"
            : snapshot.hosted ? "waiting"
            : (snapshot.enabled ? (snapshot.deviceReady ? "waiting" : "no_device") : "off");
        snapshot.reason = gLastReason;
        snapshot.gameLatency = gGameLatencyUsed.load();
        if (gRuntimeTried.load() && !gRuntime.ready) {
            snapshot.runtimeSearch = xess_runtime_search::Utf8(gRuntime.search.recommended.wstring());
            snapshot.runtimeGame = xess_runtime_search::Utf8(gRuntime.search.gameFolder.wstring());
            snapshot.runtimeMisnamed = xess_runtime_search::Utf8(gRuntime.search.misnamed);
        }
    } catch (...) {}
    return snapshot;
}

// ---------------------------------------------------------------- OptiScaler-hosted XeFG (mfg_xefg_vk.h)
namespace hosted
{
DeviceEditPtr PrepareDevice(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, uint32_t instanceApiVersion, VkPhysicalDevice physical,
                            const VkDeviceCreateInfo* info) noexcept
{
    if (!instance || !gipa || !physical || !info) return nullptr;
    tLastRefusal.clear();
    const HostedInstance owner{instance, gipa, instanceApiVersion};
    auto edit = PrepareDeviceImpl(physical, info, &owner);
    if (!edit) NoteReason(tLastRefusal.empty() ? "device_refused" : tLastRefusal);
    return edit;
}

bool DeviceCreated(DeviceEditPtr edit, VkDevice device, OriginalProc originals, void* user) noexcept
{
    return DeviceCreatedImpl(std::move(edit), device, originals, user);
}

VkResult CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, HWND window, bool invertedDepth,
                         VkSwapchainKHR* output) noexcept
{
    if (!info || !output) return VK_ERROR_INITIALIZATION_FAILED;
    try {
        auto host = FindDevice(device);
        if (!host || !host->hosted) { NoteReason("device_not_prepared"); return VK_ERROR_INITIALIZATION_FAILED; }
        // The checks of Eligible() that concern the chain; the present-id and present-wait flags are served here.
        constexpr VkSwapchainCreateFlagsKHR kAccepted = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
                                                        VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR |
                                                        VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR;
        FormatChoice format;
        std::string reason;
        // vkswapext.1: chain options of the routed extensions. Full-screen exclusive goes to the real swap chain
        // (CreateFake); the ones the engine's chain can't serve keep the game's own chain.
        const uint32_t routes = gHostRoutesDeclared.load() ? gHostRoutes.load() : 0;
        const auto* counters = FindStruct<VkSwapchainCounterCreateInfoEXT>(info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT);
        const auto* group = FindStruct<VkDeviceGroupSwapchainCreateInfoKHR>(info->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR);
        const auto* barrier = FindStruct<VkSwapchainPresentBarrierCreateInfoNV>(info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_BARRIER_CREATE_INFO_NV);
        if (!window || !IsWindow(window)) reason = "surface_window_unknown";
        else if (info->imageArrayLayers != 1 || (info->flags & ~kAccepted)) reason = "swapchain_flags";
        else if (!ChooseFormat(info->imageFormat, info->imageColorSpace, format)) reason = "swapchain_format";
        else if (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT) reason = "swapchain_concurrent";
        else if (!info->imageExtent.width || !info->imageExtent.height) reason = "swapchain_extent";
        else if (info->presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
                 info->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) reason = "swapchain_shared_present_mode";
        else if (PNextHas(info->pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT) &&
                 !(routes & MFG_XEFG_VK_ROUTE_FULL_SCREEN_EXCLUSIVE)) reason = "fullscreen_exclusive";
        else if (counters && counters->surfaceCounters) reason = "swapchain_counters";
        else if (group && (group->modes & ~VkDeviceGroupPresentModeFlagsKHR(VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR)))
            reason = "swapchain_device_group_modes";
        else if (barrier && barrier->presentBarrierEnable) reason = "swapchain_present_barrier";
        if (reason.empty()) {
            std::lock_guard lock(host->mutex);
            EnsureLayer(*host, reason);
        }
        if (!reason.empty()) {
            NoteReason(reason);
            Log("vk_hosted_refused", "\"reason\":\"" + Escape(reason) + "\"");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // The game recreates one of ours (resize): XeFG's chain goes first, so the window is free for the next one.
        std::shared_ptr<FakeChain> previous = info->oldSwapchain ? FindFake(info->oldSwapchain) : nullptr;
        if (previous) {
            std::lock_guard lock(previous->mutex);
            if (!previous->retired) ReleaseFake(*previous);
        }
        VkSwapchainCreateInfoKHR local = *info;
        local.flags &= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        std::shared_ptr<FakeChain> chain;
        bool retired = false;
        {
            std::lock_guard lock(host->mutex);
            chain = CreateFake(host, local, window, previous ? VK_NULL_HANDLE : info->oldSwapchain, invertedDepth, reason, retired);
        }
        if (!chain) {
            NoteReason(reason);
            Log("vk_takeover_failed", "\"reason\":\"" + Escape(reason) + "\",\"hosted\":true");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        chain->hosted = true;
        chain->targetFrames = chain->framesApplied;   // until OptiScaler sets its count
        const auto handle = reinterpret_cast<VkSwapchainKHR>(chain.get());
        {
            std::lock_guard lock(gMutex);
            gFakes[handle] = chain;
            host->active = chain;
        }
        NoteReason("active");
        // vkswapext.1: what the game asked of the chain beyond the basics (the routed extensions' options).
        {
            const auto* fse = FindStruct<VkSurfaceFullScreenExclusiveInfoEXT>(info->pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT);
            const auto* hdr = FindStruct<VkSwapchainDisplayNativeHdrCreateInfoAMD>(info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_DISPLAY_NATIVE_HDR_CREATE_INFO_AMD);
            Log("vk_hosted_swapchain_options", "\"present_mode\":" + std::to_string(int(info->presentMode)) +
                ",\"full_screen_exclusive\":" + std::to_string(fse ? int(fse->fullScreenExclusive) : -1) +
                ",\"device_group\":" + (group ? "true" : "false") + ",\"local_dimming\":" +
                std::to_string(hdr ? int(hdr->localDimmingEnable) : -1) + ",\"routes\":" + std::to_string(routes));
        }
        *output = handle;
        return VK_SUCCESS;
    } catch (...) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VkResult RecordFrame(VkSwapchainKHR swapchain, VkCommandBuffer command, const DlssInputs& in) noexcept
{
    if (!command || !in.depthView || !in.motionView || !in.renderWidth || !in.renderHeight || !in.motionWidth || !in.motionHeight)
        return VK_ERROR_INITIALIZATION_FAILED;
    try {
        auto chain = FindFake(swapchain);
        if (!chain || !chain->hosted) return VK_ERROR_SURFACE_LOST_KHR;
        return RecordInputsFor(chain->owner, chain, command, in) ? VK_SUCCESS : VK_INCOMPLETE;
    } catch (...) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VkResult WaitForPresent(VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) noexcept
{
    try {
        auto chain = FindFake(swapchain);
        if (!chain) return VK_ERROR_SURFACE_LOST_KHR;
        std::unique_lock lock(chain->mutex);
        auto done = [&] { return chain->lastPresentId >= presentId || chain->broken || chain->retired; };
        if (!done()) {
            if (!timeout) return VK_TIMEOUT;
            // The game waits for ids it presented already (they are recorded at QueuePresent); a longer wait means an
            // id it never presents to this chain, and an unbounded wait would hang it.
            const auto limit = std::chrono::nanoseconds(std::min<uint64_t>(timeout, 2'000'000'000ull));
            if (!chain->presented.wait_for(lock, limit, done)) return timeout == UINT64_MAX ? VK_SUCCESS : VK_TIMEOUT;
        }
        return chain->broken || chain->retired ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
    } catch (...) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
}

void SetEnabled(VkSwapchainKHR swapchain, bool enabled) noexcept
{
    try {
        if (auto chain = FindFake(swapchain)) {
            std::lock_guard lock(chain->mutex);
            chain->userEnabled = enabled;
        }
    } catch (...) {}
}

void SetDebugView(VkSwapchainKHR swapchain, bool enabled) noexcept
{
    try {
        if (auto chain = FindFake(swapchain)) {
            std::lock_guard lock(chain->mutex);
            chain->debugView = enabled;
        }
    } catch (...) {}
}

bool SetInterpolatedFrames(VkSwapchainKHR swapchain, uint32_t count) noexcept
{
    try {
        auto chain = FindFake(swapchain);
        if (!chain || count > kMaxInterpolatedFrames) return false;
        std::lock_guard lock(chain->mutex);
        chain->targetFrames = count + 1;   // 1 (no interpolated frame) switches generation off
        return true;
    } catch (...) {
        return false;
    }
}

bool OwnsSwapchain(VkSwapchainKHR swapchain) noexcept
{
    try {
        std::lock_guard lock(gMutex);
        auto found = gFakes.find(swapchain);
        return found != gFakes.end() && found->second->hosted;
    } catch (...) {
        return false;
    }
}

bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) noexcept { return DestroySwapchainImpl(device, swapchain); }

VkResult GetSwapchainImages(VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) noexcept
{
    VkResult result = VK_ERROR_SURFACE_LOST_KHR;
    return GetSwapchainImagesImpl(swapchain, count, images, result) ? result : VK_ERROR_SURFACE_LOST_KHR;
}

VkResult AcquireNextImage(VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) noexcept
{
    VkResult result = VK_ERROR_SURFACE_LOST_KHR;
    return AcquireNextImageImpl(swapchain, timeout, semaphore, fence, index, result) ? result : VK_ERROR_SURFACE_LOST_KHR;
}

VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) noexcept
{
    VkResult result = VK_ERROR_SURFACE_LOST_KHR;
    return QueuePresentImpl(queue, info, result) ? result : VK_ERROR_SURFACE_LOST_KHR;
}

void SetHdrMetadata(VkSwapchainKHR swapchain, const VkHdrMetadataEXT* metadata) noexcept { SetHdrMetadataImpl(swapchain, metadata); }

VkResult ReleaseSwapchainImages(VkSwapchainKHR swapchain, uint32_t count, const uint32_t* indices) noexcept
{
    try {
        auto chain = FindFake(swapchain);
        if (!chain || !chain->hosted) return VK_ERROR_SURFACE_LOST_KHR;
        std::lock_guard lock(chain->mutex);
        // The acquired back buffer goes back unpresented; the next acquisition hands out the same one again. Its
        // acquisition took a count of XeFG's frame-latency waitable (not a semaphore we can release) that only a
        // present gives back: the next acquisition uses that count instead of waiting for another.
        for (uint32_t i = 0; i < count && indices; ++i)
            if (chain->acquired >= 0 && indices[i] == uint32_t(chain->acquired)) {
                chain->acquired = -1;
                chain->latencyHeld = true;
                Log("vk_release_image", "\"index\":" + std::to_string(indices[i]));
            }
        return VK_SUCCESS;
    } catch (...) {
        return VK_ERROR_UNKNOWN;
    }
}

void SetHostRoutes(uint32_t routes) noexcept
{
    gHostRoutes = routes & MFG_XEFG_VK_ROUTES_ALL;
    gHostRoutesDeclared = true;
    Log("vk_host_routes", "\"routes\":" + std::to_string(routes & MFG_XEFG_VK_ROUTES_ALL));
}

void SetLatency(uint32_t mode, bool lowLatency, bool boost, float outputFpsLimit) noexcept
{
    try {
        if (mode > MFG_XEFG_VK_LATENCY_ANTILAG2) mode = MFG_XEFG_VK_LATENCY_XELL;
        if (!(outputFpsLimit > 0.0f) || !std::isfinite(outputFpsLimit)) outputFpsLimit = 0.0f;
        gLatencyRequested |= 1u << mode;
        bool changed = false;
        {
            std::lock_guard lock(gLatencyMutex);
            changed = gLatency.mode != mode || (gLatency.lowLatency != 0) != lowLatency || (gLatency.boost != 0) != boost ||
                      gLatency.outputFpsLimit != outputFpsLimit;
            gLatency.mode = mode;
            gLatency.lowLatency = lowLatency ? 1 : 0;
            gLatency.boost = boost ? 1 : 0;
            gLatency.outputFpsLimit = outputFpsLimit;
        }
        if (changed)
            Log("vk_latency_setting", std::string("\"mode\":\"") + kLatencyNames[mode] + "\",\"low_latency\":" + (lowLatency ? "true" : "false") +
                ",\"boost\":" + (boost ? "true" : "false") + ",\"fps_limit\":" + std::to_string(outputFpsLimit));
    } catch (...) {}
}
} // namespace hosted
}
