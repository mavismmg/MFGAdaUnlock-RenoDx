#include "sdk_pacing.h"
#include "sdk_pacing_meter.h"
#include "pacing_trace.h"
#include "pacing_diag.h"
#include "latency_pacing.h"
#include "cadence_stats.h"
#include "xess_policy.h"
#include "xess_mod.h"
#include "overlay_device_identity.h"
#include "overlay_native.h"
#include "optiscaler_host.h"
#include "adapter_vendor_kmt.h"
#include "port/port_support.h"
#include "diagnostic_paths.h"
#include "xvk/xvk_output_frame.h"
#include "latency_probe.h"
#include "third_party/minhook/include/MinHook.h"
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dxgi1_4.h>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#if defined(MFGUNLOCK_NATIVE_XEMFG)
#include "../native_trace.hpp"
#endif
#include <thread>
#include <utility>
#include <vector>

namespace sdk_pacing
{
namespace
{
// Published once under installMutex, before the pinned provider's hooks are enabled.
uintptr_t kNativePresent=0x21f730, kPace=0x21ee30, kExtraReturn=0x2202ed, kFirstReturn=0x220db2, kRealReturn=0x220467;
uintptr_t kImplementationTable=0x157c3a0, kChainTable=0x157a980, kGetter=0xf410;
// The family's object fields (runtime_layout.h Fields), published with the RVAs.
uintptr_t kImplementationChain=8, kChainQueue=0x28, kChainNative=0x30, kChainPresenter=0x120;
// The profiled presenter prologue establishes rbp = caller-rsp + 0x89.
// At the intercepted call the return slot is caller-rsp - 8, hence:
constexpr uintptr_t kSummaryFromReturn = 0x48; // [rbp - 0x49]
constexpr uintptr_t kBundleFromReturn = 0xf8;  // [rbp + 0x67]
using Present = HRESULT(*)(void*, UINT, UINT, uint64_t, const DXGI_PRESENT_PARAMETERS*, ID3D12Resource* const*, bool);
using Pace = bool(*)(void*, const void*, uint8_t, const void*, uint32_t);

// AMD's driver flip meter as FSR's ML frame-generation swap chain uses it (Adrenalin 26.10.1 amdxcffx64.dll).
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
// its one generated frame; ApplyAmdMeter sends the batch's generated frames),
// flags (bit 0 = caps bit 7, bit 1 = caps bit 5, bit 2 = caps bit 6; bits 4-7 are the driver's copy of count), then the
// driver's output value and status (0 = applied).
struct AmdFlipMeterConfig { uint32_t mode, parameter, count, flags, output, status; uint8_t reserved[0x28]; };
static_assert(sizeof(AmdFlipMeterConfig) == 0x40);
constexpr uint32_t kAmdFlipMeterMode = 2;
// Refused SetConfig calls in a row after which an owner stops asking (the meter is still released at teardown).
constexpr uint32_t kAmdFlipMeterFailureLimit = 4;
// Decisions that need no driver call logged per owner (amd_flip_meter_decision).
constexpr uint64_t kAmdDecisionLogLimit = 64;
// FSR asks only in exclusive or borderless fullscreen; the window is checked at most this often.
constexpr int64_t kAmdFullscreenCheckNs = 250000000;

// The flip queue (sdk_pacing_meter.h). One GPU gate per owner: Present n waits on the SDK's
// presentation queue for gate value n, which the meter thread signals at the output's release time. The SDK's own Pace
// is skipped and no extra pace is added, so no thread the game or the SDK presenter depends on sleeps for display
// timing; they are held only by the native swap chain's own queue limits. A gate is signalled at its slot whether or not
// the copy has completed, so the GPU flips at the later of the two without a CPU round trip.
struct FlipQueue;
// What the periodic flip_queue_* lines need, copied by the meter thread (no allocation) and formatted on a thread-pool
// thread (StatsCallback), so the meter thread never builds strings. keepalive holds the queue until the callback is done.
struct StatsSnapshot
{
    std::shared_ptr<FlipQueue> keepalive;
    uint64_t generation{}, batches{};
    uint32_t outputs{}, buffers{}, maxLatency{}, sync{}, vendor{};
    int64_t interval{}, target{}, latency{}, cap{}, productionP50{}, productionP90{}, floor{}, gateLead{}, passMedian{};
    int64_t waitBaseline{}; // pacerfix.3
    int32_t correctionPpm{};
    uint64_t lateFirsts{}, hitches{}, coupled{}, coupledIdle{}, coupledFull{}, coupledLead{}, deep{}, kept{}, deepRefused{},
        floorResets{}, gapHolds{}, compensated{}, readyTimeouts{}, vsyncBatches{}, passSamples{}, passTimeouts{};
    uint64_t displaySamples{}, displaySkipped{}, displayFailures{}, displayDisjoint{};
    // sleepsight.1: first outputs with a measured host sleep since the last line, their sleep, and the clock's counter.
    uint64_t hostMeasuredBatches{}, heldSuppressed{};
    int64_t hostSleptSumNs{};
    int compositionMode{-1};
    bool metered{}, gatePass{};
    bool compositionOff{}; // dispoll.1
    cadence_stats::Window<180> gaps, holds, late, idle, passes, handovers, seams, display;
};
struct FlipQueue
{
    // vsync: the batch presents at sync interval 1. idleNs (first outputs): how long the presenter waited for this batch
    // after the previous gated Present returned. pass: a sampled pass fence Signal follows this output's gate Wait.
    // full (first outputs): the blocked Present waited behind more than a whole batch of gates (the native
    // latency exceeds the batch's outputs), so our queue held the game whether or not the presenter was backlogged.
    // gameWaitNs (first outputs): the game's Present wait since the previous batch (-1: no evidence); gameSleptNs its
    // low-latency sleep (0: no evidence); beginNs / beginSeq: the start and number of the game's latest Present on the
    // standalone host (0 elsewhere); meteredBatch: the driver meters this batch and only its first output is gated.
    struct Item
    {
        uint64_t value{}; int64_t submitNs{}, idleNs{}; uint32_t outputs{}, index{};
        bool first{}, coupled{}, vsync{}, pass{}, full{};
        int64_t gameWaitNs{-1}, beginNs{}, gameSleptNs{};
        uint64_t beginSeq{};
        bool meteredBatch{};
        int64_t gameHostSleptNs{-1}; // sleepsight.1 (first outputs; -1: not measured)
    };
    // pacerfix.2: the meter's items in a fixed ring (no allocation on the presenting threads; under mutex).
    static constexpr size_t kItemCapacity = 64;
    bool PushItem(const Item& item) noexcept
    {
        if (itemCount == kItemCapacity) return false;
        items[(itemHead+itemCount)%kItemCapacity] = item;
        ++itemCount;
        return true;
    }
    bool PopItem(Item& item) noexcept
    {
        if (!itemCount) return false;
        item = items[itemHead];
        itemHead = (itemHead+1)%kItemCapacity;
        --itemCount;
        return true;
    }
    Item& LastItem() noexcept { return items[(itemHead+itemCount-1)%kItemCapacity]; }
    ~FlipQueue()
    {
        // Every queued Wait was released before the thread exited. The GPU may still be retiring them, so the
        // fences are kept for the process lifetime.
        gate.Detach(); ready.Detach(); pass.Detach();
        if (readyEvent) CloseHandle(readyEvent);
        if (wake) CloseHandle(wake);
        // dispoll.1: the display thread holds the queue until it exits, so neither is still waited on.
        if (displayWake) CloseHandle(displayWake);
        if (displayDone) CloseHandle(displayDone);
    }
    uint64_t generation{};
    Microsoft::WRL::ComPtr<ID3D12Fence> gate, ready;
    HANDLE readyEvent{}, wake{};
    std::mutex mutex;
    std::array<Item, kItemCapacity> items{};
    size_t itemHead{}, itemCount{};
    std::atomic<uint64_t> issued{}; // the highest gate value queued as a Wait (written under mutex)
    bool exited{};     // every issued value is released and no more are signalled (mutex)
    std::atomic<uint64_t> signaled{};
    std::atomic<bool> stop{}, failed{}, reset{}, metered{};
    // A gated Present blocked in the native Present (the queue was full): the next batch's hand-over followed our
    // flips (JitterBufferClock coupled), if the presenter was backlogged.
    std::atomic<bool> blocked{};
    // When the last gated native Present returned (presenting threads), and the blocked batches whose
    // presenter waited for the game afterwards (not coupled).
    std::atomic<int64_t> returnedNs{};
    std::atomic<uint64_t> idleBlocked{};
    // Blocked batches whose Present waited behind more than a batch of gates (Item::full).
    std::atomic<uint64_t> fullBlocked{};
    // Batches coupled because the presenter was backlogged although no Present blocked.
    std::atomic<uint64_t> leadBacklogged{};
    // Published by the meter thread: the clock's per-output interval (the driver's batch time / outputs).
    std::atomic<int64_t> intervalNs{};
    // Watchdog: when the gate last made progress or became busy again (presenting threads and meter).
    std::atomic<int64_t> progressNs{};
    // latency: the native chain's maximum frame latency now, moved per batch (AdjustNativeLatency) between baseLatency
    // (XeFG's own, at most 3) and maxLatency (the raised chain's buffers - 1). After StartQueue only first outputs, on
    // XeFG's presenter thread, touch maxLatency. buffers is read again after a resize (RefreshNativeCapacity).
    std::atomic<uint32_t> buffers{};
    uint32_t baseLatency{}, maxLatency{};
    std::atomic<uint32_t> latency{};
    std::atomic<bool> recheck{};
    bool latencyRefused{}; // the chain refused a raised maximum latency once: it keeps XeFG's own (presenter thread)
    // The window the SDK's native swap chain presents to (its monitor's refresh period floors the meter under
    // V-Sync) and the sync interval of the last gated batch (stats).
    HWND window{};
    // That monitor's refresh period (0 = not known yet), measured on a thread-pool thread (QueryDisplayConfig can take
    // milliseconds) when V-Sync turns on and every kRefreshCheckNs while it stays on.
    std::atomic<int64_t> refreshNs{};
    std::atomic<bool> refreshPending{};
    std::atomic<uint32_t> sync{};
    std::atomic<unsigned> logs{};
    // The pass fence, signalled on the presentation queue right after a sampled output's gate Wait with the
    // gate's value, so it completes when the queue got past that gate (null: not created, nothing measured). passOff: a
    // pass Signal failed or the fence was lost; nothing more is sampled and the lead is kQueueSignalLead again. vendor:
    // the queue's adapter, its PCI vendor id from the kernel (0 = unknown; logged).
    Microsoft::WRL::ComPtr<ID3D12Fence> pass;
    std::atomic<bool> passOff{};
    uint32_t passRandom{0x9e3779b9u}; // the sampler (mutex)
    // pacerfix.2: kGatePassSettled samples are in: sample one output in kGatePassSettledSampling.
    std::atomic<bool> passSettled{};
    uint32_t vendor{};
    // pacerfix.2: the game's measured Present wait (gameWaitAccumNs) and low-latency sleep (gameSleepAccumNs) since the
    // last batch's first output, for the clock, and when evidence last came (0: never). Fed by EndGamePresent on the
    // standalone D3D12 host and by the real frame's own Present elsewhere; scoped to this owner and generation.
    std::atomic<int64_t> gameWaitAccumNs{}, gameWaitSeenNs{}, gameSleepAccumNs{};
    // sleepsight.1: the host's measured low-latency sleeps since the last batch's first output (HostSleepScope, merged by
    // FeedEvidence): their time and how many (0 = not measured), for the clock's hostSleptNs. And, on the meter thread, the
    // first outputs that carried a measurement and their sleep since the last flip_queue_sleep line.
    std::atomic<int64_t> gameHostSleepAccumNs{};
    std::atomic<uint64_t> gameHostSleeps{};
    uint64_t hostMeasuredBatches{};
    int64_t hostSleptSumNs{};
    // pacerfix.2: the start and number of the game's latest Present on the standalone host (BeginGamePresent; 0
    // elsewhere), for the production time and the schedule the low-latency sleep is bounded by.
    std::atomic<int64_t> lastBeginNs{};
    std::atomic<uint64_t> lastBeginSeq{};
    // pacerfix.2: what reached the display (DisplayPoll): XeFG's native chain and its proven table, the last displayed
    // present count and its sync time, the displayed gaps, and the composition mode (-1 unknown). dispoll.1: DisplayPoll
    // runs on the queue's display thread. A gated Present hands it one reference on the chain for its next poll
    // (displayTicket) and wakes it (displayWake); displayClosed once the thread takes no more, displayDone once it has
    // released the last. displayChain and displayTable are set before the queue is published and only read after.
    // displayMutex guards what the stats snapshot reads, never a DXGI call; the rest is the display thread's.
    IDXGISwapChain* displayChain{};
    void** displayTable{};
    HANDLE displayWake{}, displayDone{};
    std::atomic<IDXGISwapChain*> displayTicket{};
    std::atomic<bool> displayClosed{};
    std::mutex displayMutex;
    UINT displayCount{};
    int64_t displayQpc{}, compositionCheckedNs{};
    uint64_t displaySamples{}, displaySkipped{}, displayFailures{}, displayDisjoint{};
    uint32_t compositionFailures{};
    int compositionMode{-1};
    bool displayOff{}, compositionOff{};
    cadence_stats::Window<180> displayGaps;
    // The per-output trace (pacing_trace.h), written by the meter thread and dumped when it stops; null unless wanted.
    std::unique_ptr<pacing_trace::Ring> trace;
    // pacediag.1: the display-side event stream (pacing_diag.h), null unless the pacing diagnostics are on
    // (PacingDiagnosticsWanted). diagStem: the chunk files' path up to ".<n>.csv"; diagChunks: chunks handed to a writer.
    // diagLastPresentId / diagPresentNs: the native chain's present id and the time the latest gated Present returned
    // with (presenting threads; the display thread polls until that id is displayed, and samples the GPU only while
    // Presents come). diagLuid: the queue's adapter for the GPU rows. The rest is the display thread's.
    std::shared_ptr<pacing_diag::Recorder> diag;
    std::wstring diagStem;
    std::atomic<uint32_t> diagChunks{};
    std::atomic<int64_t> diagLastPresentId{-1}, diagPresentNs{};
    LUID diagLuid{};
    struct DiagNode { int64_t global{-1}, process{-1}, globalSum{}, processSum{}; };
    std::vector<DiagNode> diagNodes;
    int64_t diagGpuNs{}, diagGpuSummaryNs{}, diagGpuWallSum{};
    uint32_t diagGpuFailures{};
    bool diagGpuOff{}, diagCompositionLogged{};
    int64_t diagLastDisplayed{-1};
    // pacerfix.2: the periodic stats lines are formatted on a thread-pool thread (StatsSnapshot); statsBusy while one is.
    std::unique_ptr<StatsSnapshot> stats;
    std::atomic<bool> statsBusy{};
    // Meter thread only.
    cadence::JitterBufferClock clock;
    int64_t lastSignalNs{};
    // pacerfix.2: when the previous output's gate actually opened for the queue (the later of its signal and its
    // readiness: a gate signalled ahead waits for the copy), the clock's gateSignal.
    int64_t lastGateOpenNs{};
    // pacerfix.2: the production time (game Present start -> the batch's first output ready) of the last batches.
    std::array<int64_t, 32> production{};
    size_t productionCount{}, productionNext{};
    // pacerfix.1: the V-Sync floor in effect (floorNs) and when the refresh period was last asked for.
    int64_t floorNs{}, refreshAskedNs{};
    // pacerfix.1: the buffer count the clock was given (SetCapacity), and the meter's precise-wait margin.
    uint32_t clockBuffers{};
    cadence::WaitMargin waitMargin;
    uint64_t batches{}, readyTimeouts{}, vsyncBatches{}, passTimeouts{};
    // flip_queue_deep_refused / flip_queue_floor_reset lines written (LogClockEvents).
    uint32_t refusedLogs{}, floorLogs{};
    bool vsyncFloor{};
    cadence::GatePassLead passLead;
    cadence_stats::Window<180> gaps, holds, late, idle, passes;
    // handovers = the first outputs' submit spacing (the game's cadence, XeLL's sleep included);
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
        // Unregister releases the meter; this covers an owner that never reached the route.
        ReleaseAmdMeter(*this);
        // A queue that no longer proves its table is quarantined (never released), as is every other
        // SDK-owned interface kept.
        if (flipQueue && !single_overlay::native::PinInterface(flipQueue.Get(), 2)) flipQueue.Detach();
        // An owner that never reached the route (or was unregistered) stops its meter; its thread then
        // releases whatever it issued.
        if (queue)
        {
            queue->stop.store(true, std::memory_order_release);
            if (queue->wake) SetEvent(queue->wake);
            if (queue->readyEvent) SetEvent(queue->readyEvent); // pacerfix.1: as StopQueue
            if (queue->displayWake) SetEvent(queue->displayWake); // dispoll.1: its display thread ends too
        }
    }
    void* handle{};
    uint64_t generation{};
    uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    std::atomic<unsigned> pacedLogs{}, skippedLogs{};
    std::atomic<bool> disabled{}, retired{};
    // Accepted transitions (Revoke: Reset and Unregister, from any thread).
    std::atomic<uint64_t> revoked{};
    // pacerfix.2: the regions holding the SDK objects, proven committed and readable at Register. Live() copies their
    // fields under a fault handler (GuardedCopy) instead of a ReadProcessMemory call each; anything else goes through Read().
    struct Region { uintptr_t begin{}, end{}; };
    std::array<Region, 4> regions{};
    // The SDK's presentation queue (presenter+0x10 = chain+kChainQueue, the queue its native
    // swap chain presents on); null keeps this owner on plain pacing. The configuration in effect is
    // written by the presenting thread before a native Present (flipMutex).
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> flipQueue;
    std::mutex flipMutex;
    struct Flip
    {
        bool configured{}, metered{}, refused{};
        uint32_t outputs{}, feedback{}, consecutiveFailures{};
        int64_t spacing{}, batchNs{};
        int status{};
        uint64_t calls{}, limited{};
    } flip;
    std::atomic<unsigned> flipLogs{}, flipSkipLogs{};
    // The flip queue; null keeps the CPU pacing. The batch being gated (gateMutex): its frame,
    // outputs and the next output's position, set by its first output.
    std::shared_ptr<FlipQueue> queue;
    // AMD's flip meter for this owner's device (CreateAmdMeter; amdMutex: presenting threads and teardown). meter: the
    // driver object (one reference); applied: the mode in effect; count: its count (the batch's generated frames; 0
    // with mode 0); failures: refused SetConfig calls in a row. reason / generated: the last decision, decisions: those
    // logged without a driver call.
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
        // pacediag.1: the driver's configuration read back about once a second (A rows), the last one and the changes
        // logged (pacediag_amd).
        int64_t diagReadNs{};
        AmdFlipMeterConfig diagLast{};
        uint32_t diagLogs{};
        bool diagRead{};
    } amd;
    std::atomic<bool> amdMetered{};
    std::mutex gateMutex;
    uint64_t gateFrame{};
    uint32_t gateOutputs{}, gateNext{}, gateSync{};
    bool gateBatch{};
    // pacerfix.2: the batch being gated is metered by NVIDIA's driver: only its first output is gated (gateMutex).
    bool gateMetered{};
    // The current batch is not gated (its first output was refused), so the SDK's own Pace and the extra pacing run
    // for it as without the queue. Written by the batch's first output.
    std::atomic<bool> sdkPace{};
    std::atomic<unsigned> ungatedLogs{};
    // flip_queue_small_chain written (once per owner).
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
    std::atomic<bool> enabled{};
    std::atomic<std::shared_ptr<const Owners>> owners;
    std::mutex ownersMutex;
};
std::atomic<Route*> route{};
std::mutex installMutex;

// SetFlipMetering. firstLayout: Install verified the profiled first-output caller
// (0x220c70), whose frame First() reads for the batch's output count.
std::atomic<SetFlipConfigFunction> flipFunction{};
std::atomic<bool> flipEnabled{}, firstLayout{};
std::atomic<uint32_t> nativeLatencyRequested{0}; // MFGAdaUnlock 1.7.1: XeFGNativeLatency
std::atomic<int64_t> flipLeadNs{2500000};
// SetAmdFlipMeter: amdxc64.dll's AmdExtD3DCreateInterface (null = off) and the mode: 1 auto (FSR's rule, a
// window covering its monitor), 2 on (any window).
std::atomic<AmdExtCreateInterfaceFunction> amdCreate{};
std::atomic<HMODULE> amdDriver{};
std::atomic<int> amdMode{};
// SetFlipQueue: owners registered from now on get the flip queue.
std::atomic<bool> queueEnabled{true};
constexpr double kQueueBufferFraction = 0.5;
// pacerfix.2: the refresh period while a queue's batches present with V-Sync (0: none), for the hosts' auto output cap
// (VsyncRefreshNs), and when it was published.
std::atomic<int64_t> vsyncRefreshNs{}, vsyncRefreshSeenNs{};
// pacerfix.2: the flip queue's schedule for the low-latency sleep (ScheduleCap), published at each batch's first output
// on the standalone D3D12 host (PublishSchedule). scheduleSpanNs: the game's time from the end of its sleep point to its
// next Present (p90 of the last 32; BeginGamePresent). gameBeginSeq counts the game's Presents on that host.
std::atomic<int64_t> scheduleNextBatchNs{}, scheduleBatchNs{}, scheduleProductionNs{}, scheduleSeenNs{}, scheduleSpanNs{};
std::atomic<uint64_t> scheduleBeginSeq{}, gameBeginSeq{};
// latency.1: SetLatencyPacing's mode (latency_pacing.h), the controller (latencyMutex: the real frame's presenting
// thread and the game's sleep points), the frame value of the last generated output (a real frame counts only after
// its batch's generated outputs: frame generation on), the real frame's Pace on this thread (added to its Present),
// and the log's counters (latencyMutex).
std::atomic<uint32_t> latencyMode{latency_pacing::kAuto};
std::mutex latencyMutex;
latency_pacing::Controller latencyController;
latency_pacing::Stats latencyLogged{}, latencyRead{};
std::atomic<uint64_t> latencyGeneratedFrame{UINT64_MAX};
thread_local int64_t tLatencyPaceNs{};
std::atomic<uint32_t> latencyLimitLogs{};
// pacediag.1: SetPacingDiagnostics, decided once per process before the first owner registers; and the gate value this
// thread's current Present was queued behind (GateBeforePresent; 0: none), for its P row.
std::atomic<bool> diagEnabled{};
thread_local uint64_t tDiagGate{};
// AMD batches above 4 outputs are gated like any other: without a driver flip meter, ungated outputs bunch at the batch
// start in a GPU-bound game.
// The queue passes a Wait about 0.5-0.8 ms after the CPU Signal, so a gate is signalled this much early. Inside a batch
// the driver meters, an inner gate is signalled flipLeadNs early instead and the display hardware holds the flip to the
// batch's spacing; a batch's first flip is not held. Unmetered gates use the queue's measured pass latency once known.
constexpr int64_t kQueueSignalLead = 500000;
// About one output in kGatePassSampling gets a pass fence Signal after its gate Wait (one in kGatePassSettledSampling
// once kGatePassSettled samples are in: each sample busy-polls the time-critical meter thread for up to kGatePassPollNs).
constexpr uint32_t kGatePassSampling = 8, kGatePassSettledSampling = 32;
constexpr uint64_t kGatePassSettled = 128;
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
// ... and only when the next batch's first output reached its gate within this long of that batch's last gated
// Present returning (the batch was already waiting: the presenter was backlogged, so its hand-over time is ours).
constexpr int64_t kQueueBacklogNs = 1000000;
// The shortest batch time the driver's flip metering gets from the flip queue; shorter = that batch unmetered.
constexpr int64_t kQueueMinimumBatchNs = 4000000;
// The driver refusing this many configurations in a row turns the owner's flip metering off.
constexpr uint32_t kFlipFailureLimit = 8;
// The batch time the driver gets grows at most 1/kFlipGrowthDivisor per batch over the one in
// effect (same output count): every flip of the batch is held to it, so one high estimate must not
// slow a whole batch down at once. Shorter times apply at once.
constexpr int64_t kFlipGrowthDivisor = 8;
// pacerfix.1: while V-Sync floors the meter, the monitor's refresh period is measured again this often.
constexpr int64_t kRefreshCheckNs = 1000000000;
// pacerfix.1: XeMFG standalone's D3D12 host feeds the low-latency pacing from its game-thread Present while its last
// such Present began within this long; otherwise the real frame's own Present feeds it.
constexpr int64_t kHostFeedStaleNs = 250000000;
// pacerfix.2: a queue's game-wait evidence (FlipQueue::gameWaitAccumNs) counts for the clock while its last feed is at
// most this old; older, the batch has no evidence (-1) and the clock keeps its inference rules.
constexpr int64_t kEvidenceStaleNs = 250000000;
// pacerfix.2: the low-latency sleep's schedule bound (ScheduleCap): the schedule must be this fresh, the game's next
// hand-over is kept this far ahead of when its first output has to be ready, and the controller's margin is
// kScheduledMarginNs (instead of latency_pacing::kMarginNs) while the bound applies.
constexpr int64_t kScheduleStaleNs = 250000000, kScheduleGuardNs = 500000, kScheduledMarginNs = 500000;
// pacerfix.2: the game's whole wait in the host's Present sizes the low-latency sleep only while the game sleeps at its
// own XeLL / Reflex point (a proven pre-input point) at least this often; otherwise the real frame's own hold does.
constexpr int64_t kProvenSleepStaleNs = 1000000000;

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
// pacerfix.1: a read inside a caller frame on this thread's own stack, after StackRange proved the whole range: that
// memory is committed and above the current frame, so it is copied directly instead of through ReadProcessMemory.
template<class T> void StackValue(uintptr_t address, T& output) noexcept
{
    memcpy(&output, reinterpret_cast<const void*>(address), sizeof(output));
}
// pacerfix.2: a copy from memory a fault may have freed since it was proven: an access violation, a guard page or an
// in-page error fails the copy (the caller then fails closed); anything else is not ours to handle.
bool GuardedCopy(void* output, const void* address, size_t bytes) noexcept
{
    __try
    {
        memcpy(output, address, bytes);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
        || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}
// pacerfix.2: the committed, readable region (no guard page) holding address; empty when it is not such memory.
struct ProvenRegion { uintptr_t begin{}, end{}; };
ProvenRegion RegionOf(uintptr_t address) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (address < 0x10000 || VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)
        || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
            | PAGE_EXECUTE_WRITECOPY))) return {};
    const auto begin = uintptr_t(info.BaseAddress);
    if (info.RegionSize > UINTPTR_MAX-begin) return {};
    return {begin, begin+info.RegionSize};
}
// pacerfix.2: an SDK object field: a guarded copy inside the regions proven at Register, otherwise Read().
template<class T> bool OwnerValue(const Owner& owner, uintptr_t address, T& output) noexcept
{
    if (address <= UINTPTR_MAX-sizeof(T))
        for (const auto& region : owner.regions)
            if (region.end && address >= region.begin && address+sizeof(T) <= region.end)
                return GuardedCopy(&output, reinterpret_cast<const void*>(address), sizeof(T));
    return Value(address, output);
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
// The present modes the flip queue gates: V-Sync off (sync interval 0, with or without tearing) and V-Sync on at
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
    StackValue(call.returnSlot, savedReturn); // StackRange above covers [returnSlot, +kBundleFromReturn+8)
    StackValue(call.returnSlot+kBundleFromReturn, bundle);
    if (savedReturn != call.returnAddress || !bundle
        || !OwnerValue(owner, owner.chain+kChainPresenter, currentPresenter) || currentPresenter != owner.presenter
        || !OwnerValue(owner, owner.presenter+0x18, native) || native != owner.native) return "ownership_changed";
    uint8_t intel{}, hardware{}, software{};
    if (!OwnerValue(owner, owner.presenter+0x64, intel) || !OwnerValue(owner, owner.presenter+0x340, hardware)
        || !OwnerValue(owner, owner.presenter+0x341, software) || intel || hardware || software != 1)
        return "software_pacing_unavailable";
    std::array<unsigned char, 0xc1> data{};
    std::array<int64_t, 2> summary{};
    static_assert(kSummaryFromReturn+sizeof(summary) <= kBundleFromReturn+8);
    StackValue(call.returnSlot+kSummaryFromReturn, summary);
    if (!Read(bundle, data.data(), data.size())) return "unreadable_bundle";
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

// On an accepted enable/count/resize transition (Reset) and at Unregister: the queue's clock starts over.
void Revoke(Owner& owner) noexcept
{
    owner.revoked.fetch_add(1, std::memory_order_relaxed);
    if (owner.queue)
    {
        owner.queue->reset.store(true, std::memory_order_release);
        // pacerfix.1: a resize may have changed the native chain's buffers (RefreshNativeCapacity).
        owner.queue->recheck.store(true, std::memory_order_release);
    }
}

bool Live(const Owner& owner) noexcept
{
    if (owner.retired.load(std::memory_order_acquire) || owner.disabled.load(std::memory_order_acquire)) return false;
    uint32_t magic{}; uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    uint8_t intel{}, hardware{}, software{};
    return OwnerValue(owner, uintptr_t(owner.handle), magic) && magic == 0x47466558
        && OwnerValue(owner, uintptr_t(owner.handle)+8, implementation) && implementation == owner.implementation
        && OwnerValue(owner, implementation+kImplementationChain, chain) && chain == owner.chain
        && OwnerValue(owner, chain+kChainPresenter, presenter) && presenter == owner.presenter
        && OwnerValue(owner, chain+kChainNative, native) && native == owner.native
        && OwnerValue(owner, presenter+0x18, native) && native == owner.native
        && OwnerValue(owner, presenter, device) && device == owner.device
        && OwnerValue(owner, presenter+0x64, intel) && !intel
        && OwnerValue(owner, presenter+0x340, hardware) && !hardware
        && OwnerValue(owner, presenter+0x341, software) && software == 1;
}

int64_t NowNs() noexcept
{
    // The frequency is fixed at boot; read once (0 = unusable).
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 && value.QuadPart <= INT64_MAX/1000000000
            ? int64_t(value.QuadPart) : int64_t(0);
    }();
    LARGE_INTEGER ticks{};
    if (!frequency || !QueryPerformanceCounter(&ticks) || ticks.QuadPart < 0
        || ticks.QuadPart/frequency > INT64_MAX/1000000000) return 0;
    return (ticks.QuadPart/frequency)*1000000000 + (ticks.QuadPart%frequency)*1000000000/frequency;
}

// ---- NVIDIA driver flip metering ---------------------------------
// The batch's first generated output (the profiled 0x220c70 caller, verified at Install): its
// frame holds the output vector pair, the generated count and this call's own arguments.
// Prologue: the return slot is caller-rsp - 8. V-Sync at interval 1 is accepted (the flip queue
// gates it); flip metering refuses V-Sync itself (FlipBeforePresent).
bool First(const Route& current, const Owner& owner, const Invocation& call, uint32_t& generated) noexcept
{
    if (!call.generated || call.returnAddress != uintptr_t(current.module)+kFirstReturn
        || call.presenter != reinterpret_cast<void*>(owner.presenter)
        || !GatedPresentMode(call) || (call.returnSlot & 7)
        || !StackRange(call.returnSlot, 0xf4)) return false;
    // pacerfix.1: the caller frame's fields are read directly (StackRange proved [returnSlot, +0xf4)) and checked
    // before the owner's SDK objects (Live: ReadProcessMemory), which are read only for a matching frame.
    uintptr_t saved{}, pair{}, parameters{}, resources{}, resource{};
    uint64_t frame{}, count{}; uint32_t presented{}, sync{}, flags{};
    StackValue(call.returnSlot, saved);
    StackValue(call.returnSlot+0xe8, pair);
    StackValue(call.returnSlot+0xf0, presented);
    StackValue(call.returnSlot+0xd8, frame);
    StackValue(call.returnSlot+0xe0, parameters);
    StackValue(call.returnSlot+0xc8, sync);
    StackValue(call.returnSlot+0xd0, flags);
    if (saved != call.returnAddress || !pair || presented != 1 || frame != call.frame
        || parameters != uintptr_t(call.parameters) || sync != call.sync || flags != call.flags
        || !Live(owner)
        || !Value(pair, resources) || resources != uintptr_t(call.resource)
        || !Value(pair+8, count) || !count || count > xess_mod::kGeneratedCapacity
        || count+1 > kMaximumFramesPerFlipBatch
        || !Value(resources, resource) || !resource) return false;
    generated = uint32_t(count);
    return true;
}

// pacerfix.1: First() once per Present; the NVIDIA metering, AMD meter and flip queue paths share the first answer.
struct FirstCache
{
    const Owner* owner{};
    bool ok{};
    uint32_t generated{};
};
bool CachedFirst(FirstCache& cache, const Route& current, const Owner& owner, const Invocation& call,
    uint32_t& generated) noexcept
{
    if (cache.owner != &owner)
    {
        cache = {&owner, false, 0};
        cache.ok = First(current, owner, call, cache.generated);
    }
    generated = cache.generated;
    return cache.ok;
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
        const auto r = CallSetFlipConfig(function, owner.flipQueue.Get(), &config);
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

// Before the SDK's native Present: a batch's first output configures the batch, any other
// Present on the presenter (frame generation off) turns a configuration off; inner outputs and
// the real frame belong to the configured batch.
// pacerfix.1: `owners` is Dispatch's one snapshot of the route's owners and `firstCache` its First() answer.
void FlipBeforePresent(Route& current, const Owners* owners, const Invocation& call, FirstCache& firstCache)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    if (!first && (offset == kExtraReturn || offset == kRealReturn)) return;
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
        uint32_t generated{};
        int64_t batchNs{};
        if (!flipEnabled.load(std::memory_order_acquire)) skip = "disabled";
        else if (flip.refused) skip = "driver_refused";
        else if (!firstLayout.load(std::memory_order_acquire)) skip = "first_layout";
        // Under V-Sync sl.dlss_g paces against the vblanks itself (RSYNC); the driver's timed flips are not
        // proven to combine with V-Sync flips, so a V-Sync batch is not metered (the flip queue still times it).
        else if (call.sync) skip = "vsync";
        else if (!CachedFirst(firstCache, current, owner, call, generated)) skip = "first_call_layout";
        // Only the flip queue's batches are metered (its clock gives the batch time).
        else if (!owner.Queued()) skip = "not_queued";
        else
        {
            // The flip queue's clock times this batch; the driver gets the same batch time.
            batchNs = owner.queue->intervalNs.load(std::memory_order_relaxed)*int64_t(generated+1);
            if (batchNs <= 0) skip = "interval_unknown";
            else if (batchNs < kQueueMinimumBatchNs) skip = "interval_too_short";
        }
        // Growth over the configuration in effect for the same count is limited.
        if (!skip && flip.configured && flip.outputs == generated+1 && flip.batchNs > 0
            && batchNs > flip.batchNs+flip.batchNs/kFlipGrowthDivisor)
        {
            batchNs = flip.batchNs+flip.batchNs/kFlipGrowthDivisor;
            ++flip.limited;
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
        return;
    }
}

// pacerfix.2: whether the driver meters the batch `outputs` long that this first output opens (FlipBeforePresent just
// configured it on this thread): then only that first output is gated, as DLSS-G leaves the batch's spacing to the
// driver (the display hardware holds each later flip to timePerBatch / outputs from the first).
bool MeteredBatch(Owner& owner, uint32_t outputs) noexcept
{
    if (!flipEnabled.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(owner.flipMutex);
    return owner.flip.configured && owner.flip.metered && owner.flip.outputs == outputs;
}

// ---- Precise waits ---------------------------------------------------------------------------------------------------
// The shortest timer wait worth arming; closer to the margin the wait spins.
constexpr int64_t kMinimumTimerWaitNs = 50000;

// pacerfix.1: a wait until `target` (NowNs time). It sleeps on the high-resolution waitable timer until the waiter's
// learned margin before the target (cadence::WaitMargin: the timer's own late wake-ups, each wake-up a sample), then
// spins on the pause instruction, re-reading the clock (SwitchToThread could hand the core to a lower-priority thread for
// a whole time slice). Without a timer it sleeps 1 ms at a time while more than 1.5 ms is left. `stop` ends it early.
void PreciseSleepUntil(HANDLE timer, int64_t target, cadence::WaitMargin& margin, const std::atomic<bool>* stop) noexcept
{
    for (;;)
    {
        if (stop && stop->load(std::memory_order_acquire)) return;
        const auto now = NowNs();
        if (!now || now >= target) return;
        const auto left = target-now;
        const auto guard = margin.Margin();
        if (timer && left > guard+kMinimumTimerWaitNs)
        {
            const int64_t sleep = left-guard;
            LARGE_INTEGER due{}; due.QuadPart = -LONGLONG(sleep/100);
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)
                && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
            {
                if (const auto woke = NowNs()) margin.Add(woke-(now+sleep));
                continue;
            }
            if (left > 1500000) { Sleep(1); continue; }
        }
        else if (!timer && left > 1500000) { Sleep(1); continue; }
        for (int i = 0; i < 8; ++i) YieldProcessor();
    }
}

// ---- The flip queue ------------------------------------------------------------------------------------------------
void QueueSleepUntil(HANDLE timer, int64_t target, FlipQueue& queue) noexcept
{
    PreciseSleepUntil(timer, target, queue.waitMargin, &queue.stop);
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

// pacerfix.2 (gate-ahead): an output's readiness, but no later than `deadline` (its gate's signal time): ready (readyNs =
// when seen), the deadline came first, or failed (removed, stopped, or not ready within kQueueReadyTimeoutMs). It sleeps
// on the ready event and the timer together until the learned margin before the deadline, then spins on the fence.
enum class ReadyWait : uint8_t { kReady, kDeadline, kFailed };
ReadyWait WaitReadyUntil(FlipQueue& queue, uint64_t value, int64_t deadline, HANDLE timer, int64_t& readyNs) noexcept
{
    readyNs = 0;
    if (deadline <= 0)
    {
        if (!WaitQueueReady(queue, value)) return ReadyWait::kFailed;
        readyNs = NowNs();
        return readyNs ? ReadyWait::kReady : ReadyWait::kFailed;
    }
    const auto began = GetTickCount64();
    bool armed{};
    for (;;)
    {
        const auto completed = queue.ready->GetCompletedValue();
        if (completed == UINT64_MAX) { queue.failed.store(true, std::memory_order_release); return ReadyWait::kFailed; }
        const auto now = NowNs();
        if (!now) return ReadyWait::kFailed;
        if (completed >= value) { readyNs = now; return ReadyWait::kReady; }
        if (queue.stop.load(std::memory_order_acquire)) return ReadyWait::kFailed;
        if (now >= deadline) return ReadyWait::kDeadline;
        if (GetTickCount64()-began >= kQueueReadyTimeoutMs) { ++queue.readyTimeouts; return ReadyWait::kFailed; }
        const int64_t left = deadline-now;
        const int64_t guard = queue.waitMargin.Margin();
        if (timer && left > guard+kMinimumTimerWaitNs)
        {
            if (!armed) armed = SUCCEEDED(queue.ready->SetEventOnCompletion(value, queue.readyEvent));
            const int64_t sleep = left-guard;
            LARGE_INTEGER due{}; due.QuadPart = -LONGLONG(sleep/100);
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            {
                const HANDLE handles[2]{timer, queue.readyEvent};
                const DWORD woke = WaitForMultipleObjects(armed ? 2 : 1, handles, FALSE, INFINITE);
                if (woke == WAIT_OBJECT_0)
                    if (const auto at = NowNs()) queue.waitMargin.Add(at-(now+sleep));
                continue;
            }
        }
        for (int i = 0; i < 8; ++i) YieldProcessor();
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

// ---- The gate's pass latency ---------------------------------------------------------------------------------------
// Whether the next gated output gets a pass fence Signal (under queue.mutex): about one in kGatePassSampling, at random,
// so the samples do not stay on one position of a 2-, 4- or 8-output batch.
bool GatePassSample(FlipQueue& queue) noexcept
{
    auto& x = queue.passRandom;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x%(queue.passSettled.load(std::memory_order_relaxed) ? kGatePassSettledSampling : kGatePassSampling) == 0;
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
// Unmetered, every flip happens when the queue passes its gate: UnmeteredLead. While the refresh floor sets the
// interval under V-Sync (the display, not the game, limits the rate), the gate opens half a refresh early, so each flip
// waits at the display's V-Sync instead of at our gate: the flips keep the meter's spacing and the display absorbs the
// GPU's gate and present jitter (fixture, V-Sync + VRR at 120 Hz, 4X at 30 fps: displayed gaps 6.3-10.4 ms in pairs every
// 12 outputs, from releases exactly 8.333 ms apart). Meter thread.
int64_t GateLead(const FlipQueue& queue, const FlipQueue::Item& item, int64_t interval) noexcept
{
    int64_t lead = !queue.metered.load(std::memory_order_relaxed) ? UnmeteredLead(queue)
        : item.first ? kQueueSignalLead : flipLeadNs.load(std::memory_order_relaxed);
    // pacerfix.1: the floor in effect (floorNs, the refresh period ApplyPresentMode applied).
    if (item.vsync && queue.vsyncFloor && queue.floorNs > 0 && interval > 0 && interval <= queue.floorNs)
        lead = (std::max)(lead, queue.floorNs/2);
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
    if (queue.passLead.Samples() >= kGatePassSettled) queue.passSettled.store(true, std::memory_order_relaxed);
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

// Count, p50, p95 and max only, so flip_queue_timing stays within one queued log line (752 characters).
std::string CompactJson(const char* name, const cadence_stats::Window<180>::Summary& s)
{
    char text[160]{};
    snprintf(text, sizeof(text), "\"%s\":{\"n\":%zu,\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f}", name, s.count, s.p50, s.p95, s.max);
    return text;
}

// pacerfix.2: the periodic lines, written on a thread-pool thread from the meter's snapshot. keepalive goes last: the
// queue (and this snapshot) may go with it.
void CALLBACK StatsCallback(PTP_CALLBACK_INSTANCE, void* context) noexcept
{
    auto* s = static_cast<StatsSnapshot*>(context);
    try
    {
        // At most the 768 characters of a queued log line (single_module kUiLogChars).
        xess_mod::WriteLog("flip_queue_stats", "\"generation\":"+std::to_string(s->generation)+
            ",\"batches\":"+std::to_string(s->batches)+",\"outputs\":"+std::to_string(s->outputs)+
            ",\"interval_ms\":"+Milliseconds(s->interval)+",\"target_ms\":"+Milliseconds(s->target)+
            ",\"latency_ms\":"+Milliseconds(s->latency)+",\"cap_ms\":"+Milliseconds(s->cap)+
            ",\"production_p50_ms\":"+Milliseconds(s->productionP50)+",\"production_p90_ms\":"+Milliseconds(s->productionP90)+
            ",\"correction_ppm\":"+std::to_string(s->correctionPpm)+",\"late_firsts\":"+std::to_string(s->lateFirsts)+
            ",\"hitches\":"+std::to_string(s->hitches)+",\"coupled\":"+std::to_string(s->coupled)+
            ",\"compensated\":"+std::to_string(s->compensated)+",\"wait_baseline_ms\":"+Milliseconds(s->waitBaseline)+
            ",\"coupled_idle\":"+std::to_string(s->coupledIdle)+",\"coupled_full\":"+std::to_string(s->coupledFull)+
            ",\"coupled_lead\":"+std::to_string(s->coupledLead)+",\"deep\":"+std::to_string(s->deep)+
            ",\"rate_kept\":"+std::to_string(s->kept)+",\"deep_refused\":"+std::to_string(s->deepRefused)+
            ",\"floor_resets\":"+std::to_string(s->floorResets)+",\"first_gap_holds\":"+std::to_string(s->gapHolds)+
            ",\"ready_timeouts\":"+std::to_string(s->readyTimeouts)+",\"metered\":"+(s->metered ? "true" : "false")+
            ",\"buffers\":"+std::to_string(s->buffers)+",\"max_latency\":"+std::to_string(s->maxLatency)+
            ",\"sync\":"+std::to_string(s->sync)+",\"vsync_batches\":"+std::to_string(s->vsyncBatches)+
            ",\"vsync_floor_ms\":"+Milliseconds(s->floor)+",\"vendor_id\":"+std::to_string(s->vendor)+
            ",\"gate_pass\":"+(s->gatePass ? "true" : "false")+",\"gate_lead_ms\":"+Milliseconds(s->gateLead)+
            ",\"gate_pass_samples\":"+std::to_string(s->passSamples)+",\"gate_pass_timeouts\":"+std::to_string(s->passTimeouts)+
            ",\"local_time\":\""+LocalClock()+"\"");
        // gate_pass_ms = the sampled gates' pass - signal latency (timeouts are counted in flip_queue_stats).
        xess_mod::WriteLog("flip_queue_timing", "\"generation\":"+std::to_string(s->generation)+
            ",\"batches\":"+std::to_string(s->batches)+","+cadence_stats::Json("signal_gap_ms", s->gaps.Take())+
            ","+cadence_stats::Json("hold_ms", s->holds.Take())+","+cadence_stats::Json("signal_late_ms", s->late.Take())+
            ","+cadence_stats::Json("idle_ms", s->idle.Take())+","+cadence_stats::Json("gate_pass_ms", s->passes.Take())+
            ","+CompactJson("handover_ms", s->handovers.Take())+","+CompactJson("seam_ms", s->seams.Take()));
        // pacerfix.2: what the display reported (DXGI frame statistics of XeFG's native chain): the gaps between
        // consecutive displayed presents, presents not seen one by one (skipped), failed or disjoint polls, and the
        // composition mode (0 composed, 1 overlay, 2 none, 3 composition failure; -1 unknown; composition_off: the query
        // stopped). Not proof of scanout timing under VRR: cross-check with PresentMon before letting it steer anything.
        xess_mod::WriteLog("flip_queue_display", "\"generation\":"+std::to_string(s->generation)+
            ",\"batches\":"+std::to_string(s->batches)+","+CompactJson("displayed_gap_ms", s->display.Take())+
            ",\"samples\":"+std::to_string(s->displaySamples)+",\"skipped\":"+std::to_string(s->displaySkipped)+
            ",\"failures\":"+std::to_string(s->displayFailures)+",\"disjoint\":"+std::to_string(s->displayDisjoint)+
            ",\"composition\":"+std::to_string(s->compositionMode)+
            ",\"composition_off\":"+(s->compositionOff ? "true" : "false")+",\"pass_p50_ms\":"+Milliseconds(s->passMedian)+
            ",\"local_time\":\""+LocalClock()+"\"");
        // sleepsight.1: the host's measured low-latency sleep (the game's Reflex / XeLL / Anti-Lag 2 sleep) the clock saw,
        // only where it is measured: first outputs that carried it and their mean sleep since the last line, then (since
        // the queue started) held probes not run because the game did not sleep, and the probes that ran (deep).
        if (s->hostMeasuredBatches || s->heldSuppressed)
            xess_mod::WriteLog("flip_queue_sleep", "\"generation\":"+std::to_string(s->generation)+
                ",\"batches\":"+std::to_string(s->batches)+",\"measured\":"+std::to_string(s->hostMeasuredBatches)+
                ",\"host_slept_mean_ms\":"+Milliseconds(s->hostMeasuredBatches
                    ? s->hostSleptSumNs/int64_t(s->hostMeasuredBatches) : 0)+
                ",\"held_suppressed\":"+std::to_string(s->heldSuppressed)+",\"deep\":"+std::to_string(s->deep)+
                ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
    const auto keep = std::move(s->keepalive);
    if (keep) keep->statsBusy.store(false, std::memory_order_release);
}

// Every 180 flips: the clock's state and the spacing of the gate signals, holds (release - ready) and signal lateness
// against the intended signal time (intendedNs; 0: the gate went at readiness), in milliseconds. While the previous
// snapshot is still being written (StatsCallback), this period's samples are dropped.
void RecordQueueStats(const std::shared_ptr<FlipQueue>& pointer, const FlipQueue::Item& item, int64_t readyNs,
    int64_t intendedNs, const cadence::JitterBufferClock::Release& release, int64_t signaledNs) noexcept
{
    auto& queue = *pointer;
    if (item.first) ++queue.batches;
    if (item.first && item.idleNs > 0) queue.idle.Add(double(item.idleNs)/1e6);
    if (item.first)
    {
        if (queue.lastFirstSubmitNs && item.submitNs > queue.lastFirstSubmitNs)
            queue.handovers.Add(double(item.submitNs-queue.lastFirstSubmitNs)/1e6);
        queue.lastFirstSubmitNs = item.submitNs;
        if (queue.realSignalNs && signaledNs > queue.realSignalNs) queue.seams.Add(double(signaledNs-queue.realSignalNs)/1e6);
    }
    if (item.index+1 == item.outputs || item.meteredBatch) queue.realSignalNs = signaledNs;
    if (queue.lastSignalNs && signaledNs > queue.lastSignalNs) queue.gaps.Add(double(signaledNs-queue.lastSignalNs)/1e6);
    queue.holds.Add(double(release.ns-readyNs)/1e6);
    const bool full = queue.late.Add(intendedNs > 0 && signaledNs > intendedNs ? double(signaledNs-intendedNs)/1e6 : 0.0);
    if (!full) return;
    const auto clear = [&queue] {
        queue.gaps = {}; queue.holds = {}; queue.late = {}; queue.idle = {}; queue.passes = {};
        queue.handovers = {}; queue.seams = {};
    };
    auto* s = queue.stats.get();
    if (!s || queue.statsBusy.exchange(true, std::memory_order_acq_rel)) { clear(); return; }
    const auto& clock = queue.clock;
    s->generation = queue.generation; s->batches = queue.batches; s->outputs = item.outputs;
    s->interval = clock.Interval(); s->target = clock.Target(); s->latency = clock.Latency(); s->cap = clock.Cap();
    s->productionP50 = clock.ProductionQuantile(0.5); s->productionP90 = clock.ProductionQuantile(0.9);
    s->correctionPpm = clock.CorrectionPpm(); s->lateFirsts = clock.LateFirsts(); s->hitches = clock.Hitches();
    s->coupled = clock.Coupled(); s->compensated = clock.Compensated(); s->waitBaseline = clock.WaitBaseline();
    s->coupledIdle = queue.idleBlocked.load(std::memory_order_relaxed);
    s->coupledFull = queue.fullBlocked.load(std::memory_order_relaxed);
    s->coupledLead = queue.leadBacklogged.load(std::memory_order_relaxed);
    s->deep = clock.Deep(); s->kept = clock.Kept(); s->deepRefused = clock.DeepRefused(); s->floorResets = clock.FloorResets();
    s->heldSuppressed = clock.HeldSuppressed();
    s->hostMeasuredBatches = std::exchange(queue.hostMeasuredBatches, 0);
    s->hostSleptSumNs = std::exchange(queue.hostSleptSumNs, 0);
    s->gapHolds = clock.GapHolds(); s->readyTimeouts = queue.readyTimeouts;
    s->metered = queue.metered.load(std::memory_order_relaxed);
    s->buffers = queue.buffers.load(std::memory_order_relaxed); s->maxLatency = queue.latency.load(std::memory_order_relaxed);
    s->sync = queue.sync.load(std::memory_order_relaxed); s->vsyncBatches = queue.vsyncBatches;
    s->floor = queue.vsyncFloor ? queue.floorNs : 0; s->vendor = queue.vendor;
    s->gatePass = queue.pass && !queue.passOff.load(std::memory_order_relaxed);
    s->gateLead = UnmeteredLead(queue); s->passSamples = queue.passLead.Samples(); s->passTimeouts = queue.passTimeouts;
    s->passMedian = queue.passLead.Median();
    s->gaps = queue.gaps; s->holds = queue.holds; s->late = queue.late; s->idle = queue.idle; s->passes = queue.passes;
    s->handovers = queue.handovers; s->seams = queue.seams;
    clear();
    {
        std::lock_guard lock(queue.displayMutex);
        s->display = queue.displayGaps;
        queue.displayGaps = {};
        s->displaySamples = queue.displaySamples; s->displaySkipped = queue.displaySkipped;
        s->displayFailures = queue.displayFailures; s->displayDisjoint = queue.displayDisjoint;
        s->compositionMode = queue.compositionMode;
        s->compositionOff = queue.compositionOff;
    }
    s->keepalive = pointer;
    if (!TrySubmitThreadpoolCallback(StatsCallback, s, nullptr))
    {
        s->keepalive.reset();
        queue.statsBusy.store(false, std::memory_order_release);
    }
}

// The clock's two runaway guards, logged so a game that trips them shows in its log. A deep decay refused because our
// clock had not held the previous batch's real frame (the GPU or the game's own queue was behind): the first one per
// queue, then every 600th, at most 8 lines. A rate restart after the interval sat at the floor (some other rule ran it
// away): each one, at most 8 lines. previousHold: the hold of the output planned before this one (for a batch's first
// output, the previous real frame). Meter thread.
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

// The refresh period of the monitor showing window in its current mode (the display configuration's exact
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

// pacerfix.1: measures the refresh period off the meter thread into queue.refreshNs; the context holds a queue reference.
void CALLBACK RefreshPeriodCallback(PTP_CALLBACK_INSTANCE, void* context) noexcept
{
    std::unique_ptr<std::shared_ptr<FlipQueue>> queue(static_cast<std::shared_ptr<FlipQueue>*>(context));
    if (const int64_t period = RefreshPeriod((*queue)->window); period > 0)
        (*queue)->refreshNs.store(period, std::memory_order_release);
    (*queue)->refreshPending.store(false, std::memory_order_release);
}

// Meter thread: asks for a new measurement unless one is under way.
void RequestRefreshPeriod(const std::shared_ptr<FlipQueue>& queue, int64_t now) noexcept
{
    if (queue->refreshPending.exchange(true, std::memory_order_acq_rel)) return;
    queue->refreshAskedNs = now ? now : 1;
    std::shared_ptr<FlipQueue>* context{};
    try { context = new std::shared_ptr<FlipQueue>(queue); }
    catch (...) { queue->refreshPending.store(false, std::memory_order_release); return; }
    if (!TrySubmitThreadpoolCallback(RefreshPeriodCallback, context, nullptr))
    {
        delete context;
        queue->refreshPending.store(false, std::memory_order_release);
    }
}

// A batch's present mode sets the meter's floor. Under V-Sync no flip is planned faster than the display
// refreshes: above the refresh rate (6x on a 120 Hz display) the outputs would otherwise queue behind the vblanks and
// add up to a full swap chain of latency while the display still shows one per refresh.
// pacerfix.1: the floor follows each measured period (RequestRefreshPeriod); until one arrives, the last known (0 at first).
void ApplyPresentMode(const std::shared_ptr<FlipQueue>& queuePointer, const FlipQueue::Item& item) noexcept
{
    if (!item.first) return;
    auto& queue = *queuePointer;
    const auto now = NowNs();
    if (item.vsync)
    {
        ++queue.vsyncBatches;
        if (item.vsync != queue.vsyncFloor || !queue.refreshAskedNs || now-queue.refreshAskedNs >= kRefreshCheckNs)
            RequestRefreshPeriod(queuePointer, now);
    }
    const int64_t floor = item.vsync ? queue.refreshNs.load(std::memory_order_acquire) : 0;
    // pacerfix.2: published for the hosts' auto output cap (VsyncRefreshNs).
    vsyncRefreshNs.store(floor, std::memory_order_relaxed);
    vsyncRefreshSeenNs.store(now, std::memory_order_release);
    if (item.vsync == queue.vsyncFloor && floor == queue.floorNs) return;
    queue.clock.SetFloor(floor);
    queue.vsyncFloor = item.vsync;
    queue.floorNs = floor;
    try
    {
        xess_mod::WriteLog("flip_queue_vsync", "\"generation\":"+std::to_string(queue.generation)+
            ",\"vsync\":"+(item.vsync ? "true" : "false")+",\"refresh_ns\":"+
            std::to_string(queue.refreshNs.load(std::memory_order_relaxed))+
            ",\"floor_ns\":"+std::to_string(floor)+",\"batches\":"+std::to_string(queue.batches)+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// p90 of the first `count` values (count <= 32).
int64_t P90(const std::array<int64_t, 32>& values, size_t count) noexcept
{
    if (!count) return 0;
    std::array<int64_t, 32> sorted{};
    std::copy_n(values.begin(), count, sorted.begin());
    const size_t k = count*9/10;
    std::nth_element(sorted.begin(), sorted.begin()+k, sorted.begin()+count);
    return sorted[k];
}

// pacerfix.2: at a batch's first output (meter thread, standalone D3D12 host only), the schedule ScheduleCap bounds the
// low-latency sleep by: the first output's production time from the game's Present start (p90 of the last 32) and the
// earliest flip of the next batch's first output. Nothing is published until 8 batches were measured.
void PublishSchedule(FlipQueue& queue, const FlipQueue::Item& item, int64_t readyNs,
    const cadence::JitterBufferClock::Release& release) noexcept
{
    if (item.beginNs <= 0) return;
    if (readyNs > item.beginNs && readyNs-item.beginNs < kScheduleStaleNs)
    {
        queue.production[queue.productionNext] = readyNs-item.beginNs;
        queue.productionNext = (queue.productionNext+1)%queue.production.size();
        if (queue.productionCount < queue.production.size()) ++queue.productionCount;
    }
    if (queue.productionCount < 8 || release.interval <= 0 || item.outputs < 2) return;
    const int64_t next = release.ns+int64_t((double(item.outputs)-1.0+cadence::JitterBufferClock::kFirstGap)
        *double(release.interval));
    scheduleProductionNs.store(P90(queue.production, queue.productionCount), std::memory_order_relaxed);
    scheduleNextBatchNs.store(next, std::memory_order_relaxed);
    scheduleBatchNs.store(int64_t(item.outputs)*release.interval, std::memory_order_relaxed);
    scheduleBeginSeq.store(item.beginSeq, std::memory_order_relaxed);
    scheduleSeenNs.store(NowNs(), std::memory_order_release);
}

// pacerfix.2: the per-output trace is opt-in: RTX_MFG_PACING_TRACE=1 in the game's environment before it starts.
bool PacingTraceWanted() noexcept
{
    static const bool wanted = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(MFG_ENV_PREFIX_W L"PACING_TRACE", value, DWORD(std::size(value)));
        return length == 1 && value[0] == L'1';
    }();
    return wanted;
}

// pacerfix.2: the last queue's trace as <log name>.pacing.trace beside the game's log (at most 4096 records), written
// after every Wait is released so teardown never waits for the file.
void DumpTrace(const FlipQueue& queue) noexcept
{
    if (!queue.trace || !queue.trace->Count()) return;
    try
    {
        const auto& directory = diagnostic_paths::LogDirectory();
        const auto name = diagnostic_paths::FileName(diagnostic_paths::Executable(), L".pacing.trace");
        if (directory.empty() || name.empty()) return;
        const auto path = (std::filesystem::path(directory)/name).wstring();
        std::FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"wb") || !file) return;
        const bool written = pacing_trace::Write(file, *queue.trace, queue.generation);
        std::fclose(file);
        std::string narrow;
        for (const wchar_t ch : name) narrow += ch < 0x80 ? char(ch) : '?';
        xess_mod::WriteLog("flip_queue_trace", "\"generation\":"+std::to_string(queue.generation)+",\"file\":\""+narrow+
            "\",\"records\":"+std::to_string(queue.trace->Count())+",\"dropped\":"+std::to_string(queue.trace->Dropped())+
            ",\"written\":"+(written ? "true" : "false"));
    }
    catch (...) {}
}

// ---- pacediag.1: display-side pacing diagnostics (pacing_diag.h) -----------------------------------------------------
// Opt-in: OptiScaler.ini [XeFG] XeFGPacingDiagnostics = on (SetPacingDiagnostics) or RTX_MFG_PACING_DIAG=1 in the game's
// environment, decided when a queue starts. Measurement only: nothing here changes a gate, a slot or a driver setting.
bool PacingDiagnosticsWanted() noexcept
{
    if (diagEnabled.load(std::memory_order_acquire)) return true;
    static const bool environment = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(MFG_ENV_PREFIX_W L"PACING_DIAG", value, DWORD(std::size(value)));
        return length == 1 && value[0] == L'1';
    }();
    return environment;
}

// The frame statistics' SyncQPCTime on NowNs()'s scale.
int64_t QpcToNs(int64_t ticks) noexcept
{
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 && value.QuadPart <= INT64_MAX/1000000000
            ? int64_t(value.QuadPart) : int64_t(0);
    }();
    if (!frequency || ticks < 0 || ticks/frequency > INT64_MAX/1000000000) return 0;
    return (ticks/frequency)*1000000000 + (ticks%frequency)*1000000000/frequency;
}

// A full chunk on its way to a file (thread-pool callback), or the last one at the queue's stop (written in place).
struct DiagWrite
{
    std::shared_ptr<pacing_diag::Recorder> recorder;
    std::vector<pacing_diag::Event>* chunk{};
    std::wstring path;
    uint64_t generation{};
    uint32_t number{};
};

void WriteDiagChunk(DiagWrite& job) noexcept
{
    const size_t events = job.chunk ? job.chunk->size() : 0;
    const uint64_t dropped = job.recorder ? job.recorder->Dropped() : 0;
    bool written{};
    try
    {
        std::FILE* file{};
        if (job.chunk && !_wfopen_s(&file, job.path.c_str(), L"wb") && file)
        {
            written = pacing_diag::Write(file, *job.chunk, job.generation, job.number, dropped);
            std::fclose(file);
        }
        std::string narrow;
        for (const wchar_t ch : std::filesystem::path(job.path).filename().wstring()) narrow += ch < 0x80 ? char(ch) : '?';
        xess_mod::WriteLog("pacediag_chunk", "\"generation\":"+std::to_string(job.generation)+",\"chunk\":"+
            std::to_string(job.number)+",\"file\":\""+narrow+"\",\"events\":"+std::to_string(events)+",\"dropped\":"+
            std::to_string(dropped)+",\"written\":"+(written ? "true" : "false")+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
    if (job.recorder) job.recorder->Return(job.chunk);
}

void CALLBACK DiagWriteCallback(PTP_CALLBACK_INSTANCE, void* context) noexcept
{
    std::unique_ptr<DiagWrite> job(static_cast<DiagWrite*>(context));
    WriteDiagChunk(*job);
}

// Hands a chunk the recorder gave out to a thread-pool writer, or writes it here (`now`: the meter's stop, after every
// Wait is released). A chunk that cannot be handed over goes back to the recorder unwritten.
void SubmitDiagChunk(FlipQueue& queue, std::vector<pacing_diag::Event>* chunk, bool now = false) noexcept
{
    if (!chunk || !queue.diag) return;
    DiagWrite* job{};
    try
    {
        const uint32_t number = queue.diagChunks.fetch_add(1, std::memory_order_relaxed)+1;
        job = new DiagWrite{queue.diag, chunk, queue.diagStem+L"."+std::to_wstring(number)+L".csv", queue.generation,
            number};
    }
    catch (...) { queue.diag->Return(chunk); return; }
    if (now) { WriteDiagChunk(*job); delete job; return; }
    if (!TrySubmitThreadpoolCallback(DiagWriteCallback, job, nullptr)) { queue.diag->Return(chunk); delete job; }
}

// Any thread: one event (dropped once the queue's chunks are spent or both are full).
void DiagPush(FlipQueue& queue, const pacing_diag::Event& event) noexcept
{
    auto* recorder = queue.diag.get(); // set before the queue is published; never changed
    if (!recorder) return;
    std::vector<pacing_diag::Event>* full{};
    if (recorder->Push(event, full)) SubmitDiagChunk(queue, full);
}

// The meter's stop: the partly filled chunk, then a summary line.
void StopDiagnostics(FlipQueue& queue) noexcept
{
    if (!queue.diag) return;
    SubmitDiagChunk(queue, queue.diag->Close(), true);
    try
    {
        xess_mod::WriteLog("pacediag_stopped", "\"generation\":"+std::to_string(queue.generation)+",\"chunks\":"+
            std::to_string(queue.diagChunks.load())+",\"dropped\":"+std::to_string(queue.diag->Dropped())+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// D3DKMTQueryStatistics (gdi32), laid out as the Windows SDK 10.0.22621 d3dkmthk.h D3DKMT_QUERYSTATISTICS on x64
// (C_ASSERT 0x328 there): Type, AdapterLuid, hProcess, the 0x308-byte result union, then the query union (QueryNode.NodeId).
// Results read: AdapterInformation.NodeCount (result + 4) and the RunningTime that opens both NodeInformation
// (GlobalInformation) and ProcessNodeInformation (result + 0). RunningTime's unit is logged raw beside the wall time.
struct KmtStatistics
{
    uint32_t type{};
    LUID adapter{};
    HANDLE process{};
    alignas(8) unsigned char result[0x308]{};
    uint32_t node{};
    uint32_t padding{};
};
static_assert(sizeof(KmtStatistics) == 0x328 && offsetof(KmtStatistics, process) == 16
    && offsetof(KmtStatistics, result) == 24 && offsetof(KmtStatistics, node) == 0x320, "D3DKMT_QUERYSTATISTICS");
constexpr uint32_t kKmtAdapter = 0, kKmtNode = 5, kKmtProcessNode = 6;
using KmtQueryStatisticsFunction = LONG (APIENTRY*)(KmtStatistics*);
LONG KmtCall(KmtQueryStatisticsFunction query, KmtStatistics* statistics) noexcept
{
    __try { return query(statistics); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return LONG(0xC0000005L); }
}
KmtQueryStatisticsFunction KmtQuery() noexcept
{
    static const KmtQueryStatisticsFunction query = [] {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return gdi ? reinterpret_cast<KmtQueryStatisticsFunction>(GetProcAddress(gdi, "D3DKMTQueryStatistics")) : nullptr;
    }();
    return query;
}
int64_t KmtRunning(const KmtStatistics& statistics) noexcept
{
    int64_t value{};
    std::memcpy(&value, statistics.result, sizeof(value));
    return value;
}

// Display thread: every GPU engine's running time about ten times a second while gated Presents come (G rows), and every
// ten seconds the busiest engines as a log line. Five failed rounds in a row, or no D3DKMTQueryStatistics (Wine), stop it.
constexpr int64_t kDiagGpuPeriodNs = 100000000, kDiagGpuActiveNs = 1000000000, kDiagGpuSummaryNs = 10000000000;
constexpr uint32_t kDiagGpuNodeLimit = 64, kDiagGpuFailureLimit = 5;
void DiagGpuSummary(FlipQueue& queue) // callers catch
{
    if (!queue.diagGpuWallSum) return;
    std::array<size_t, 3> top{SIZE_MAX, SIZE_MAX, SIZE_MAX};
    for (size_t i = 0; i < queue.diagNodes.size(); ++i)
        for (size_t slot = 0; slot < top.size(); ++slot)
            if (top[slot] == SIZE_MAX || queue.diagNodes[i].globalSum > queue.diagNodes[top[slot]].globalSum)
            {
                for (size_t move = top.size()-1; move > slot; --move) top[move] = top[move-1];
                top[slot] = i;
                break;
            }
    std::string nodes;
    for (const size_t i : top)
    {
        if (i == SIZE_MAX || !queue.diagNodes[i].globalSum) continue;
        const auto& node = queue.diagNodes[i];
        nodes += std::string(nodes.empty() ? "" : ",")+"{\"node\":"+std::to_string(i)+",\"global\":"+
            std::to_string(node.globalSum)+",\"process\":"+std::to_string(node.processSum)+"}";
    }
    try
    {
        xess_mod::WriteLog("pacediag_gpu", "\"generation\":"+std::to_string(queue.generation)+",\"wall_ns\":"+
            std::to_string(queue.diagGpuWallSum)+",\"nodes\":"+std::to_string(queue.diagNodes.size())+",\"busiest\":["+
            nodes+"],\"unit\":\"raw running time; 100 ns units if global/wall_ns*100 <= 1\",\"local_time\":\""+
            LocalClock()+"\"");
    }
    catch (...) {}
    for (auto& node : queue.diagNodes) node.globalSum = node.processSum = 0;
    queue.diagGpuWallSum = 0;
}

void DiagGpuSample(FlipQueue& queue) noexcept
{
    if (queue.diagGpuOff || !queue.diag) return;
    const int64_t now = NowNs();
    if (!now || (queue.diagGpuNs && now-queue.diagGpuNs < kDiagGpuPeriodNs)) return;
    const int64_t presented = queue.diagPresentNs.load(std::memory_order_relaxed);
    const auto query = KmtQuery();
    try
    {
        if (!query)
        {
            queue.diagGpuOff = true;
            xess_mod::WriteLog("pacediag_gpu", "\"generation\":"+std::to_string(queue.generation)+
                ",\"off\":\"no_d3dkmt_query_statistics\"");
            return;
        }
        if (queue.diagNodes.empty())
        {
            KmtStatistics adapter{};
            adapter.type = kKmtAdapter; adapter.adapter = queue.diagLuid;
            const LONG status = KmtCall(query, &adapter);
            uint32_t count{};
            std::memcpy(&count, adapter.result+4, sizeof(count));
            if (status < 0 || !count || count > kDiagGpuNodeLimit)
            {
                queue.diagGpuOff = true;
                xess_mod::WriteLog("pacediag_gpu", "\"generation\":"+std::to_string(queue.generation)+
                    ",\"off\":\"adapter_query\",\"status\":"+std::to_string(status)+",\"nodes\":"+std::to_string(count));
                return;
            }
            queue.diagNodes.resize(count);
            queue.diagGpuSummaryNs = now;
            xess_mod::WriteLog("pacediag_gpu", "\"generation\":"+std::to_string(queue.generation)+",\"nodes\":"+
                std::to_string(count)+",\"local_time\":\""+LocalClock()+"\"");
        }
        // Only while Presents come: an idle stretch starts the baselines over (no delta spans it).
        if (!presented || now-presented > kDiagGpuActiveNs)
        {
            for (auto& node : queue.diagNodes) node.global = node.process = -1;
            queue.diagGpuNs = 0;
            return;
        }
        const int64_t wall = queue.diagGpuNs ? now-queue.diagGpuNs : 0;
        bool any{};
        for (uint32_t i = 0; i < queue.diagNodes.size(); ++i)
        {
            auto& node = queue.diagNodes[i];
            KmtStatistics global{}, process{};
            global.type = kKmtNode; global.adapter = queue.diagLuid; global.node = i;
            process.type = kKmtProcessNode; process.adapter = queue.diagLuid; process.process = GetCurrentProcess();
            process.node = i;
            const LONG globalStatus = KmtCall(query, &global);
            const LONG processStatus = KmtCall(query, &process);
            const int64_t globalRunning = globalStatus >= 0 ? KmtRunning(global) : -1;
            const int64_t processRunning = processStatus >= 0 ? KmtRunning(process) : -1;
            any = any || globalStatus >= 0 || processStatus >= 0;
            const int64_t globalDelta = wall > 0 && node.global >= 0 && globalRunning >= node.global
                ? globalRunning-node.global : -1;
            const int64_t processDelta = wall > 0 && node.process >= 0 && processRunning >= node.process
                ? processRunning-node.process : -1;
            if (globalDelta > 0 || processDelta > 0)
            {
                pacing_diag::Event event{};
                event.kind = pacing_diag::kGpu; event.t = now;
                event.v[0] = i; event.v[1] = globalDelta; event.v[2] = processDelta; event.v[3] = wall;
                event.v[4] = globalStatus; event.v[5] = processStatus;
                DiagPush(queue, event);
                if (globalDelta > 0) node.globalSum += globalDelta;
                if (processDelta > 0) node.processSum += processDelta;
            }
            node.global = globalRunning; node.process = processRunning;
        }
        queue.diagGpuFailures = any ? 0 : queue.diagGpuFailures+1;
        if (queue.diagGpuFailures >= kDiagGpuFailureLimit)
        {
            queue.diagGpuOff = true;
            xess_mod::WriteLog("pacediag_gpu", "\"generation\":"+std::to_string(queue.generation)+",\"off\":\"node_queries\"");
            return;
        }
        if (wall > 0) queue.diagGpuWallSum += wall;
        queue.diagGpuNs = now;
        if (now-queue.diagGpuSummaryNs >= kDiagGpuSummaryNs) { DiagGpuSummary(queue); queue.diagGpuSummaryNs = now; }
    }
    catch (...) { queue.diagGpuOff = true; }
}

bool CoversMonitor(HWND window) noexcept;

// StartQueue (before the queue is published): the recorder, its files' stem beside the game's log, and what the native
// chain was made with (pacediag_start).
void StartDiagnostics(const Owner& owner, FlipQueue& queue, ID3D12Device* device) noexcept
{
    try
    {
        const auto& directory = diagnostic_paths::LogDirectory();
        SYSTEMTIME local{};
        GetLocalTime(&local);
        wchar_t suffix[64]{};
        swprintf_s(suffix, L".pacediag.%04u%02u%02u-%02u%02u%02u.g%llu", local.wYear, local.wMonth, local.wDay, local.wHour,
            local.wMinute, local.wSecond, static_cast<unsigned long long>(queue.generation));
        const auto name = diagnostic_paths::FileName(diagnostic_paths::Executable(), suffix);
        if (directory.empty() || name.empty()) return;
        queue.diagStem = (std::filesystem::path(directory)/name).wstring();
        queue.diagLuid = device->GetAdapterLuid();
        queue.diag = std::make_shared<pacing_diag::Recorder>();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        BOOL fullscreen{};
        HRESULT descResult = E_FAIL, fullscreenResult = E_FAIL;
        try
        {
            auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
            if (native && single_overlay::native::PinInterface(native, 18))
            {
                descResult = native->GetDesc1(&desc);
                fullscreenResult = native->GetFullscreenState(&fullscreen, nullptr);
            }
        }
        catch (...) {}
        const int64_t refresh = queue.window ? RefreshPeriod(queue.window) : 0;
        std::string narrow;
        for (const wchar_t ch : name) narrow += ch < 0x80 ? char(ch) : '?';
        xess_mod::WriteLog("pacediag_start", "\"generation\":"+std::to_string(queue.generation)+",\"files\":\""+narrow+
            ".<n>.csv\",\"buffers\":"+std::to_string(queue.buffers.load())+",\"base_latency\":"+
            std::to_string(queue.baseLatency)+",\"max_latency\":"+std::to_string(queue.maxLatency)+",\"latency_refused\":"+
            (queue.latencyRefused ? "true" : "false")+",\"desc_result\":"+std::to_string(descResult)+",\"width\":"+
            std::to_string(desc.Width)+",\"height\":"+std::to_string(desc.Height)+",\"format\":"+std::to_string(desc.Format)+
            ",\"buffer_count\":"+std::to_string(desc.BufferCount)+",\"swap_effect\":"+std::to_string(desc.SwapEffect)+
            ",\"chain_flags\":"+std::to_string(desc.Flags)+",\"scaling\":"+std::to_string(desc.Scaling)+
            ",\"fullscreen_result\":"+std::to_string(fullscreenResult)+",\"fullscreen\":"+(fullscreen ? "true" : "false")+
            ",\"covers_monitor\":"+(CoversMonitor(queue.window) ? "true" : "false")+",\"refresh_ns\":"+
            std::to_string(refresh)+",\"vendor_id\":"+std::to_string(queue.vendor)+",\"amd_mode\":"+
            std::to_string(amdMode.load())+",\"latency_mode\":"+std::to_string(latencyMode.load())+",\"chunk_events\":"+
            std::to_string(pacing_diag::Recorder::kChunkEvents)+",\"max_chunks\":"+
            std::to_string(pacing_diag::Recorder::kMaxChunks)+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) { queue.diag.reset(); }
}

void QueueThread(std::shared_ptr<FlipQueue> queue) noexcept
{
    // pacerfix.1: time-critical. The thread sleeps on its timer until a learned margin before each release
    // (PreciseSleepUntil) and spins only that margin, so it must run as soon as the timer fires.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    queue->clock.SetBufferFraction(kQueueBufferFraction);
    queue->clock.SetFirstAtReadiness(true);
    queue->clockBuffers = queue->buffers.load(std::memory_order_acquire);
    queue->clock.SetCapacity(queue->clockBuffers);
    for (;;)
    {
        if (queue->stop.load(std::memory_order_acquire) || queue->failed.load(std::memory_order_acquire)) break;
        FlipQueue::Item item{};
        bool have{};
        {
            std::lock_guard lock(queue->mutex);
            have = queue->PopItem(item);
        }
        if (!have) { WaitForSingleObject(queue->wake, 50); continue; }
        const bool reset = queue->reset.exchange(false, std::memory_order_acq_rel);
        if (reset) queue->clock.Reset();
        // pacerfix.1: the native chain's buffer count after a resize (RefreshNativeCapacity).
        if (const auto buffers = queue->buffers.load(std::memory_order_acquire); buffers != queue->clockBuffers)
        {
            queue->clockBuffers = buffers;
            queue->clock.SetCapacity(buffers);
        }
        ApplyPresentMode(queue, item);
        // NVIDIA's driver flip metering can switch per batch; every other queue is unmetered.
        const bool metered = queue->metered.load(std::memory_order_relaxed);
        queue->clock.SetUnmetered(!metered);
        // pacerfix.2 (gate-ahead): the slot comes from the clock before the copy is ready. Wait for readiness, but no
        // longer than the gate's signal time: an output ready early is held to its slot (at most kMaximumHold after its
        // readiness); one not ready by then has its gate signalled anyway and the GPU flips it once its copy completes.
        const int64_t slot = queue->clock.Slot(item.first);
        const int64_t lead = GateLead(*queue, item, queue->clock.Interval());
        int64_t readyNs{};
        const ReadyWait wait = WaitReadyUntil(*queue, item.value, slot > 0 ? slot-lead : 0, timer, readyNs);
        int64_t committed = slot;
        if (wait == ReadyWait::kReady && slot > 0)
        {
            committed = (std::min)(slot, readyNs+cadence::JitterBufferClock::kMaximumHold);
            QueueSleepUntil(timer, committed-lead, *queue);
        }
        const int64_t intended = slot > 0 ? committed-lead : 0;
        const auto signaledNs = NowNs();
        const HRESULT result = QueueSignal(*queue, item.value);
        const bool readyAtSignal = wait == ReadyWait::kReady;
        bool ready = readyAtSignal;
        if (wait == ReadyWait::kDeadline)
        {
            // Signalled ahead: readiness only for the clock (the gate is open; the GPU no longer waits on this thread).
            ready = WaitQueueReady(*queue, item.value);
            readyNs = ready ? NowNs() : 0;
            ready = ready && readyNs;
        }
        cadence::JitterBufferClock::Release release{};
        bool compensated{};
        // An unready copy (timeout, removal, stop) is released at once; nothing waits on it.
        if (ready)
        {
            // Unmetered, an output signalled at readiness (no slot) flips the measured pass latency after its signal; one
            // signalled at or ahead of its slot flips at the slot or with its copy, so no pass latency is added.
            queue->clock.SetPassLatency(slot <= 0 && !metered && queue->pass && !queue->passOff.load(std::memory_order_acquire)
                ? queue->passLead.Pass() : 0);
            const int64_t previousHold = queue->clock.LastHold();
            const uint64_t compensatedBefore = queue->clock.Compensated();
            release = queue->clock.Plan(readyNs, item.submitNs, item.first, item.outputs, queue->lastGateOpenNs,
                item.coupled, item.full, item.gameWaitNs, slot > 0 ? committed : 0, item.gameSleptNs, item.gameHostSleptNs);
            if (item.first && item.gameHostSleptNs >= 0)
            { ++queue->hostMeasuredBatches; queue->hostSleptSumNs += item.gameHostSleptNs; }
            compensated = queue->clock.Compensated() != compensatedBefore;
            // pacerfix.2: the rest of a metered batch flips ungated, one interval apart (the driver holds them).
            if (item.meteredBatch && item.outputs > 1) queue->clock.AdvanceUngated(item.outputs-1);
            if (release.deepRefused || release.floorReset) LogClockEvents(*queue, item, release, previousHold);
            if (item.first) PublishSchedule(*queue, item, readyNs, release);
        }
        else { queue->clock.Reset(); release.ns = signaledNs; }
        if (ready && item.first) queue->intervalNs.store(release.interval, std::memory_order_relaxed);
        // A sampled gate's pass latency, only where the gate was what the queue waited for (the copy was ready first).
        const int64_t passNs = item.pass && readyAtSignal && SUCCEEDED(result) && signaledNs
            && !queue->passOff.load(std::memory_order_acquire) ? PollGatePass(*queue, item.value, signaledNs) : -1;
        const auto number = queue->logs.fetch_add(1, std::memory_order_relaxed);
        if (number < 36 || FAILED(result) || !ready || (item.first && queue->batches%600 == 0))
            try
            {
                xess_mod::WriteLog("flip_queue", "\"generation\":"+std::to_string(queue->generation)+
                    ",\"gate\":"+std::to_string(item.value)+",\"index\":"+std::to_string(item.index)+
                    ",\"outputs\":"+std::to_string(item.outputs)+",\"ready\":"+(ready ? "true" : "false")+
                    ",\"ready_ns\":"+std::to_string(readyNs)+",\"submit_ns\":"+std::to_string(item.submitNs)+
                    ",\"slot_ns\":"+std::to_string(slot > 0 ? committed : 0)+",\"ahead\":"+
                    (wait == ReadyWait::kDeadline ? "true" : "false")+
                    ",\"release_ns\":"+std::to_string(release.ns)+",\"signal_ns\":"+std::to_string(signaledNs)+
                    ",\"interval_ns\":"+std::to_string(release.interval)+",\"held\":"+(release.held ? "true" : "false")+
                    ",\"late\":"+(release.late ? "true" : "false")+",\"hitch\":"+(release.hitch ? "true" : "false")+
                    ",\"coupled\":"+(release.coupled ? "true" : "false")+",\"deep\":"+(release.deep ? "true" : "false")+
                    ",\"game_wait_ns\":"+std::to_string(item.gameWaitNs)+",\"game_slept_ns\":"+
                    std::to_string(item.gameSleptNs)+",\"host_slept_ns\":"+std::to_string(item.gameHostSleptNs)+
                    ",\"metered_batch\":"+
                    (item.meteredBatch ? "true" : "false")+",\"vsync\":"+(item.vsync ? "true" : "false")+
                    ",\"lead_ns\":"+std::to_string(lead)+(passNs >= 0 ? ",\"pass_ns\":"+std::to_string(passNs) : "")+
                    ",\"signal_result\":"+std::to_string(result));
            }
            catch (...) {}
        RecordGatePass(*queue, passNs);
        RecordQueueStats(queue, item, ready ? readyNs : signaledNs, intended, release, signaledNs);
        if (queue->trace || queue->diag)
        {
            const uint32_t flags = (item.first ? pacing_trace::kFirst : 0u) | (release.held ? pacing_trace::kHeld : 0u)
                | (release.late ? pacing_trace::kLate : 0u) | (item.coupled ? pacing_trace::kCoupled : 0u)
                | (item.full ? pacing_trace::kFull : 0u) | (item.vsync ? pacing_trace::kVsync : 0u)
                | (metered ? pacing_trace::kMetered : 0u) | (wait == ReadyWait::kDeadline ? pacing_trace::kAhead : 0u)
                | (item.meteredBatch ? pacing_trace::kMeteredBatch : 0u) | (release.hitch ? pacing_trace::kHitch : 0u)
                | (ready ? pacing_trace::kReady : 0u) | (reset ? pacing_trace::kReset : 0u)
                | (compensated ? pacing_trace::kCompensated : 0u)
                | (item.first && item.gameHostSleptNs >= 0
                    && item.gameHostSleptNs < cadence::JitterBufferClock::kEvidenceMinimum ? pacing_trace::kHostQuiet : 0u);
            if (queue->trace)
            {
                pacing_trace::Record record{};
                record.value = item.value; record.submitNs = item.submitNs; record.readyNs = readyNs;
                record.slotNs = slot > 0 ? committed : 0; record.signalNs = signaledNs;
                record.gateOpenNs = queue->lastGateOpenNs;
                record.releaseNs = release.ns; record.intervalNs = release.interval; record.gameWaitNs = item.gameWaitNs;
                record.sleptNs = uint32_t((std::min)(item.gameSleptNs, int64_t(UINT32_MAX)));
                record.floorNs = queue->vsyncFloor ? queue->floorNs : 0; record.outputs = item.outputs;
                record.index = item.index;
                record.passNs = uint32_t(queue->clock.PassLatency()); record.capacity = queue->clockBuffers;
                record.flags = flags;
                queue->trace->Push(record);
            }
            // pacediag.1: the same output as a Q row (pacing_diag.h Columns).
            if (queue->diag)
            {
                pacing_diag::Event event{};
                event.kind = pacing_diag::kQueue;
                event.t = signaledNs;
                event.v = {int64_t(item.value), int64_t(item.index), int64_t(item.outputs), int64_t(flags),
                    int64_t(item.submitNs), int64_t(readyNs), int64_t(slot > 0 ? committed : 0), int64_t(signaledNs),
                    int64_t(release.ns), int64_t(release.interval), int64_t(passNs), int64_t(item.gameWaitNs),
                    int64_t(item.gameSleptNs), int64_t(item.gameHostSleptNs), int64_t(item.idleNs),
                    int64_t(queue->clock.Target())};
                DiagPush(*queue, event);
            }
        }
        queue->lastSignalNs = signaledNs;
        queue->lastGateOpenNs = ready ? (std::max)(signaledNs, readyNs) : signaledNs;
        if (FAILED(result)) { queue->failed.store(true, std::memory_order_release); break; }
    }
    // Release every Wait this owner ever queued; no Present may stay gated.
    uint64_t issued{};
    {
        std::lock_guard lock(queue->mutex);
        queue->itemHead = queue->itemCount = 0;
        issued = queue->issued;
        ReleaseQueue(*queue);
    }
    if (timer) CloseHandle(timer);
    // pacerfix.2: no V-Sync refresh or schedule is published for a queue that stopped.
    vsyncRefreshNs.store(0, std::memory_order_relaxed);
    scheduleSeenNs.store(0, std::memory_order_release);
    DumpTrace(*queue);
    StopDiagnostics(*queue); // pacediag.1
    try
    {
        xess_mod::WriteLog("flip_queue_stopped", "\"generation\":"+std::to_string(queue->generation)+
            ",\"released_gate\":"+std::to_string(issued)+",\"failed\":"+(queue->failed.load() ? "true" : "false")+
            ",\"late_firsts\":"+std::to_string(queue->clock.LateFirsts())+",\"ready_timeouts\":"+
            std::to_string(queue->readyTimeouts)+",\"gate_pass_samples\":"+std::to_string(queue->passLead.Samples())+
            ",\"gate_pass_p50_ns\":"+std::to_string(queue->passLead.Median())+",\"gate_lead_ns\":"+
            std::to_string(UnmeteredLead(*queue))+",\"wait_margin_ns\":"+std::to_string(queue->waitMargin.Margin())+
            ",\"wait_late_ns\":"+std::to_string(queue->waitMargin.Late())+",\"wait_samples\":"+
            std::to_string(queue->waitMargin.Samples())+",\"compensated\":"+std::to_string(queue->clock.Compensated())+
            ",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// dispoll.1: how long StopQueue waits for the display thread to drop its reference on XeFG's native chain (ms).
constexpr DWORD kDisplayStopWaitMs = 200;

// Stops the owner's meter and releases every queued Wait before returning: XeFG's Destroy joins its presenter
// threads, which may sit in a native Present behind the gate.
void StopQueue(Owner& owner) noexcept
{
    const auto queue = owner.queue;
    if (!queue) return;
    queue->stop.store(true, std::memory_order_release);
    if (queue->wake) SetEvent(queue->wake);
    // A meter waiting for an output's copy (WaitQueueReady, up to 250 ms) wakes too and sees stop.
    if (queue->readyEvent) SetEvent(queue->readyEvent);
    if (queue->displayWake) SetEvent(queue->displayWake); // dispoll.1
    bool exited{};
    for (int i = 0; i < 200 && !exited; ++i)
    {
        { std::lock_guard lock(queue->mutex); exited = queue->exited; }
        if (!exited) Sleep(1);
    }
    if (!exited)
    {
        std::lock_guard lock(queue->mutex);
        ReleaseQueue(*queue);
    }
    // dispoll.1: and the display thread has released its last reference on the native chain. On the tracked route XeFG's
    // Destroy ran before this (Unregister follows it), so a poll still in flight may hold the chain's last reference: it
    // goes before the destroy hook returns, ahead of any new swap chain on the window. Bounded: past it the thread still
    // owns a valid reference and releases it when its query returns.
    if (queue->displayDone) WaitForSingleObject(queue->displayDone, kDisplayStopWaitMs);
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

// pacerfix.1: the native swap chain's back buffers now; false when not read (the caller keeps what it had).
bool NativeBufferCount(const Owner& owner, uint32_t& buffers) noexcept
{
    try
    {
        auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
        if (!native || !single_overlay::native::PinInterface(native, 18)) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(native->GetDesc1(&desc)) || desc.BufferCount < 2 || desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
            return false;
        buffers = desc.BufferCount;
        return true;
    }
    catch (...) { return false; }
}

// The window of the SDK's native swap chain (IDXGISwapChain1::GetHwnd, slot 20); null when not proven.
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
// waitable credits when the limit rises. Before the first Present of a new owner. Under OptiScaler the menu caps the
// buffers (8, 16 with its fix; sdk_pacing.h), so a latency of 7 or 15.
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

void StartDisplayThread(const std::shared_ptr<FlipQueue>& queue) noexcept; // dispoll.1

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
        // Measurement only; without the pass fence the queue gates with the fixed lead.
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->pass)))) queue->pass.Reset();
        adapter_vendor_kmt::Ids ids{};
        if (adapter_vendor_kmt::Query(device->GetAdapterLuid(), ids)) queue->vendor = ids.vendor;
        queue->readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!queue->readyEvent || !queue->wake) return "event_creation";
        uint32_t buffers{}, latency{};
        NativeCapacity(owner, buffers, latency);
        queue->buffers.store(buffers, std::memory_order_release);
        queue->window = NativeWindow(owner);
        queue->baseLatency = latency;
        if (buffers > latency+1)
        {
            if (const auto raised = RaiseNativeLatency(owner, buffers-1)) latency = raised;
            else queue->latencyRefused = true;
        }
        queue->maxLatency = latency;
        queue->latency.store(latency, std::memory_order_release);
        // pacerfix.2: the display statistics read XeFG's native chain through the table proven here (slots 0-18 cover
        // GetFrameStatistics, slot 16); the stats snapshot and (opt-in) the trace ring are made once, now.
        if (auto* native = reinterpret_cast<IDXGISwapChain*>(owner.native);
            native && single_overlay::native::PinInterface(native, 18))
        {
            queue->displayChain = native;
            queue->displayTable = *reinterpret_cast<void***>(native);
        }
        const bool diagnostics = PacingDiagnosticsWanted(); // pacediag.1: the binary trace comes with it
        if (PacingTraceWanted() || diagnostics) queue->trace = std::make_unique<pacing_trace::Ring>();
        if (diagnostics) StartDiagnostics(owner, *queue, device.Get());
        queue->stats = std::make_unique<StatsSnapshot>();
        std::thread(QueueThread, queue).detach();
        StartDisplayThread(queue); // dispoll.1: before the queue is published (a gated Present reads displayWake)
        owner.queue = std::move(queue);
        return nullptr;
    }
    catch (...) { return "queue_exception"; }
}

// latency.1: with low-latency pacing on, a raised chain's maximum latency follows the batch: its outputs plus one (the
// next batch's first output), between XeFG's own latency and buffers - 1 (off: buffers - 1). A deeper limit lets whole
// batches wait after the game's Present returned, where the pacing cannot see them; bounded, the same wait holds the
// Present and moves before the input. From the batch's first output, outside the owner's gate lock (flip_queue_latency).
void AdjustNativeLatency(const Owner& owner, FlipQueue& queue, uint32_t outputs) noexcept
{
    if (queue.maxLatency <= queue.baseLatency) return; // not raised: XeFG's own
    const uint32_t wanted = latency_pacing::Enabled(latencyMode.load(std::memory_order_acquire))
        ? std::clamp(outputs+1, queue.baseLatency, queue.maxLatency) : queue.maxLatency;
    const uint32_t current = queue.latency.load(std::memory_order_acquire);
    if (wanted == current) return;
    const uint32_t applied = RaiseNativeLatency(owner, wanted);
    if (applied) queue.latency.store(applied, std::memory_order_release);
    else { queue.maxLatency = queue.baseLatency; queue.latencyRefused = true; } // refused: no batch asks again
    if (latencyLimitLogs.fetch_add(1, std::memory_order_relaxed) < 16)
        try
        {
            xess_mod::WriteLog("flip_queue_latency", "\"generation\":"+std::to_string(owner.generation)+
                ",\"outputs\":"+std::to_string(outputs)+",\"wanted\":"+std::to_string(wanted)+
                ",\"was\":"+std::to_string(current)+",\"applied\":"+std::to_string(applied)+
                ",\"buffers\":"+std::to_string(queue.buffers.load())+",\"local_time\":\""+LocalClock()+"\"");
        }
        catch (...) {}
}

// pacerfix.1: a game resize gives XeFG's native chain the game's buffer count back (NativeBuffersScope raised it only at
// creation). After one (recheck: Revoke, or RecheckCapacity from the standalone host) the next gated first output reads
// the count again on XeFG's presenter thread, where First() just proved the chain. The capacity follows, a higher
// maximum latency comes down at once, and the meter gives the clock the new count (flip_queue_capacity).
void RefreshNativeCapacity(const Owner& owner, FlipQueue& queue) noexcept
{
    if (!queue.recheck.exchange(false, std::memory_order_acq_rel)) return;
    uint32_t buffers{};
    if (!NativeBufferCount(owner, buffers)) return;
    const uint32_t was = queue.buffers.load(std::memory_order_acquire);
    if (buffers == was) return;
    const uint32_t capacity = queue.latencyRefused ? queue.baseLatency : cadence::ChainCapacity(buffers, queue.baseLatency);
    const uint32_t current = queue.latency.load(std::memory_order_acquire);
    uint32_t applied = current;
    if (current > capacity)
        if (const auto set = RaiseNativeLatency(owner, capacity))
        {
            applied = set;
            queue.latency.store(set, std::memory_order_release);
        }
    queue.maxLatency = capacity;
    queue.buffers.store(buffers, std::memory_order_release);
    try
    {
        xess_mod::WriteLog("flip_queue_capacity", "\"generation\":"+std::to_string(owner.generation)+
            ",\"buffers_was\":"+std::to_string(was)+",\"buffers\":"+std::to_string(buffers)+
            ",\"capacity\":"+std::to_string(capacity)+",\"latency_was\":"+std::to_string(current)+
            ",\"latency\":"+std::to_string(applied)+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

// ---- AMD's driver flip meter ----------------------------------------------------------------------------------------
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

// Sets `mode` with FSR's configuration (the caps flags; nothing else) and `count`, the batch's generated frames (FSR's
// 1 fits only its one generated frame; mode 0 goes with FSR's 1). Under owner.amdMutex. A refused call keeps the
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

// A decision the driver already has in effect (no SetConfig call), e.g. the meter left off at 3X+ after frame
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

// pacediag.1: about once a second, what the driver reports its flip meter is set to (GetConfig), as an A row beside what
// we last applied; a change from the previous read is logged (pacediag_amd, the first 32). Under owner.amdMutex.
constexpr int64_t kDiagAmdReadNs = 1000000000;
constexpr uint32_t kDiagAmdLogLimit = 32;
void DiagAmdReadback(Owner& owner, FlipQueue& queue) noexcept
{
    auto& amd = owner.amd;
    const int64_t now = NowNs();
    if (!amd.meter || !now || (amd.diagReadNs && now-amd.diagReadNs < kDiagAmdReadNs)) return;
    amd.diagReadNs = now;
    AmdFlipMeterConfig config{};
    const HRESULT result = AmdConfigCall(amd.meter, 5, &config);
    pacing_diag::Event event{};
    event.kind = pacing_diag::kAmd;
    event.t = now;
    event.v[0] = config.mode; event.v[1] = config.count; event.v[2] = config.flags; event.v[3] = config.status;
    event.v[4] = config.output; event.v[5] = result; event.v[6] = amd.applied; event.v[7] = amd.count;
    DiagPush(queue, event);
    const bool changed = !amd.diagRead || config.mode != amd.diagLast.mode || config.count != amd.diagLast.count
        || config.flags != amd.diagLast.flags || config.status != amd.diagLast.status;
    amd.diagLast = config;
    amd.diagRead = true;
    if (changed && amd.diagLogs < kDiagAmdLogLimit)
        try
        {
            ++amd.diagLogs;
            xess_mod::WriteLog("pacediag_amd", "\"generation\":"+std::to_string(owner.generation)+",\"result\":"+
                std::to_string(result)+",\"mode\":"+std::to_string(config.mode)+",\"count\":"+std::to_string(config.count)+
                ",\"flags\":"+std::to_string(config.flags)+",\"status\":"+std::to_string(config.status)+",\"output\":"+
                std::to_string(config.output)+",\"applied\":"+std::to_string(amd.applied)+",\"applied_count\":"+
                std::to_string(amd.count)+",\"local_time\":\""+LocalClock()+"\"");
        }
        catch (...) {}
}

// Before the SDK's native Present (presenting threads): a batch's first generated output asks for mode 2 (auto: while
// the window covers its monitor, checked every kAmdFullscreenCheckNs) with its generated count read from the caller's
// frame as the flip queue reads it; a count above the caps limit, or a frame that does not give it, asks for mode 0, as
// does any other Present on the presenter (frame generation off). The driver is called only on a change of mode or
// count (Dynamic MFG's multiplier changes included); a changed reason or count without a call is logged.
void AmdBeforePresent(Route& current, const Owners* owners, const Invocation& call, FirstCache& firstCache)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    if (!first && (offset == kExtraReturn || offset == kRealReturn)) return;
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
        if (first && !CachedFirst(firstCache, current, owner, call, generated)) reason = "first_call_layout";
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
        if (auto* queue = owner.queue.get(); queue && queue->diag) DiagAmdReadback(owner, *queue); // pacediag.1
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

// The flip queue owns this presenter's pacing: the SDK's own Pace and the extra pacing are skipped. Not for a batch
// whose first output was not gated (sdkPace).
bool QueuedPresenter(const Owners* owners, const void* presenter) noexcept
{
    if (owners) for (const auto& owner : *owners)
        if (owner->presenter == uintptr_t(presenter))
            return owner->Queued() && !owner->retired.load(std::memory_order_acquire)
                && !owner->disabled.load(std::memory_order_acquire) && !owner->sdkPace.load(std::memory_order_acquire);
    return false;
}

// Before the SDK's native Present (after its copy was queued): an output of the batch being gated gets its ready
// value and then its gate Wait on the SDK's presentation queue, and the meter thread its release. The batch is the one
// its first output (the profiled first-output caller, First()) opened; the generated outputs and then the real frame
// follow in order on the SDK's worker with the same frame value and present mode. V-Sync at interval 1 is
// gated like V-Sync off. Anything else (higher sync intervals or other present flags, frame generation off, an unproven
// first output) is not gated and simply follows the gated Presents on the queue; a refused first output gives its batch
// back to the SDK's own pacing. Returns true when gated.
// On a route without driver flip metering (AMD, Intel, NVIDIA with it switched off), a batch whose outputs exceed the
// chain's capacity + 1 (queue.maxLatency, cadence::SmallChain) goes back to the SDK's own pacing: on XeFG's 3-buffer
// chain a 5- or 6-output batch cannot queue, so a gated Present blocks in every batch and the game's hand-over follows
// our flips undetected (backlog and full stay false). The rule keys on the route's metering (flipEnabled), not the
// batch's: NVIDIA's is off after each multiplier change until gated batches teach the clock, so 5X/6X would stay
// ungated for good. It uses the capacity, not the lower maximum latency low-latency pacing may set. Logged as
// flip_queue_small_chain and flip_queue_ungated.
bool GateBeforePresent(Route& current, const Owners* owners, const Invocation& call, FirstCache& firstCache,
    std::shared_ptr<FlipQueue>& gated)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    const bool extra = offset == kExtraReturn && call.generated;
    const bool real = offset == kRealReturn && !call.generated;
    if (!first && !extra && !real) return false;
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
        const bool mode = GatedPresentMode(call);
        uint32_t generated{};
        const bool proven = first && mode && CachedFirst(firstCache, current, owner, call, generated);
        // The chain is proven XeFG's (alive) for a resize's capacity check; outside the gate lock (a DXGI call).
        if (proven) RefreshNativeCapacity(owner, queue);
        // pacerfix.2: a batch NVIDIA's driver meters (FlipBeforePresent configured it just now) is gated at its first
        // output only; the rest present at once and the display hardware holds them to the batch's spacing, as DLSS-G.
        const bool meteredBatch = proven && MeteredBatch(owner, generated+1);
        {
            std::lock_guard lock(owner.gateMutex);
            if (first)
            {
                owner.gateBatch = proven;
                // The small-chain rule (see above): capacity below the generated count.
                if (owner.gateBatch && cadence::SmallChain(flipEnabled.load(std::memory_order_acquire), queue.maxLatency,
                    generated))
                { owner.gateBatch = false; smallChain = generated+1; }
                owner.sdkPace.store(!owner.gateBatch, std::memory_order_release);
                owner.gateMetered = owner.gateBatch && meteredBatch;
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
                // pacerfix.2: inside a metered batch the output presents without a gate. The queue still owns its pacing
                // (QueuedPresenter: no SDK Pace, no extra pace) and Dispatch still times its Present for the queue
                // (`gated`: the backlog reference, display statistics, the real frame's evidence).
                if (owner.gateMetered) { gated = owner.queue; return false; }
            }
        }
        if (ungated)
        {
            if (smallChain && !owner.smallChainLogged.exchange(true, std::memory_order_relaxed))
                try
                {
                    xess_mod::WriteLog("flip_queue_small_chain", "\"generation\":"+std::to_string(owner.generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"outputs\":"+std::to_string(smallChain)+
                        ",\"buffers\":"+std::to_string(queue.buffers.load())+",\"max_latency\":"+
                        std::to_string(queue.latency.load())+",\"capacity\":"+std::to_string(queue.maxLatency)+
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
        if (first) AdjustNativeLatency(owner, queue, outputs); // latency.1, before this batch queues
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
        // The meter's item exists in the fixed ring before its GPU Wait does (a full ring leaves this output ungated). The
        // slot (its value already set, so even an unfilled one releases its gate at once) is filled below, under this lock.
        FlipQueue::Item reserved{};
        reserved.value = value;
        if (!queue.PushItem(reserved)) return false;
        if (FAILED(owner.flipQueue->Signal(queue.ready.Get(), value))
            || FAILED(owner.flipQueue->Wait(queue.gate.Get(), value)))
        {
            // A ready value alone holds nothing; a failed Wait queued nothing.
            --queue.itemCount;
            queue.failed.store(true, std::memory_order_release);
            return false;
        }
        // A sampled output's pass Signal right behind its gate Wait. Measurement only: a failure turns it off
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
        // A blocked previous batch held the game only if this batch was already waiting for the presenter. With more
        // outputs than the native latency (4X+ on XeFG's 3-buffer chain) a gated Present blocks inside every batch,
        // also while the game hands its batches over on its own; counted as coupled, each such batch would decay the
        // clock 3%, the meter would run ahead of the game and the batch's first output miss its slot.
        const bool blocked = first && queue.blocked.exchange(false, std::memory_order_acq_rel);
        const int64_t returned = queue.returnedNs.load(std::memory_order_acquire);
        const int64_t idle = first && returned && now > returned ? now-returned : 0;
        const bool backlog = returned && idle < kQueueBacklogNs;
        // A block with more than a whole batch of gates waiting held the game; otherwise, with 8 native buffers
        // (latency 7) at 3X, the clock learns its own cadence up to 57.6 ms per output.
        const bool full = blocked && queue.latency > outputs;
        if (blocked && !backlog && !full) queue.idleBlocked.fetch_add(1, std::memory_order_relaxed);
        if (full) queue.fullBlocked.fetch_add(1, std::memory_order_relaxed);
        // With a deep native chain (latency 9) nothing blocks in the native Present, yet the batch waits for our
        // previous Present, so the hand-over follows our flips. A backlogged first output counts as coupled with or
        // without a block; the clock's follow rule, on-time check and floor still bound the decay.
        if (first && backlog && !blocked) queue.leadBacklogged.fetch_add(1, std::memory_order_relaxed);
        FlipQueue::Item& item = queue.LastItem();
        item = {value, first ? now : 0, idle, outputs, index, first, backlog || full, call.sync != 0, pass, full};
        if (first)
        {
            // pacerfix.2: the game's measured wait since the previous batch (consumed here), when evidence is fresh; the
            // game Present's start on the standalone host; a metered batch.
            const int64_t accumulated = queue.gameWaitAccumNs.exchange(0, std::memory_order_acq_rel);
            const int64_t slept = queue.gameSleepAccumNs.exchange(0, std::memory_order_acq_rel);
            const int64_t seen = queue.gameWaitSeenNs.load(std::memory_order_acquire);
            const bool fresh = seen && now > seen-kEvidenceStaleNs && now-seen < kEvidenceStaleNs;
            item.gameWaitNs = fresh ? accumulated : -1;
            item.gameSleptNs = fresh && slept > 0 ? slept : 0;
            // sleepsight.1: the host's measured sleep, when any was measured since the last batch.
            const int64_t hostSlept = queue.gameHostSleepAccumNs.exchange(0, std::memory_order_acq_rel);
            const uint64_t hostSleeps = queue.gameHostSleeps.exchange(0, std::memory_order_acq_rel);
            item.gameHostSleptNs = fresh && hostSleeps ? (std::max)(hostSlept, int64_t{0}) : -1;
            item.beginSeq = queue.lastBeginSeq.load(std::memory_order_acquire);
            item.beginNs = queue.lastBeginNs.load(std::memory_order_acquire);
            item.meteredBatch = meteredBatch;
        }
        SetEvent(queue.wake);
        gated = owner.queue;
        tDiagGate = value; // pacediag.1: this Present's P row
        return true;
    }
    return false;
}

// How many gated Presents must still be waiting when a native Present returns blocked for the queue to have held it.
// XeFG waits on its native swap chain's frame-latency waitable before each Present, so the queue holds it only once
// about its maximum latency is queued: 2 on XeFG's own 3-buffer chain. A fixed 2 is too few on a deeper chain whose
// Present is slow on its own (vkd3d-proton): nearly every batch would count as coupled and the clock decay too far.
uint64_t CoupledPending(const FlipQueue& queue) noexcept
{
    return (std::max)(kQueueCoupledPending, uint64_t(queue.latency > 1 ? queue.latency-1 : 1));
}

// Each thread presenting XeFG's outputs (the game's for the real frame, XeFG's presenter for the generated
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

// XeFG's native Presents (OutputPresents).
std::atomic<uint64_t> outputPresents{};

// ---- latency.1: low-latency pacing (latency_pacing.h) ---------------------------------------------------------------
// One latency_pacing line per this many real frames.
constexpr uint64_t kLatencyLogFrames = 600;

double LatencyMs(int64_t sum, uint64_t frames) noexcept
{
    return frames ? double(sum)/double(frames)/1e6 : 0.0;
}

// A sleep point's sleep on the game's thread, with the high-resolution timer the meter thread uses: one per thread that
// sleeps (the game's one or two), kept for the process lifetime, with this thread's own learned margin.
void LatencySleep(int64_t ns) noexcept
{
    thread_local HANDLE timer{};
    thread_local cadence::WaitMargin margin;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto start = NowNs();
    if (!start) return;
    PreciseSleepUntil(timer, start+ns, margin, nullptr);
}

// pacerfix.1: what the game's thread waited inside XeMFG standalone's Present (BeginGamePresent / EndGamePresent), which
// can hold the game far longer than the real frame's own Present and Pace. The wait is the Present's wall time less the
// thread's CPU time in it (QueryThreadCycleTime, calibrated TSC), so XeFG's own work is never slept away.
// gameFeedNs: when the host's last timed Present began (0: never). While it is recent (kHostFeedStaleNs) a real frame's
// own hold goes to pendingHoldNs / pendingSeq and EndGamePresent feeds the controller once per game frame with the larger
// of the two, but only while the game sleeps at its own XeLL / Reflex point (provenSleepNs, within kProvenSleepStaleNs),
// where the sleep is before its input; otherwise the real frame's own hold sizes the sleep.
// The same waits are the flip queue's evidence (FlipQueue::gameWaitAccumNs), whatever the low-latency mode: the game's
// whole wait in the host's Present or else the real frame's own hold, plus the low-latency sleep (pendingSleptNs, merged
// into the next feed: on the standalone host at the next BeginGamePresent; on the OptiScaler route a batch later).
std::atomic<int64_t> gameFeedNs{}, pendingHoldNs{}, provenSleepNs{}, pendingSleptNs{}, lastSleepEndNs{};
std::atomic<uint64_t> pendingSeq{}, consumedSeq{}, gameFedFrames{};
std::atomic<uint64_t> tscOrigin{};
std::atomic<int64_t> tscOriginNs{};
// handle: the context the game's Present went to (BeginGamePresent), whose queue gets the wait.
struct GamePresent { int64_t enteredNs{}; ULONG64 cycles{}; void* handle{}; bool active{}; };
thread_local GamePresent tGamePresent;
// pacerfix.2: the game's Present spans (sleep point end -> next Present start) for scheduleSpanNs (latencyMutex).
std::array<int64_t, 32> spanSamples{};
size_t spanCount{}, spanNext{};

// sleepsight.1: the host's low-latency sleep, measured where the host sees it (HostSleepScope: the game's Reflex sleep
// through Streamline, whatever OptiScaler turns it into, and libxell's own sleep, which OptiScaler's export redirect keeps
// from Hook_xellSleep). The queue logs it per batch (flip_queue host_slept_ns, flip_queue_sleep), and the clock's held
// probe does not run for a batch whose game measurably did not sleep (JitterBufferClock hostSleptNs).
// pendingHostSleptNs / pendingHostSleeps: what the outer scopes measured since the last feed; our own sleep inside a
// scope (SleepBeforeInput, tHostOwnSleptNs) stays in pendingSleptNs.
std::atomic<int64_t> pendingHostSleptNs{}, lastHostSleepEndNs{};
std::atomic<uint64_t> pendingHostSleeps{};
std::atomic<uint32_t> hostSleepSourcesSeen{};
thread_local uint32_t tHostSleepDepth{};
thread_local int64_t tHostOwnSleptNs{};

// optisleep.1: OptiScaler's route (HostSleepPoint, HostPresentMarker). markerFeedNs: when the host's PRESENT_START /
// PRESENT_END pair last matched on one thread (0: never). While it is recent (kHostFeedStaleNs) a real frame's own hold
// waits in pendingHoldNs / pendingSeq for the pair's end, as with the standalone host's gameFeedNs; the flip queue still
// takes that hold as its evidence at once.
std::atomic<int64_t> markerFeedNs{};
std::atomic<uint64_t> markerFedFrames{}, hostSleepPoints{};
std::atomic<bool> markerFeedLogged{};
// optispace.1: cycleTimed: the start read the thread's CPU time (QueryThreadCycleTime; a stub returning FALSE under Wine).
struct MarkerPresent { int64_t enteredNs{}; ULONG64 cycles{}; uint32_t frame{}; bool active{}, cycleTimed{}; };
thread_local MarkerPresent tMarkerPresent;
// optispace.1: for the log, the PRESENT_START / PRESENT_END edges seen and the matched pairs timed without CPU time.
std::atomic<uint64_t> markerStarts{}, markerEnds{}, markerSpanTimed{};
// optispace.1: the marker thread's recent spans, for the wait without the thread's CPU time (latency_pacing SpanFloor).
thread_local latency_pacing::SpanFloor tMarkerSpans;

const char* HostSleepSourceName(HostSleepSource source) noexcept
{
    switch (source)
    {
    case HostSleepSource::kReflex: return "streamline_reflex";
    case HostSleepSource::kXell: return "libxell";
    }
    return "unknown";
}

// The game waited `waitNs` on the pacing (returned at `now`): the queue's evidence for its next batch, with the
// low-latency sleep and the host's measured sleeps (and their count) since the last feed, each in its own sum.
void FeedEvidence(FlipQueue& queue, int64_t now, int64_t waitNs) noexcept
{
    // A sleep that ended long ago (no queue took it then) is not this batch's.
    int64_t slept = pendingSleptNs.exchange(0, std::memory_order_acq_rel);
    const int64_t sleepEnd = lastSleepEndNs.load(std::memory_order_acquire);
    if (slept < 0 || !sleepEnd || (now > sleepEnd && now-sleepEnd > kEvidenceStaleNs)) slept = 0;
    queue.gameWaitAccumNs.fetch_add(waitNs > 0 ? waitNs : 0, std::memory_order_acq_rel);
    if (slept) queue.gameSleepAccumNs.fetch_add(slept, std::memory_order_acq_rel);
    int64_t hostSlept = pendingHostSleptNs.exchange(0, std::memory_order_acq_rel);
    uint64_t hostSleeps = pendingHostSleeps.exchange(0, std::memory_order_acq_rel);
    const int64_t hostEnd = lastHostSleepEndNs.load(std::memory_order_acquire);
    if (!hostEnd || (now > hostEnd && now-hostEnd > kEvidenceStaleNs)) { hostSlept = 0; hostSleeps = 0; }
    if (hostSleeps)
    {
        queue.gameHostSleepAccumNs.fetch_add(hostSlept > 0 ? hostSlept : 0, std::memory_order_acq_rel);
        queue.gameHostSleeps.fetch_add(hostSleeps, std::memory_order_acq_rel);
    }
    queue.gameWaitSeenNs.store(now ? now : 1, std::memory_order_release);
}

// pacerfix.2: the queue of the owner on context `handle`, if it is gating (one owners snapshot; null otherwise).
std::shared_ptr<FlipQueue> HandleQueue(void* handle) noexcept
{
    if (!handle) return {};
    if (auto* current = route.load(std::memory_order_acquire))
        if (const auto owners = current->owners.load(std::memory_order_acquire)) for (const auto& owner : *owners)
            if (owner->handle == handle)
                return owner->Queued() && !owner->retired.load(std::memory_order_acquire) ? owner->queue : nullptr;
    return {};
}

// pacerfix.2: how long the low-latency sleep starting at `now` may last so the game's next frame can still reach its
// batch's first slot: that slot, less the first output's production (p90) and the game's span from its sleep point to
// its Present (p90), less kScheduleGuardNs. -1: no bound (no fresh schedule). With an engine that presents a frame later
// on its render thread the span is a frame short and the slot a batch early, which cancel.
int64_t ScheduleCap(int64_t now) noexcept
{
    const int64_t seen = scheduleSeenNs.load(std::memory_order_acquire);
    if (!seen || (now > seen && now-seen > kScheduleStaleNs)) return -1;
    const int64_t next = scheduleNextBatchNs.load(std::memory_order_relaxed);
    const int64_t batch = scheduleBatchNs.load(std::memory_order_relaxed);
    const int64_t production = scheduleProductionNs.load(std::memory_order_relaxed);
    const int64_t span = scheduleSpanNs.load(std::memory_order_relaxed);
    const uint64_t published = scheduleBeginSeq.load(std::memory_order_relaxed);
    const uint64_t begun = gameBeginSeq.load(std::memory_order_acquire);
    if (next <= 0 || batch <= 0 || production <= 0 || span <= 0 || !published || begun < published || begun-published > 3)
        return -1;
    const int64_t latest = next+int64_t(begun-published)*batch-production-span-kScheduleGuardNs;
    return latest > now ? latest-now : 0;
}

// The calibration needs this long between its two readings.
constexpr int64_t kTscCalibrationNs = 200000000;

// TSC cycles per nanosecond since the first timed Present; 0 until kTscCalibrationNs have passed.
double TscPerNs(int64_t now) noexcept
{
    const int64_t originNs = tscOriginNs.load(std::memory_order_acquire);
    const uint64_t origin = tscOrigin.load(std::memory_order_acquire);
    if (!originNs || !origin || now-originNs < kTscCalibrationNs) return 0.0;
    const uint64_t tsc = __rdtsc();
    return tsc > origin ? double(tsc-origin)/double(now-originNs) : 0.0;
}

// The TSC calibration's first reading, once (the first timed Present: the standalone host's or the host's markers).
void CalibrateTsc(int64_t now) noexcept
{
    if (tscOriginNs.load(std::memory_order_acquire)) return;
    const uint64_t tsc = __rdtsc();
    int64_t expected{};
    if (tscOriginNs.compare_exchange_strong(expected, now, std::memory_order_acq_rel))
        tscOrigin.store(tsc, std::memory_order_release);
}

// The controller takes one real frame: its hold (in Present, and Pace) ending at `returned`. optispace.1: paired: the
// game's own Present wait from the host's markers, which the controller takes per frame pair (latency_pacing SetPaired).
void FeedLatency(int64_t returned, int64_t held, bool paired = false) noexcept
{
    try
    {
        latency_pacing::Stats now{}, then{};
        bool log{};
        // pacerfix.2: while the flip queue's schedule bounds the sleep (and the queue takes the game's measured wait as
        // evidence), the hold kept for the clock's inference rules is not needed: a smaller margin.
        const int64_t margin = ScheduleCap(returned) >= 0 ? kScheduledMarginNs : latency_pacing::kMarginNs;
        {
            std::lock_guard lock(latencyMutex);
            latencyController.SetMargin(margin);
            latencyController.SetPaired(paired);
            latencyController.RealFrame(returned, held);
            now = latencyController.Read();
            if (now.frames-latencyLogged.frames >= kLatencyLogFrames) { then = latencyLogged; latencyLogged = now; log = true; }
        }
        if (!log) return;
        const uint64_t frames = now.frames-then.frames;
        char fields[1024]{};
        sprintf_s(fields, "\"mode\":%u,\"frames\":%llu,\"held_ms\":%.3f,\"slack_ms\":%.3f,\"slept_ms\":%.3f,\"sleep_ms\":%.3f,"
            "\"takes\":%llu,\"missed\":%llu,\"resets\":%llu,\"skipped\":%llu,\"backoffs\":%llu,\"ineffective\":%llu,"
            "\"capped\":%llu,\"margin_ms\":%.3f,\"game_fed\":%llu,\"marker_fed\":%llu,\"host_sleep_points\":%llu,"
            "\"paired\":%s,\"marker_starts\":%llu,\"marker_ends\":%llu,\"marker_span_timed\":%llu,"
            "\"local_time\":\"",
            latencyMode.load(std::memory_order_relaxed), static_cast<unsigned long long>(frames),
            LatencyMs(now.heldSumNs-then.heldSumNs, frames), LatencyMs(now.slackSumNs-then.slackSumNs, frames),
            LatencyMs(now.sleptSumNs-then.sleptSumNs, frames), double(now.sleepNs)/1e6,
            static_cast<unsigned long long>(now.takes-then.takes), static_cast<unsigned long long>(now.missed-then.missed),
            static_cast<unsigned long long>(now.resets-then.resets), static_cast<unsigned long long>(now.skipped-then.skipped),
            static_cast<unsigned long long>(now.backoffs-then.backoffs),
            static_cast<unsigned long long>(now.ineffective-then.ineffective),
            static_cast<unsigned long long>(now.capped-then.capped), double(margin)/1e6,
            static_cast<unsigned long long>(gameFedFrames.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(markerFedFrames.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(hostSleepPoints.load(std::memory_order_relaxed)),
            paired ? "true" : "false",
            static_cast<unsigned long long>(markerStarts.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(markerEnds.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(markerSpanTimed.load(std::memory_order_relaxed)));
        xess_mod::WriteLog("latency_pacing", fields+LocalClock()+"\"");
    }
    catch (...) {}
}

// pacerfix.1: the low-latency pacing's history is process-wide. An owner's teardown (Unregister: destroy,
// or a context initialized again) starts it over, so the next context learns its own hold, and drops the last
// generated frame id and any real frame's hold not yet fed.
void ResetLatencyHistory() noexcept
{
    try
    {
        std::lock_guard lock(latencyMutex);
        latencyController.Reset();
    }
    catch (...) {}
    latencyGeneratedFrame.store(UINT64_MAX, std::memory_order_relaxed);
    consumedSeq.store(pendingSeq.load(std::memory_order_acquire), std::memory_order_release);
    // pacerfix.2: no sleep of the old context is the next one's evidence, and its schedule bounds nothing.
    pendingSleptNs.store(0, std::memory_order_release);
    pendingHostSleptNs.store(0, std::memory_order_release);
    pendingHostSleeps.store(0, std::memory_order_release);
    scheduleSeenNs.store(0, std::memory_order_release);
    // optisleep.1: the next context's real frames feed at once until the host's markers match again.
    markerFeedNs.store(0, std::memory_order_release);
}

// The real frame's native Present returned on its thread (the game's): how long it and its Pace held the thread.
// pacerfix.2: queue: the presenter's flip queue when the Present went through it (Dispatch's `gated`), which takes the
// hold as evidence unless the standalone host times the game's whole Present (EndGamePresent feeds it then).
void LatencyRealFrame(int64_t entered, int64_t returned, FlipQueue* queue) noexcept
{
    const int64_t pace = std::exchange(tLatencyPaceNs, 0);
    if (!entered || !returned || returned < entered) return;
    const int64_t held = returned-entered+pace;
    const int64_t feed = gameFeedNs.load(std::memory_order_acquire);
    // The standalone host times the game's whole Present (EndGamePresent, whatever the mode); this hold is its floor.
    if (feed && (returned < feed || returned-feed < kHostFeedStaleNs))
    {
        pendingHoldNs.store(held, std::memory_order_release);
        pendingSeq.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    if (queue) FeedEvidence(*queue, returned, held);
    // optisleep.1: OptiScaler's route while the host's Present markers match: the pair's end feeds the controller once
    // per game frame (HostPresentMarker), this hold its floor.
    const int64_t marked = markerFeedNs.load(std::memory_order_acquire);
    if (marked && (returned < marked || returned-marked < kHostFeedStaleNs))
    {
        pendingHoldNs.store(held, std::memory_order_release);
        pendingSeq.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    if (latency_pacing::Enabled(latencyMode.load(std::memory_order_acquire))) FeedLatency(returned, held);
}

// pacerfix.2: what the display reported for XeFG's native chain (DXGI frame statistics): displayed-Present gaps and, about
// once a second, the composition mode. Measurement only. A changed table or repeated failures with no sample turn it off;
// the composition query, never yet seen to succeed, stops after kCompositionFailureLimit failures in a row.
// dispoll.1: queried only on the queue's display thread (DisplayThread), which XeFG's presenter thread just wakes.
constexpr int64_t kCompositionCheckNs = 1000000000;
constexpr uint64_t kDisplayFailureLimit = 64;
constexpr uint32_t kCompositionFailureLimit = 3;
// dispoll.1: the display thread's idle wait (ms), after which it checks for a stop that came without a wake.
constexpr DWORD kDisplayIdleWaitMs = 100;
double QpcMs(int64_t ticks) noexcept
{
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? double(value.QuadPart) : 0.0;
    }();
    return frequency > 0.0 ? double(ticks)*1000.0/frequency : 0.0;
}

// dispoll.1: one frame-statistics query and the display counters it feeds (display thread; chain is its ticket).
// advanced = how far the displayed present count moved since the previous query (0: unchanged or unknown).
HRESULT FrameStatsPoll(FlipQueue& queue, IDXGISwapChain* chain, DXGI_FRAME_STATISTICS& stats, UINT& advanced)
{
    advanced = 0;
    stats = {};
    if (*reinterpret_cast<void***>(chain) != queue.displayTable) { queue.displayOff = true; return E_ABORT; }
    const HRESULT result = chain->GetFrameStatistics(&stats);
    std::lock_guard lock(queue.displayMutex);
    if (result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) { ++queue.displayDisjoint; queue.displayCount = 0; }
    else if (FAILED(result))
    {
        if (++queue.displayFailures >= kDisplayFailureLimit && !queue.displaySamples) queue.displayOff = true;
    }
    else if (stats.PresentCount && stats.PresentCount != queue.displayCount)
    {
        if (queue.displayCount && stats.SyncQPCTime.QuadPart > queue.displayQpc)
        {
            advanced = stats.PresentCount-queue.displayCount;
            if (advanced == 1)
            {
                queue.displayGaps.Add(QpcMs(stats.SyncQPCTime.QuadPart-queue.displayQpc));
                ++queue.displaySamples;
            }
            else if (advanced < 1024) queue.displaySkipped += advanced-1;
        }
        queue.displayCount = stats.PresentCount;
        queue.displayQpc = stats.SyncQPCTime.QuadPart;
    }
    return result;
}

// The chain's composition mode about once a second (IDXGISwapChainMedia; -1 unknown). With the diagnostics on, the
// first failure says which step failed.
void CompositionPoll(FlipQueue& queue, IDXGISwapChain* chain)
{
    if (queue.compositionOff) return;
    const auto now = NowNs();
    if (!now || (queue.compositionCheckedNs && now-queue.compositionCheckedNs < kCompositionCheckNs)) return;
    queue.compositionCheckedNs = now;
    int mode = -1;
    const char* step = "query_interface";
    HRESULT stepResult{};
    IDXGISwapChainMedia* media{};
    stepResult = chain->QueryInterface(IID_PPV_ARGS(&media));
    if (SUCCEEDED(stepResult) && media)
    {
        step = "pin";
        if (single_overlay::native::PinInterface(media, 3)) // quarantined otherwise
        {
            step = "frame_statistics_media";
            DXGI_FRAME_STATISTICS_MEDIA mediaStats{};
            stepResult = media->GetFrameStatisticsMedia(&mediaStats);
            if (SUCCEEDED(stepResult)) mode = int(mediaStats.CompositionMode);
            media->Release();
        }
    }
    if (mode < 0 && queue.diag && !queue.diagCompositionLogged)
    {
        queue.diagCompositionLogged = true;
        xess_mod::WriteLog("pacediag_composition", "\"generation\":"+std::to_string(queue.generation)+",\"step\":\""+step+
            "\",\"result\":"+std::to_string(stepResult)+",\"local_time\":\""+LocalClock()+"\"");
    }
    queue.compositionFailures = mode < 0 ? queue.compositionFailures+1 : 0;
    std::lock_guard lock(queue.displayMutex);
    queue.compositionMode = mode;
    if (queue.compositionFailures >= kCompositionFailureLimit) queue.compositionOff = true;
}

// dispoll.1: the display thread only; chain is the ticket it holds (a reference on XeFG's native chain) for this poll.
void DisplayPoll(FlipQueue& queue, IDXGISwapChain* chain) noexcept
{
    try
    {
        DXGI_FRAME_STATISTICS stats{};
        UINT advanced{};
        const HRESULT result = FrameStatsPoll(queue, chain, stats, advanced);
        if (FAILED(result)) return;
        CompositionPoll(queue, chain);
    }
    catch (...) {}
}

// pacediag.1: with the diagnostics on, the ticket's poll repeats about every kDiagPollStepNs until the native chain reports
// the latest gated Present displayed (diagLastPresentId), at most kDiagPollWindowNs, or the queue stops, so every
// displayed present gets its own D row. The ticket's reference is held that long; a stop ends the loop within a step.
constexpr int64_t kDiagPollWindowNs = 12000000, kDiagPollStepNs = 500000;
void DiagWait(HANDLE timer) noexcept
{
    LARGE_INTEGER due{};
    due.QuadPart = -(kDiagPollStepNs/100);
    if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 5);
    else Sleep(1);
}

void DiagDisplayPolls(FlipQueue& queue, IDXGISwapChain* chain, HANDLE timer) noexcept
{
    const int64_t began = NowNs();
    bool composition = true;
    HRESULT lastFailure = S_OK;
    for (;;)
    {
        DXGI_FRAME_STATISTICS stats{};
        UINT advanced{};
        HRESULT result = E_FAIL;
        try { result = FrameStatsPoll(queue, chain, stats, advanced); }
        catch (...) {}
        const int64_t now = NowNs();
        const bool fresh = SUCCEEDED(result) && stats.PresentCount && int64_t(stats.PresentCount) != queue.diagLastDisplayed;
        if (fresh || (FAILED(result) && result != lastFailure))
        {
            if (fresh) queue.diagLastDisplayed = int64_t(stats.PresentCount);
            else lastFailure = result;
            pacing_diag::Event event{};
            event.kind = pacing_diag::kDisplay;
            event.t = now;
            event.v[0] = fresh ? int64_t(stats.PresentCount) : -1;
            event.v[1] = int64_t(stats.PresentRefreshCount);
            event.v[2] = int64_t(stats.SyncRefreshCount);
            event.v[3] = fresh ? QpcToNs(stats.SyncQPCTime.QuadPart) : 0;
            event.v[4] = int64_t(advanced);
            event.v[5] = int64_t(result);
            DiagPush(queue, event);
        }
        if (composition && SUCCEEDED(result))
        {
            composition = false;
            try { CompositionPoll(queue, chain); }
            catch (...) {}
        }
        if (queue.displayOff || queue.stop.load(std::memory_order_acquire) || queue.failed.load(std::memory_order_acquire))
            break;
        // Done once the latest gated Present is displayed. A count far beyond the present ids (not the chain's own
        // numbering on some driver) never ends the window early: the polls then run the whole window.
        const int64_t wanted = queue.diagLastPresentId.load(std::memory_order_relaxed);
        if (SUCCEEDED(result) && wanted >= 0 && int64_t(stats.PresentCount) >= wanted
            && int64_t(stats.PresentCount)-wanted < 1024) break;
        if (!now || !began || now-began >= kDiagPollWindowNs) break;
        DiagWait(timer);
    }
}

// pacediag.1: a gated Present's P row (presenting thread, right after its native Present; XeFG holds the chain): the
// gate it was queued behind and the chain's present id (GetLastPresentCount, slot 17), also for the display polls.
void DiagPresent(FlipQueue& queue, const Invocation& call, uintptr_t site, int64_t entered, int64_t returned,
    HRESULT result) noexcept
{
    int64_t id = -1;
    try
    {
        auto* chain = queue.displayChain;
        UINT count{};
        if (chain && *reinterpret_cast<void***>(chain) == queue.displayTable && SUCCEEDED(chain->GetLastPresentCount(&count)))
            id = int64_t(count);
    }
    catch (...) {}
    if (id >= 0)
    {
        int64_t seen = queue.diagLastPresentId.load(std::memory_order_relaxed);
        while (id > seen && !queue.diagLastPresentId.compare_exchange_weak(seen, id, std::memory_order_relaxed)) {}
    }
    if (returned) queue.diagPresentNs.store(returned, std::memory_order_relaxed);
    pacing_diag::Event event{};
    event.kind = pacing_diag::kPresent;
    event.t = returned;
    event.v[0] = int64_t(tDiagGate);
    event.v[1] = id;
    event.v[2] = entered;
    event.v[3] = returned;
    event.v[4] = int64_t(call.sync);
    event.v[5] = int64_t(call.flags);
    event.v[6] = call.generated ? 1 : 0;
    event.v[7] = int64_t(call.frame);
    event.v[8] = site == kFirstReturn ? 0 : site == kExtraReturn ? 1 : site == kRealReturn ? 2 : 3;
    event.v[9] = int64_t(GetCurrentThreadId());
    event.v[10] = int64_t(result);
    DiagPush(queue, event);
}

// dispoll.1: the queue's display statistics, off XeFG's presenter thread. Each poll uses the reference a gated Present handed
// over (displayTicket), taken while that Present had the chain alive, and releases it as soon as the queries return, so a
// chain XeFG releases or re-creates is never kept past one query in flight. The thread ends on the queue's stop or failure
// or once the poll turns itself off; it then closes (no more tickets), releases a pending ticket and sets displayDone.
void ReleaseDisplayTicket(IDXGISwapChain* chain) noexcept
{
    try { if (chain) chain->Release(); } catch (...) {}
}

void DisplayThread(std::shared_ptr<FlipQueue> queue) noexcept
{
    // pacediag.1: with the diagnostics on, the polls repeat on a high-resolution timer above normal priority (so a busy
    // game thread does not stretch the steps), and the GPU's engines are sampled between tickets (DiagGpuSample).
    const bool diagnostics = queue->diag != nullptr;
    HANDLE timer = diagnostics ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS) : nullptr;
    if (diagnostics) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;)
    {
        const DWORD wait = WaitForSingleObject(queue->displayWake, kDisplayIdleWaitMs);
        if (wait == WAIT_FAILED || queue->displayOff) break;
        if (queue->stop.load(std::memory_order_acquire) || queue->failed.load(std::memory_order_acquire)) break;
        if (diagnostics) DiagGpuSample(*queue);
        if (auto* chain = queue->displayTicket.exchange(nullptr, std::memory_order_seq_cst))
        {
            if (diagnostics) DiagDisplayPolls(*queue, chain, timer);
            else DisplayPoll(*queue, chain);
            ReleaseDisplayTicket(chain);
        }
    }
    if (timer) CloseHandle(timer);
    if (diagnostics)
        try { DiagGpuSummary(*queue); }
        catch (...) {}
    // Closed first, then drained: a Present issuing at the same time either sees the close or leaves its ticket here.
    queue->displayClosed.store(true, std::memory_order_seq_cst);
    ReleaseDisplayTicket(queue->displayTicket.exchange(nullptr, std::memory_order_seq_cst));
    SetEvent(queue->displayDone);
}

// dispoll.1: from a presenting thread right after its gated Present returned (XeFG still holds the chain there): one
// reference for the display thread's next poll, unless one is still pending (that poll covers this Present too). A ticket
// that lands after the thread closed is taken back here and released, which is never the chain's last reference.
void IssueDisplayTicket(FlipQueue& queue) noexcept
{
    auto* chain = queue.displayChain;
    if (!chain || !queue.displayWake || queue.displayClosed.load(std::memory_order_acquire)
        || queue.displayTicket.load(std::memory_order_acquire)) return;
    try
    {
        chain->AddRef();
        IDXGISwapChain* expected{};
        if (!queue.displayTicket.compare_exchange_strong(expected, chain, std::memory_order_seq_cst))
        { chain->Release(); return; } // another presenting thread's ticket is pending
        if (queue.displayClosed.load(std::memory_order_seq_cst))
        {
            expected = chain;
            if (queue.displayTicket.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst)) chain->Release();
            return;
        }
        SetEvent(queue.displayWake);
    }
    catch (...) {}
}

// dispoll.1: from StartQueue before the queue is published; without either event or the thread, display_stats is false.
void StartDisplayThread(const std::shared_ptr<FlipQueue>& queue) noexcept
{
    if (!queue->displayChain) return;
    queue->displayWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->displayDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool started{};
    if (queue->displayWake && queue->displayDone)
    {
        try { std::thread(DisplayThread, queue).detach(); started = true; }
        catch (...) {}
    }
    if (started) return;
    if (queue->displayWake) CloseHandle(queue->displayWake);
    if (queue->displayDone) CloseHandle(queue->displayDone);
    queue->displayWake = queue->displayDone = nullptr;
    queue->displayChain = nullptr;
}

HRESULT Dispatch(Route& current, const Invocation& call)
{
    const DWORD incomingError = GetLastError();
    NotePresenterThread();
    outputPresents.fetch_add(1, std::memory_order_relaxed);
    // latency.1: a real frame after its batch's generated outputs (frame generation on) is timed for the low-latency
    // pacing, with its Pace on this thread (HookPace); any other real-frame Present drops that Pace's time.
    const auto site = call.returnAddress-uintptr_t(current.module);
    if (call.generated) latencyGeneratedFrame.store(call.frame, std::memory_order_relaxed);
    const bool latencyReal = site == kRealReturn && !call.generated
        && latencyGeneratedFrame.load(std::memory_order_relaxed) == call.frame;
    if (site == kRealReturn && !call.generated && !latencyReal) tLatencyPaceNs = 0;
    tDiagGate = 0; // pacediag.1: GateBeforePresent sets it for a gated output
    // pacerfix.1: one owners snapshot and First() answer for all this Present's paths (dropped before its native Present).
    auto owners = current.owners.load(std::memory_order_acquire);
    FirstCache firstCache{};
    if (current.enabled.load(std::memory_order_acquire) && flipFunction.load(std::memory_order_acquire))
    {
        try { FlipBeforePresent(current, owners.get(), call, firstCache); }
        catch (...) {} // Flip metering must not replace the original Present result.
    }
    if (current.enabled.load(std::memory_order_acquire) && amdCreate.load(std::memory_order_acquire))
    {
        try { AmdBeforePresent(current, owners.get(), call, firstCache); }
        catch (...) {} // Nor may AMD's.
    }
    bool queued{};
    std::shared_ptr<FlipQueue> gated;
    if (current.enabled.load(std::memory_order_acquire))
    {
        try
        {
            queued = GateBeforePresent(current, owners.get(), call, firstCache, gated)
                || QueuedPresenter(owners.get(), call.presenter);
        }
        catch (...) {} // The flip queue must not replace the original Present result.
    }
    if (!queued && current.enabled.load(std::memory_order_acquire)
        && call.returnAddress == uintptr_t(current.module)+kExtraReturn && call.generated)
    {
        try
        {
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
                    mfgunlock::xemfg::native::RuntimeCall trace("xefg_extra_pace", 0, call.frame);
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
    owners.reset();
    SetLastError(incomingError);
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::xemfg::native::RuntimeCall trace("xefg_presenter", 0, call.frame);
#endif
    if (!gated && !latencyReal)
        return current.original(call.presenter, call.sync, call.flags, call.frame, call.parameters, call.resource, call.generated);
    if (!gated)
    {
        // latency.1: the CPU-paced real frame (its Pace above it held the thread too).
        const auto entered = NowNs();
        SetLastError(incomingError);
        const HRESULT result = current.original(call.presenter, call.sync, call.flags, call.frame, call.parameters,
            call.resource, call.generated);
        const DWORD outgoingError = GetLastError();
        LatencyRealFrame(entered, NowNs(), nullptr);
        SetLastError(outgoingError);
        return result;
    }
    // How long the gated native Present held this thread (the queue's back-pressure on the game).
    const auto entered = NowNs();
    SetLastError(incomingError);
    const HRESULT result = current.original(call.presenter, call.sync, call.flags, call.frame, call.parameters, call.resource,
        call.generated);
    const DWORD outgoingError = GetLastError();
    const auto returned = NowNs();
    // Held by our own queue only while gated Presents are waiting (ReShade on the native swap chain makes every
    // Present slow, with the queue empty), as many as fill the native swap chain (CoupledPending).
    if (entered && returned-entered > kQueueBlockedNs
        && gated->issued-gated->signaled.load(std::memory_order_acquire) >= CoupledPending(*gated))
        gated->blocked.store(true, std::memory_order_release);
    if (returned) gated->returnedNs.store(returned, std::memory_order_release); // the backlog reference
    if (latencyReal) LatencyRealFrame(entered, returned, gated.get());
    // pacediag.1: its P row and present id, before the ticket wakes the display thread's polls for it.
    if (gated->diag) DiagPresent(*gated, call, site, entered, returned, result);
    if (SUCCEEDED(result)) IssueDisplayTicket(*gated);
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
    // On the Vulkan host, the xvk swap chain's Present1 runs inside this call and labels the output for
    // Reflex (its batch's frame, real or generated).
    const xvk::OutputFrameScope output(frame, generated);
    // latmon.1: the latency probe's label for the native Present that follows on this thread (its output layer).
    latency_probe::XefgOutput(frame, generated);
    return Dispatch(*current, call);
}

// The SDK's own Pace runs unchanged; the hook provides the trampoline the extra-output
// pacing in Dispatch calls. A presenter on the flip queue is not paced on the CPU (the real frame's Pace
// would also wait for output 0's present fence, which the gate holds); its return value is not used (0x220411).
// A batch the queue did not gate is paced by the SDK as without the queue.
__declspec(noinline) bool HookPace(void* presenter, const void* bundle, uint8_t firstFence,
    const void* summary, uint32_t index)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->pace) return false;
    try
    {
        if (current->enabled.load(std::memory_order_acquire)
            && QueuedPresenter(current->owners.load(std::memory_order_acquire).get(), presenter)) return false;
    }
    catch (...) {}
    // latency.1: how long the real frame's Pace holds the game's thread, added to its Present (LatencyRealFrame).
    const DWORD incomingError = GetLastError();
    const auto began = latency_pacing::Enabled(latencyMode.load(std::memory_order_relaxed)) ? NowNs() : 0;
    SetLastError(incomingError);
    const bool paced = current->pace(presenter, bundle, firstFence, summary, index);
    if (began)
    {
        const DWORD outgoingError = GetLastError();
        if (const auto ended = NowNs(); ended > began) tLatencyPaceNs += ended-began;
        SetLastError(outgoingError);
    }
    return paced;
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
        // Present and Pace only; XeFG's deadline (0x224b30) is left alone.
        for (const auto function : {0x21f730u,0x21ee30u})
            layout.VerifyFunction(provider,function);
        // First() reads the first-output caller's frame; without it only flip metering is off.
        bool firstVerified{};
        try { layout.VerifyFunction(provider, 0x220c70u); firstVerified = true; }
        catch (const std::exception&) {}
        kNativePresent=layout.Rva(0x21f730); kPace=layout.Rva(0x21ee30);
        kExtraReturn=layout.Rva(0x2202ed); kFirstReturn=layout.Rva(0x220db2); kRealReturn=layout.Rva(0x220467);
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
        const std::array<Hook, 2> hooks{{
            {kNativePresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&current->original)},
            {kPace, reinterpret_cast<void*>(&HookPace), reinterpret_cast<void**>(&current->pace)}}};
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
            ",\"real_return_rva\":"+std::to_string(kRealReturn)+",\"deadline_hook\":false,\"profile_verified\":true"+
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
        // pacerfix.2: the regions Live() and Prepare() then read directly (OwnerValue).
        {
            const uintptr_t objects[4]{uintptr_t(handle), owner->implementation, owner->chain, owner->presenter};
            for (size_t i = 0; i < owner->regions.size(); ++i)
            {
                const auto region = RegionOf(objects[i]);
                owner->regions[i] = {region.begin, region.end};
            }
        }
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
        // MFGAdaUnlock 1.7.1: XeFG waits on its native chain's frame-latency waitable before each output, so that
        // limit bounds the outputs queued ahead of the display; [XeFG] XeFGNativeLatency changes it when the flip queue
        // (which sets its own) is off.
        if (const auto requested = nativeLatencyRequested.load(std::memory_order_acquire);
            requested && !queueEnabled.load(std::memory_order_acquire))
        {
            uint32_t buffers{}, latency{};
            NativeCapacity(*owner, buffers, latency);
            const auto applied = RaiseNativeLatency(*owner, requested);
            xess_mod::WriteLog("native_chain_capacity", "\"generation\":"+std::to_string(generation)+
                ",\"buffers\":"+std::to_string(buffers)+",\"max_latency\":"+std::to_string(latency)+
                ",\"requested\":"+std::to_string(requested)+",\"applied\":"+std::to_string(applied));
        }
        // Only with a driver function; the owner keeps plain pacing without its queue.
        if (flipFunction.load(std::memory_order_acquire))
        {
            const char* reason = ResolveFlipQueue(*owner, device);
            xess_mod::WriteLog("flip_metering_queue", "\"generation\":"+std::to_string(generation)+
                ",\"resolved\":"+(reason ? "false" : "true")+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : "")+
                ",\"queue\":"+std::to_string(uintptr_t(owner->flipQueue.Get()))+
                ",\"enabled\":"+(flipEnabled.load() ? "true" : "false")+",\"lead_ns\":"+std::to_string(flipLeadNs.load())+
                ",\"first_caller_verified\":"+(firstLayout.load() ? "true" : "false"));
        }
        // The flip queue, on every vendor (the gate is plain D3D12); it needs the presentation queue and the
        // verified first-output caller (First() opens each gated batch).
        if (queueEnabled.load(std::memory_order_acquire))
        {
            const char* reason = !firstLayout.load(std::memory_order_acquire) ? "first_layout"
                : owner->flipQueue ? nullptr : ResolveFlipQueue(*owner, device);
            if (!reason) reason = StartQueue(*owner);
            xess_mod::WriteLog("flip_queue_owner", "\"generation\":"+std::to_string(generation)+
                ",\"started\":"+(reason ? "false" : "true")+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : "")+
                ",\"buffers\":"+std::to_string(owner->queue ? owner->queue->buffers.load() : 0u)+
                ",\"max_latency\":"+std::to_string(owner->queue ? owner->queue->latency.load() : 0u)+
                ",\"gate_ahead\":true,\"display_stats\":"+(owner->queue && owner->queue->displayChain ? "true" : "false")+
                ",\"display_thread\":"+(owner->queue && owner->queue->displayDone ? "true" : "false")+ // dispoll.1
                ",\"trace\":"+(owner->queue && owner->queue->trace ? "true" : "false")+
                ",\"first_lead_ns\":"+std::to_string(kQueueSignalLead)+",\"metered_inner_lead_ns\":"+
                std::to_string(flipLeadNs.load())+",\"vendor_id\":"+std::to_string(owner->queue ? owner->queue->vendor : 0)+
                ",\"gate_pass\":"+(owner->queue && owner->queue->pass ? "true" : "false")+",\"gate_lead_bounds_ns\":["+
                std::to_string(cadence::GatePassLead::kMinimumLead)+","+std::to_string(cadence::GatePassLead::kMaximumLead)+"]");
        }
        // AMD's flip meter on the presentation queue's device (the driver's own D3D12 device behind any proxy),
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

void RecheckCapacity(void* handle) noexcept
{
    if (auto* current = route.load(std::memory_order_acquire))
        if (const auto owners = current->owners.load(std::memory_order_acquire)) for (const auto& owner : *owners)
            if (owner->handle == handle && owner->queue) owner->queue->recheck.store(true, std::memory_order_release);
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
        bool removed{};
        for (const auto& entry : *prior)
            if (entry->handle != handle || entry->generation != generation) next->push_back(entry);
            // In-flight callbacks retain their Owner snapshot.
            else
            {
                entry->retired.store(true, std::memory_order_release); Revoke(*entry); StopQueue(*entry); ReleaseAmdMeter(*entry);
                removed = true;
            }
        current->owners.store(next, std::memory_order_release);
        if (removed) ResetLatencyHistory();
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
        ResetLatencyHistory();
    }
}

void SetFlipMetering(SetFlipConfigFunction setFlipConfig, bool enabled, int64_t innerLeadNs) noexcept
{
    flipLeadNs.store(innerLeadNs >= 0 && innerLeadNs <= 8000000 ? innerLeadNs : 2500000, std::memory_order_relaxed);
    flipEnabled.store(enabled && setFlipConfig, std::memory_order_release);
    flipFunction.store(setFlipConfig, std::memory_order_release);
}

// MFGAdaUnlock: the panel's live flip-metering switch (applies from the next batch) and its read-only status.
void RequestFlipMetering(bool enabled) noexcept
{
    flipEnabled.store(enabled && flipFunction.load(std::memory_order_acquire), std::memory_order_release);
}
void RequestNativeLatency(uint32_t latency) noexcept
{
    nativeLatencyRequested.store(latency <= 16 ? latency : 0, std::memory_order_release);
}
FlipStatus Snapshot(void* handle) noexcept
{
    FlipStatus status{};
    status.enabled = flipEnabled.load(std::memory_order_acquire);
    status.requested = status.enabled;
    status.available = flipFunction.load(std::memory_order_acquire) != nullptr;
    const auto current = route.load(std::memory_order_acquire);
    if (!current) return status;
    status.installed = current->enabled.load(std::memory_order_acquire);
    const auto owners = current->owners.load(std::memory_order_acquire);
    if (!owners) return status;
    for (const auto& owner : *owners)
    {
        if (owner->handle != handle || owner->retired.load() || owner->disabled.load()) continue;
        status.registered = true;
        status.queue = bool(owner->flipQueue);
        std::unique_lock guard(owner->flipMutex, std::try_to_lock);
        if (!guard.owns_lock()) { status.busy = true; return status; }
        status.configured = owner->flip.configured; status.metered = owner->flip.metered;
        status.refused = owner->flip.refused; status.outputs = owner->flip.outputs;
        status.feedback = owner->flip.feedback; status.status = owner->flip.status; status.calls = owner->flip.calls;
        uint32_t buffers{}, latency{};
        NativeCapacity(*owner, buffers, latency);
        status.nativeBuffers = buffers; status.nativeLatency = latency;
        return status;
    }
    return status;
}

void SetAmdFlipMeter(AmdExtCreateInterfaceFunction create, HMODULE driver, int mode) noexcept
{
    amdDriver.store(driver, std::memory_order_release);
    amdMode.store(create && mode >= 1 && mode <= 2 ? mode : 0, std::memory_order_release);
    amdCreate.store(create, std::memory_order_release);
}

namespace
{
// NativeBuffersScope: copies of factory vtables, each with CreateSwapChainForHwnd (IDXGIFactory2 slot 15) raising the
// buffer count for the scoped thread's provider calls and then calling what the copied table held in that slot (DXGI's
// own table, another image's factory wrapper, or DXGI's table with that slot hooked in place). The copies live for the
// process lifetime and are never rewritten, so a thread that read a swapped pointer can always finish its call through it.
using CreateSwapChainForHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
constexpr size_t kCreateForHwndSlot = 15, kFactoryCopySlots = 64, kFactoryCopies = 8;
struct FactoryCopy
{
    alignas(64) void* table[kFactoryCopySlots+1]{}; // the RTTI locator, then the entries
    void** source{};                                // the table copied (factoryTableMutex); null: free
    size_t entries{};                               // slots copied, the locator included
    std::atomic<CreateSwapChainForHwndFn> original{};
};
std::mutex factoryTableMutex;
FactoryCopy factoryCopies[kFactoryCopies];
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

// The file name (no folder) of the module holding address into out; "" when no module holds it.
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

// wrapbuffers.1: committed, readable image memory (a compiler's vtable in a loaded module), and committed executable code.
bool ImageTableAt(const void* address) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.Type == MEM_IMAGE
        && memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
}
bool CodeAt(const void* address) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT
        && !(memory.Protect & PAGE_GUARD)
        && (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

HRESULT STDMETHODCALLTYPE RaisedCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIOutput* output,
    IDXGISwapChain1** swapchain)
{
    // The copy this object's table pointer was swapped to, and the slot its source held.
    CreateSwapChainForHwndFn original{};
    void* const* const table = self ? *reinterpret_cast<void* const* const*>(self) : nullptr;
    for (const auto& copy : factoryCopies)
        if (table == &copy.table[1]) { original = copy.original.load(std::memory_order_acquire); break; }
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
    // Once per scope, XeFG's native swap chain as created.
    if (created && SUCCEEDED(result) && swapchain && *swapchain)
    {
        tCreated = nullptr;
        created(device, window, swapchain, tCreatedContext);
    }
    return result;
}

// The copy of `vtable`, a table in `owner`'s image: its RTTI locator and entries as far as they lie in that image, slot 15
// ours. Reused while the source holds the same entries; null with `reason` when there is none.
FactoryCopy* AcquireCopy(void** vtable, HMODULE owner, const char*& reason) noexcept
{
    void* entries[kFactoryCopySlots+1]{};
    size_t count = 0;
    for (; count <= kFactoryCopySlots; ++count)
    {
        void* const* at = vtable-1+count;
        if (!CallerIn(owner, at)) break;
        entries[count] = *at;
    }
    if (count <= 1+kCreateForHwndSlot || !entries[1+kCreateForHwndSlot]) { reason = "factory_table"; return nullptr; }
    std::lock_guard lock(factoryTableMutex);
    for (auto& copy : factoryCopies)
    {
        if (copy.source != vtable || copy.entries != count) continue;
        bool same = reinterpret_cast<void*>(copy.original.load(std::memory_order_relaxed)) == entries[1+kCreateForHwndSlot];
        for (size_t i = 0; same && i < count; ++i) same = i == 1+kCreateForHwndSlot || copy.table[i] == entries[i];
        if (same) return &copy;
    }
    for (auto& copy : factoryCopies)
    {
        if (copy.source) continue;
        std::copy_n(entries, count, copy.table);
        copy.original.store(reinterpret_cast<CreateSwapChainForHwndFn>(entries[1+kCreateForHwndSlot]), std::memory_order_release);
        copy.table[1+kCreateForHwndSlot] = reinterpret_cast<void*>(&RaisedCreateSwapChainForHwnd);
        copy.entries = count;
        copy.source = vtable;
        return &copy;
    }
    reason = "factory_copies_full";
    return nullptr;
}
}

NativeBuffersScope::NativeBuffersScope(IDXGIFactory2* factory, HMODULE provider, uint32_t buffers,
    NativeChainCreated created, void* context) noexcept
{
    try
    {
        if (buffers < 2) buffers = 0;
        if (!factory || !provider || (!buffers && !created)) { reason = "not_requested"; return; }
        // OptiScaler's menu draws on the chain created here and holds 8 buffers, 16 with its fix (sdk_pacing.h).
        const uint32_t menuBuffers = optiscaler_host::MenuBackBuffers();
        if (buffers > menuBuffers) buffers = menuBuffers;
        if (!single_overlay::native::PinInterface(factory, kCreateForHwndSlot)) { reason = "factory_interface"; return; }
        auto** vtable = *reinterpret_cast<void***>(factory);
        // Windows' own dxgi.dll's table with its own slot. wrapbuffers.1: also another image's wrapper such as ReShade's
        // (left alone, it keeps XeFG's chain at 3 buffers), or dxgi.dll's table with the slot hooked in place. The copy
        // calls whatever the slot held, as XeFG's own call would, and the wrapper passes the raised count down to DXGI.
        // The table must lie in a loaded image (the copy is bounded by that image) and the slot be committed code.
        HMODULE owner{};
        wchar_t path[MAX_PATH]{}, system[MAX_PATH]{};
        const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
        const bool tableInImage = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(vtable), &owner) && owner;
        const bool tableInDxgi = tableInImage && GetModuleFileNameW(owner, path, MAX_PATH) && systemLength
            && systemLength < MAX_PATH-10 && !wcscat_s(system, L"\\dxgi.dll") && _wcsicmp(path, system) == 0;
        void* const create = vtable[kCreateForHwndSlot];
        const bool dxgiOwn = tableInDxgi && CallerIn(owner, create);
        if (dxgiOwn) via = "dxgi";
        else if (tableInImage && ImageTableAt(vtable) && CodeAt(create)) via = tableInDxgi ? "dxgi_hooked" : "wrapper";
        // Name what holds the table and the slot: the wrapper or hook raised through, or the one that keeps XeFG's chain at
        // the game's buffer count.
        tableSystemDxgi = tableInDxgi;
        if (!dxgiOwn)
        {
            ModuleFileName(vtable, tableModule, std::size(tableModule));
            ModuleFileName(create, createModule, std::size(createModule));
        }
        if (!via) { reason = "factory_not_dxgi"; return; }
        FactoryCopy* const copy = AcquireCopy(vtable, owner, reason);
        if (!copy) { via = nullptr; return; }
        tRaiseBuffers = buffers;
        tRaiseProvider = provider;
        tCreated = created;
        tCreatedContext = context;
        object = reinterpret_cast<void**>(factory);
        swapped = &copy->table[1];
        original = InterlockedCompareExchangePointer(object, swapped, vtable);
        if (original != vtable)
        {
            tRaiseBuffers = 0; tRaiseProvider = nullptr; tCreated = nullptr; tCreatedContext = nullptr;
            object = nullptr; swapped = nullptr; via = nullptr; reason = "factory_swapped";
        }
    }
    catch (...) { via = nullptr; reason = "factory_exception"; }
}

NativeBuffersScope::~NativeBuffersScope()
{
    tRaiseBuffers = 0;
    tRaiseProvider = nullptr;
    tCreated = nullptr;
    tCreatedContext = nullptr;
    if (object) InterlockedCompareExchangePointer(object, original, swapped);
}

void SetFlipQueue(bool enabled) noexcept
{
    queueEnabled.store(enabled, std::memory_order_release);
}

void SetPacingDiagnostics(bool enabled) noexcept
{
    diagEnabled.store(enabled, std::memory_order_release);
}

bool PacingDiagnostics() noexcept
{
    return PacingDiagnosticsWanted();
}

void SetLatencyPacing(uint32_t mode) noexcept
{
    if (mode > latency_pacing::kOff) mode = latency_pacing::kAuto;
    const uint32_t previous = latencyMode.exchange(mode, std::memory_order_acq_rel);
    if (latency_pacing::Enabled(previous) == latency_pacing::Enabled(mode)) return;
    try
    {
        // Switched either way: the next real frames start over (no sleep until the controller relearns the hold).
        {
            std::lock_guard lock(latencyMutex);
            latencyController.Reset();
        }
        xess_mod::WriteLog("latency_pacing_mode", "\"mode\":"+std::to_string(mode)+",\"enabled\":"+
            (latency_pacing::Enabled(mode) ? "true" : "false")+",\"local_time\":\""+LocalClock()+"\"");
    }
    catch (...) {}
}

HostSleepScope::HostSleepScope(HostSleepSource source) noexcept : source_(source)
{
    if (tHostSleepDepth++ != 0) return;
    outer_ = true;
    tHostOwnSleptNs = 0;
    start_ = NowNs();
}

HostSleepScope::~HostSleepScope()
{
    if (tHostSleepDepth) --tHostSleepDepth;
    if (!outer_) return;
    const int64_t own = std::exchange(tHostOwnSleptNs, 0);
    const int64_t end = NowNs();
    if (!start_ || !end || end < start_) return;
    const int64_t slept = end-start_ > own ? end-start_-own : 0;
    // A run of sleeps no feed took (for longer than the evidence lasts, or for kHostSleepBacklog sleeps: no queue is
    // taking them) starts over.
    constexpr uint64_t kHostSleepBacklog = 64;
    const int64_t previous = lastHostSleepEndNs.exchange(end, std::memory_order_acq_rel);
    if (!previous || end-previous > kEvidenceStaleNs
        || pendingHostSleeps.load(std::memory_order_acquire) >= kHostSleepBacklog)
    {
        pendingHostSleptNs.store(0, std::memory_order_release);
        pendingHostSleeps.store(0, std::memory_order_release);
    }
    pendingHostSleptNs.fetch_add(slept, std::memory_order_acq_rel);
    pendingHostSleeps.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t bit = 1u << (uint32_t(source_) & 31u);
    if (!(hostSleepSourcesSeen.fetch_or(bit, std::memory_order_acq_rel) & bit))
        try
        {
            xess_mod::WriteLog("host_sleep_source", std::string("\"source\":\"")+HostSleepSourceName(source_)+
                "\",\"thread\":"+std::to_string(GetCurrentThreadId())+",\"slept_ns\":"+std::to_string(slept)+
                ",\"local_time\":\""+LocalClock()+"\"");
        }
        catch (...) {}
}

void SleepBeforeInput(bool proven) noexcept
{
    try
    {
        const auto now = NowNs();
        if (!now) return;
        // pacerfix.2: the game sleeps at its own XeLL / Reflex point (EndGamePresent).
        if (proven) provenSleepNs.store(now, std::memory_order_release);
        int64_t sleep{};
        if (latency_pacing::Enabled(latencyMode.load(std::memory_order_acquire)))
        {
            // pacerfix.2: bounded by the flip queue's schedule where the standalone host publishes one.
            const int64_t cap = ScheduleCap(now);
            std::lock_guard lock(latencyMutex);
            sleep = latencyController.Take(now, cap);
        }
        if (sleep > 0)
        {
            LatencySleep(sleep);
            // The game waited on the pacing here too: the flip queue's evidence. Inside a host's sleep scope it is ours,
            // not the host's.
            if (const auto slept = NowNs(); slept > now)
            {
                pendingSleptNs.fetch_add(slept-now, std::memory_order_acq_rel);
                if (tHostSleepDepth) tHostOwnSleptNs += slept-now;
            }
        }
        lastSleepEndNs.store(NowNs(), std::memory_order_release);
    }
    catch (...) {}
}

void BeginGamePresent(void* handle) noexcept
{
    auto& present = tGamePresent;
    present.active = false;
    const auto now = NowNs();
    ULONG64 cycles{};
    if (!now || !QueryThreadCycleTime(GetCurrentThread(), &cycles)) return;
    CalibrateTsc(now);
    present = {now, cycles, handle, true};
    gameFeedNs.store(now, std::memory_order_release);
    // pacerfix.2: the Present's number and start for the context's queue, the low-latency sleep before this frame as that
    // queue's evidence (this frame's batch is the one it delayed), and the game's span from its last sleep point to here.
    const uint64_t sequence = gameBeginSeq.fetch_add(1, std::memory_order_acq_rel)+1;
    if (const auto queue = HandleQueue(handle))
    {
        queue->lastBeginSeq.store(sequence, std::memory_order_release);
        queue->lastBeginNs.store(now, std::memory_order_release);
        FeedEvidence(*queue, now, 0);
    }
    const int64_t sleepEnd = lastSleepEndNs.load(std::memory_order_acquire);
    if (sleepEnd && now > sleepEnd && now-sleepEnd < kScheduleStaleNs)
        try
        {
            std::lock_guard lock(latencyMutex);
            spanSamples[spanNext] = now-sleepEnd;
            spanNext = (spanNext+1)%spanSamples.size();
            if (spanCount < spanSamples.size()) ++spanCount;
            scheduleSpanNs.store(spanCount >= 8 ? P90(spanSamples, spanCount) : 0, std::memory_order_relaxed);
        }
        catch (...) {}
}

void EndGamePresent() noexcept
{
    auto& present = tGamePresent;
    if (!present.active) return;
    present.active = false;
    try
    {
        const auto now = NowNs();
        ULONG64 cycles{};
        if (!now || now < present.enteredNs || !QueryThreadCycleTime(GetCurrentThread(), &cycles)) return;
        // The game's frame took its real frame (frame generation on) since the last one: one feed per game frame.
        const uint64_t sequence = pendingSeq.load(std::memory_order_acquire);
        if (consumedSeq.exchange(sequence, std::memory_order_acq_rel) == sequence) return;
        const int64_t own = pendingHoldNs.load(std::memory_order_acquire);
        int64_t waited{};
        if (const double rate = TscPerNs(now); rate > 0.0 && cycles >= present.cycles)
        {
            const int64_t wall = now-present.enteredNs;
            const int64_t cpu = int64_t(double(cycles-present.cycles)/rate);
            waited = wall > cpu ? wall-cpu : 0;
        }
        // pacerfix.2: the flip queue's evidence is the game's whole wait, whatever the low-latency mode.
        if (const auto queue = HandleQueue(present.handle)) FeedEvidence(*queue, now, (std::max)(own, waited));
        if (!latency_pacing::Enabled(latencyMode.load(std::memory_order_acquire))) return;
        // pacerfix.2: the whole wait sizes the sleep only while the game sleeps at its own pre-input point.
        const int64_t proven = provenSleepNs.load(std::memory_order_acquire);
        const bool provenRecent = proven && (now <= proven || now-proven < kProvenSleepStaleNs);
        gameFedFrames.fetch_add(1, std::memory_order_relaxed);
        FeedLatency(now, provenRecent ? (std::max)(own, waited) : own);
    }
    catch (...) {}
}

void HostSleepPoint() noexcept
{
    // The first point a real frame's sleep reaches takes it (latency_pacing Take): a Reflex sleep that reached libxell
    // sleeps at the libxell point, inside OptiScaler's own sleep timing (its Dynamic MFG counts the game's work as the
    // frame less its sleep and Present wait), and the Reflex point after it finds nothing due.
    const uint64_t points = hostSleepPoints.fetch_add(1, std::memory_order_relaxed);
    SleepBeforeInput(true);
    if (!points)
        try
        {
            xess_mod::WriteLog("host_sleep_point", "\"thread\":"+std::to_string(GetCurrentThreadId())+
                ",\"local_time\":\""+LocalClock()+"\"");
        }
        catch (...) {}
}

void HostPresentMarker(bool start, uint32_t frame) noexcept
{
    auto& present = tMarkerPresent;
    const auto now = NowNs();
    ULONG64 cycles{};
    if (start)
    {
        markerStarts.fetch_add(1, std::memory_order_relaxed);
        present.active = false;
        if (!now) return;
        // optispace.1: Wine's QueryThreadCycleTime is a stub (FALSE): the span is still timed, without the CPU time.
        const bool cycleTimed = QueryThreadCycleTime(GetCurrentThread(), &cycles) != FALSE;
        if (cycleTimed) CalibrateTsc(now);
        present = {now, cycleTimed ? cycles : 0, frame, true, cycleTimed};
        return;
    }
    markerEnds.fetch_add(1, std::memory_order_relaxed);
    // An end without its frame's start on this thread (another thread's start, a missed start) times nothing.
    if (!present.active || present.frame != frame) { present.active = false; return; }
    present.active = false;
    try
    {
        if (!now || now < present.enteredNs) return;
        const int64_t wall = now-present.enteredNs;
        int64_t waited{};
        bool spanTimed{};
        if (present.cycleTimed)
        {
            if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return;
            if (const double rate = TscPerNs(now); rate > 0.0 && cycles >= present.cycles)
            {
                const int64_t cpu = int64_t(double(cycles-present.cycles)/rate);
                waited = wall > cpu ? wall-cpu : 0;
            }
        }
        else
        {
            // optispace.1: no CPU time: the wait above the shortest recent span; nothing fed until enough spans are in.
            waited = tMarkerSpans.Add(wall);
            if (waited < 0) return;
            spanTimed = true;
            markerSpanTimed.fetch_add(1, std::memory_order_relaxed);
        }
        markerFeedNs.store(now, std::memory_order_release);
        // The game's frame took its real frame (frame generation on) since the last feed: one feed per game frame.
        const uint64_t sequence = pendingSeq.load(std::memory_order_acquire);
        if (consumedSeq.exchange(sequence, std::memory_order_acq_rel) == sequence) return;
        if (!latency_pacing::Enabled(latencyMode.load(std::memory_order_acquire))) return;
        const int64_t own = pendingHoldNs.load(std::memory_order_acquire);
        // As on the standalone host: the game's wait sizes the sleep only while the game sleeps at a proven pre-input
        // point (HostSleepPoint, or the game's own XeLL sleep through Hook_xellSleep).
        // optispace.1: here it is the game's own wait, per frame pair. XeFG presents the real frame on its presenter
        // thread (0x220130: Pace, then the real Present), so `own` is that thread's hold; the game's Present waits for
        // the previous batch and presents the first generated output (0x220c70); under VKD3D at 3x+ that wait alternates
        // between short and long frames.
        const int64_t proven = provenSleepNs.load(std::memory_order_acquire);
        const bool provenRecent = proven && (now <= proven || now-proven < kProvenSleepStaleNs);
        markerFedFrames.fetch_add(1, std::memory_order_relaxed);
        if (!markerFeedLogged.exchange(true, std::memory_order_acq_rel))
            xess_mod::WriteLog("host_present_markers", "\"thread\":"+std::to_string(GetCurrentThreadId())+
                ",\"frame\":"+std::to_string(frame)+",\"wall_ns\":"+std::to_string(wall)+
                ",\"waited_ns\":"+std::to_string(waited)+",\"own_ns\":"+std::to_string(own)+",\"proven\":"+
                (provenRecent ? "true" : "false")+",\"cpu_time\":\""+(spanTimed ? "shortest_span" : "thread_cycles")+
                "\",\"local_time\":\""+LocalClock()+"\"");
        if (provenRecent) FeedLatency(now, waited, true);
        else FeedLatency(now, own);
    }
    catch (...) {}
}

int64_t VsyncRefreshNs() noexcept
{
    const int64_t seen = vsyncRefreshSeenNs.load(std::memory_order_acquire);
    const int64_t refresh = vsyncRefreshNs.load(std::memory_order_relaxed);
    const int64_t now = NowNs();
    return refresh > 0 && seen && now && (now <= seen || now-seen < kRefreshCheckNs) ? refresh : 0;
}

LatencyPacingSnapshot ReadLatencyPacing() noexcept
{
    LatencyPacingSnapshot snapshot{};
    snapshot.mode = latencyMode.load(std::memory_order_acquire);
    try
    {
        latency_pacing::Stats now{}, then{};
        {
            std::lock_guard lock(latencyMutex);
            now = latencyController.Read();
            then = latencyRead;
            latencyRead = now;
        }
        const uint64_t frames = now.frames-then.frames;
        snapshot.frames = now.frames;
        snapshot.windowFrames = frames;
        snapshot.takes = now.takes-then.takes;
        snapshot.missed = now.missed-then.missed;
        snapshot.backoffs = now.backoffs;
        snapshot.ineffective = now.ineffective;
        snapshot.heldMs = LatencyMs(now.heldSumNs-then.heldSumNs, frames);
        snapshot.slackMs = LatencyMs(now.slackSumNs-then.slackSumNs, frames);
        snapshot.sleptMs = LatencyMs(now.sleptSumNs-then.sleptSumNs, frames);
        const auto clock = NowNs();
        snapshot.ageMs = now.lastFrameNs && clock > now.lastFrameNs ? (clock-now.lastFrameNs)/1000000 : -1;
    }
    catch (...) {}
    return snapshot;
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
