#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

struct IDXGISwapChain1;
struct IDXGIFactory2;

namespace sdk_pacing
{
// Hooks the exact profiled provider's native Present, Pace and Deadline. Install before
// registering an owner. The published route and the two modules remain pinned for
// process lifetime; no hook is removed during play.
bool Install(HMODULE provider, std::string& failure) noexcept;
// Called under the backend's context lock after a successful SDK initialization;
// presentation callbacks never take that context lock. Plain owner: extra outputs go
// through the SDK's own pacing wait; deadlines follow the DLSS-G batch interval
// (sdk_pacing_dlssg.h). The native swap chain's buffer count is unchanged. flipmeter.1:
// the SDK's presentation queue is also resolved for flip metering; without it the
// owner keeps plain pacing (log flip_metering_queue).
bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure) noexcept;
// Clears the owner's DLSS-G batch-interval history on an accepted enable/count/resize transition.
void Reset(void* handle, uint64_t generation) noexcept;
// The SDK has joined its presenter threads before a successful Destroy returns.
void Unregister(void* handle, uint64_t generation) noexcept;
// flipmeter.1 (test build): NVIDIA driver flip metering for the plain owner, as sl.dlss_g uses it
// (presentFrame -> setFlipConfig, 2.14.1 0x5F040). Right before the first Present of each batch the
// SDK's presentation queue gets NvAPI_D3D12_SetFlipConfig with nFramesPerBatch = presented = the
// batch's outputs and timePerBatchMs = the DLSS-G batch-interval estimate (sdk_pacing_dlssg.h); any
// other Present on that presenter (frame generation off) turns it off again. While the driver accepts
// it with independent flip, each deadline after the batch's first output follows the configured
// spacing and is released innerLeadNs early, so the display hardware, not the presenting thread's
// wake-up, places the flip. setFlipConfig: the NVIDIA driver's NvAPI_D3D12_SetFlipConfig, resolved by
// the caller on an NVIDIA adapter (never a fakenvapi); nullptr keeps the path off. enabled: the user's
// switch, which Page Up toggles live (logged as flip_metering_toggle) after SetFlipToggleHotkey(true).
using SetFlipConfigFunction = int (__cdecl*)(IUnknown* queue, UINT vidPnSourceId, void* config);
void SetFlipMetering(SetFlipConfigFunction setFlipConfig, bool enabled, int64_t innerLeadNs) noexcept;
// V1.5.1: Page Up toggles the switch live only after the user's opt-in (OptiScaler.ini [XeFG]
// XeFGFlipMeteringHotkey = on); off by default, as Page Up is also OptiScaler's FPS overlay key.
void SetFlipToggleHotkey(bool enabled) noexcept;
// Overlay controls only queue a request. The presenter commits it at the next
// batch boundary, using the already verified NVAPI entry point.
void RequestFlipMetering(bool enabled) noexcept;
// Optional comparison path: keep the provider's original arithmetic deadline.
// Commits on output 0 of a generated batch, disables driver metering for that
// batch, and preserves all SDK waits and the extra-output pacing hook.
void RequestSdkDeadlines(bool enabled) noexcept;
// Commits at a first generated output, together with the metering configuration.
void RequestQueueAhead(bool enabled) noexcept;
// MFGAdaUnlock 1.7.1: [XeFG] XeFGNativeLatency, the maximum frame latency of XeFG's native swap chain for owners
// registered from now on (1..16; 0 keeps XeFG's own, at most 3). Not applied with the flip queue, which sets its own.
void RequestNativeLatency(uint32_t latency) noexcept;
struct FlipStatus {
    bool requested{}, enabled{}, available{}, installed{}, registered{}, queue{}, busy{};
    bool configured{}, metered{}, refused{};
    uint32_t outputs{}, feedback{};
    int status{};
    uint64_t calls{};
    bool sdkDeadlinesRequested{},sdkDeadlinesActive{};
    bool queueAheadRequested{},queueAheadActive{};
    uint64_t queueAheadDeadlines{};
    uint32_t nativeBuffers{},nativeLatency{};
};
// Read only, scoped to the exact live SDK handle (never another context).
FlipStatus Snapshot(void* handle) noexcept;
// V1.6.2-alpha.13 amdflip.1: AMD's driver flip meter (IAmdExtFlipMeter; Adrenalin 26.10.1, RX 7000/9000 only) as FSR's
// frame-generation swap chain uses it. Owners registered from now on create it on their presentation queue's device
// (amdxc64.dll AmdExtD3DCreateInterface -> IAmdExtD3DFactory -> IAmdExtFlipMeter; logged as amd_flip_meter). While
// frame generation presents, each batch's first output sets mode 2 with FSR's configuration when the game's window covers
// its monitor (mode 1, auto: FSR's own rule, exclusive or borderless fullscreen) or always (mode 2, on); any other
// Present on the presenter (frame generation off) sets mode 0. alpha.19 amdcount.1: the config's count is the batch's
// generated frames (FSR generates one and always sends 1); a batch above the caps limit, or one whose count the first
// output's caller frame does not give, sets mode 0. Calls go to the driver only when the mode or count changes
// (amd_flip_meter_config); the owner's teardown sets mode 0 and releases the meter. The flip queue's gates keep their
// unmetered timing: the driver times each flip after its gate opens. create: amdxc64.dll's AmdExtD3DCreateInterface,
// resolved by the caller on an AMD adapter; nullptr or mode 0 keeps it off. driver: amdxc64.dll, the module whose own
// factory objects are released after use (OptiScaler's stand-in factory hands its object out without a reference).
using AmdExtCreateInterfaceFunction = HRESULT (__cdecl*)(IUnknown* outer, REFIID iid, void** object);
void SetAmdFlipMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, int mode) noexcept;
// V1.6.2-alpha.14 amdqueue.1: whether AMD's driver flip meter is usable on `device` (the driver's own D3D12 device): the
// meter is opened as SetAmdFlipMeter's owners open it (the device as its outer object), its caps read, and released at
// once. nullptr = usable; otherwise the reason (no_flip_meter on RX 6000 and older or drivers before Adrenalin 26.10.1,
// factory, caps, caps_limit, caps_disabled, ...). capsMask and result are for the log.
const char* ProbeAmdFlipMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, ID3D12Device* device, uint32_t& capsMask,
    HRESULT& result) noexcept;
// V1.6.1 pacing.1: the flip queue (sdk_pacing_meter.h) for owners registered from now on, on by default. Each output
// of a batch (the first included) waits on the SDK's presentation queue for a GPU gate that a meter thread opens at
// its slot on a clock locked to the game's hand-over rate, with a jitter buffer between hand-over and the first flip;
// the SDK's own Pace and the extra pacing are skipped, so no thread the game depends on sleeps for display timing. On
// NVIDIA, SetFlipMetering's driver metering still times the flips inside a batch. bufferPercent: the base buffer in
// percent of a batch (0..100, default 50). Off keeps V1.6.0's CPU pacing.
void SetFlipQueue(bool enabled, int bufferPercent) noexcept;
// V1.7.0 firstready.1: for owners registered from now on, a batch's first output flips at readiness, at least 0.9 of an
// interval after the previous flip, with no jitter buffer before it (sdk_pacing_meter.h SetFirstAtReadiness); the
// outputs inside a batch keep the clock's spacing. False keeps the jitter buffer (bufferPercent, the phase target).
// On by default.
void SetFlipQueueFirst(bool ready) noexcept;
// V1.6.1 pacing.1: the flip queue can only hold as many outputs as XeFG's native swap chain has back buffers, and XeFG
// creates it with the game's count (usually 3: about two outputs of buffer). For one InitFromSwapChainDesc call on this
// thread, CreateSwapChainForHwnd called by the provider image on `factory` (DXGI's own object only; its vtable pointer is
// swapped to a copy for the call and put back) creates XeFG's native swap chain with at least `buffers` back buffers.
// XeFG's proxy keeps reporting the game's count (its GetDesc1 returns the init descriptor's count, 0x2087B0) and each
// copy fetches the native buffer by index (0x21D3C0), so the game is unaffected (dev.28-31 ran 8-16 for hours via the
// resize path). A failed raised creation is retried with the game's descriptor. A later game resize goes back to the
// game's count. Logged as native_buffers.
// V1.6.2-alpha.5 menucap.1: OptiScaler's contexts only, so `buffers` is capped at optiscaler_host::kMenuBackBuffers (8):
// OptiScaler's overlay menu draws on this chain and holds 8 buffers. With 8 the queue's maximum latency is 7, so a 5X or
// 6X batch fits; an opt-in 7X batch fills it with no headroom and an 8X batch does not fit.
// V1.6.2-alpha.9 menu16.1: the cap is optiscaler_host::MenuBackBuffers(), 16 when the OptiScaler image carries the
// upstream menu fix (its arrays hold 16) and 8 otherwise.
// V1.7.0 xefgmenu.1: `created` (optional) runs on the scoped thread right after that provider creation succeeded, with
// its queue and window, and may replace the returned swap chain (taking over its reference). The scope also applies for
// `created` alone (buffers below 2: the game's count is kept).
using NativeChainCreated = void (*)(IUnknown* queue, HWND window, IDXGISwapChain1** swapchain, void* context) noexcept;
class NativeBuffersScope
{
public:
    NativeBuffersScope(IDXGIFactory2* factory, HMODULE provider, uint32_t buffers,
        NativeChainCreated created = nullptr, void* context = nullptr) noexcept;
    ~NativeBuffersScope();
    NativeBuffersScope(const NativeBuffersScope&) = delete;
    NativeBuffersScope& operator=(const NativeBuffersScope&) = delete;
    const char* Reason() const noexcept { return reason; }
    // factoryhook.1 (factory_not_dxgi only): the file names of the modules holding the factory's table and its
    // CreateSwapChainForHwnd ("" when none), and whether the table itself is System32's dxgi.dll (then the slot was
    // patched; otherwise the factory is another module's wrapper).
    const wchar_t* TableModule() const noexcept { return tableModule; }
    const wchar_t* CreateModule() const noexcept { return createModule; }
    bool TableSystemDxgi() const noexcept { return tableSystemDxgi; }
private:
    void** object{};
    void* original{};
    const char* reason{};
    wchar_t tableModule[64]{}, createModule[64]{};
    bool tableSystemDxgi{};
};
// V1.6.1 pacing.1 (log): the entry count of the context's output ring, the chain field at countOffset
// (host_unlock.h OutputRing). False before Install or for a handle that is not the profiled provider's.
bool OutputRingDepth(void* handle, uint32_t countOffset, uint64_t& depth) noexcept;
// xemfgsa.5: every frame XeFG's native Presents handed to DXGI so far (real and generated, any context), for XeMFG
// standalone's output frame rate. 0 before Install.
uint64_t OutputPresents() noexcept;
}
