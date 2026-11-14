#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

struct IDXGISwapChain1;
struct IDXGIFactory2;

namespace sdk_pacing
{
// Hooks the exact profiled provider's native Present and Pace. Install before registering an owner. The published
// route and the two modules remain pinned for process lifetime; no hook is removed during play.
bool Install(HMODULE provider, std::string& failure) noexcept;
// Called under the backend's context lock after a successful SDK initialization; presentation callbacks never take that
// context lock. Plain owner (no flip queue): extra outputs go through the SDK's own pacing wait and XeFG's own
// deadlines. The native swap chain's buffer count is unchanged. The SDK's presentation queue is also resolved for flip
// metering; without it the owner keeps plain pacing (log flip_metering_queue).
bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure) noexcept;
// Starts the owner's flip-queue clock over on an accepted enable/count/resize transition.
void Reset(void* handle, uint64_t generation) noexcept;
// pacerfix.1: the game resized the chain of context `handle` (XeMFG standalone's D3D12 host): the flip queue reads its
// native chain's back buffers again at the next gated batch (the OptiScaler route's Reset does the same).
void RecheckCapacity(void* handle) noexcept;
// The SDK has joined its presenter threads before a successful Destroy returns.
void Unregister(void* handle, uint64_t generation) noexcept;
// NVIDIA driver flip metering for the flip queue's owners, as sl.dlss_g uses it
// (presentFrame -> setFlipConfig, 2.14.1 0x5F040). Right before the first Present of each batch the
// SDK's presentation queue gets NvAPI_D3D12_SetFlipConfig with nFramesPerBatch = presented = the
// batch's outputs and timePerBatchMs = the flip queue clock's batch time; any
// other Present on that presenter (frame generation off) turns it off again. A metered batch is gated at its first
// output only and the rest present at once, so the display hardware places each later flip (as DLSS-G); innerLeadNs is
// the lead of inner gates otherwise. An owner without the flip queue is not metered. setFlipConfig: the NVIDIA driver's
// NvAPI_D3D12_SetFlipConfig, resolved by the caller on an NVIDIA adapter (never a fakenvapi); nullptr keeps the path
// off. enabled: the user's switch.
using SetFlipConfigFunction = int (__cdecl*)(IUnknown* queue, UINT vidPnSourceId, void* config);
void SetFlipMetering(SetFlipConfigFunction setFlipConfig, bool enabled, int64_t innerLeadNs) noexcept;
// AMD's driver flip meter (IAmdExtFlipMeter; Adrenalin 26.10.1; opens only where the driver enables it on the adapter:
// the RX 9070 does, the RX 7700/7800/7900 XT return E_NOINTERFACE, the RX 6000 driver lacks it) as FSR's frame-generation swap chain
// uses it. Owners registered from now on create it on their presentation queue's device
// (amdxc64.dll AmdExtD3DCreateInterface -> IAmdExtD3DFactory -> IAmdExtFlipMeter; logged as amd_flip_meter). While
// frame generation presents, each batch's first output sets mode 2 with FSR's configuration when the game's window covers
// its monitor (mode 1, auto: FSR's own rule, exclusive or borderless fullscreen) or always (mode 2, on); any other
// Present on the presenter (frame generation off) sets mode 0. The config's count is the batch's generated frames (FSR
// generates one and always sends 1); a batch above the caps limit, or one whose count the first output's caller frame
// does not give, sets mode 0. Calls go to the driver only when the mode or count changes
// (amd_flip_meter_config); the owner's teardown sets mode 0 and releases the meter. The flip queue's gates keep their
// unmetered timing: the driver times each flip after its gate opens. create: amdxc64.dll's AmdExtD3DCreateInterface,
// resolved by the caller on an AMD adapter; nullptr or mode 0 keeps it off. driver: amdxc64.dll, the module whose own
// factory objects are released after use (OptiScaler's stand-in factory hands its object out without a reference).
using AmdExtCreateInterfaceFunction = HRESULT (__cdecl*)(IUnknown* outer, REFIID iid, void** object);
void SetAmdFlipMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, int mode) noexcept;
// MFGAdaUnlock: the panel's live flip-metering switch; it commits at the next batch boundary.
void RequestFlipMetering(bool enabled) noexcept;
// MFGAdaUnlock 1.7.1: [XeFG] XeFGNativeLatency, the maximum frame latency of XeFG's native swap chain for owners
// registered from now on (1..16; 0 keeps XeFG's own, at most 3). Not applied with the flip queue, which sets its own.
void RequestNativeLatency(uint32_t latency) noexcept;
struct FlipStatus {
    bool requested{}, enabled{}, available{}, installed{}, registered{}, queue{}, busy{};
    bool configured{}, metered{}, refused{};
    uint32_t outputs{}, feedback{};
    int status{};
    uint64_t calls{};
    uint32_t nativeBuffers{}, nativeLatency{};
};
// Read only, scoped to the exact live SDK handle (never another context).
FlipStatus Snapshot(void* handle) noexcept;
// The flip queue (sdk_pacing_meter.h) for owners registered from now on, on by default on every route. Each output of a
// batch waits on the SDK's presentation queue for a GPU gate that a meter thread opens at its slot on a clock locked to
// the game's hand-over rate; the SDK's own Pace and the extra pacing are skipped, so no thread the game depends on
// sleeps for display timing. Off keeps XeFG's own pacing.
void SetFlipQueue(bool enabled) noexcept;
// pacediag.1: the flip queue's display-side diagnostics (pacing_diag.h) for queues started from now on, opt-in:
// OptiScaler.ini [XeFG] XeFGPacingDiagnostics = on, or RTX_MFG_PACING_DIAG=1 in the game's environment
// (PacingDiagnostics answers either). Each queue then writes CSV chunks beside the game's log
// (<log name>.pacediag.<start>.g<generation>.<n>.csv) plus the binary per-output trace. Measurement only.
void SetPacingDiagnostics(bool enabled) noexcept;
bool PacingDiagnostics() noexcept;
// latency.1: low-latency pacing (latency_pacing.h), the control key latencyPacing (0 auto = on, 1 on, 2 off),
// applied live. The real frame's native Present (and its Pace) on the game's thread is timed; the part of that hold
// every recent frame had moves to a sleep at the game's next sleep point (SleepBeforeInput: its XeLL or Reflex sleep,
// or XeMFG standalone's own after its Present), so the game reads its input later and the frame still reaches its
// display slot. While on, a raised native chain's maximum latency follows the batch (outputs + 1), so whole batches
// cannot wait unseen after the Present. Switching resets the controller.
void SetLatencyPacing(uint32_t mode) noexcept;
// A sleep point of the game's frame loop, before it reads its input: the sleep due for the last real frame, once.
// Never call it under a lock other threads present with. pacerfix.2: proven = the game's own XeLL / Reflex sleep (a
// point before its input); false for a host's own fallback point after its Present. Where the flip queue publishes its
// schedule (XeMFG standalone's D3D12 host), the sleep is bounded so the game's next frame still reaches its batch's
// first slot. The time slept is the flip queue's evidence too.
void SleepBeforeInput(bool proven) noexcept;
// sleepsight.1: a host's low-latency sleep on the calling thread: the game's Reflex sleep through Streamline (whatever
// OptiScaler or the driver turns it into: XeLL, Anti-Lag 2, Reflex) or libxell's own sleep. Its time, less any of our
// own sleep inside it (SleepBeforeInput), is the flip queue's evidence of how long the game slept (JitterBufferClock
// hostSleptNs); nested scopes on a thread count once, as the outermost. The first sleep of each source is logged
// (host_sleep_source).
enum class HostSleepSource : uint32_t { kReflex = 1, kXell = 2 };
class HostSleepScope
{
public:
    explicit HostSleepScope(HostSleepSource source) noexcept;
    ~HostSleepScope();
    HostSleepScope(const HostSleepScope&) = delete;
    HostSleepScope& operator=(const HostSleepScope&) = delete;
private:
    HostSleepSource source_{};
    bool outer_{};
    int64_t start_{};
};
// optisleep.1: latency.1 on OptiScaler's route, where Hook_xellSleep never runs (OptiScaler calls libxell through its
// own export detours). HostSleepPoint: after a host's low-latency sleep returned on this thread (its HostSleepScope
// closed; OptiScaler's calls only, universal_hooks.cpp / patcher.cpp decide): SleepBeforeInput(true). The first point a
// real frame's sleep reaches takes it, so a Reflex sleep that reached libxell sleeps at the libxell point, inside
// OptiScaler's own timing of its sleep, and the Reflex point after it finds nothing due.
void HostSleepPoint() noexcept;
// HostPresentMarker: the host's XeLL PRESENT_START (start) or PRESENT_END marker for `frame` on the calling thread, which
// bracket the game's Present on OptiScaler's route as BeginGamePresent / EndGamePresent bracket it on the standalone
// host. A start and end of one frame on one thread feed the low-latency pacing once per game frame with the thread's wait
// in between (wall time less CPU time) while the game sleeps at a proven point, the real frame's own hold as its floor.
// The flip queue's evidence stays the real frame's own hold, and nothing schedules by the markers.
void HostPresentMarker(bool start, uint32_t frame) noexcept;
// pacerfix.1: XeMFG standalone's D3D12 host brackets the game's Present into XeFG on the game's thread (Begin right
// before it, End right after it returns, before the host's sleep point); `handle` is the XeFG context it goes to. The
// game thread's wait in that Present (wall time less CPU time) is the frame's hold, with the real frame's own Present
// and Pace as the floor, but sizes the sleep only while the game sleeps at a proven point (SleepBeforeInput(true) within
// the last second). Whatever the low-latency mode, the wait is the evidence of that context's flip queue, and the
// Present's start times the first output's production for the schedule.
void BeginGamePresent(void* handle) noexcept;
void EndGamePresent() noexcept;
// pacerfix.2: the display's refresh period while a flip queue's batches present with V-Sync (refreshed within the last
// second), for the hosts' automatic output cap; 0 otherwise.
int64_t VsyncRefreshNs() noexcept;
// For the status: the counters, and the means over the real frames since the previous call (the status writer's).
struct LatencyPacingSnapshot
{
    uint32_t mode{};
    uint64_t frames{}, windowFrames{}, takes{}, missed{}, backoffs{}, ineffective{};
    double heldMs{}, slackMs{}, sleptMs{};
    int64_t ageMs{-1}; // since the last real frame (-1: none yet)
};
LatencyPacingSnapshot ReadLatencyPacing() noexcept;
// The flip queue can only hold as many outputs as XeFG's native swap chain has back buffers, and XeFG creates it with
// the game's count (usually 3: about two outputs of buffer). For one InitFromSwapChainDesc call on this thread,
// CreateSwapChainForHwnd called by the provider image on `factory` (its vtable pointer is swapped to a copy for the call
// and put back; DXGI's own object, wrapbuffers.1: or a wrapper's, see Via()) creates XeFG's native swap chain with at least
// `buffers` back buffers.
// XeFG's proxy keeps reporting the game's count (its GetDesc1 returns the init descriptor's count, 0x2087B0) and each
// copy fetches the native buffer by index (0x21D3C0), so the game is unaffected. A failed raised creation is retried
// with the game's descriptor; a game resize restores the game's count (the flip queue reads it again). Log: native_buffers.
// OptiScaler's contexts only: `buffers` is capped at optiscaler_host::MenuBackBuffers(), as OptiScaler's menu draws on
// this chain: 16 with the upstream menu fix (its arrays hold 16), else 8. With 8 the maximum latency is 7: a 5X or 6X
// batch fits, an opt-in 7X batch fills it with no headroom and an 8X batch does not fit.
// `created` (optional) runs on the scoped thread right after that provider creation succeeded, with its queue and
// window, and may replace the returned swap chain (taking over its reference). The scope also applies for `created`
// alone (buffers below 2: the game's count is kept).
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
    // wrapbuffers.1: how the raise applies (null when it does not): "dxgi" (System32 dxgi.dll's table and slot), "wrapper"
    // (another image's factory wrapper, e.g. ReShade's proxy, which passes the count down to DXGI and wraps the chain) or
    // "dxgi_hooked" (dxgi.dll's table with CreateSwapChainForHwnd hooked in place; the hook is called as XeFG's call would).
    const char* Via() const noexcept { return via; }
    // For factory_not_dxgi and the wrapper and hooked routes: the file names of the modules holding the factory's table and
    // its CreateSwapChainForHwnd ("" when none), and whether the table itself is System32's dxgi.dll (then the slot was
    // patched; otherwise the factory is another module's wrapper).
    const wchar_t* TableModule() const noexcept { return tableModule; }
    const wchar_t* CreateModule() const noexcept { return createModule; }
    bool TableSystemDxgi() const noexcept { return tableSystemDxgi; }
private:
    void** object{};
    void* original{};
    void* swapped{}; // the table copy the object points at during the scope
    const char* reason{};
    const char* via{};
    wchar_t tableModule[64]{}, createModule[64]{};
    bool tableSystemDxgi{};
};
// For the log: the entry count of the context's output ring, the chain field at countOffset
// (host_unlock.h OutputRing). False before Install or for a handle that is not the profiled provider's.
bool OutputRingDepth(void* handle, uint32_t countOffset, uint64_t& depth) noexcept;
// Every frame XeFG's native Presents handed to DXGI so far (real and generated, any context), for XeMFG
// standalone's output frame rate. 0 before Install.
uint64_t OutputPresents() noexcept;
}
