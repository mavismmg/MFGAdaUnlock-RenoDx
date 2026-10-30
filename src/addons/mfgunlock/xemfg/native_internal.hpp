// SPDX-License-Identifier: MIT
#pragma once
#include "native_bridge.hpp"
#include "native_controls.hpp"
#include "frame_store.hpp"
#include "native_diagnostics.hpp"
#include "native_trace.hpp"
#include "hud_inputs.hpp"
#include "native_reframework.hpp"
#include "compatibility_policy.hpp"
#include "native_session.hpp"
#include "native_dejitter.hpp"
#include "native_ui_mask.hpp"
#include "native_scrgb.hpp"
#include "native_hud_copy.hpp"
#include "display_cap.hpp"
#include "vendor/port/port_support.h"
#include "vendor/overlay_native.h"
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace mfgunlock::xemfg::native {
struct Api {
  HMODULE fgModule{},llModule{};
#define F(n) decltype(&n) n{};
  F(xefgSwapChainD3D12CreateContext) F(xefgSwapChainD3D12InitFromSwapChainDesc)
  F(xefgSwapChainD3D12GetSwapChainPtr) F(xefgSwapChainD3D12TagFrameResource)
  F(xefgSwapChainTagFrameConstants) F(xefgSwapChainSetPresentId)
  F(xefgSwapChainSetEnabled) F(xefgSwapChainSetNumInterpolatedFrames)
  F(xefgSwapChainSetLatencyReduction) F(xefgSwapChainGetLastPresentStatus)
  F(xefgSwapChainDestroy) F(xefgSwapChainSetUiCompositionState)
  F(xefgSwapChainSetLoggingCallback)
  F(xellD3D12CreateContext) F(xellSetSleepMode) F(xellSleep)
  F(xellAddMarkerData) F(xellDestroyContext)
  // Optional diagnostics; their absence never blocks an output.
  F(xellSetLoggingCallback) F(xellGetVersion)
#undef F
  bool Load();
};
struct Frame {
  struct ResourceReceipt {
    uint32_t width{},height{},format{},incomingState{},lifecycle{};
    int sdkResult{};
    const char* rejection{}; // static reason, never an object name or address
    bool observed{},accepted{};
  } resourceReceipts[4];
  compatibility::Failure firstFailure{compatibility::Failure::None};
  uint32_t rejectedMarker{UINT32_MAX}, markerObservedBeforeSleep{};
  int markerSdkResult{};
  uint64_t sleepUs{};
  uint32_t id{}, viewport{}, resources{}, markers{}, observedMarkers{}, sourceSleepTarget{};
  bool sourceSleepResolved{};
  bool constants{}, valid{}, sleeping{}, sleepInFlight{}, failed{};
  uint32_t motionWidth{}, motionHeight{};
  // Jittered outputs (plan 4.2): the motion input converted before Present,
  // either the game resource (until-present tag) or a producer-list copy.
  ComPtr<ID3D12Resource> motionSource;
  D3D12_RESOURCE_STATES motionState{};
  DejitterRegion motionRegion{};
  // Plan 4.3: the game's kBufferTypeBackbuffer extent; empty means the whole buffer.
  DejitterRegion backbufferRegion{};
  uint32_t hudIssues{};
  // 1.5.7: optional HUD kinds (1u<<xefg type) filtered or tagged through a copy.
  uint32_t hudFiltered{}, hudCopied{};
  HudTagIdentity hudTags[2]{};
  // Pin the allocation identity until frame retirement, never infer its
  // generation from a reused address. Contents still obey the SDK lifecycle.
  ComPtr<IUnknown> hudAllocations[2];
  // 1.5.7: HUD inputs valid until Present whose type differs from the output
  // format (CopyableHudFormat). Copied and tagged on the presentation queue
  // at the source Present. Index 0 HUD-less, 1 UI.
  struct PendingHud {
    ComPtr<ID3D12Resource> source;
    D3D12_RESOURCE_STATES state{};
    DejitterRegion region{};
    xefg_swapchain_resource_type_t type{};
    bool encode{}; // FP16 scRGB HUD-less encoded to HDR10 like the backbuffer
  } pendingHud[2];
  // 1.7 (plan E.2): the game's HUD-less color, held back from XeFG while HUD
  // stabilization runs; it tells the mask exactly where HUD is drawn.
  PendingHud heldHudless;
  uint32_t constantIssues{};
  LatencyOutput latencyOutput{LatencyOutput::XeLL};
  xefg_swapchain_frame_constant_data_t data{};
};
struct Output {
  HWND hwnd{};
  std::recursive_mutex io;
  std::recursive_mutex presents;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGIFactory2> factory;
  std::shared_ptr<WindowControl> window;
  std::unique_ptr<QueueTrace> gpuTrace;
  DWORD windowBindError{};
  xefg_swapchain_handle_t fg{};
  xell_context_handle_t ll{};
  FrameStore<Frame> frames;
  uint32_t last{}, appliedCount{}, width{}, height{};
  uint32_t lastAdmission{UINT32_MAX};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
  uint32_t hudLogBudget{},hudKindsSeen{};
  uint32_t rejectedTagLogBudget{};
  // Tags the game removes when it pauses FG are routine; their own budget
  // keeps rejectedTagLogBudget for real refusals.
  uint32_t removedTagLogBudget{};
  // 1.5.5: periodic native_summary (also a heartbeat while this output
  // presents). Each field holds the totals at the previous summary.
  uint64_t summaryTick{}, summaryFrames{}, summaryRejectedFrames{}, summarySleeps{};
  uint64_t summaryTagsRejected{}, summaryTagsRemoved{}, summaryReservedStates{}, summaryHudFiltered{};
  // 1.7.1: where a source frame's time goes, averaged per native_summary (latency work).
  std::atomic<uint64_t> summaryPresentUs{}, summaryPresentCalls{};
  uint64_t summarySleepUs{}, summarySleepCalls{};
  uint32_t sleepAssociationLogBudget{};
  uint32_t unsleptSimulationStarts{};
  uint32_t sleepRejectLogBudget{},boundarySkipLogBudget{};
  // Diagnostics only: every tag call, including unselected viewports and
  // buffer types the bridge does not map. Index 63 collects larger types.
  uint64_t tagTypeCounts[64]{}, taggedCalls{}, otherViewportTags{};
  struct ViewportCount { uint32_t viewport; uint64_t count; } tagViewports[8]{};
  bool constantsSampled{}, histogramLogged{};
  uint64_t rejectedExplicitSleeps{};
  uint32_t lastXellSleep{}; // newest id dispatched to xellSleep for this context
  bool explicitSleepObserved{};
  uint32_t markerRejectLogBudget[3]{};
  uint32_t presentFailureLogBudget{};
  uint32_t constantLogBudget{},lastConstantIssues{UINT32_MAX};
  uint32_t bypassPresentLogBudget{};
  uint64_t admissionTick{};
  uint64_t epoch{};
  uint64_t diagnosticOutput{};
  uint32_t sleepInterval{};
  bool sleepConfigured{}, sleepActive{},sleepLowLatency{true};
  // XeFG initializes with composition disabled. A live preference is applied
  // only at the source Present boundary, with this frame's HUD-less input.
  bool uiCompositionKnown{true}, uiCompositionApplied{}, uiCompositionAvailable{};
  int uiCompositionResult{};
  uint64_t uiCompositionUpdates{};
  bool invert{}, jitter{}, enabled{}, compatible{true},retired{};
  // Present when the game's vectors are jittered; XeFG then runs unjittered.
  std::unique_ptr<MotionDejitter> dejitter;
  // 1.5.7: owned copies of HUD inputs in the output format; created on first use.
  std::unique_ptr<HudCopy> hudCopy;
  uint32_t hudCopyLogBudget{};
  // 1.7 (plan E.2): HUD stabilization; created on first use.
  std::unique_ptr<UiMask> uiMask;
  bool uiStabilized{}, uiMaskFailed{};
  uint32_t uiMaskLogBudget{};
  uint64_t refreshTick{};
  bool displayCapLogged{};
  int64_t lastSourcePresentQpc{};
  DejitterVector lastJitter{};
  uint32_t lastJitterId{}, dejitterLogBudget{};
  uint32_t backbufferLogBudget{};
  bool viewportReset{}; // the selected viewport changed; the next admitted frame resets history
  uint32_t duplicateTagLogBudget{};
  uint32_t reservedStateLogBudget{};
  DejitterRegion backbufferRegion{}; // last region the game declared; HUD inputs match it
  // Plan 4.4: the game's FP16 scRGB view over the Intel HDR10 proxy. The game
  // renders into scrgbBuffers; Present encodes them into Intel's buffers.
  bool scrgb{};
  std::unique_ptr<ScrgbEncoder> scrgbEncoder;
  std::vector<ComPtr<ID3D12Resource>> scrgbBuffers;
  uint32_t scrgbLogBudget{};
  bool deviceLost{};
  bool presentationSupported{true};
  // 1.5.8: the game asked for exclusive fullscreen and is served borderless
  // instead; it is told it is in fullscreen (on exclusiveTarget, when given).
  // The Intel chain stays windowed. exclusiveTarget is guarded by io.
  std::atomic<bool> exclusiveServed{};
  ComPtr<IDXGIOutput> exclusiveTarget;
  HRESULT presentFailure{S_OK},removedReason{S_OK};
  HRESULT dredResult{E_NOINTERFACE};
  uint32_t dredNodes{},dredUnfinished{};
  compatibility::Failure blocker{compatibility::Failure::NoFrame};
  std::shared_ptr<reframework::Peer> reframeworkPeer;
  ComPtr<IDXGISwapChain4> quarantinedProxy;
  // The application proxy owns presentation. Shared operation leases may keep
  // XeLL alive after this method has released the window/Intel FG context.
  void RetirePresentation(ComPtr<IDXGISwapChain4> publicProxy={});
  ~Output();
};
struct Bridge {
  std::recursive_mutex lock;
  Api api;
  Status status;
  std::weak_ptr<Output> output;
  std::atomic<uint32_t> recording{0}, presenting{0}, viewport{UINT32_MAX};
  std::atomic<bool> nativeOff{false}, reflexOff{false}, requested{false};
  std::atomic<bool> retirementFailed{false};
  std::atomic<bool> reflexOptionsReady{false};
  // Output changes are saved by the UI and take effect only after restart.
  // This avoids replaying a game's Reflex setter on an arbitrary UI thread.
  std::atomic<LatencyOutput> latencyOutput{LatencyOutput::XeLL};
  std::atomic<uint64_t> reflexOutputTick{0},reflexSleeps{0},xellSleeps{0};
  std::atomic<uint32_t> multiplier{4}, limitUs{0};
  std::atomic<uint64_t> outputPresents{0};
  std::atomic<bool> hasOutput{false}; // lock-free view of `output` for the output-rate Present event
  std::atomic<uint64_t> latencySleeps{0}, latencyMarkers{0}, inputTick{0}, outputTick{0};
  // Last Streamline latency marker (GetTickCount64) and whether the game's
  // selected viewport requests DLSS-G. Both together mean the Streamline route
  // owns the frames, so synthesized frames stay passive (1.5.1). Markers alone
  // are only Reflex: FSR frame generation stays available.
  std::atomic<uint64_t> streamlineMarkerTick{0};
  std::atomic<bool> streamlineGeneration{false};
  std::atomic<uint64_t> latencyModeUpdates{0}, lastSleepUs{0};
  std::atomic<uint32_t> gameReflexMode{0};
  // Sync interval of the game's last Present, before ForceVSyncOff; UINT32_MAX until one is seen.
  std::atomic<uint32_t> gameSyncInterval{UINT32_MAX};
  // 1.5.7: refresh of the output's display (mHz, refreshed every 2 s) and the
  // smoothed interval between source Presents, for the display cap and notice.
  std::atomic<uint32_t> displayRefreshMilliHz{0}, sourceIntervalUs{0};
  std::atomic<bool> displayCap{false}; // [XeFG] DisplayCap, set by the controls
  // 1.5.8: exclusive fullscreen requests served as borderless (creation and runtime).
  std::atomic<uint64_t> exclusiveRequests{0};
  // Totals for native_summary; the per-event logs keep only their first entries.
  std::atomic<uint64_t> tagsRejected{0},tagsRemoved{0},reservedStates{0};
  std::atomic<uint32_t> summaryIntervalMs{10000};
  std::atomic<uint32_t> lastHudIssues{0},lastHudInputs{0},lastHudFiltered{0},lastHudCopied{0};
  std::atomic<uint64_t> hudRejected{0};
  bool lowLatency{true}, invert{true}, jitter{false};
  // Plan 3.3: conventions set explicitly in the INI are never relearned.
  bool explicitInvert{}, explicitJitter{};
  std::wstring executableIdentity; // name|version|size, keys [XeFG.Learned]
  std::atomic<uint32_t> hookCapabilities{0};
  std::atomic<uint64_t> staleInputs{0},externalResolutions{0};
};
Bridge& State();
// 1.5.7: minimum source interval of the display cap for the XeLL output, or 0.
inline uint32_t DisplayCapIntervalForOutput() noexcept {
  auto& s=State();
  if(!s.displayCap.load(std::memory_order_relaxed) || s.latencyOutput.load()!=LatencyOutput::XeLL) return 0;
  return DisplayCapIntervalUs(s.displayRefreshMilliHz.load(),s.multiplier.load());
}
inline uint64_t QpcMicroseconds(int64_t ticks) noexcept {
  static const int64_t frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart?f.QuadPart:1; }();
  return uint64_t(ticks)*1000000/uint64_t(frequency);
}
// Refresh of the display showing the window (mHz), 0 when unknown.
uint32_t WindowRefreshMilliHz(HWND window) noexcept;
inline Frame* FindFrame(Output& o,uint32_t id) {
  return o.frames.Acquire(id);
}
sl::Result SleepSourceFrame(const std::shared_ptr<Output>& output,uint32_t id,
    PFun_slReflexSleep* nativeSleep=nullptr,const sl::FrameToken* token=nullptr,
    std::optional<uint64_t> expectedEpoch={});
bool ForwardSourceMarker(const std::shared_ptr<Output>& output,uint32_t id,uint32_t marker);
// Caller holds output.io.
void LogTagHistogram(Output& output,const char* phase);
bool ApplyUiComposition(Output& output,bool hudlessAvailable,bool stabilized=false);
// 1.7 (plan E.2): records the HUD stabilization pass for `id` and tags its
// HUD-less and UI outputs. False leaves the frame without them.
bool StabilizeHud(Output& output,Frame& frame,uint32_t id);
void Report(const char* reason);
bool Hook(void* entry,void* replacement,void** trampoline);
void DiscoverStreamline();
// Plan 3.3 learning for conventions an input adapter observed (plan 5.3).
void LearnStatedConventions(std::optional<bool> invert,std::optional<bool> jitter);
std::string CompatibilityReport(const char* exportReason=nullptr);
// Writes MFGAdaUnlock-XeMFG-compatibility.json beside the game. "shutdown"
// runs synchronously; other reasons use a coalesced worker thread.
void ExportCompatibilityReport(const char* reason) noexcept;
void RecordDeviceFailure(Output&,HRESULT);
// [Diagnostics] ForceDRED / D3D12DebugLayer: opt-in local diagnostics that
// change device creation; the failure log then names DRED lists and ops.
bool D3D12DiagnosticsForced();
std::shared_ptr<Output> Active();
void BeforePresent(const std::shared_ptr<Output>& o);
void AfterPresent(const std::shared_ptr<Output>& o,CpuProbe* probe=nullptr,HRESULT result=S_OK);
HRESULT CreateOutput(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
IDXGISwapChain1* Wrap(ComPtr<IDXGISwapChain4> chain,std::shared_ptr<Output> owner);
// Plan 4.4: creates the encoder, the game's FP16 buffers and the HDR10 color space.
bool PrepareScrgb(Output& o,IDXGISwapChain4* chain);
// Token zero maps to one. Used consistently by inputs, markers and Present.
inline uint32_t FrameId(uint32_t token) { return token==UINT32_MAX?0:token+1; }
}
