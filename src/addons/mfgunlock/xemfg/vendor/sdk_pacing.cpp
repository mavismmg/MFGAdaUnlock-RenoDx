#include "sdk_pacing.h"
#include "sdk_pacing_dlssg.h"
#include "sdk_pacing_meter.h"
#include "cadence_stats.h"
#include "xess_policy.h"
#include "xess_mod.h"
#include "overlay_device_identity.h"
#include "overlay_native.h"
#include "optiscaler_host.h"
#include "adapter_vendor_kmt.h"
#include "port/port_support.h"
#include "xvk/xvk_output_frame.h"
#include "third_party/minhook/include/MinHook.h"
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <dxgi1_4.h>
#include <memory>
#include <mutex>
#if defined(MFGUNLOCK_NATIVE_XEMFG)
#include "../native_trace.hpp"
#endif
#include <thread>
#include <vector>

namespace sdk_pacing
{
namespace
{
// Published once under installMutex, before the pinned provider's hooks are enabled.
uintptr_t kNativePresent=0x21f730, kPace=0x21ee30, kExtraReturn=0x2202ed, kFirstReturn=0x220db2, kRealReturn=0x220467;
uintptr_t kDeadlineReturn=0x21ef96, kDeadline=0x224b30;
uintptr_t kImplementationTable=0x157c3a0, kChainTable=0x157a980, kGetter=0xf410;
// xefg134.1: the family's object fields (runtime_layout.h Fields), published with the RVAs.
uintptr_t kImplementationChain=8, kChainQueue=0x28, kChainNative=0x30, kChainPresenter=0x120;
// The profiled presenter prologue establishes rbp = caller-rsp + 0x89.
// At the intercepted call the return slot is caller-rsp - 8, hence:
constexpr uintptr_t kSummaryFromReturn = 0x48; // [rbp - 0x49]
constexpr uintptr_t kBundleFromReturn = 0xf8;  // [rbp + 0x67]
using Present = HRESULT(*)(void*, UINT, UINT, uint64_t, const DXGI_PRESENT_PARAMETERS*, ID3D12Resource* const*, bool);
using Pace = bool(*)(void*, const void*, uint8_t, const void*, uint32_t);
using Deadline = int64_t*(*)(void*, int64_t*, const void*, const void*, uint32_t, uint32_t);

// amdflip.1: AMD's driver flip meter as FSR's ML frame-generation swap chain uses it (Adrenalin 26.10.1 amdxcffx64.dll
// 0x1801037D0 / 0x180103EC0; amdxc64.dll's object 0x1859127C8; build\amd-flipqueue-lock-20261022\RE-amd-flip-meter-26.10.1.md).
// AmdExtD3DCreateInterface(device, IAmdExtD3DFactory) -> CreateInterface(swap chain, IAmdExtFlipMeter). The driver builds
// the meter from the factory's device (the swap chain argument is not used) and refuses it (E_NOINTERFACE) where the
// adapter has no hardware support; the RX 6000 driver branch has no such interface. Each call is a kernel-driver escape;
// the driver times the flips itself, nothing is sent per frame. Vtable: IUnknown, GetCaps 3, SetConfig 4, GetConfig 5.
constexpr GUID kAmdExtD3DFactoryIid{0x014937ec, 0x9288, 0x446f, {0xa9, 0xac, 0xd7, 0x5a, 0x8e, 0x3a, 0x98, 0x4f}};
constexpr GUID kAmdFlipMeterIid{0x543820dd, 0x06c9, 0x4f7e, {0xa3, 0xd0, 0xca, 0x26, 0x0d, 0x79, 0x4a, 0xdd}};
// GetCaps(index 0, size 16): mask (bits 0-2 modes 1-3, bit 3 FSR leaves the meter off, bits 5-7 the config flags, bits
// 11-14 the count limit), limit = (mask >> 11) & 15 (FSR requires it), 0x3F, status (0 = ready; FSR presets 0x0B).
struct AmdFlipMeterCaps { uint32_t mask, limit, constant, status; };
static_assert(sizeof(AmdFlipMeterCaps) == 16);
// SetConfig / GetConfig: mode (0 off; FSR always asks for 2), parameter (mode 3 only), count (1 to the caps limit; FSR 1,
// its one generated frame; amdcount.1 sends the batch's generated frames),
// flags (bit 0 = caps bit 7, bit 1 = caps bit 5, bit 2 = caps bit 6; bits 4-7 are the driver's copy of count), then the
// driver's output value and status (0 = applied).
struct AmdFlipMeterConfig { uint32_t mode, parameter, count, flags, output, status; uint8_t reserved[0x28]; };
static_assert(sizeof(AmdFlipMeterConfig) == 0x40);
constexpr uint32_t kAmdFlipMeterMode = 2;
// Refused SetConfig calls in a row after which an owner stops asking (the meter is still released at teardown).
constexpr uint32_t kAmdFlipMeterFailureLimit = 4;
// amdlog.1: decisions that need no driver call logged per owner (amd_flip_meter_decision).
constexpr uint64_t kAmdDecisionLogLimit = 64;
// FSR asks only in exclusive or borderless fullscreen; the window is checked at most this often.
constexpr int64_t kAmdFullscreenCheckNs = 250000000;

// V1.6.1 pacing.1: the flip queue (sdk_pacing_meter.h). One GPU gate per owner: Present n waits on the SDK's
// presentation queue for gate value n, which the meter thread signals at the output's release time once the
// output's copy (queued before the gate; the ready fence marks it) has completed. The SDK's own Pace is skipped and no
// extra pace is added, so no thread the game or the SDK presenter depends on sleeps for display timing; they are held
// only by the native swap chain's own queue limits.
struct FlipQueue
{
    // vsync: the output's batch presents with V-Sync on (sync interval 1, vsync.1). idleNs (first outputs, vsync.2): how
    // long the presenter waited for this batch after the previous gated Present returned. pass (gatepass.1): a pass fence
    // Signal follows this output's gate Wait (a sample).
    // full (first outputs, lockin.1): the blocked Present waited behind more than a whole batch of gates (the native
    // latency exceeds the batch's outputs), so our queue held the game whether or not the presenter was backlogged.
    struct Item { uint64_t value{}; int64_t submitNs{}, idleNs{}; uint32_t outputs{}, index{}; bool first{}, coupled{}, vsync{}, pass{}, full{}; };
    ~FlipQueue()
    {
        // Every queued Wait was released before the thread exited. The GPU may still be retiring them, so the
        // fences are kept for the process lifetime.
        gate.Detach(); ready.Detach(); pass.Detach();
        if (readyEvent) CloseHandle(readyEvent);
        if (wake) CloseHandle(wake);
    }
    uint64_t generation{};
    Microsoft::WRL::ComPtr<ID3D12Fence> gate, ready;
    HANDLE readyEvent{}, wake{};
    std::mutex mutex;
    std::deque<Item> items;
    std::atomic<uint64_t> issued{}; // the highest gate value queued as a Wait (written under mutex)
    bool exited{};     // every issued value is released and no more are signalled (mutex)
    std::atomic<uint64_t> signaled{};
    std::atomic<bool> stop{}, failed{}, reset{}, metered{};
    // A gated Present blocked in the native Present (the queue was full): the next batch's hand-over followed our
    // flips (JitterBufferClock coupled), if the presenter was backlogged (vsync.2).
    std::atomic<bool> blocked{};
    // vsync.2: when the last gated native Present returned (presenting threads), and the blocked batches whose
    // presenter waited for the game afterwards (not coupled).
    std::atomic<int64_t> returnedNs{};
    std::atomic<uint64_t> idleBlocked{};
    // lockin.1: blocked batches whose Present waited behind more than a batch of gates (Item::full).
    std::atomic<uint64_t> fullBlocked{};
    // presenterlead.1: batches coupled because the presenter was backlogged although no Present blocked.
    std::atomic<uint64_t> leadBacklogged{};
    // Published by the meter thread: the clock's per-output interval (the driver's batch time / outputs).
    std::atomic<int64_t> intervalNs{};
    // Watchdog: when the gate last made progress or became busy again (presenting threads and meter).
    std::atomic<int64_t> progressNs{};
    uint32_t buffers{}, latency{};
    // vsync.1: the window the SDK's native swap chain presents to (its monitor's refresh period floors the meter under
    // V-Sync) and the sync interval of the last gated batch (stats).
    HWND window{};
    std::atomic<uint32_t> sync{};
    std::atomic<unsigned> logs{};
    // gatepass.1: the pass fence, signalled on the presentation queue right after a sampled output's gate Wait with the
    // gate's value, so it completes when the queue got past that gate (null: not created, nothing measured). passOff: a
    // pass Signal failed or the fence was lost; nothing more is sampled and the lead is kQueueSignalLead again. vendor:
    // the queue's adapter, its PCI vendor id from the kernel (0 = unknown; logged).
    Microsoft::WRL::ComPtr<ID3D12Fence> pass;
    std::atomic<bool> passOff{};
    uint32_t passRandom{0x9e3779b9u}; // the sampler (mutex)
    uint32_t vendor{};
    // Meter thread only.
    cadence::JitterBufferClock clock;
    int64_t lastSignalNs{}, refreshNs{};
    uint64_t batches{}, readyTimeouts{}, vsyncBatches{}, passTimeouts{};
    // deepheld.1: flip_queue_deep_refused / flip_queue_floor_reset lines written (LogClockEvents).
    uint32_t refusedLogs{}, floorLogs{};
    bool vsyncFloor{};
    cadence::GatePassLead passLead;
    cadence_stats::Window<180> gaps, holds, late, idle, passes;
    // alpha.15 (B4-09): handovers = the first outputs' submit spacing (the game's cadence, XeLL's sleep included);
    // seams = a batch's first output signal after the previous batch's last (real) output signal.
    int64_t lastFirstSubmitNs{}, realSignalNs{};
    cadence_stats::Window<180> handovers, seams;
};

struct Owner;
void ReleaseAmdMeter(Owner& owner) noexcept;
struct Owner
{
    ~Owner()
    {
        // amdflip.1: Unregister releases the meter; this covers an owner that never reached the route.
        ReleaseAmdMeter(*this);
        // A queue that no longer proves its table is quarantined (never released), as beta.1 did
        // with every SDK-owned interface it kept.
        if (flipQueue && !single_overlay::native::PinInterface(flipQueue.Get(), 2)) flipQueue.Detach();
        // pacing.1: an owner that never reached the route (or was unregistered) stops its meter; its thread then
        // releases whatever it issued.
        if (queue)
        {
            queue->stop.store(true, std::memory_order_release);
            if (queue->wake) SetEvent(queue->wake);
        }
    }
    void* handle{};
    uint64_t generation{};
    uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    std::atomic<unsigned> pacedLogs{}, skippedLogs{};
    std::atomic<bool> disabled{}, retired{};
    std::mutex clockMutex;
    // DLSS-G CPU pacer interval for the SDK's own pacing (all counts).
    cadence::BatchIntervalEstimator batchInterval;
    std::atomic<unsigned> intervalLogs{};
    // hitch.1: batch-start hitches and estimator resets for the log (clockMutex).
    struct Hitches
    {
        uint64_t batches{}, hitches{}, bridged{}, resetGap{}, resetRange{}, resetBase{}, revoked{};
        unsigned hitchLogs{}, resetLogs{};
    } hitches;
    // flipmeter.1: the SDK's presentation queue (presenter+0x10 = chain+kChainQueue, the queue its native
    // swap chain presents on); null keeps this owner on plain pacing. The configuration in effect is
    // written by the presenting thread before a native Present and read by its deadlines (flipMutex).
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> flipQueue;
    std::mutex flipMutex;
    struct Flip
    {
        bool configured{}, metered{}, refused{};
        uint32_t outputs{}, feedback{}, consecutiveFailures{};
        int64_t spacing{}, batchNs{};
        int status{};
        uint64_t calls{}, limited{};
        // seam.1: output 0's Present call of the batch (its frame's low 32 bits), the base of the
        // batch's metered deadlines. The SDK stamps its batch start only after waiting for output 0's
        // present fence, so deadlines from that stamp put G1 -> G2 a fence latency late and the
        // real -> G1 seam that much short (PresentMon, Subnautica 2: 4x 4.1 / 13.5 / 7.2 / 7.9 ms).
        uint32_t anchorFrame{};
        int64_t anchorNs{};
        bool queueAhead{};
        uint64_t queueAheadDeadlines{};
        uint32_t nativeBuffers{},nativeLatency{}; // MFGAdaUnlock 1.7.1: XeFG's native chain, as registered
    } flip;
    std::atomic<unsigned> flipLogs{}, flipSkipLogs{};
    // pacing.1: the flip queue; null keeps the CPU pacing of V1.6.0. The batch being gated (gateMutex): its frame,
    // outputs and the next output's position, set by its first output.
    std::shared_ptr<FlipQueue> queue;
    // amdflip.1: AMD's flip meter for this owner's device (CreateAmdMeter; amdMutex: presenting threads and teardown).
    // meter: the driver object (one reference); applied: the mode in effect; count: its count (amdcount.1, the batch's
    // generated frames; 0 with mode 0); failures: refused SetConfig calls in a row. reason / generated: the last decision
    // (amdlog.1), decisions: those logged without a driver call.
    std::mutex amdMutex;
    struct AmdMeter
    {
        void* meter{};
        HWND window{};
        AmdFlipMeterCaps caps{};
        const char* reason{};
        uint32_t flags{}, applied{}, count{}, failures{}, generated{};
        uint64_t calls{}, decisions{};
        int64_t checkedNs{};
        bool fullscreen{}, refused{}, driverFactory{};
    } amd;
    std::atomic<bool> amdMetered{};
    std::mutex gateMutex;
    uint64_t gateFrame{};
    uint32_t gateOutputs{}, gateNext{}, gateSync{};
    bool gateBatch{};
    // vsync.1: the current batch is not gated (its first output was refused), so the SDK's own Pace and V1.6.0's extra
    // pacing run for it as without the queue (pacing.2 left such a batch unpaced). Written by the batch's first output.
    std::atomic<bool> sdkPace{};
    std::atomic<unsigned> ungatedLogs{};
    // smallchain.1: flip_queue_small_chain written (once per owner).
    std::atomic<bool> smallChainLogged{};
    bool Queued() const noexcept
    {
        const auto& q = queue;
        return q && !q->failed.load(std::memory_order_acquire) && !q->stop.load(std::memory_order_acquire);
    }
};
using Owners = std::vector<std::shared_ptr<Owner>>;
struct Route
{
    HMODULE module{};
    Present original{};
    Pace pace{};
    Deadline deadline{};
    std::atomic<bool> enabled{};
    std::atomic<std::shared_ptr<const Owners>> owners;
    std::mutex ownersMutex;
};
std::atomic<Route*> route{};
std::mutex installMutex;

// flipmeter.1 (SetFlipMetering). firstLayout: Install verified the profiled first-output caller
// (0x220c70), whose frame First() reads for the batch's output count.
std::atomic<SetFlipConfigFunction> flipFunction{};
std::atomic<bool> flipEnabled{}, firstLayout{}, flipToggleDown{}, flipToggleKey{};
std::atomic<bool> flipRequested{};
std::atomic<bool> sdkDeadlinesRequested{},sdkDeadlinesActive{};
std::atomic<bool> queueAheadRequested{true};
std::atomic<uint32_t> nativeLatencyRequested{0};
std::atomic<int64_t> flipLeadNs{2500000};
std::atomic<unsigned> flipToggleLogs{};
// amdflip.1 (SetAmdFlipMeter): amdxc64.dll's AmdExtD3DCreateInterface (null = off) and the mode: 1 auto (FSR's rule, a
// window covering its monitor), 2 on (any window).
std::atomic<AmdExtCreateInterfaceFunction> amdCreate{};
std::atomic<HMODULE> amdDriver{};
std::atomic<int> amdMode{};
// pacing.1 (SetFlipQueue): owners registered from now on get the flip queue; the base buffer, percent of a batch.
std::atomic<bool> queueEnabled{true};
std::atomic<int> queueBufferPercent{50};
// firstready.1 (SetFlipQueueFirst): batch-first outputs flip at readiness instead of after the jitter buffer.
std::atomic<bool> queueFirstReady{true};
// V1.7.0 amd6x.1: V1.6.2-alpha.23's amdqueue.3 (AMD batches above 4 outputs ungated, because the clock settled far below
// the game at 5X/6X: a tester, Cyberpunk, RX 7900 XTX, 12.4 ms per output against 38 ms of production) is gone. heldrate.1
// fixed that lock-in (a tester, alpha.24 + XeFGFlipQueue = on, 6X: 6.0 ms per output, base 26 against 19), and the ungated
// path bunched the outputs: without a driver flip meter the CPU deadlines carry XeFG's whole present latency as lead, so in
// a GPU-bound game every deadline fell on the batch start (a tester, Expedition 33, RX 7800 XT, beta.4 6X: gpu_latency 24 ms
// against 5.5 ms per output, outputs 2-5 presented 1.0 / 1.9 / 2.7 / 7.9 ms after it). a tester: "still 5x and 6x broken".
// The queue passes a Wait about 0.5-0.8 ms after the CPU Signal (flip-gate fixture, RTX 4080, median 0.65 ms), so a
// gate is signalled this much early. Inside a batch the driver meters, an inner gate is signalled flipLeadNs early
// instead and the display hardware holds the flip to the batch's spacing (dev.23 sweep: first 0.5 / inner 2.5 ms gave
// a displayed-gap sd of 0.21-0.30 ms against DLSS-G's 0.34); a batch's first flip is not held by the hardware.
// gatepass.1: an unmetered owner's gates use their own queue's measured pass latency once known (UnmeteredLead).
constexpr int64_t kQueueSignalLead = 500000;
// gatepass.1: about one output in kGatePassSampling gets a pass fence Signal after its gate Wait; the meter polls the
// fence for at most kGatePassPollNs after signalling that gate.
constexpr uint32_t kGatePassSampling = 8;
constexpr int64_t kGatePassPollNs = 3000000;
// The meter waits at most this long for an output's copy; a copy that is not ready by then is released at once.
constexpr DWORD kQueueReadyTimeoutMs = 250;
// A gate with pending Waits and no progress for this long is released from the presenting thread (watchdog).
constexpr int64_t kQueueStallNs = 400000000;
// A gated native Present (XeFG's frame-latency wait, then DXGI Present1) taking longer than this was held by the full
// presentation queue.
constexpr int64_t kQueueBlockedNs = 1000000;
// ... while at least this many gated Presents were still waiting for their gate when it returned.
constexpr uint64_t kQueueCoupledPending = 2;
// vsync.2: ... and only when the next batch's first output reached its gate within this long of that batch's last gated
// Present returning (the batch was already waiting: the presenter was backlogged, so its hand-over time is ours).
constexpr int64_t kQueueBacklogNs = 1000000;
// pacing.2: the shortest batch time the driver's flip metering gets from the flip queue; shorter = that batch unmetered.
constexpr int64_t kQueueMinimumBatchNs = 4000000;
// A batch is configured once the estimate holds this many batch deltas (DLSS-G averages six).
constexpr uint32_t kFlipMinimumSamples = 3;
// The driver refusing this many configurations in a row turns the owner's flip metering off.
constexpr uint32_t kFlipFailureLimit = 8;
// hitch.1: the batch time the driver gets grows at most 1/FlipPolicy::kGrowthDivisor per batch over the
// one in effect (same output count): every flip of the batch is held to it, so one high estimate must
// not slow a whole batch down at once. Shorter times apply at once. flip.2 (cadence::FlipPolicy): the
// driver gets slightly less than the estimate, and only from a settled window.
// hitch.1 log: a batch-start delta of at least kHitchRatio x the window median is a hitch.
constexpr int64_t kHitchRatio = 2;
constexpr unsigned kHitchLogLimit = 64, kResetLogLimit = 64;
// Every this many observed batches: pacing_stats.
constexpr uint64_t kHitchStatsBatches = 1800;

// NV_FLIP_CONFIG_V2 (Streamline 2.14.1 external/nvapi/nvapi.h); version = MAKE_NVAPI_VERSION(V2, 2).
struct NvFlipConfigV2
{
    UINT version, nFramesPerBatch, flipConfigFeedbackMask, nPresentedFramesPerBatch;
    float timePerBatchMs, minControlFrameTimeMs;
};
static_assert(sizeof(NvFlipConfigV2) == 24);
constexpr UINT kFlipConfigVersion2 = UINT(sizeof(NvFlipConfigV2)) | (2u << 16);
constexpr UINT kFlipFeedbackNoIflip = 1u; // NVAPI_FLIP_CONFIG_FEEDBACK_NO_IFLIP
constexpr UINT kMaximumFramesPerFlipBatch = 8; // NVAPI_MAX_FRAMES_PER_FLIP_BATCH

bool Read(uintptr_t address, void* output, size_t bytes) noexcept
{
    if (address < 0x10000 || !output || !bytes || bytes > 512
        || address > UINTPTR_MAX-bytes) return false;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, bytes, &copied)
        && copied == bytes;
}
template<class T> bool Value(uintptr_t address, T& output) noexcept
{
    return Read(address, &output, sizeof(output));
}

bool MatchesApplicationDevice(ID3D12Device* device, uintptr_t presenterDevice) noexcept
{
    if (!device || !presenterDevice) return false;
    if (presenterDevice == uintptr_t(device)) return true;
    // A Streamline queue can make XeSS retain the native device while the
    // application supplied its proxy at CreateContext. Resolve only the
    // caller-owned interface; never invoke an unproven pointer read from SDK
    // memory. The presenter's field must still equal that exact native device.
    Microsoft::WRL::ComPtr<IUnknown> identity;
    Microsoft::WRL::ComPtr<ID3D12Device> native;
    return single_overlay::dx12::DeviceIdentity(device, identity)
        && SUCCEEDED(identity.As(&native)) && native
        && presenterDevice == uintptr_t(native.Get());
}
template<class T, size_t N> T At(const std::array<unsigned char, N>& bytes, size_t offset) noexcept
{
    T value{};
    if (offset <= N && sizeof(T) <= N-offset) memcpy(&value, bytes.data()+offset, sizeof(T));
    return value;
}
bool StackRange(uintptr_t address, size_t bytes) noexcept
{
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return address >= uintptr_t(tib->StackLimit) && address <= uintptr_t(tib->StackBase)
        && bytes <= uintptr_t(tib->StackBase)-address;
}
bool ImageRange(const unsigned char* base, uintptr_t rva, size_t bytes, DWORD* protection = nullptr) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(base+rva, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != base
        || info.Type != MEM_IMAGE || info.State != MEM_COMMIT || info.Protect != PAGE_EXECUTE_READ
        || uintptr_t(base+rva) > uintptr_t(info.BaseAddress)+info.RegionSize
        || bytes > uintptr_t(info.BaseAddress)+info.RegionSize-uintptr_t(base+rva)) return false;
    if (protection) *protection = info.Protect;
    return true;
}

bool ContextParts(const Route& current, void* handle, uintptr_t& implementation, uintptr_t& chain) noexcept
{
    uint32_t signature{}; uintptr_t table{}, getter{};
    const auto base = uintptr_t(current.module);
    return Value(uintptr_t(handle), signature) && signature == 0x47466558
        && Value(uintptr_t(handle)+8, implementation)
        && Value(implementation, table) && table == base+kImplementationTable
        && Value(table+0x80, getter) && getter == base+kGetter
        && Value(implementation+kImplementationChain, chain)
        && Value(chain, table) && table == base+kChainTable;
}

struct Invocation
{
    void* presenter{};
    UINT sync{}, flags{};
    uint64_t frame{};
    const DXGI_PRESENT_PARAMETERS* parameters{};
    ID3D12Resource* const* resource{};
    bool generated{};
    uintptr_t returnAddress{}, returnSlot{};
};
// vsync.1: the present modes the flip queue gates: V-Sync off (sync interval 0, with or without tearing) and V-Sync on at
// interval 1, the only interval sl.dlss_g 2.14.1 keeps with frame generation ("VSync interval %d not supported with FG,
// clamping to 1"). Higher intervals and other present flags keep the SDK's own path.
bool GatedPresentMode(const Invocation& call) noexcept
{
    return call.sync <= 1 && !(call.flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING));
}
struct Extra
{
    const void* bundle{};
    const void* summary{};
    uint32_t index{}, generatedCount{};
    uint8_t firstFence{};
    int64_t periodNs{};
};

const char* Prepare(const Route& current, const Owner& owner, const Invocation& call, Extra& output) noexcept
{
    if (owner.retired.load(std::memory_order_acquire)) return "retired_owner";
    if (call.returnAddress != uintptr_t(current.module)+kExtraReturn || !call.generated)
        return "different_call_site";
    if (call.presenter != reinterpret_cast<void*>(owner.presenter)) return "different_presenter";
    if (call.sync || (call.flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING))) return "different_present_mode";
    if ((call.returnSlot & 7) || !StackRange(call.returnSlot, kBundleFromReturn+8)) return "caller_stack_bounds";
    uintptr_t savedReturn{}, bundle{}, currentPresenter{}, native{};
    if (!Value(call.returnSlot, savedReturn) || savedReturn != call.returnAddress
        || !Value(call.returnSlot+kBundleFromReturn, bundle) || !bundle
        || !Value(owner.chain+kChainPresenter, currentPresenter) || currentPresenter != owner.presenter
        || !Value(owner.presenter+0x18, native) || native != owner.native) return "ownership_changed";
    uint8_t intel{}, hardware{}, software{};
    if (!Value(owner.presenter+0x64, intel) || !Value(owner.presenter+0x340, hardware)
        || !Value(owner.presenter+0x341, software) || intel || hardware || software != 1)
        return "software_pacing_unavailable";
    std::array<unsigned char, 0xc1> data{};
    std::array<int64_t, 2> summary{};
    if (!Read(bundle, data.data(), data.size())
        || !Read(call.returnSlot+kSummaryFromReturn, summary.data(), sizeof(summary))) return "unreadable_bundle";
    const auto resources = At<uintptr_t>(data, 0);
    const auto count = At<uint64_t>(data, 8);
    const auto resourceSlot = uintptr_t(call.resource);
    if (count < 2 || count > xess_mod::kGeneratedCapacity || !resources || resourceSlot <= resources
        || resourceSlot-resources >= count*8 || ((resourceSlot-resources) & 7)) return "intermediate_index_rejected";
    if (At<UINT>(data, 0x28) != call.sync || At<UINT>(data, 0x2c) != call.flags
        || At<uint64_t>(data, 0x30) != call.frame || uintptr_t(call.parameters) != bundle+0x38)
        return "bundle_arguments_changed";
    uintptr_t resource{};
    if (!Value(resourceSlot, resource) || !resource) return "missing_output_resource";
    // This is the SDK's median nanosecond duration from at most nine history
    // entries. Startup/paused/very slow frames retain the SDK's original path.
    if (summary[0] < 1 || summary[0] > 9 || summary[1] < 1000 || summary[1] > 250000000)
        return "timing_history_unavailable";
    output = {reinterpret_cast<void*>(bundle), reinterpret_cast<void*>(call.returnSlot+kSummaryFromReturn),
        uint32_t((resourceSlot-resources)/8), uint32_t(count), uint8_t(data[0xc0] & 1), summary[1]};
    return nullptr;
}

void Revoke(Owner& owner) noexcept
{
    std::lock_guard lock(owner.clockMutex);
    owner.batchInterval.Reset();
    ++owner.hitches.revoked;
    if (owner.queue) owner.queue->reset.store(true, std::memory_order_release);
}

bool Live(const Owner& owner) noexcept
{
    if (owner.retired.load(std::memory_order_acquire) || owner.disabled.load(std::memory_order_acquire)) return false;
    uint32_t magic{}; uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    uint8_t intel{}, hardware{}, software{};
    return Value(uintptr_t(owner.handle), magic) && magic == 0x47466558
        && Value(uintptr_t(owner.handle)+8, implementation) && implementation == owner.implementation
        && Value(implementation+kImplementationChain, chain) && chain == owner.chain
        && Value(chain+kChainPresenter, presenter) && presenter == owner.presenter
        && Value(chain+kChainNative, native) && native == owner.native
        && Value(presenter+0x18, native) && native == owner.native
        && Value(presenter, device) && device == owner.device
        && Value(presenter+0x64, intel) && !intel
        && Value(presenter+0x340, hardware) && !hardware
        && Value(presenter+0x341, software) && software == 1;
}

int64_t NowNs() noexcept
{
    LARGE_INTEGER ticks{}, frequency{};
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency)
        || ticks.QuadPart < 0 || frequency.QuadPart <= 0
        || frequency.QuadPart > INT64_MAX/1000000000
        || ticks.QuadPart/frequency.QuadPart > INT64_MAX/1000000000) return 0;
    return (ticks.QuadPart/frequency.QuadPart)*1000000000
        + (ticks.QuadPart%frequency.QuadPart)*1000000000/frequency.QuadPart;
}

// ---- flipmeter.1: NVIDIA driver flip metering ---------------------------------
// The batch's first generated output (beta.1 First; the profiled 0x220c70 caller, verified at
// Install): its frame holds the output vector pair, the generated count and this call's own
// arguments. Prologue: the return slot is caller-rsp - 8. vsync.1: V-Sync at interval 1 is accepted (the flip queue
// gates it); flip metering refuses V-Sync itself (FlipBeforePresent).
bool First(const Route& current, const Owner& owner, const Invocation& call, uint32_t& generated) noexcept
{
    if (!call.generated || call.returnAddress != uintptr_t(current.module)+kFirstReturn
        || call.presenter != reinterpret_cast<void*>(owner.presenter) || !Live(owner)
        || !GatedPresentMode(call) || (call.returnSlot & 7)
        || !StackRange(call.returnSlot, 0xf4)) return false;
    uintptr_t saved{}, pair{}, parameters{}, resources{}, resource{};
    uint64_t frame{}, count{}; uint32_t presented{}, sync{}, flags{};
    if (!Value(call.returnSlot, saved) || saved != call.returnAddress
        || !Value(call.returnSlot+0xe8, pair) || !pair
        || !Value(pair, resources) || resources != uintptr_t(call.resource)
        || !Value(pair+8, count) || !count || count > xess_mod::kGeneratedCapacity
        || count+1 > kMaximumFramesPerFlipBatch
        || !Value(call.returnSlot+0xf0, presented) || presented != 1
        || !Value(call.returnSlot+0xd8, frame) || frame != call.frame
        || !Value(call.returnSlot+0xe0, parameters) || parameters != uintptr_t(call.parameters)
        || !Value(call.returnSlot+0xc8, sync) || sync != call.sync
        || !Value(call.returnSlot+0xd0, flags) || flags != call.flags
        || !Value(resources, resource) || !resource) return false;
    generated = uint32_t(count);
    return true;
}

// The SDK's presentation queue for SetFlipConfig: the presenter's queue field equals the chain's
// (the queue XeFG created its native swap chain on), a pinned DIRECT queue, unwrapped through the
// public contracts, on the application's device. Returns the reason it is not.
const char* ResolveFlipQueue(Owner& owner, ID3D12Device* device) noexcept
{
    try
    {
        uintptr_t presenterQueue{}, chainQueue{};
        if (!Value(owner.presenter+0x10, presenterQueue) || !presenterQueue
            || !Value(owner.chain+kChainQueue, chainQueue) || chainQueue != presenterQueue) return "queue_layout";
        Microsoft::WRL::ComPtr<IUnknown> deviceIdentity, queueIdentity;
        if (!single_overlay::dx12::DeviceIdentity(device, deviceIdentity)) return "device_identity";
        auto* input = reinterpret_cast<ID3D12CommandQueue*>(presenterQueue);
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (!single_overlay::native::PinInterface(input, 2) || !single_overlay::native::Unwrap(input, queue)
            || !queue || !single_overlay::native::PinInterface(queue.Get(), 18)) return "queue_interface";
        ID3D12Device* raw{};
        const HRESULT result = queue->GetDevice(IID_PPV_ARGS(&raw));
        if (!raw || !single_overlay::native::PinInterface(raw, 2)) return "queue_device"; // quarantined
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
        queueDevice.Attach(raw);
        if (FAILED(result) || !single_overlay::dx12::DeviceIdentity(queueDevice.Get(), queueIdentity)
            || queueIdentity.Get() != deviceIdentity.Get()) return "queue_device";
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return "queue_type";
        owner.flipQueue = std::move(queue);
        return nullptr;
    }
    catch (...) { return "queue_exception"; }
}

int CallSetFlipConfig(SetFlipConfigFunction function, IUnknown* queue, NvFlipConfigV2* config) noexcept
{
    __try { return function(queue, 0, config); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}

// sl.dlss_g presentFrame -> setFlipConfig: nFramesPerBatch = presented = outputs, timePerBatchMs,
// minControlFrameTimeMs 0, VidPN source 0; outputs 0 is the off form (one presented frame per
// batch, no metering). Under the owner's flipMutex, on the presenting thread.
void ConfigureFlip(Owner& owner, uint32_t outputs, int64_t batchNs, const char* reason) noexcept
{
    const auto function = flipFunction.load(std::memory_order_acquire);
    if (!function || !owner.flipQueue) return;
    auto& flip = owner.flip;
    NvFlipConfigV2 config{kFlipConfigVersion2, outputs, 0, outputs ? outputs : 1u,
        outputs && batchNs > 0 ? float(double(batchNs)/1e6) : -1.0f, 0.0f};
    const int status = [&] {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
        mfgunlock::xemfg::native::RuntimeCall trace("nvapi_flip_config");
#endif
        const auto r=CallSetFlipConfig(function, owner.flipQueue.Get(), &config);
#if defined(MFGUNLOCK_NATIVE_XEMFG)
        trace.Result(r);
#endif
        return r;
    }();
    ++flip.calls;
    const bool statusChanged = status != flip.status, feedbackChanged = config.flipConfigFeedbackMask != flip.feedback;
    flip.status = status; flip.feedback = config.flipConfigFeedbackMask;
    if (status == 0) flip.consecutiveFailures = 0;
    else if (outputs && ++flip.consecutiveFailures >= kFlipFailureLimit) flip.refused = true;
    flip.configured = outputs && status == 0;
    flip.metered = flip.configured && !(config.flipConfigFeedbackMask & kFlipFeedbackNoIflip);
    flip.queueAhead = flip.metered && queueAheadRequested.load(std::memory_order_acquire);
    if (owner.queue) owner.queue->metered.store(flip.metered, std::memory_order_relaxed);
    flip.outputs = flip.configured ? outputs : 0;
    flip.batchNs = flip.configured ? batchNs : 0;
    flip.spacing = flip.configured ? batchNs/int64_t(outputs) : 0;
    if (flip.calls <= 8 || !outputs || flip.calls%240 == 0 || ((statusChanged || feedbackChanged)
        && owner.flipLogs.fetch_add(1, std::memory_order_relaxed) < 32))
        try
        {
            xess_mod::WriteLog("flip_metering_config", "\"generation\":"+std::to_string(owner.generation)+
                ",\"frames_per_batch\":"+std::to_string(config.nFramesPerBatch)+
                ",\"presented_per_batch\":"+std::to_string(config.nPresentedFramesPerBatch)+
                ",\"time_per_batch_ms\":"+std::to_string(config.timePerBatchMs)+
                ",\"status\":"+std::to_string(status)+",\"feedback\":"+std::to_string(config.flipConfigFeedbackMask)+
                ",\"metered\":"+(flip.metered ? "true" : "false")+",\"refused\":"+(flip.refused ? "true" : "false")+
                ",\"calls\":"+std::to_string(flip.calls)+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : ""));
        }
        catch (...) {}
}

// Wall-clock time of a log line, to line it up with other logs (OptiScaler's, the game's).
std::string LocalClock()
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char clock[32]{};
    sprintf_s(clock, "%02u:%02u:%02u.%03u", local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    return clock;
}

// Page Up toggles flip metering live (presenting threads, once per batch) once the user opted in
// (SetFlipToggleHotkey). flipmeter.2: was Ctrl+Shift+F8, which Subnautica 2 also binds to its in-game
// feedback screen. V1.5.1: opt-in, as Page Up is also OptiScaler's FPS overlay key.
void PollFlipToggle() noexcept
{
    if (!flipToggleKey.load(std::memory_order_acquire)) return;
    const bool down = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    if (!down) { flipToggleDown.store(false, std::memory_order_relaxed); return; }
    if (flipToggleDown.exchange(true, std::memory_order_relaxed)) return;
    const bool enabled = !flipRequested.load(std::memory_order_acquire);
    flipRequested.store(enabled, std::memory_order_release);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    if (flipToggleLogs.fetch_add(1, std::memory_order_relaxed) < 256)
        try
        {
            xess_mod::WriteLog("flip_metering_toggle", "\"enabled\":"+std::string(enabled ? "true" : "false")+
                ",\"local_time\":\""+LocalClock()+"\",\"now_ns\":"+std::to_string(NowNs())+",\"qpc\":"+std::to_string(qpc.QuadPart));
        }
        catch (...) {}
}

// Before the SDK's native Present: a batch's first output configures the batch, any other
// Present on the presenter (frame generation off) turns a configuration off; inner outputs and
// the real frame belong to the configured batch.
void FlipBeforePresent(Route& current, const Invocation& call)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    if (!first && (offset == kExtraReturn || offset == kRealReturn)) return;
    if (first) {
        PollFlipToggle();
        flipEnabled.store(!sdkDeadlinesActive.load(std::memory_order_acquire)
            && flipRequested.load(std::memory_order_acquire) && flipFunction.load(std::memory_order_acquire),
            std::memory_order_release);
    }
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (!owners) return;
    for (const auto& entry : *owners)
    {
        auto& owner = *entry;
        if (owner.presenter != uintptr_t(call.presenter)) continue;
        if (!owner.flipQueue || owner.retired.load(std::memory_order_acquire)
            || owner.disabled.load(std::memory_order_acquire)) return;
        std::lock_guard lock(owner.flipMutex);
        auto& flip = owner.flip;
        if (!first)
        {
            if (flip.configured) ConfigureFlip(owner, 0, 0, "frame_generation_off");
            return;
        }
        const char* skip{};
        uint32_t generated{}, samples{};
        int64_t batchNs{};
        if (!flipEnabled.load(std::memory_order_acquire)) skip = "disabled";
        else if (flip.refused) skip = "driver_refused";
        else if (!firstLayout.load(std::memory_order_acquire)) skip = "first_layout";
        // vsync.1: under V-Sync sl.dlss_g paces against the vblanks itself (RSYNC); the driver's timed flips are not
        // proven to combine with V-Sync flips, so a V-Sync batch is not metered (the flip queue still times it).
        else if (call.sync) skip = "vsync";
        else if (!First(current, owner, call, generated)) skip = "first_call_layout";
        else if (owner.Queued())
        {
            // pacing.1: the flip queue's clock times this batch; the driver gets the same batch time.
            batchNs = owner.queue->intervalNs.load(std::memory_order_relaxed)*int64_t(generated+1);
            if (batchNs <= 0) skip = "interval_unknown";
            else if (batchNs < kQueueMinimumBatchNs) skip = "interval_too_short";
        }
        else
        {
            std::lock_guard clock(owner.clockMutex);
            batchNs = owner.batchInterval.BatchTime(generated+1);
            samples = owner.batchInterval.Samples();
            if (!samples || batchNs <= 0) skip = "interval_unknown";
            else if (!cadence::FlipReady(owner.batchInterval)) skip = "settling";
        }
        if (!skip)
        {
            bool limited{};
            batchNs = cadence::FlipBatchTime(batchNs, flip.configured ? flip.batchNs : 0,
                flip.configured && flip.outputs == generated+1, limited);
            if (limited) ++flip.limited;
        }
        if (skip)
        {
            if (owner.flipSkipLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                try
                {
                    xess_mod::WriteLog("flip_metering_skipped", "\"generation\":"+std::to_string(owner.generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"reason\":\""+skip+"\"");
                }
                catch (...) {}
            if (flip.configured) ConfigureFlip(owner, 0, 0, skip);
        }
        else ConfigureFlip(owner, generated+1, batchNs, nullptr);
        // seam.1: taken last, right before the original Present of output 0.
        flip.anchorFrame = uint32_t(call.frame);
        flip.anchorNs = NowNs();
        return;
    }
}

// ---- pacing.1: the flip queue --------------------------------------------------------------------------------------
void QueueSleepUntil(HANDLE timer, int64_t target, const FlipQueue& queue) noexcept
{
    for (;;)
    {
        if (queue.stop.load(std::memory_order_acquire)) return;
        const auto now = NowNs();
        if (!now || now >= target) return;
        const auto left = target-now;
        if (left > 1500000)
        {
            LARGE_INTEGER due{}; due.QuadPart = -LONGLONG((left-1000000)/100);
            if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
            else Sleep(1);
        }
        else if (left > 200000) SwitchToThread();
        else YieldProcessor();
    }
}

// GPU readiness of one output: its copy, queued before its ready value, has completed.
bool WaitQueueReady(FlipQueue& queue, uint64_t value) noexcept
{
    const auto began = GetTickCount64();
    for (;;)
    {
        const auto completed = queue.ready->GetCompletedValue();
        if (completed == UINT64_MAX) { queue.failed.store(true, std::memory_order_release); return false; } // removed
        if (completed >= value) return true;
        if (queue.stop.load(std::memory_order_acquire)) return false;
        const auto elapsed = GetTickCount64()-began;
        if (elapsed >= kQueueReadyTimeoutMs) { ++queue.readyTimeouts; return false; }
        if (FAILED(queue.ready->SetEventOnCompletion(value, queue.readyEvent))) return false;
        WaitForSingleObject(queue.readyEvent, DWORD(kQueueReadyTimeoutMs-elapsed));
    }
}

// Signals value unless every Wait was already released (StopQueue, watchdog).
HRESULT QueueSignal(FlipQueue& queue, uint64_t value) noexcept
{
    std::lock_guard lock(queue.mutex);
    if (queue.exited) return S_OK;
    const HRESULT result = queue.gate->Signal(value);
    if (SUCCEEDED(result))
    {
        queue.signaled.store(value, std::memory_order_release);
        queue.progressNs.store(NowNs(), std::memory_order_relaxed);
    }
    return result;
}

// Releases every queued Wait; afterwards no value is signalled again. Under queue.mutex.
void ReleaseQueue(FlipQueue& queue) noexcept
{
    if (queue.exited) return;
    if (queue.issued) queue.gate->Signal(queue.issued);
    queue.signaled.store(queue.issued, std::memory_order_release);
    queue.exited = true;
}

std::string Milliseconds(int64_t ns)
{
    char text[32]{};
    sprintf_s(text, "%.3f", double(ns)/1e6);
    return text;
}

// ---- gatepass.1: the gate's pass latency ---------------------------------------------------------------------------
// Whether the next gated output gets a pass fence Signal (under queue.mutex): about one in kGatePassSampling, at random,
// so the samples do not stay on one position of a 2-, 4- or 8-output batch.
bool GatePassSample(FlipQueue& queue) noexcept
{
    auto& x = queue.passRandom;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x%kGatePassSampling == 0;
}

// Measurement off for good (a failed pass Signal on a presenting thread, a lost fence on the meter): the lead falls back.
void GatePassOff(FlipQueue& queue, const char* reason) noexcept
{
    if (queue.passOff.exchange(true, std::memory_order_acq_rel)) return;
    try
    {
        xess_mod::WriteLog("flip_queue_gate_pass", "\"generation\":"+std::to_string(queue.generation)+
            ",\"measuring\":false,\"reason\":\""+reason+"\",\"lead_ns\":"+std::to_string(kQueueSignalLead)+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// The lead of an unmetered owner's gates: its queue's measured pass latency (GatePassLead), kQueueSignalLead until
// enough gates were measured or without measurement.
int64_t UnmeteredLead(const FlipQueue& queue) noexcept
{
    return queue.pass && !queue.passOff.load(std::memory_order_acquire) ? queue.passLead.Lead(kQueueSignalLead)
        : kQueueSignalLead;
}

// How long before its release an output's gate is signalled. Metered (NVIDIA flip metering): a batch's first gate
// kQueueSignalLead early, an inner gate flipLeadNs early (the display hardware holds those flips to the batch spacing).
// Unmetered, every flip happens when the queue passes its gate: UnmeteredLead. vsync.2: while the refresh floor sets the
// interval under V-Sync (the display, not the game, limits the rate), the gate opens half a refresh early, so each flip
// waits at the display's V-Sync instead of at our gate: the flips keep the meter's spacing and the display absorbs the
// GPU's gate and present jitter (fixture, V-Sync + VRR at 120 Hz, 4X at 30 fps: displayed gaps 6.3-10.4 ms in pairs every
// 12 outputs, from releases exactly 8.333 ms apart). Meter thread.
int64_t GateLead(const FlipQueue& queue, const FlipQueue::Item& item, int64_t interval) noexcept
{
    int64_t lead = !queue.metered.load(std::memory_order_relaxed) ? UnmeteredLead(queue)
        : item.first ? kQueueSignalLead : flipLeadNs.load(std::memory_order_relaxed);
    if (item.vsync && queue.vsyncFloor && queue.refreshNs > 0 && interval > 0 && interval <= queue.refreshNs)
        lead = (std::max)(lead, queue.refreshNs/2);
    return lead;
}

// After the meter signalled a sampled gate at signaledNs: how long until the queue got past it (the pass fence reached
// value), polled for at most kGatePassPollNs. Polling never holds a later gate back: the next output's ready value is
// signalled behind this gate on the same queue, so the meter could not plan that output before the pass anyway.
// Returns the latency (> 0), 0 when the gate did not pass within the limit, < 0 for no sample.
int64_t PollGatePass(FlipQueue& queue, uint64_t value, int64_t signaledNs) noexcept
{
    for (;;)
    {
        const auto completed = queue.pass->GetCompletedValue();
        const auto now = NowNs();
        if (completed == UINT64_MAX) { GatePassOff(queue, "fence_lost"); return -1; } // removed
        if (completed >= value) return now > signaledNs ? now-signaledNs : 1;
        if (!now || queue.stop.load(std::memory_order_acquire)) return -1;
        if (now-signaledNs >= kGatePassPollNs) return 0;
        for (int i = 0; i < 16; ++i) YieldProcessor();
    }
}

// One sampled gate (PollGatePass): a pass goes to the log window and the lead. A gate that did not pass within the poll
// limit is counted, and the lead takes it as passing at the limit (the median stays right while most gates pass in time,
// and moves to the upper bound when they do not). The first time the lead can apply, a line says what it is.
void RecordGatePass(FlipQueue& queue, int64_t passNs) noexcept
{
    if (passNs < 0) return;
    if (passNs) queue.passes.Add(double(passNs)/1e6);
    else ++queue.passTimeouts;
    queue.passLead.Add(passNs ? passNs : kGatePassPollNs);
    if (queue.passLead.Samples() != cadence::GatePassLead::kMinimumSamples) return;
    try
    {
        xess_mod::WriteLog("flip_queue_gate_lead", "\"generation\":"+std::to_string(queue.generation)+
            ",\"lead_ns\":"+std::to_string(UnmeteredLead(queue))+",\"pass_p50_ns\":"+std::to_string(queue.passLead.Median())+
            ",\"samples\":"+std::to_string(queue.passLead.Samples())+",\"timeouts\":"+std::to_string(queue.passTimeouts)+
            ",\"vendor_id\":"+std::to_string(queue.vendor)+
            ",\"metered\":"+(queue.metered.load(std::memory_order_relaxed) ? "true" : "false")+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// alpha.15 (B4-09): count, p50, p95 and max only, so flip_queue_timing stays within one queued log line (752 characters).
std::string CompactJson(const char* name, const cadence_stats::Window<180>::Summary& s)
{
    char text[160]{};
    snprintf(text, sizeof(text), "\"%s\":{\"n\":%zu,\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f}", name, s.count, s.p50, s.p95, s.max);
    return text;
}

// Every 180 flips: the clock's state and the spacing of the gate signals, holds (release - ready) and signal lateness
// against the intended signal time, in milliseconds. Two lines (a log line keeps a bounded size).
void RecordQueueStats(FlipQueue& queue, const FlipQueue::Item& item, int64_t readyNs, int64_t lead,
    const cadence::JitterBufferClock::Release& release, int64_t signaledNs) noexcept
{
    if (item.first) ++queue.batches;
    if (item.first && item.idleNs > 0) queue.idle.Add(double(item.idleNs)/1e6);
    if (item.first)
    {
        if (queue.lastFirstSubmitNs && item.submitNs > queue.lastFirstSubmitNs)
            queue.handovers.Add(double(item.submitNs-queue.lastFirstSubmitNs)/1e6);
        queue.lastFirstSubmitNs = item.submitNs;
        if (queue.realSignalNs && signaledNs > queue.realSignalNs) queue.seams.Add(double(signaledNs-queue.realSignalNs)/1e6);
    }
    if (item.index+1 == item.outputs) queue.realSignalNs = signaledNs;
    if (queue.lastSignalNs && signaledNs > queue.lastSignalNs) queue.gaps.Add(double(signaledNs-queue.lastSignalNs)/1e6);
    queue.holds.Add(double(release.ns-readyNs)/1e6);
    const bool full = queue.late.Add(release.held ? double(signaledNs-(release.ns-lead))/1e6 : 0.0);
    if (!full) return;
    try
    {
        const auto& clock = queue.clock;
        xess_mod::WriteLog("flip_queue_stats", "\"generation\":"+std::to_string(queue.generation)+
            ",\"batches\":"+std::to_string(queue.batches)+",\"outputs\":"+std::to_string(item.outputs)+
            ",\"interval_ms\":"+Milliseconds(clock.Interval())+",\"target_ms\":"+Milliseconds(clock.Target())+
            ",\"latency_ms\":"+Milliseconds(clock.Latency())+",\"cap_ms\":"+Milliseconds(clock.Cap())+
            ",\"production_p50_ms\":"+Milliseconds(clock.ProductionQuantile(0.5))+
            ",\"production_p90_ms\":"+Milliseconds(clock.ProductionQuantile(0.9))+
            ",\"correction_ppm\":"+std::to_string(clock.CorrectionPpm())+",\"late_firsts\":"+std::to_string(clock.LateFirsts())+
            ",\"hitches\":"+std::to_string(clock.Hitches())+",\"coupled\":"+std::to_string(clock.Coupled())+
            ",\"coupled_idle\":"+std::to_string(queue.idleBlocked.load(std::memory_order_relaxed))+
            ",\"coupled_full\":"+std::to_string(queue.fullBlocked.load(std::memory_order_relaxed))+
            ",\"coupled_lead\":"+std::to_string(queue.leadBacklogged.load(std::memory_order_relaxed))+
            ",\"deep\":"+std::to_string(clock.Deep())+",\"rate_kept\":"+std::to_string(clock.Kept())+
            ",\"deep_refused\":"+std::to_string(clock.DeepRefused())+",\"floor_resets\":"+std::to_string(clock.FloorResets())+
            ",\"first_ready\":"+(clock.FirstAtReadiness() ? "true" : "false")+",\"first_gap_holds\":"+
            std::to_string(clock.GapHolds())+
            ",\"ready_timeouts\":"+std::to_string(queue.readyTimeouts)+
            ",\"metered\":"+(queue.metered.load(std::memory_order_relaxed) ? "true" : "false")+
            ",\"buffers\":"+std::to_string(queue.buffers)+",\"max_latency\":"+std::to_string(queue.latency)+
            ",\"sync\":"+std::to_string(queue.sync.load(std::memory_order_relaxed))+
            ",\"vsync_batches\":"+std::to_string(queue.vsyncBatches)+",\"vsync_floor_ms\":"+
            Milliseconds(queue.vsyncFloor ? queue.refreshNs : 0)+",\"vendor_id\":"+std::to_string(queue.vendor)+
            ",\"gate_pass\":"+(queue.pass && !queue.passOff.load(std::memory_order_relaxed) ? "true" : "false")+
            ",\"gate_lead_ms\":"+Milliseconds(UnmeteredLead(queue))+",\"gate_pass_samples\":"+
            std::to_string(queue.passLead.Samples())+",\"gate_pass_timeouts\":"+std::to_string(queue.passTimeouts)+
            ",\"local_time\":\""+LocalClock()+"\"");
        // gatepass.1: gate_pass_ms = the sampled gates' pass - signal latency (timeouts are counted in flip_queue_stats).
        xess_mod::WriteLog("flip_queue_timing", "\"generation\":"+std::to_string(queue.generation)+
            ",\"batches\":"+std::to_string(queue.batches)+","+cadence_stats::Json("signal_gap_ms", queue.gaps.Take())+
            ","+cadence_stats::Json("hold_ms", queue.holds.Take())+","+cadence_stats::Json("signal_late_ms", queue.late.Take())+
            ","+cadence_stats::Json("idle_ms", queue.idle.Take())+","+cadence_stats::Json("gate_pass_ms", queue.passes.Take())+
            ","+CompactJson("handover_ms", queue.handovers.Take())+","+CompactJson("seam_ms", queue.seams.Take()));
    }
    catch (...)
    {
        queue.gaps.Take(); queue.holds.Take(); queue.late.Take(); queue.idle.Take(); queue.passes.Take();
        queue.handovers.Take(); queue.seams.Take();
    }
}

// deepheld.1: the clock's two runaway guards, logged so a game that trips them shows in its log. A deep decay refused
// because our clock had not held the previous batch's real frame (the GPU or the game's own queue was behind: ID9442's
// Wuthering Waves pattern): the first one per queue, then every 600th, at most 8 lines. A rate restart after the interval
// sat at the floor (some other rule ran it away): each one, at most 8 lines. previousHold: the hold of the output planned
// before this one (for a batch's first output, the previous real frame). Meter thread.
void LogClockEvents(FlipQueue& queue, const FlipQueue::Item& item, const cadence::JitterBufferClock::Release& release,
    int64_t previousHold) noexcept
{
    const auto& clock = queue.clock;
    try
    {
        if (release.deepRefused && queue.refusedLogs < 8 && (clock.DeepRefused() == 1 || clock.DeepRefused()%600 == 0))
        {
            ++queue.refusedLogs;
            xess_mod::WriteLog("flip_queue_deep_refused", "\"generation\":"+std::to_string(queue.generation)+
                ",\"batches\":"+std::to_string(queue.batches)+",\"outputs\":"+std::to_string(item.outputs)+
                ",\"reason\":\"previous_real_frame_not_held\",\"previous_hold_ms\":"+Milliseconds(previousHold)+
                ",\"interval_ms\":"+Milliseconds(release.interval)+",\"latency_ms\":"+Milliseconds(release.latency)+
                ",\"target_ms\":"+Milliseconds(release.target)+",\"correction_ppm\":"+std::to_string(clock.CorrectionPpm())+
                ",\"full\":"+(item.full ? "true" : "false")+",\"production_p50_ms\":"+Milliseconds(clock.ProductionQuantile(0.5))+
                ",\"deep_refused\":"+std::to_string(clock.DeepRefused())+",\"deep\":"+std::to_string(clock.Deep())+
                ",\"local_time\":\""+LocalClock()+"\"");
        }
        if (release.floorReset && queue.floorLogs < 8)
        {
            ++queue.floorLogs;
            xess_mod::WriteLog("flip_queue_floor_reset", "\"generation\":"+std::to_string(queue.generation)+
                ",\"batches\":"+std::to_string(queue.batches)+",\"outputs\":"+std::to_string(item.outputs)+
                ",\"floor_run\":"+std::to_string(cadence::JitterBufferClock::kFloorRun)+
                ",\"floor_resets\":"+std::to_string(clock.FloorResets())+",\"coupled\":"+std::to_string(clock.Coupled())+
                ",\"deep\":"+std::to_string(clock.Deep())+",\"deep_refused\":"+std::to_string(clock.DeepRefused())+
                ",\"rate_kept\":"+std::to_string(clock.Kept())+",\"late_firsts\":"+std::to_string(clock.LateFirsts())+
                ",\"hitches\":"+std::to_string(clock.Hitches())+",\"metered\":"+
                (queue.metered.load(std::memory_order_relaxed) ? "true" : "false")+",\"local_time\":\""+LocalClock()+"\"");
        }
    }
    catch (...) {}
}

// vsync.1: the refresh period of the monitor showing window in its current mode (the display configuration's exact
// rational rate; EnumDisplaySettings' whole hertz as the fallback); 0 when unknown. The primary monitor without a window.
int64_t RefreshPeriod(HWND window) noexcept
{
    try
    {
        const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
            : MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
        UINT32 paths{}, modes{};
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) == ERROR_SUCCESS && paths && paths <= 64
            && modes <= 128)
        {
            std::vector<DISPLAYCONFIG_PATH_INFO> pathInfo(paths);
            std::vector<DISPLAYCONFIG_MODE_INFO> modeInfo(modes ? modes : 1);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, pathInfo.data(), &modes, modeInfo.data(), nullptr)
                == ERROR_SUCCESS)
                for (UINT32 i = 0; i < paths && i < pathInfo.size(); ++i)
                {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    source.header.size = sizeof(source);
                    source.header.adapterId = pathInfo[i].sourceInfo.adapterId;
                    source.header.id = pathInfo[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
                        || wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
                    const auto& rate = pathInfo[i].targetInfo.refreshRate;
                    const double hz = rate.Denominator ? double(rate.Numerator)/double(rate.Denominator) : 0.0;
                    if (hz >= 23.0 && hz <= 1000.0) return int64_t(1e9/hz);
                }
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)
            && mode.dmDisplayFrequency >= 23 && mode.dmDisplayFrequency <= 1000)
            return int64_t(1e9/double(mode.dmDisplayFrequency));
    }
    catch (...) {}
    return 0;
}

// vsync.1: a batch's present mode sets the meter's floor. Under V-Sync no flip is planned faster than the display
// refreshes: above the refresh rate (6x on a 120 Hz display) the outputs would otherwise queue behind the vblanks and
// add up to a full swap chain of latency while the display still shows one per refresh.
void ApplyPresentMode(FlipQueue& queue, const FlipQueue::Item& item) noexcept
{
    if (!item.first) return;
    if (item.vsync) ++queue.vsyncBatches;
    if (item.vsync == queue.vsyncFloor) return;
    if (item.vsync) queue.refreshNs = RefreshPeriod(queue.window);
    queue.clock.SetFloor(item.vsync ? queue.refreshNs : 0);
    queue.vsyncFloor = item.vsync;
    try
    {
        xess_mod::WriteLog("flip_queue_vsync", "\"generation\":"+std::to_string(queue.generation)+
            ",\"vsync\":"+(item.vsync ? "true" : "false")+",\"refresh_ns\":"+std::to_string(queue.refreshNs)+
            ",\"floor_ns\":"+std::to_string(item.vsync ? queue.refreshNs : 0)+",\"batches\":"+std::to_string(queue.batches)+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

void QueueThread(std::shared_ptr<FlipQueue> queue) noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    queue->clock.SetBufferFraction(double(queueBufferPercent.load(std::memory_order_relaxed))/100.0);
    queue->clock.SetFirstAtReadiness(queueFirstReady.load(std::memory_order_relaxed));
    queue->clock.SetCapacity(queue->buffers);
    for (;;)
    {
        if (queue->stop.load(std::memory_order_acquire) || queue->failed.load(std::memory_order_acquire)) break;
        FlipQueue::Item item{};
        bool have{};
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->items.empty()) { item = queue->items.front(); queue->items.pop_front(); have = true; }
        }
        if (!have) { WaitForSingleObject(queue->wake, 50); continue; }
        if (queue->reset.exchange(false, std::memory_order_acq_rel)) queue->clock.Reset();
        ApplyPresentMode(*queue, item);
        const bool ready = WaitQueueReady(*queue, item.value);
        const auto now = NowNs();
        cadence::JitterBufferClock::Release release{};
        // An unready copy (timeout, removal, stop) is released at once; nothing waits on it.
        if (ready && now)
        {
            // heldrate.1: NVIDIA's driver flip metering can switch per batch; every other queue is unmetered.
            queue->clock.SetUnmetered(!queue->metered.load(std::memory_order_relaxed));
            const int64_t previousHold = queue->clock.LastHold();
            release = queue->clock.Plan(now, item.submitNs, item.first, item.outputs, queue->lastSignalNs, item.coupled,
                item.full);
            if (release.deepRefused || release.floorReset) LogClockEvents(*queue, item, release, previousHold);
        }
        else { queue->clock.Reset(); release.ns = now; }
        if (ready && now && item.first) queue->intervalNs.store(release.interval, std::memory_order_relaxed);
        const int64_t lead = GateLead(*queue, item, release.interval);
        if (release.held) QueueSleepUntil(timer, release.ns-lead, *queue);
        const auto signaledNs = NowNs();
        const HRESULT result = QueueSignal(*queue, item.value);
        // gatepass.1: a sampled gate's pass latency (an unready copy is not a sample: its queue was still behind).
        const int64_t passNs = item.pass && ready && SUCCEEDED(result) && signaledNs
            && !queue->passOff.load(std::memory_order_acquire) ? PollGatePass(*queue, item.value, signaledNs) : -1;
        const auto number = queue->logs.fetch_add(1, std::memory_order_relaxed);
        if (number < 36 || FAILED(result) || !ready || (item.first && queue->batches%600 == 0))
            try
            {
                xess_mod::WriteLog("flip_queue", "\"generation\":"+std::to_string(queue->generation)+
                    ",\"gate\":"+std::to_string(item.value)+",\"index\":"+std::to_string(item.index)+
                    ",\"outputs\":"+std::to_string(item.outputs)+",\"ready\":"+(ready ? "true" : "false")+
                    ",\"ready_ns\":"+std::to_string(now)+",\"submit_ns\":"+std::to_string(item.submitNs)+
                    ",\"release_ns\":"+std::to_string(release.ns)+",\"signal_ns\":"+std::to_string(signaledNs)+
                    ",\"interval_ns\":"+std::to_string(release.interval)+",\"target_ns\":"+std::to_string(release.target)+
                    ",\"latency_ns\":"+std::to_string(release.latency)+",\"held\":"+(release.held ? "true" : "false")+
                    ",\"late\":"+(release.late ? "true" : "false")+",\"hitch\":"+(release.hitch ? "true" : "false")+
                    ",\"coupled\":"+(release.coupled ? "true" : "false")+",\"deep\":"+(release.deep ? "true" : "false")+
                    ",\"vsync\":"+(item.vsync ? "true" : "false")+
                    ",\"lead_ns\":"+std::to_string(lead)+(passNs >= 0 ? ",\"pass_ns\":"+std::to_string(passNs) : "")+
                    ",\"signal_result\":"+std::to_string(result));
            }
            catch (...) {}
        RecordGatePass(*queue, passNs);
        RecordQueueStats(*queue, item, now, lead, release, signaledNs);
        queue->lastSignalNs = signaledNs;
        if (FAILED(result)) { queue->failed.store(true, std::memory_order_release); break; }
    }
    // Release every Wait this owner ever queued; no Present may stay gated.
    uint64_t issued{};
    {
        std::lock_guard lock(queue->mutex);
        queue->items.clear();
        issued = queue->issued;
        ReleaseQueue(*queue);
    }
    if (timer) CloseHandle(timer);
    try
    {
        xess_mod::WriteLog("flip_queue_stopped", "\"generation\":"+std::to_string(queue->generation)+
            ",\"released_gate\":"+std::to_string(issued)+",\"failed\":"+(queue->failed.load() ? "true" : "false")+
            ",\"late_firsts\":"+std::to_string(queue->clock.LateFirsts())+",\"ready_timeouts\":"+
            std::to_string(queue->readyTimeouts)+",\"gate_pass_samples\":"+std::to_string(queue->passLead.Samples())+
            ",\"gate_pass_p50_ns\":"+std::to_string(queue->passLead.Median())+",\"gate_lead_ns\":"+
            std::to_string(UnmeteredLead(*queue))+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// Stops the owner's meter and releases every queued Wait before returning: XeFG's Destroy joins its presenter
// threads, which may sit in a native Present behind the gate.
void StopQueue(Owner& owner) noexcept
{
    const auto queue = owner.queue;
    if (!queue) return;
    queue->stop.store(true, std::memory_order_release);
    if (queue->wake) SetEvent(queue->wake);
    for (int i = 0; i < 200; ++i)
    {
        { std::lock_guard lock(queue->mutex); if (queue->exited) return; }
        Sleep(1);
    }
    std::lock_guard lock(queue->mutex);
    ReleaseQueue(*queue);
}

// The SDK's native swap chain (chain+kChainNative): its back buffers and maximum frame latency bound how many gated
// Presents can wait (the clock's capacity). Unknown values are taken as XeFG's own default of 3 (0x209B20 creates the
// native swap chain with the game's buffer count and a latency of at most 3), never as unbounded.
void NativeCapacity(const Owner& owner, uint32_t& buffers, uint32_t& latency) noexcept
{
    buffers = 3; latency = 3;
    try
    {
        auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
        if (!native || !single_overlay::native::PinInterface(native, 18)) return;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (SUCCEEDED(native->GetDesc1(&desc)) && desc.BufferCount >= 2 && desc.BufferCount <= DXGI_MAX_SWAP_CHAIN_BUFFERS)
            buffers = desc.BufferCount;
        IDXGISwapChain2* raw{};
        if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&raw))) && raw)
        {
            if (single_overlay::native::PinInterface(raw, 32))
            {
                UINT value{};
                if (SUCCEEDED(raw->GetMaximumFrameLatency(&value)) && value >= 1 && value <= 16) latency = value;
                raw->Release();
            }
        }
    }
    catch (...) {}
}

// vsync.1: the window of the SDK's native swap chain (IDXGISwapChain1::GetHwnd, slot 20); null when not proven.
HWND NativeWindow(const Owner& owner) noexcept
{
    try
    {
        auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
        HWND window{};
        if (native && single_overlay::native::PinInterface(native, 20) && SUCCEEDED(native->GetHwnd(&window))
            && window && IsWindow(window)) return window;
    }
    catch (...) {}
    return nullptr;
}

// XeFG waits on its native swap chain's frame-latency waitable before each native Present (0x21F730) and sets the
// latency to at most 3; with more back buffers (NativeBuffersScope) the queue can hold buffers - 1 Presents. DXGI adds
// waitable credits when the limit rises (dev.25 GPU test). Before the first Present of a new owner. menucap.1: at most
// 8 buffers under OptiScaler, so a latency of 7 (pacing.1's 10 gave 9).
uint32_t RaiseNativeLatency(const Owner& owner, uint32_t latency) noexcept
{
    try
    {
        auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
        if (!native || !single_overlay::native::PinInterface(native, 18)) return 0;
        IDXGISwapChain2* raw{};
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&raw))) || !raw) return 0;
        if (!single_overlay::native::PinInterface(raw, 32)) return 0; // quarantined
        const HRESULT result = raw->SetMaximumFrameLatency(latency);
        UINT now{};
        const bool read = SUCCEEDED(raw->GetMaximumFrameLatency(&now));
        raw->Release();
        return SUCCEEDED(result) && read ? now : 0;
    }
    catch (...) { return 0; }
}

// The owner's gate, ready fence and meter thread, on the device of the SDK's presentation queue (owner.flipQueue).
const char* StartQueue(Owner& owner) noexcept
{
    try
    {
        auto queue = std::make_shared<FlipQueue>();
        queue->generation = owner.generation;
        ID3D12Device* raw{};
        const HRESULT result = owner.flipQueue->GetDevice(IID_PPV_ARGS(&raw));
        if (!raw || !single_overlay::native::PinInterface(raw, 2)) return "queue_device"; // quarantined
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        device.Attach(raw);
        if (FAILED(result)) return "queue_device";
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->gate)))
            || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->ready)))) return "fence_creation";
        // gatepass.1: measurement only; without the pass fence the queue gates with the fixed lead.
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->pass)))) queue->pass.Reset();
        adapter_vendor_kmt::Ids ids{};
        if (adapter_vendor_kmt::Query(device->GetAdapterLuid(), ids)) queue->vendor = ids.vendor;
        queue->readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!queue->readyEvent || !queue->wake) return "event_creation";
        NativeCapacity(owner, queue->buffers, queue->latency);
        queue->window = NativeWindow(owner);
        if (queue->buffers > queue->latency+1)
            if (const auto raised = RaiseNativeLatency(owner, queue->buffers-1)) queue->latency = raised;
        std::thread(QueueThread, queue).detach();
        owner.queue = std::move(queue);
        return nullptr;
    }
    catch (...) { return "queue_exception"; }
}

// ---- amdflip.1: AMD's driver flip meter -----------------------------------------------------------------------------
// Driver (and OptiScaler hook) code behind raw vtables: a fault is a failed call, and the meter stays off.
constexpr HRESULT kAmdCallFault = HRESULT(0x8000FFFFL); // E_UNEXPECTED
HRESULT AmdCreateCall(AmdExtCreateInterfaceFunction create, IUnknown* device, void** factory) noexcept
{
    __try { return create(device, kAmdExtD3DFactoryIid, factory); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kAmdCallFault; }
}
HRESULT AmdFactoryCall(void* factory, IUnknown* outer, void** meter) noexcept
{
    using Create = HRESULT (STDMETHODCALLTYPE*)(void*, IUnknown*, REFIID, void**);
    __try { return reinterpret_cast<Create>((*static_cast<void***>(factory))[3])(factory, outer, kAmdFlipMeterIid, meter); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kAmdCallFault; }
}
HRESULT AmdCapsCall(void* meter, AmdFlipMeterCaps* caps) noexcept
{
    using GetCaps = HRESULT (STDMETHODCALLTYPE*)(void*, uint32_t, AmdFlipMeterCaps*, uint32_t);
    __try { return reinterpret_cast<GetCaps>((*static_cast<void***>(meter))[3])(meter, 0, caps, uint32_t(sizeof(*caps))); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kAmdCallFault; }
}
// slot 4 SetConfig, 5 GetConfig.
HRESULT AmdConfigCall(void* meter, size_t slot, AmdFlipMeterConfig* config) noexcept
{
    using Config = HRESULT (STDMETHODCALLTYPE*)(void*, AmdFlipMeterConfig*);
    __try { return reinterpret_cast<Config>((*static_cast<void***>(meter))[slot])(meter, config); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kAmdCallFault; }
}
void AmdReleaseCall(void* object) noexcept
{
    using Release = ULONG (STDMETHODCALLTYPE*)(void*);
    __try { reinterpret_cast<Release>((*static_cast<void***>(object))[2])(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// FSR's fullscreen test (amdxcffx64 0x180103EC0): the window's client area in screen coordinates is its monitor's
// rectangle (exclusive fullscreen or borderless).
bool CoversMonitor(HWND window) noexcept
{
    RECT client{};
    POINT origin{};
    if (!window || !IsWindow(window) || !GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return false;
    OffsetRect(&client, origin.x, origin.y);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    return monitor && GetMonitorInfoW(monitor, &info) && EqualRect(&client, &info.rcMonitor);
}

// AMD's meter on `device` as FSR opens it: the factory from the device, the meter from the factory with `outer` as its
// outer object, then caps (index 0, 16 bytes, status preset 0x0B). Usable only with caps status 0, a count limit and caps
// bit 3 clear (FSR leaves the meter off then). Returns the reason it is not usable (nothing is then held); otherwise the
// meter's one reference is in `meter`. result = the failing call; caps stays zero until read.
const char* OpenAmdMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, IUnknown* device, IUnknown* outer,
    void*& meter, AmdFlipMeterCaps& caps, bool& driverFactory, HRESULT& result) noexcept
{
    meter = nullptr; caps = {}; driverFactory = false; result = S_OK;
    if (!create) return "off";
    if (!device) return "device";
    void* factory{};
    result = AmdCreateCall(create, device, &factory);
    if (FAILED(result) || !factory) return "factory";
    if (!single_overlay::native::PinInterface(static_cast<IUnknown*>(factory), 3)) return "factory_interface"; // quarantined
    // OptiScaler's own AmdExtD3DFactory (its FSR 4 route) hands out its object without a reference and forwards Release
    // to the driver factory it keeps: releasing it would free that one. Only the driver's own factory is released.
    HMODULE factoryModule{};
    driverFactory = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(*static_cast<void**>(factory)), &factoryModule) && factoryModule && factoryModule == driver;
    void* opened{};
    result = AmdFactoryCall(factory, outer, &opened);
    if (driverFactory) AmdReleaseCall(factory);
    // E_NOINTERFACE: no hardware flip metering on this adapter (and no such interface in the RX 6000 driver branch).
    if (FAILED(result) || !opened) return "no_flip_meter";
    if (!single_overlay::native::PinInterface(static_cast<IUnknown*>(opened), 5)) return "meter_interface"; // quarantined
    caps = {0, 0, 0, 0x0b};
    result = AmdCapsCall(opened, &caps);
    const char* reason = FAILED(result) || caps.status ? "caps" : !caps.limit ? "caps_limit" : (caps.mask & 8) ? "caps_disabled"
        : nullptr;
    if (reason) { AmdReleaseCall(opened); return reason; }
    meter = opened;
    return nullptr;
}

// The owner's meter on `device` (the presentation queue's), with the native swap chain as its outer object, as FSR
// creates its own. The config flags are FSR's: bit 0 = caps bit 7, bits 1-2 = caps bits 5-6. Returns the reason it is not
// kept; result = the failing call.
const char* CreateAmdMeter(Owner& owner, IUnknown* device, uint32_t vendor, HWND window, HRESULT& result) noexcept
{
    result = S_OK;
    const auto create = amdCreate.load(std::memory_order_acquire);
    if (!create || !amdMode.load(std::memory_order_acquire)) return "off";
    if (vendor != 0x1002) return "not_amd";
    void* meter{};
    AmdFlipMeterCaps caps{};
    bool driverFactory{};
    const char* reason = OpenAmdMeter(create, amdDriver.load(std::memory_order_acquire), device,
        reinterpret_cast<IUnknown*>(owner.native), meter, caps, driverFactory, result);
    std::lock_guard lock(owner.amdMutex);
    owner.amd.caps = caps;
    owner.amd.driverFactory = driverFactory;
    if (reason) return reason;
    owner.amd.flags = ((caps.mask >> 7) & 1) | ((caps.mask >> 4) & 6);
    owner.amd.window = window;
    owner.amd.meter = meter;
    owner.amdMetered.store(true, std::memory_order_release);
    return nullptr;
}

// Sets `mode` with FSR's configuration (the caps flags; nothing else) and `count`, the batch's generated frames (amdcount.1;
// FSR's 1 fits only its one generated frame; mode 0 goes with FSR's 1). Under owner.amdMutex. A refused call keeps the
// mode and count in effect; kAmdFlipMeterFailureLimit in a row stop the owner asking. A nonzero mode is read back.
void ApplyAmdMeter(Owner& owner, uint32_t mode, uint32_t count, const char* reason) noexcept
{
    auto& amd = owner.amd;
    if (!amd.meter) return;
    AmdFlipMeterConfig config{};
    config.mode = mode; config.count = mode ? count : 1; config.flags = amd.flags;
    const uint32_t sent = config.count;
    const HRESULT result = AmdConfigCall(amd.meter, 4, &config);
    ++amd.calls;
    AmdFlipMeterConfig readback{};
    const HRESULT read = SUCCEEDED(result) && mode ? AmdConfigCall(amd.meter, 5, &readback) : S_FALSE;
    const uint32_t previous = amd.applied;
    if (SUCCEEDED(result)) { amd.applied = mode; amd.count = mode ? count : 0; amd.failures = 0; }
    else if (++amd.failures >= kAmdFlipMeterFailureLimit) amd.refused = true;
    if (amd.calls <= 64 || amd.refused)
        try
        {
            xess_mod::WriteLog("amd_flip_meter_config", "\"generation\":"+std::to_string(owner.generation)+
                ",\"requested\":"+std::to_string(mode)+",\"previous\":"+std::to_string(previous)+
                ",\"applied\":"+std::to_string(amd.applied)+",\"reason\":\""+reason+"\",\"count\":"+std::to_string(sent)+
                ",\"caps_limit\":"+std::to_string(amd.caps.limit)+",\"flags\":"+
                std::to_string(amd.flags)+",\"result\":"+std::to_string(result)+",\"status\":"+std::to_string(config.status)+
                ",\"output\":"+std::to_string(config.output)+",\"readback_result\":"+std::to_string(read)+
                ",\"readback_mode\":"+std::to_string(readback.mode)+",\"readback_count\":"+std::to_string(readback.count)+
                ",\"readback_flags\":"+std::to_string(readback.flags)+",\"calls\":"+std::to_string(amd.calls)+
                ",\"refused\":"+(amd.refused ? "true" : "false")+",\"local_time\":\""+LocalClock()+"\"");
        }
        catch (...) {}
}

// amdlog.1: a decision the driver already has in effect (no SetConfig call), e.g. the meter left off at 3X+ after frame
// generation was off: logged when its reason or the batch's generated count changes. Under owner.amdMutex.
void LogAmdDecision(Owner& owner, uint32_t mode, uint32_t generated, const char* reason) noexcept
{
    auto& amd = owner.amd;
    if (++amd.decisions > kAmdDecisionLogLimit) return;
    try
    {
        xess_mod::WriteLog("amd_flip_meter_decision", "\"generation\":"+std::to_string(owner.generation)+
            ",\"requested\":"+std::to_string(mode)+",\"applied\":"+std::to_string(amd.applied)+",\"reason\":\""+reason+
            "\",\"generated\":"+std::to_string(generated)+",\"caps_limit\":"+std::to_string(amd.caps.limit)+
            ",\"driver_call\":false,\"decisions\":"+std::to_string(amd.decisions)+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// Before the SDK's native Present (presenting threads): a batch's first generated output asks for mode 2 (auto: while
// the window covers its monitor, checked every kAmdFullscreenCheckNs) with its generated count read from the caller's
// frame as the flip queue reads it (amdcount.1); a count above the caps limit, or a frame that does not give it, asks for
// mode 0, as does any other Present on the presenter (frame generation off). The driver is called only on a change of
// mode or count (Dynamic MFG's multiplier changes included); a changed reason or count without a call is logged
// (amdlog.1).
void AmdBeforePresent(Route& current, const Invocation& call)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    if (!first && (offset == kExtraReturn || offset == kRealReturn)) return;
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (!owners) return;
    for (const auto& entry : *owners)
    {
        auto& owner = *entry;
        if (owner.presenter != uintptr_t(call.presenter)) continue;
        if (!owner.amdMetered.load(std::memory_order_acquire) || owner.retired.load(std::memory_order_acquire)) return;
        std::lock_guard lock(owner.amdMutex);
        auto& amd = owner.amd;
        if (!amd.meter || amd.refused) return;
        uint32_t mode{}, generated{};
        const char* reason = "frame_generation_off";
        if (first && !First(current, owner, call, generated)) reason = "first_call_layout";
        else if (first && generated > amd.caps.limit) reason = "count_above_limit";
        else if (first && amdMode.load(std::memory_order_acquire) == 2) { mode = kAmdFlipMeterMode; reason = "on"; }
        else if (first)
        {
            const auto now = NowNs();
            if (!amd.checkedNs || now-amd.checkedNs >= kAmdFullscreenCheckNs)
            {
                amd.fullscreen = CoversMonitor(amd.window);
                amd.checkedNs = now;
            }
            mode = amd.fullscreen ? kAmdFlipMeterMode : 0;
            reason = amd.fullscreen ? "fullscreen" : "windowed";
        }
        const uint32_t count = mode ? generated : 0;
        const bool changed = !amd.reason || strcmp(amd.reason, reason) || amd.generated != generated;
        if (mode != amd.applied || count != amd.count) ApplyAmdMeter(owner, mode, count, reason);
        else if (changed) LogAmdDecision(owner, mode, generated, reason);
        amd.reason = reason; amd.generated = generated;
        return;
    }
}

// Teardown (Unregister, or an owner that never reached the route): mode 0 if set, then the meter's reference, as FSR.
void ReleaseAmdMeter(Owner& owner) noexcept
{
    try
    {
        std::lock_guard lock(owner.amdMutex);
        owner.amdMetered.store(false, std::memory_order_release);
        auto& amd = owner.amd;
        if (!amd.meter) return;
        if (amd.applied) ApplyAmdMeter(owner, 0, 0, "teardown");
        AmdReleaseCall(amd.meter);
        amd.meter = nullptr;
        xess_mod::WriteLog("amd_flip_meter_released", "\"generation\":"+std::to_string(owner.generation)+
            ",\"calls\":"+std::to_string(amd.calls)+",\"applied\":"+std::to_string(amd.applied)+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// The flip queue owns this presenter's pacing: the SDK's own Pace and V1.6.0's extra pacing are skipped. vsync.1: not
// for a batch whose first output was not gated (sdkPace).
bool QueuedPresenter(const Route& current, const void* presenter) noexcept
{
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (owners) for (const auto& owner : *owners)
        if (owner->presenter == uintptr_t(presenter))
            return owner->Queued() && !owner->retired.load(std::memory_order_acquire)
                && !owner->disabled.load(std::memory_order_acquire) && !owner->sdkPace.load(std::memory_order_acquire);
    return false;
}

// Before the SDK's native Present (after its copy was queued): an output of the batch being gated gets its ready
// value and then its gate Wait on the SDK's presentation queue, and the meter thread its release. The batch is the one
// its first output (the profiled first-output caller, First()) opened; the generated outputs and then the real frame
// follow in order on the SDK's worker with the same frame value and present mode. vsync.1: V-Sync at interval 1 is
// gated like V-Sync off. Anything else (higher sync intervals or other present flags, frame generation off, an unproven
// first output) is not gated and simply follows the gated Presents on the queue; a refused first output gives its batch
// back to the SDK's own pacing. Returns true when gated.
// V1.7.0 smallchain.1 (a tester, RX 7900 XTX, perffix.1: The Last of Us Part II and Dying Light 2 at 5X/6X; Cyberpunk
// fine): native_buffers_scope refused with factory_not_dxgi, so XeFG's native chain kept the game's 3 buffers (latency 3).
// A 5- or 6-output batch cannot queue on it, a gated Present blocks inside every batch, and the game's hand-over follows
// our flips: base 21.8 at 5X/6X against 41.9 at 4X, every output held 6-8 ms past readiness while coupled, full and deep
// all stayed 0 (vsync.2's backlog test and lockin.1's full test are both false on such a chain). On an unmetered queue
// (AMD, Intel, NVIDIA without flip metering) a batch whose outputs exceed the native latency + 1 goes back to the SDK's own
// pacing, as V1.6.2-beta.4/.5 ran RX 7000 with the queue off; 4X on a latency-3 chain and every batch on a raised chain
// stay gated, and NVIDIA's metered batches keep the queue (follow.1 covers the 3-buffer case there). The first such batch
// per owner is logged (flip_queue_small_chain), every one as flip_queue_ungated native_chain_too_small (first 8).
// V1.7.0 smallchain.2 (alpha.17 in Subnautica 2, RTX 4080, OptiScaler route: ReShade's dxgi.dll keeps XeFG's chain at 3
// buffers): smallchain.1 keyed on the batch's metering, and NVIDIA's metering is off at every multiplier change until the
// clock knows the batch time (FlipBeforePresent: interval_unknown), which only gated batches teach it. At 5X/6X every batch
// went to the SDK's pacing for good and flipped at readiness above the refresh (148-162 fps on 120 Hz; XeFG's debug view
// showed 2-3 of 4-5 generated frames per batch). The rule now asks whether the route has driver flip metering at all
// (flipEnabled: NVIDIA's, unless switched off), so NVIDIA keeps beta.4's gated, metered path on a small chain.
bool GateBeforePresent(Route& current, const Invocation& call, std::shared_ptr<FlipQueue>& gated)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    const bool extra = offset == kExtraReturn && call.generated;
    const bool real = offset == kRealReturn && !call.generated;
    if (!first && !extra && !real) return false;
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (!owners) return false;
    for (const auto& entry : *owners)
    {
        auto& owner = *entry;
        if (owner.presenter != uintptr_t(call.presenter)) continue;
        if (!owner.Queued() || !owner.flipQueue || owner.retired.load(std::memory_order_acquire)
            || owner.disabled.load(std::memory_order_acquire)) return false;
        auto& queue = *owner.queue;
        uint32_t outputs{}, index{}, smallChain{};
        const char* ungated{};
        {
            std::lock_guard lock(owner.gateMutex);
            const bool mode = GatedPresentMode(call);
            if (first)
            {
                uint32_t generated{};
                owner.gateBatch = mode && First(current, owner, call, generated);
                // smallchain.1: on a route without driver flip metering (smallchain.2), the native latency below the
                // batch's outputs - 1 (here: below the generated count).
                if (owner.gateBatch && !flipEnabled.load(std::memory_order_acquire) && queue.latency
                    && queue.latency < generated)
                { owner.gateBatch = false; smallChain = generated+1; }
                owner.sdkPace.store(!owner.gateBatch, std::memory_order_release);
                if (!owner.gateBatch)
                    ungated = smallChain ? "native_chain_too_small" : mode ? "first_call_layout" : "present_mode";
                else
                {
                    owner.gateFrame = call.frame; owner.gateOutputs = generated+1; owner.gateNext = 1;
                    owner.gateSync = call.sync;
                    outputs = owner.gateOutputs;
                }
            }
            else
            {
                const bool expected = owner.gateBatch && mode && call.sync == owner.gateSync && call.frame == owner.gateFrame
                    && owner.gateNext < owner.gateOutputs && (real ? owner.gateNext+1 == owner.gateOutputs
                        : owner.gateNext+1 < owner.gateOutputs);
                if (!expected) { owner.gateBatch = false; return false; }
                outputs = owner.gateOutputs; index = owner.gateNext++;
                if (real) owner.gateBatch = false;
            }
        }
        if (ungated)
        {
            if (smallChain && !owner.smallChainLogged.exchange(true, std::memory_order_relaxed))
                try
                {
                    xess_mod::WriteLog("flip_queue_small_chain", "\"generation\":"+std::to_string(owner.generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"outputs\":"+std::to_string(smallChain)+
                        ",\"buffers\":"+std::to_string(queue.buffers)+",\"max_latency\":"+std::to_string(queue.latency)+
                        ",\"vendor_id\":"+std::to_string(queue.vendor)+",\"metered\":false,\"paced\":\"sdk\""+
                        ",\"local_time\":\""+LocalClock()+"\"");
                }
                catch (...) {}
            if (owner.ungatedLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                try
                {
                    xess_mod::WriteLog("flip_queue_ungated", "\"generation\":"+std::to_string(owner.generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"reason\":\""+ungated+"\",\"sync\":"+
                        std::to_string(call.sync)+",\"flags\":"+std::to_string(call.flags));
                }
                catch (...) {}
            return false;
        }
        const auto now = NowNs();
        std::lock_guard lock(queue.mutex);
        if (queue.exited || queue.stop.load(std::memory_order_acquire)) return false;
        const uint64_t signaled = queue.signaled.load(std::memory_order_acquire);
        if (queue.issued > signaled && now-queue.progressNs.load(std::memory_order_relaxed) > kQueueStallNs)
        {
            // Watchdog: Waits pending with no progress (meter thread starved or gone): release them, stop gating.
            ReleaseQueue(queue);
            queue.failed.store(true, std::memory_order_release);
            try
            {
                xess_mod::WriteLog("flip_queue_watchdog", "\"generation\":"+std::to_string(owner.generation)+
                    ",\"released_gate\":"+std::to_string(queue.issued)+",\"signaled\":"+std::to_string(signaled)+
                    ",\"local_time\":\""+LocalClock()+"\"");
            }
            catch (...) {}
            return false;
        }
        const uint64_t value = queue.issued+1;
        if (FAILED(owner.flipQueue->Signal(queue.ready.Get(), value))
            || FAILED(owner.flipQueue->Wait(queue.gate.Get(), value)))
        {
            // A ready value alone holds nothing; a failed Wait queued nothing.
            queue.failed.store(true, std::memory_order_release);
            return false;
        }
        // gatepass.1: a sampled output's pass Signal right behind its gate Wait. Measurement only: a failure turns it off
        // and the gate goes on as before; nothing on this thread waits for the pass fence.
        bool pass{};
        if (queue.pass && !queue.passOff.load(std::memory_order_acquire) && GatePassSample(queue))
        {
            pass = SUCCEEDED(owner.flipQueue->Signal(queue.pass.Get(), value));
            if (!pass) GatePassOff(queue, "signal");
        }
        if (queue.issued == signaled) queue.progressNs.store(now, std::memory_order_relaxed); // busy again
        queue.issued = value;
        if (first) queue.sync.store(call.sync, std::memory_order_relaxed);
        // vsync.2: a blocked previous batch held the game only if this batch was already waiting for the presenter. With
        // more outputs than the native latency (4X+ on XeFG's 3-buffer chain) a gated Present blocks inside every batch,
        // also while the game hands its batches over on its own; counted as coupled, each such batch decayed the clock
        // 3%, the meter ran ahead of the game and the batch's first output missed its slot (fixture under V-Sync + VRR,
        // 4X at 25 fps: 80% late firsts, the real -> next first seam 11.1 ms against 9.6 ms inside the batch).
        const bool blocked = first && queue.blocked.exchange(false, std::memory_order_acq_rel);
        const int64_t returned = queue.returnedNs.load(std::memory_order_acquire);
        const int64_t idle = first && returned && now > returned ? now-returned : 0;
        const bool backlog = returned && idle < kQueueBacklogNs;
        // lockin.1 (Wuthering Waves, 8 native buffers, latency 7, 3X: coupled_idle 58, coupled 0, the clock learned its
        // own cadence up to 57.6 ms per output): a block with more than a whole batch of gates waiting held the game.
        const bool full = blocked && queue.latency > outputs;
        if (blocked && !backlog && !full) queue.idleBlocked.fetch_add(1, std::memory_order_relaxed);
        if (full) queue.fullBlocked.fetch_add(1, std::memory_order_relaxed);
        // presenterlead.1 (Cyberpunk, RX 7900 XTX, beta.3, 10 native buffers, latency 9, 4X-6X: presenter idle p50 0.08 ms,
        // outputs held 8-12 ms at their gates, coupled / coupled_full / deep all 0, base fps 14-21 against 1.6.1's 28-32):
        // the output ring keeps the presenter a few outputs ahead of the gates, so nothing blocks in the native Present,
        // yet the batch was waiting for our previous Present: the hand-over follows our flips. A backlogged first output
        // counts as coupled with or without a block; the clock's follow rule, on-time check and floor still bound the decay.
        if (first && backlog && !blocked) queue.leadBacklogged.fetch_add(1, std::memory_order_relaxed);
        queue.items.push_back({value, first ? now : 0, idle, outputs, index, first, backlog || full, call.sync != 0, pass,
            full});
        SetEvent(queue.wake);
        gated = owner.queue;
        return true;
    }
    return false;
}

// How many gated Presents must still be waiting when a native Present returns blocked for the queue to have held it.
// XeFG waits on its native swap chain's frame-latency waitable before each Present, so the queue holds it only once
// about its maximum latency is queued: 2 on XeFG's own 3-buffer chain (pacing.2). coupled.1 (Stellar Blade on
// Proton/vkd3d-proton, AMD, 10 native buffers, latency 9): vkd3d-proton's Present takes > 1 ms with 2-3 gates waiting,
// so the fixed 2 flagged nearly every batch coupled, the clock decayed to a quarter of the game's rate and each batch
// flipped as a burst followed by a long gap.
uint64_t CoupledPending(const FlipQueue& queue) noexcept
{
    return (std::max)(kQueueCoupledPending, uint64_t(queue.latency > 1 ? queue.latency-1 : 1));
}

// xemfgsa.4: each thread presenting XeFG's outputs (the game's for the real frame, XeFG's presenter for the generated
// ones), logged once (the first 8), so a fault's thread id in the session log can be named.
constexpr uint32_t kPresenterThreads = 8;
std::atomic<DWORD> presenterThreads[kPresenterThreads]{};
std::atomic<uint32_t> presenterThreadCount{};

void NotePresenterThread() noexcept
{
    const DWORD thread = GetCurrentThreadId();
    const uint32_t known = (std::min)(presenterThreadCount.load(std::memory_order_acquire), kPresenterThreads);
    for (uint32_t i = 0; i < known; ++i)
        if (presenterThreads[i].load(std::memory_order_relaxed) == thread) return;
    const uint32_t slot = presenterThreadCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kPresenterThreads) return;
    presenterThreads[slot].store(thread, std::memory_order_relaxed);
    try
    {
        xess_mod::WriteLog("presenter_thread", "\"thread\":"+std::to_string(thread)+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// xemfgsa.5: XeFG's native Presents (OutputPresents).
std::atomic<uint64_t> outputPresents{};

HRESULT Dispatch(Route& current, const Invocation& call)
{
    const DWORD incomingError = GetLastError();
    if(current.enabled.load(std::memory_order_acquire) && call.generated
        && call.returnAddress==uintptr_t(current.module)+kFirstReturn) {
        const bool wanted=sdkDeadlinesRequested.load(std::memory_order_acquire);
        if(sdkDeadlinesActive.exchange(wanted)!=wanted) {
            if(const auto owners=current.owners.load(std::memory_order_acquire))
                for(const auto& owner:*owners) if(owner->presenter==uintptr_t(call.presenter)) owner->intervalLogs=0;
            try { xess_mod::WriteLog("pacing_policy",wanted?"\"deadlines\":\"sdk\"":"\"deadlines\":\"adaptive\""); } catch(...) {}
        }
    }
    NotePresenterThread();
    outputPresents.fetch_add(1, std::memory_order_relaxed);
    if (current.enabled.load(std::memory_order_acquire) && flipFunction.load(std::memory_order_acquire))
    {
        try { FlipBeforePresent(current, call); }
        catch (...) {} // Flip metering must not replace the original Present result.
    }
    if (current.enabled.load(std::memory_order_acquire) && amdCreate.load(std::memory_order_acquire))
    {
        try { AmdBeforePresent(current, call); }
        catch (...) {} // amdflip.1: nor may AMD's.
    }
    bool queued{};
    std::shared_ptr<FlipQueue> gated;
    if (current.enabled.load(std::memory_order_acquire))
    {
        try { queued = GateBeforePresent(current, call, gated) || QueuedPresenter(current, call.presenter); }
        catch (...) {} // The flip queue must not replace the original Present result.
    }
    if (!queued && current.enabled.load(std::memory_order_acquire)
        && call.returnAddress == uintptr_t(current.module)+kExtraReturn && call.generated)
    {
        try
        {
            const auto owners = current.owners.load(std::memory_order_acquire);
            if (owners) for (const auto& owner : *owners)
            {
                if (owner->presenter != uintptr_t(call.presenter) || owner->disabled.load(std::memory_order_acquire)
                    || owner->retired.load(std::memory_order_acquire)) continue;
                Extra extra{};
                if (const auto* reason = Prepare(current, *owner, call, extra))
                {
                    if (owner->skippedLogs.fetch_add(1, std::memory_order_relaxed) < 4)
                        xess_mod::WriteLog("extra_pacing_skipped", "\"generation\":"+std::to_string(owner->generation)+
                            ",\"reason\":\""+reason+"\"");
                    break;
                }
                LARGE_INTEGER before{}, after{};
                QueryPerformanceCounter(&before);
                bool paced{};
                if (owner->retired.load(std::memory_order_acquire)) break;
                try {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
                    mfgunlock::xemfg::native::RuntimeCall trace("xefg_extra_pace",0,call.frame);
#endif
                    paced = current.pace(call.presenter, extra.bundle, extra.firstFence, extra.summary, extra.index);
                }
                catch (...)
                {
                    owner->disabled.store(true, std::memory_order_release);
                    xess_mod::WriteLog("extra_pacing_disabled", "\"reason\":\"sdk_pacing_exception\"");
                    break;
                }
                QueryPerformanceCounter(&after);
                if (owner->pacedLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                    xess_mod::WriteLog("extra_pacing", "\"generation\":"+std::to_string(owner->generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"index\":"+std::to_string(extra.index)+
                        ",\"generated\":"+std::to_string(extra.generatedCount)+",\"median_period_ns\":"+
                        std::to_string(extra.periodNs)+",\"paced\":"+(paced ? "true" : "false")+
                        ",\"begin_qpc\":"+std::to_string(before.QuadPart)+",\"end_qpc\":"+std::to_string(after.QuadPart));
                break;
            }
        }
        catch (...) {} // The added pacing must not replace the original Present result.
    }
    SetLastError(incomingError);
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::xemfg::native::RuntimeCall trace("xefg_presenter",0,call.frame);
#endif
    if (!gated)
        return current.original(call.presenter, call.sync, call.flags, call.frame, call.parameters, call.resource, call.generated);
    // pacing.1: how long the gated native Present held this thread (the queue's back-pressure on the game).
    const auto entered = NowNs();
    SetLastError(incomingError);
    const HRESULT result = current.original(call.presenter, call.sync, call.flags, call.frame, call.parameters, call.resource,
        call.generated);
    const DWORD outgoingError = GetLastError();
    const auto returned = NowNs();
    // pacing.2: held by our own queue only while gated Presents are waiting (ReShade on the native swap chain makes
    // every Present slow, with the queue empty: that was counted as coupling on every batch). coupled.1: as many as
    // fill the native swap chain (CoupledPending).
    if (entered && returned-entered > kQueueBlockedNs
        && gated->issued-gated->signaled.load(std::memory_order_acquire) >= CoupledPending(*gated))
        gated->blocked.store(true, std::memory_order_release);
    if (returned) gated->returnedNs.store(returned, std::memory_order_release); // vsync.2: the backlog reference
    SetLastError(outgoingError);
    return result;
}

__declspec(noinline) HRESULT HookPresent(void* presenter, UINT sync, UINT flags, uint64_t frame,
    const DXGI_PRESENT_PARAMETERS* parameters, ID3D12Resource* const* resource, bool generated)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->original) return DXGI_ERROR_INVALID_CALL;
    const Invocation call{presenter, sync, flags, frame, parameters, resource, generated,
        uintptr_t(_ReturnAddress()), uintptr_t(_AddressOfReturnAddress())};
    // vkreflex.1: on the Vulkan host, the xvk swap chain's Present1 runs inside this call and labels the output for
    // Reflex (its batch's frame, real or generated).
    const xvk::OutputFrameScope output(frame, generated);
    return Dispatch(*current, call);
}

// hitch.1: what one observed batch adds to the log. Built under the owner's clockMutex, written after
// its locks are released.
struct HitchNote
{
    bool hitch{}, reset{}, stats{};
    const char* reason{};
    int64_t delta{}, rawDelta{}, median{}, mean{}, typical{};
    uint32_t gap{}, samples{}, excluded{}, outputs{};
    Owner::Hitches counters{};
    bool Any() const noexcept { return hitch || reset || stats; }
};

void NoteBatch(Owner& owner, cadence::BatchIntervalEstimator::Event event, HitchNote& note) noexcept
{
    using Event = cadence::BatchIntervalEstimator::Event;
    if (event == Event::kRepeat) return;
    auto& h = owner.hitches;
    const auto& e = owner.batchInterval;
    ++h.batches;
    note.delta = e.LastDelta(); note.rawDelta = e.RawDelta(); note.gap = e.Gap();
    note.median = e.Median(); note.mean = e.Mean(); note.typical = e.Typical();
    note.samples = e.Samples(); note.excluded = e.Excluded(); note.outputs = e.Outputs();
    if (event == Event::kBridge) ++h.bridged;
    else if (event == Event::kResetGap) { ++h.resetGap; note.reason = "frame_gap"; }
    else if (event == Event::kResetRange) { ++h.resetRange; note.reason = "delta_range"; }
    else if (event == Event::kResetBase) { ++h.resetBase; note.reason = "no_batch_start"; }
    if (note.reason && h.resetLogs < kResetLogLimit) { ++h.resetLogs; note.reset = true; }
    if (note.delta && note.median > 0 && note.delta >= kHitchRatio*note.median)
    {
        ++h.hitches;
        if (h.hitchLogs < kHitchLogLimit) { ++h.hitchLogs; note.hitch = true; }
    }
    if (h.batches % kHitchStatsBatches == 0) { note.stats = true; note.counters = h; }
}

void LogHitches(const Owner& owner, uint32_t frame, const HitchNote& note, int64_t flipBatchNs, bool flipMetered,
    uint64_t flipLimited) noexcept
{
    try
    {
        const auto generation = ",\"generation\":"+std::to_string(owner.generation);
        const auto clock = ",\"local_time\":\""+LocalClock()+"\"";
        if (note.reset)
            xess_mod::WriteLog("pacing_reset", "\"reason\":\""+std::string(note.reason)+"\""+generation+
                ",\"frame\":"+std::to_string(frame)+",\"gap\":"+std::to_string(note.gap)+
                ",\"delta_ns\":"+std::to_string(note.rawDelta)+clock);
        if (note.hitch)
            xess_mod::WriteLog("pacing_hitch", "\"frame\":"+std::to_string(frame)+generation+
                ",\"outputs\":"+std::to_string(note.outputs)+",\"delta_ns\":"+std::to_string(note.delta)+
                ",\"median_ns\":"+std::to_string(note.median)+",\"mean_ns\":"+std::to_string(note.mean)+
                ",\"estimate_ns\":"+std::to_string(note.typical)+",\"samples\":"+std::to_string(note.samples)+
                ",\"excluded\":"+std::to_string(note.excluded)+",\"flip_time_per_batch_ns\":"+std::to_string(flipBatchNs)+
                ",\"flip_metered\":"+(flipMetered ? "true" : "false")+clock);
        if (note.stats)
        {
            const auto& c = note.counters;
            xess_mod::WriteLog("pacing_stats", "\"batches\":"+std::to_string(c.batches)+generation+
                ",\"hitches\":"+std::to_string(c.hitches)+",\"bridged\":"+std::to_string(c.bridged)+
                ",\"reset_gap\":"+std::to_string(c.resetGap)+",\"reset_range\":"+std::to_string(c.resetRange)+
                ",\"reset_base\":"+std::to_string(c.resetBase)+",\"revoked\":"+std::to_string(c.revoked)+
                ",\"estimate_ns\":"+std::to_string(note.typical)+",\"mean_ns\":"+std::to_string(note.mean)+
                ",\"median_ns\":"+std::to_string(note.median)+",\"excluded\":"+std::to_string(note.excluded)+
                ",\"outputs\":"+std::to_string(note.outputs)+",\"flip_growth_limited\":"+std::to_string(flipLimited)+clock);
        }
    }
    catch (...) {}
}

// The SDK's own Pace: replace only the arithmetic deadline with the DLSS-G interval, less the
// SDK's own present latency (latency.1).
// The SDK has already waited for output 0's present fence and stamped the base; its
// own wait helper still performs the wait.
void NativeDeadline(Route& current, void* timing, int64_t* output, const void* record,
    uint32_t index, uint32_t total) noexcept
{
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (!owners) return;
    for (const auto& entry : *owners)
    {
        auto& owner = *entry;
        if (uintptr_t(timing) != owner.presenter+0x168) continue;
        uint32_t frame{}; int64_t timestamp{}, batchStart{};
        float latencyMs{};
        if (owner.disabled.load(std::memory_order_acquire) || owner.retired.load(std::memory_order_acquire)
            || !StackRange(uintptr_t(output), sizeof(*output)) || !StackRange(uintptr_t(record), 0x28)
            || !Value(uintptr_t(record), timestamp) || timestamp <= 0
            || !Value(uintptr_t(record)+0x10, frame) || !Value(uintptr_t(record)+0x14, latencyMs)
            || !Value(uintptr_t(record)+0x20, batchStart) || !Live(owner)) return;
        // latency.1: the present latency XeFG's own deadline subtracts (0x224B30 reads record+0x14, in ms, as
        // every family's witnessed body does); a value outside 0-250 ms is not used.
        const int64_t gpuLatency = std::isfinite(latencyMs) && latencyMs > 0.0f && latencyMs <= 250.0f
            ? int64_t(double(latencyMs)*1e6) : 0;
        const auto now = NowNs();
        cadence::BatchIntervalEstimator::Decision decision{};
        bool planned{};
        HitchNote note{};
        int64_t meanInterval{};
        uint32_t excluded{};
        {
            std::lock_guard lock(owner.clockMutex);
            if (index == 1) NoteBatch(owner, owner.batchInterval.Observe(frame, total, batchStart), note);
            planned = owner.batchInterval.Plan(frame, total, index, now, decision, gpuLatency);
            if (const auto outputs = owner.batchInterval.Outputs()) meanInterval = owner.batchInterval.Mean()/int64_t(outputs);
            excluded = owner.batchInterval.Excluded();
        }
        const auto sdkDeadline = *output;
        const bool nativeDeadlines=sdkDeadlinesActive.load(std::memory_order_acquire);
        if (planned && !nativeDeadlines) *output = decision.deadline;
        // flipmeter.1: inside a batch the driver meters, the configured spacing (the hardware's)
        // places each output and it is released early by the lead.
        cadence::MeteredDeadline metered{};
        bool meteredApplied{}, flipMetered{},queueAhead{};
        int64_t spacing{}, flipBatchNs{}, flipBase{};
        uint64_t flipLimited{};
        {
            std::lock_guard lock(owner.flipMutex);
            auto& flip = owner.flip;
            // seam.1: this batch's output 0 Present, when it precedes the SDK's stamp by at most one
            // batch; otherwise the stamp, as before.
            const bool anchored = flip.anchorFrame == frame && flip.anchorNs > 0 && batchStart > 0
                && flip.anchorNs <= batchStart && batchStart-flip.anchorNs <= flip.batchNs;
            flipBase = anchored ? flip.anchorNs : batchStart;
            const cadence::MeteredBatch batch{flip.metered,flip.queueAhead,flip.anchorFrame,flip.outputs,flip.anchorNs,flip.batchNs};
            if(!nativeDeadlines && flipEnabled.load(std::memory_order_acquire) && batch.CanQueue(frame,total,index,now)) {
                // The original SDK Pace remains in control of resource/fence readiness.
                // Driver metering owns spacing; do not impose the display schedule again on the CPU.
                *output=now; queueAhead=true; meteredApplied=true; spacing=flip.spacing;
                ++flip.queueAheadDeadlines;
            }
            else if (!nativeDeadlines && flip.metered && flip.outputs == total && flipEnabled.load(std::memory_order_acquire)
                && cadence::PlanMetered(flipBase, flip.spacing, index, flipLeadNs.load(std::memory_order_relaxed),
                    now, metered))
            {
                *output = metered.deadline;
                meteredApplied = true;
                spacing = flip.spacing;
            }
            flipBatchNs = flip.batchNs; flipMetered = flip.metered; flipLimited = flip.limited;
        }
        if (note.Any()) LogHitches(owner, frame, note, flipBatchNs, flipMetered, flipLimited);
        if (owner.intervalLogs.fetch_add(1, std::memory_order_relaxed) < 24 || (index == 1 && frame%120 == 0))
            xess_mod::WriteLog("dlssg_interval_deadline", "\"generation\":"+std::to_string(owner.generation)+
                ",\"frame\":"+std::to_string(frame)+",\"index\":"+std::to_string(index)+",\"outputs\":"+std::to_string(total)+
                ",\"applied\":"+(planned && !nativeDeadlines ? "true" : "false")
                +",\"sdk_deadlines\":"+(nativeDeadlines ? "true" : "false")+",\"interval_ns\":"+std::to_string(decision.interval)+
                ",\"samples\":"+std::to_string(decision.samples)+",\"capped\":"+(decision.capped ? "true" : "false")+
                ",\"batch_start_ns\":"+std::to_string(batchStart)+",\"now_ns\":"+std::to_string(now)+
                ",\"sdk_deadline_ns\":"+std::to_string(sdkDeadline)+",\"deadline_ns\":"+std::to_string(*output)+
                ",\"flip_metered\":"+(meteredApplied ? "true" : "false")+",\"queue_ahead\":"+(queueAhead ? "true" : "false")+",\"flip_base_ns\":"+std::to_string(flipBase)+
                ",\"flip_lead_ns\":"+std::to_string(metered.lead)+
                ",\"flip_spacing_ns\":"+std::to_string(spacing)+",\"mean_interval_ns\":"+std::to_string(meanInterval)+
                ",\"excluded\":"+std::to_string(excluded)+",\"gpu_latency_ns\":"+std::to_string(gpuLatency)+
                ",\"latency_lead_ns\":"+std::to_string(decision.lead));
        return;
    }
}

int64_t* DispatchDeadline(Route& current, void* timing, int64_t* output, const void* record,
    const void* summary, uint32_t index, uint32_t total, uintptr_t returnAddress)
{
    auto* result = current.deadline(timing, output, record, summary, index, total);
    const DWORD outgoingError = GetLastError();
    if (result == output && current.enabled.load(std::memory_order_acquire)
        && returnAddress == uintptr_t(current.module)+kDeadlineReturn
        && index >= 1 && total >= 2 && total-1 <= xess_mod::kGeneratedCapacity)
    {
        try { NativeDeadline(current, timing, output, record, index, total); }
        catch (...) {} // Logging allocation must not replace the SDK deadline.
    }
    SetLastError(outgoingError);
    return result;
}

__declspec(noinline) int64_t* HookDeadline(void* timing, int64_t* output, const void* record,
    const void* summary, uint32_t index, uint32_t total)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->deadline) return output;
    return DispatchDeadline(*current, timing, output, record, summary, index, total, uintptr_t(_ReturnAddress()));
}

// The SDK's own Pace runs unchanged; the hook provides the trampoline the extra-output
// pacing in Dispatch calls. pacing.1: a presenter on the flip queue is not paced on the CPU (the real frame's Pace
// would also wait for output 0's present fence, which the gate now holds); its return value is not used (0x220411).
// vsync.1: a batch the queue did not gate is paced by the SDK as without the queue.
__declspec(noinline) bool HookPace(void* presenter, const void* bundle, uint8_t firstFence,
    const void* summary, uint32_t index)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->pace) return false;
    try
    {
        if (current->enabled.load(std::memory_order_acquire) && QueuedPresenter(*current, presenter)) return false;
    }
    catch (...) {}
    return current->pace(presenter, bundle, firstFence, summary, index);
}

bool HookPointsTo(unsigned char* target, uintptr_t hook) noexcept
{
    if (target[0] != 0xe9) return false;
    int32_t relative{}; memcpy(&relative, target+1, 4);
    const uintptr_t relay = uintptr_t(target+5)+relative;
    if (relay == hook) return true;
    std::array<unsigned char, 14> bytes{};
    if (!Read(relay, bytes.data(), bytes.size()) || bytes[0] != 0xff || bytes[1] != 0x25) return false;
    int32_t displacement{}; memcpy(&displacement, bytes.data()+2, 4);
    uintptr_t destination{};
    return Value(relay+6+displacement, destination) && destination == hook;
}
bool HookPointsToUs(unsigned char* target) noexcept { return HookPointsTo(target, uintptr_t(&HookPresent)); }
}

bool Install(HMODULE provider, std::string& failure) noexcept
{
    try
    {
        std::lock_guard lock(installMutex);
        failure.clear();
        if (auto* current = route.load(std::memory_order_acquire))
            return current->module == provider && current->enabled.load(std::memory_order_acquire);
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(provider, path, DWORD(std::size(path)));
        if (!provider || !length || length >= std::size(path)
            || !xess_layout::Get(provider).family)
            throw std::runtime_error("pacing_provider_identity");
        auto* base = reinterpret_cast<unsigned char*>(provider);
        const auto& layout=xess_layout::Get(provider);
        for (const auto function : {0x21f730u,0x21ee30u,0x224b30u})
            layout.VerifyFunction(provider,function);
        // flipmeter.1: First() reads the first-output caller's frame; without it only flip metering is off.
        bool firstVerified{};
        try { layout.VerifyFunction(provider, 0x220c70u); firstVerified = true; }
        catch (const std::exception&) {}
        kNativePresent=layout.Rva(0x21f730); kPace=layout.Rva(0x21ee30);
        kExtraReturn=layout.Rva(0x2202ed); kFirstReturn=layout.Rva(0x220db2); kRealReturn=layout.Rva(0x220467);
        kDeadlineReturn=layout.Rva(0x21ef96); kDeadline=layout.Rva(0x224b30);
        kImplementationTable=layout.Rva(0x157c3a0); kChainTable=layout.Rva(0x157a980); kGetter=layout.Rva(0xf410);
        kImplementationChain=layout.fields.implementationChain; kChainQueue=layout.fields.chainQueue;
        kChainNative=layout.fields.chainNative; kChainPresenter=layout.fields.chainPresenter;
        for (auto module : {provider, xess_mod::Self()})
        {
            HMODULE pinned{};
            if (!module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) throw std::runtime_error("pacing_module_pin");
        }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("pacing_hook_initialization");
        auto* current = new Route; // Callback and trampoline have process lifetime.
        current->module = provider;
        struct Hook { uintptr_t rva; void* callback; void** original; };
        const std::array<Hook, 3> hooks{{
            {kNativePresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&current->original)},
            {kPace, reinterpret_cast<void*>(&HookPace), reinterpret_cast<void**>(&current->pace)},
            {kDeadline, reinterpret_cast<void*>(&HookDeadline), reinterpret_cast<void**>(&current->deadline)}}};
        size_t created{};
        for (const auto& hook : hooks)
        {
            if (MH_CreateHook(base+hook.rva, hook.callback, hook.original) != MH_OK)
            {
                while (created) MH_RemoveHook(base+hooks[--created].rva);
                delete current;
                throw std::runtime_error("pacing_hook_creation");
            }
            ++created;
        }
        route.store(current, std::memory_order_release);
        for (const auto& hook : hooks)
        {
            if (MH_EnableHook(base+hook.rva) != MH_OK || !ImageRange(base, hook.rva, 16)
                || !HookPointsTo(base+hook.rva, uintptr_t(hook.callback)))
            {
                // Keep every callback/trampoline pinned, even if restoration
                // fails. The published route stays disabled and admits no owner.
                for (const auto& rollback : hooks) MH_DisableHook(base+rollback.rva);
                throw std::runtime_error("pacing_hook_activation");
            }
        }
        firstLayout.store(firstVerified, std::memory_order_release);
        current->enabled.store(true, std::memory_order_release);
        xess_mod::WriteLog("extra_pacing_hook", "\"native_present_rva\":"+std::to_string(kNativePresent)+
            ",\"extra_return_rva\":"+std::to_string(kExtraReturn)+",\"first_return_rva\":"+std::to_string(kFirstReturn)+
            ",\"real_return_rva\":"+std::to_string(kRealReturn)+",\"deadline_hook\":true,\"profile_verified\":true"+
            ",\"first_caller_verified\":"+(firstVerified ? "true" : "false")+
            ",\"family\":\""+layout.family+"\"");
        return true;
    }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "pacing_install_exception"; }
    return false;
}

bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure) noexcept
{
    try
    {
        failure.clear();
        auto* current = route.load(std::memory_order_acquire);
        if (!current || !current->enabled.load(std::memory_order_acquire) || !handle || !generation || !device)
            throw std::runtime_error("pacing_registration_unavailable");
        auto owner = std::make_shared<Owner>();
        owner->handle = handle; owner->generation = generation;
        uintptr_t native{}, presenterDevice{};
        if (!ContextParts(*current, handle, owner->implementation, owner->chain)
            || !Value(owner->chain+kChainPresenter, owner->presenter)
            || !Value(owner->chain+kChainNative, owner->native)
            || !Value(owner->presenter+0x18, native) || native != owner->native
            || !Value(owner->presenter, presenterDevice))
            throw std::runtime_error("pacing_context_ownership");
        if (!MatchesApplicationDevice(device, presenterDevice))
            throw std::runtime_error("pacing_context_device_identity");
        owner->device = presenterDevice;
        uint8_t intel{}, hardware{}, software{};
        if (!Value(owner->presenter+0x64, intel) || !Value(owner->presenter+0x340, hardware)
            || !Value(owner->presenter+0x341, software) || intel || hardware || software != 1)
            throw std::runtime_error("pacing_context_mode");
        std::lock_guard lock(current->ownersMutex);
        const auto prior = current->owners.load(std::memory_order_acquire);
        auto next = prior ? std::make_shared<Owners>(*prior) : std::make_shared<Owners>();
        for (const auto& entry : *next)
        {
            if (entry->handle == handle && entry->generation == generation && entry->presenter == owner->presenter
                && entry->chain == owner->chain && entry->native == owner->native && entry->device == owner->device) return true;
            if (entry->handle == handle || entry->presenter == owner->presenter)
                throw std::runtime_error("pacing_owner_collision");
        }
        if (next->size() >= 8) throw std::runtime_error("pacing_owner_capacity");
        // flipmeter.1: only with a driver function; the owner keeps plain pacing without its queue.
        if (flipFunction.load(std::memory_order_acquire))
        {
            const char* reason = ResolveFlipQueue(*owner, device);
            xess_mod::WriteLog("flip_metering_queue", "\"generation\":"+std::to_string(generation)+
                ",\"resolved\":"+(reason ? "false" : "true")+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : "")+
                ",\"queue\":"+std::to_string(uintptr_t(owner->flipQueue.Get()))+
                ",\"enabled\":"+(flipEnabled.load() ? "true" : "false")+",\"lead_ns\":"+std::to_string(flipLeadNs.load())+
                ",\"first_caller_verified\":"+(firstLayout.load() ? "true" : "false"));
        }
        // MFGAdaUnlock 1.7.1: XeFG waits on this chain's frame-latency waitable before each output (up to 1 s, 0x21F7B5),
        // so its limit bounds the queue of outputs ahead of the display: logged, and optionally changed, for the latency work.
        {
            uint32_t buffers{}, latency{};
            NativeCapacity(*owner, buffers, latency);
            const auto requested = nativeLatencyRequested.load(std::memory_order_acquire);
            const auto applied = requested && !queueEnabled.load(std::memory_order_acquire) ? RaiseNativeLatency(*owner, requested) : 0;
            owner->flip.nativeBuffers = buffers;
            owner->flip.nativeLatency = applied ? applied : latency;
            xess_mod::WriteLog("native_chain_capacity", "\"generation\":"+std::to_string(generation)+
                ",\"buffers\":"+std::to_string(buffers)+",\"max_latency\":"+std::to_string(latency)+
                ",\"requested\":"+std::to_string(requested)+",\"applied\":"+std::to_string(applied));
        }
        // pacing.1: the flip queue, on every vendor (the gate is plain D3D12); it needs the presentation queue and the
        // verified first-output caller (First() opens each gated batch).
        if (queueEnabled.load(std::memory_order_acquire))
        {
            const char* reason = !firstLayout.load(std::memory_order_acquire) ? "first_layout"
                : owner->flipQueue ? nullptr : ResolveFlipQueue(*owner, device);
            if (!reason) reason = StartQueue(*owner);
            xess_mod::WriteLog("flip_queue_owner", "\"generation\":"+std::to_string(generation)+
                ",\"started\":"+(reason ? "false" : "true")+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : "")+
                ",\"buffers\":"+std::to_string(owner->queue ? owner->queue->buffers : 0)+
                ",\"max_latency\":"+std::to_string(owner->queue ? owner->queue->latency : 0)+
                ",\"buffer_percent\":"+std::to_string(queueBufferPercent.load())+
                ",\"first\":\""+(queueFirstReady.load() ? "ready" : "buffer")+"\""+
                ",\"first_lead_ns\":"+std::to_string(kQueueSignalLead)+",\"metered_inner_lead_ns\":"+
                std::to_string(flipLeadNs.load())+",\"vendor_id\":"+std::to_string(owner->queue ? owner->queue->vendor : 0)+
                ",\"gate_pass\":"+(owner->queue && owner->queue->pass ? "true" : "false")+",\"gate_lead_bounds_ns\":["+
                std::to_string(cadence::GatePassLead::kMinimumLead)+","+std::to_string(cadence::GatePassLead::kMaximumLead)+"]");
        }
        // amdflip.1: AMD's flip meter on the presentation queue's device (the driver's own D3D12 device behind any proxy),
        // with or without the flip queue. The vendor is the kernel's (OptiScaler may spoof DXGI's).
        if (amdCreate.load(std::memory_order_acquire) && amdMode.load(std::memory_order_acquire))
        {
            const char* reason = owner->flipQueue ? nullptr : ResolveFlipQueue(*owner, device);
            Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
            uint32_t vendor = owner->queue ? owner->queue->vendor : 0;
            HRESULT result{};
            if (!reason)
            {
                ID3D12Device* raw{};
                const HRESULT got = owner->flipQueue->GetDevice(IID_PPV_ARGS(&raw));
                if (!raw || !single_overlay::native::PinInterface(raw, 2)) reason = "queue_device"; // quarantined
                else
                {
                    queueDevice.Attach(raw);
                    if (FAILED(got)) reason = "queue_device";
                    adapter_vendor_kmt::Ids ids{};
                    if (!vendor && adapter_vendor_kmt::Query(queueDevice->GetAdapterLuid(), ids)) vendor = ids.vendor;
                }
            }
            const HWND window = owner->queue ? owner->queue->window : NativeWindow(*owner);
            if (!reason) reason = CreateAmdMeter(*owner, queueDevice.Get(), vendor, window, result);
            const auto& caps = owner->amd.caps;
            xess_mod::WriteLog("amd_flip_meter", "\"generation\":"+std::to_string(generation)+
                ",\"created\":"+(reason ? "false" : "true")+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : "")+
                ",\"mode\":\""+(amdMode.load() == 2 ? "on" : "auto")+"\",\"vendor_id\":"+std::to_string(vendor)+
                ",\"result\":"+std::to_string(result)+",\"caps_mask\":"+std::to_string(caps.mask)+",\"caps_limit\":"+
                std::to_string(caps.limit)+",\"caps_status\":"+std::to_string(caps.status)+",\"flags\":"+
                std::to_string(owner->amd.flags)+",\"factory\":\""+(owner->amd.driverFactory ? "driver" : "other")+"\""+
                ",\"window\":"+(window ? "true" : "false")+
                ",\"covers_monitor\":"+(CoversMonitor(window) ? "true" : "false"));
        }
        next->push_back(owner);
        current->owners.store(next, std::memory_order_release);
        xess_mod::WriteLog("extra_pacing_owner", "\"generation\":"+std::to_string(generation)+
            ",\"handle\":"+std::to_string(uintptr_t(handle))+",\"sdk_swapchain\":"+std::to_string(owner->chain)+
            ",\"presenter\":"+std::to_string(owner->presenter)+",\"native_swapchain\":"+std::to_string(owner->native)+
            ",\"application_device\":"+std::to_string(uintptr_t(device))+
            ",\"presenter_device\":"+std::to_string(presenterDevice)+
            ",\"native_device_identity_used\":"+(presenterDevice != uintptr_t(device) ? "true" : "false"));
        return true;
    }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "pacing_registration_exception"; }
    return false;
}

void Reset(void* handle, uint64_t generation) noexcept
{
    try
    {
        if (auto* current = route.load(std::memory_order_acquire))
            if (const auto owners = current->owners.load(std::memory_order_acquire)) for (const auto& owner : *owners)
                if (owner->handle == handle && owner->generation == generation) Revoke(*owner);
    }
    catch (...) {}
}

void Unregister(void* handle, uint64_t generation) noexcept
{
    try
    {
        auto* current = route.load(std::memory_order_acquire);
        if (!current) return;
        std::lock_guard lock(current->ownersMutex);
        const auto prior = current->owners.load(std::memory_order_acquire);
        if (!prior) return;
        auto next = std::make_shared<Owners>();
        for (const auto& entry : *prior)
            if (entry->handle != handle || entry->generation != generation) next->push_back(entry);
            // In-flight callbacks retain their Owner snapshot.
            else { entry->retired.store(true, std::memory_order_release); Revoke(*entry); StopQueue(*entry); ReleaseAmdMeter(*entry); }
        current->owners.store(next, std::memory_order_release);
    }
    catch (...)
    {
        // Allocation failure must still remove all possible stale identities.
        if (auto* current = route.load(std::memory_order_acquire))
        {
            if (const auto owners = current->owners.load(std::memory_order_acquire)) for (const auto& owner : *owners)
            { owner->retired.store(true, std::memory_order_release); Revoke(*owner); StopQueue(*owner); ReleaseAmdMeter(*owner); }
            current->owners.store({}, std::memory_order_release);
        }
    }
}

void SetFlipMetering(SetFlipConfigFunction setFlipConfig, bool enabled, int64_t innerLeadNs) noexcept
{
    flipLeadNs.store(innerLeadNs >= 0 && innerLeadNs <= 8000000 ? innerLeadNs : 2500000, std::memory_order_relaxed);
    flipEnabled.store(enabled && setFlipConfig, std::memory_order_release);
    flipRequested.store(enabled, std::memory_order_release);
    flipFunction.store(setFlipConfig, std::memory_order_release);
}

void SetAmdFlipMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, int mode) noexcept
{
    amdDriver.store(driver, std::memory_order_release);
    amdMode.store(create && mode >= 1 && mode <= 2 ? mode : 0, std::memory_order_release);
    amdCreate.store(create, std::memory_order_release);
}

const char* ProbeAmdFlipMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, ID3D12Device* device, uint32_t& capsMask,
    HRESULT& result) noexcept
{
    void* meter{};
    AmdFlipMeterCaps caps{};
    bool driverFactory{};
    const char* reason = OpenAmdMeter(create, driver, device, device, meter, caps, driverFactory, result);
    capsMask = caps.mask;
    if (meter) AmdReleaseCall(meter);
    return reason;
}

void SetFlipToggleHotkey(bool enabled) noexcept
{
    flipToggleDown.store(false, std::memory_order_relaxed);
    flipToggleKey.store(enabled, std::memory_order_release);
}

void RequestFlipMetering(bool enabled) noexcept { flipRequested.store(enabled, std::memory_order_release); }
void RequestQueueAhead(bool enabled) noexcept { queueAheadRequested.store(enabled,std::memory_order_release); }
void RequestSdkDeadlines(bool enabled) noexcept { sdkDeadlinesRequested.store(enabled,std::memory_order_release); }
void RequestNativeLatency(uint32_t latency) noexcept { nativeLatencyRequested.store(latency<=16?latency:0,std::memory_order_release); }
FlipStatus Snapshot(void* handle) noexcept {
    FlipStatus status{};
    status.queueAheadRequested=queueAheadRequested.load(std::memory_order_acquire);
    status.requested=flipRequested.load(std::memory_order_acquire);
    status.enabled=flipEnabled.load(std::memory_order_acquire);
    status.available=flipFunction.load(std::memory_order_acquire)!=nullptr;
    status.sdkDeadlinesRequested=sdkDeadlinesRequested.load(std::memory_order_acquire);
    status.sdkDeadlinesActive=sdkDeadlinesActive.load(std::memory_order_acquire);
    const auto current=route.load(std::memory_order_acquire);
    if(!current) return status;
    status.installed=current->enabled.load(std::memory_order_acquire);
    const auto owners=current->owners.load(std::memory_order_acquire);
    if(!owners) return status;
    for(const auto& owner:*owners) {
        if(owner->handle!=handle || owner->retired.load() || owner->disabled.load()) continue;
        status.registered=true; status.queue=bool(owner->flipQueue);
        std::unique_lock guard(owner->flipMutex,std::try_to_lock);
        if(!guard.owns_lock()) { status.busy=true; return status; }
        status.queueAheadActive=owner->flip.queueAhead; status.queueAheadDeadlines=owner->flip.queueAheadDeadlines;
        status.nativeBuffers=owner->flip.nativeBuffers; status.nativeLatency=owner->flip.nativeLatency;
        status.configured=owner->flip.configured; status.metered=owner->flip.metered;
        status.refused=owner->flip.refused; status.outputs=owner->flip.outputs;
        status.feedback=owner->flip.feedback; status.status=owner->flip.status; status.calls=owner->flip.calls;
        return status;
    }
    return status;
}

namespace
{
// NativeBuffersScope: one copy of DXGI's factory vtable (all DXGI factories share it), with CreateSwapChainForHwnd
// (IDXGIFactory2 slot 15) raising the buffer count for the scoped thread's provider calls. It lives in this module's
// image for the process lifetime, so a thread that read the swapped pointer can always finish its call through it.
using CreateSwapChainForHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
constexpr size_t kCreateForHwndSlot = 15, kFactoryCopySlots = 64;
std::mutex factoryTableMutex;
alignas(64) void* factoryTable[kFactoryCopySlots+1]{};
void** factorySource{}; // the DXGI table factoryTable copies (factoryTableMutex)
std::atomic<CreateSwapChainForHwndFn> dxgiCreateSwapChainForHwnd{};
thread_local uint32_t tRaiseBuffers{};
thread_local HMODULE tRaiseProvider{};
thread_local NativeChainCreated tCreated{};
thread_local void* tCreatedContext{};
std::atomic<unsigned> nativeBufferLogs{};

bool CallerIn(HMODULE module, const void* address) noexcept
{
    HMODULE owner{};
    return module && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner) && owner == module;
}

// factoryhook.1: the file name (no folder) of the module holding address into out; "" when no module holds it.
void ModuleFileName(const void* address, wchar_t* out, size_t count) noexcept
{
    out[0] = 0;
    HMODULE module{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) || !module || !GetModuleFileNameW(module, path, MAX_PATH)) return;
    const wchar_t* name = wcsrchr(path, L'\\');
    wcsncpy_s(out, count, name ? name+1 : path, _TRUNCATE);
}

HRESULT STDMETHODCALLTYPE RaisedCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIOutput* output,
    IDXGISwapChain1** swapchain)
{
    const auto original = dxgiCreateSwapChainForHwnd.load(std::memory_order_acquire);
    const uint32_t target = tRaiseBuffers;
    const auto created = tCreated;
    if (!original) return E_UNEXPECTED;
    if ((!target && !created) || !desc || !CallerIn(tRaiseProvider, _ReturnAddress()))
        return original(self, device, window, desc, full, output, swapchain);
    HRESULT result{};
    if (target && desc->BufferCount < target)
    {
        DXGI_SWAP_CHAIN_DESC1 raised = *desc;
        raised.BufferCount = target;
        result = original(self, device, window, &raised, full, output, swapchain);
        const bool fallback = FAILED(result);
        if (fallback) result = original(self, device, window, desc, full, output, swapchain);
        if (nativeBufferLogs.fetch_add(1, std::memory_order_relaxed) < 16)
            try
            {
                xess_mod::WriteLog("native_buffers", "\"requested\":"+std::to_string(desc->BufferCount)+",\"created\":"+
                    std::to_string(fallback ? desc->BufferCount : target)+",\"raised_result\":"+std::to_string(fallback ? 1 : 0)+
                    ",\"result\":"+std::to_string(result)+",\"flags\":"+std::to_string(desc->Flags));
            }
            catch (...) {}
    }
    else result = original(self, device, window, desc, full, output, swapchain);
    // xefgmenu.1: once per scope, XeFG's native swap chain as created.
    if (created && SUCCEEDED(result) && swapchain && *swapchain)
    {
        tCreated = nullptr;
        created(device, window, swapchain, tCreatedContext);
    }
    return result;
}
}

NativeBuffersScope::NativeBuffersScope(IDXGIFactory2* factory, HMODULE provider, uint32_t buffers,
    NativeChainCreated created, void* context) noexcept
{
    try
    {
        if (buffers < 2) buffers = 0;
        if (!factory || !provider || (!buffers && !created)) { reason = "not_requested"; return; }
        // menucap.1: OptiScaler's menu draws on the chain created here and holds 8 buffers, 16 with its fix (menu16.1,
        // sdk_pacing.h).
        const uint32_t menuBuffers = optiscaler_host::MenuBackBuffers();
        if (buffers > menuBuffers) buffers = menuBuffers;
        if (!single_overlay::native::PinInterface(factory, kCreateForHwndSlot)) { reason = "factory_interface"; return; }
        auto** vtable = *reinterpret_cast<void***>(factory);
        // pacing.2: Windows' own dxgi.dll only. In Subnautica 2 the game-folder dxgi.dll is ReShade, whose factory
        // wrapper the name alone admitted (its swap chain then wraps XeFG's native one).
        HMODULE dxgi{};
        wchar_t path[MAX_PATH]{}, system[MAX_PATH]{};
        const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
        const bool tableInDxgi = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(vtable), &dxgi) && dxgi
            && GetModuleFileNameW(dxgi, path, MAX_PATH) && systemLength && systemLength < MAX_PATH-10
            && !wcscat_s(system, L"\\dxgi.dll") && _wcsicmp(path, system) == 0;
        if (!tableInDxgi || !CallerIn(dxgi, vtable[kCreateForHwndSlot]))
        {
            // factoryhook.1 (a tester's TLOU2 / Dying Light 2): name what holds the table and the slot, so the hook that
            // keeps XeFG's chain at the game's buffer count shows in the log.
            ModuleFileName(vtable, tableModule, std::size(tableModule));
            ModuleFileName(vtable[kCreateForHwndSlot], createModule, std::size(createModule));
            tableSystemDxgi = tableInDxgi;
            reason = "factory_not_dxgi"; return;
        }
        {
            std::lock_guard lock(factoryTableMutex);
            if (!factorySource)
            {
                // The table and the RTTI locator before it, as far as they lie in dxgi.dll's image.
                for (size_t i = 0; i <= kFactoryCopySlots; ++i)
                {
                    void* const* at = vtable-1+i;
                    if (!CallerIn(dxgi, at)) break;
                    factoryTable[i] = *at;
                }
                if (!factoryTable[1+kCreateForHwndSlot]) { reason = "factory_table"; return; }
                dxgiCreateSwapChainForHwnd.store(reinterpret_cast<CreateSwapChainForHwndFn>(vtable[kCreateForHwndSlot]),
                    std::memory_order_release);
                factoryTable[1+kCreateForHwndSlot] = reinterpret_cast<void*>(&RaisedCreateSwapChainForHwnd);
                factorySource = vtable;
            }
            else if (factorySource != vtable) { reason = "factory_table_changed"; return; }
        }
        tRaiseBuffers = buffers;
        tRaiseProvider = provider;
        tCreated = created;
        tCreatedContext = context;
        object = reinterpret_cast<void**>(factory);
        original = InterlockedCompareExchangePointer(object, &factoryTable[1], vtable);
        if (original != vtable)
        {
            tRaiseBuffers = 0; tRaiseProvider = nullptr; tCreated = nullptr; tCreatedContext = nullptr;
            object = nullptr; reason = "factory_swapped";
        }
    }
    catch (...) { reason = "factory_exception"; }
}

NativeBuffersScope::~NativeBuffersScope()
{
    tRaiseBuffers = 0;
    tRaiseProvider = nullptr;
    tCreated = nullptr;
    tCreatedContext = nullptr;
    if (object) InterlockedCompareExchangePointer(object, original, &factoryTable[1]);
}

void SetFlipQueue(bool enabled, int bufferPercent) noexcept
{
    queueBufferPercent.store(bufferPercent >= 0 && bufferPercent <= 100 ? bufferPercent : 50, std::memory_order_relaxed);
    queueEnabled.store(enabled, std::memory_order_release);
}

void SetFlipQueueFirst(bool ready) noexcept
{
    queueFirstReady.store(ready, std::memory_order_relaxed);
}

bool OutputRingDepth(void* handle, uint32_t countOffset, uint64_t& depth) noexcept
{
    depth = 0;
    const auto* current = route.load(std::memory_order_acquire);
    uintptr_t implementation{}, chain{};
    return current && handle && countOffset && countOffset < 0x1000 && ContextParts(*current, handle, implementation, chain)
        && Value(chain+countOffset, depth);
}

uint64_t OutputPresents() noexcept
{
    return outputPresents.load(std::memory_order_relaxed);
}
}
