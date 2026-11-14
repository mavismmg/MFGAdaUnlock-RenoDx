#pragma once
// XeMFG standalone. RTXMFG is the XeFG host without OptiScaler: on D3D12 it owns the game's swap
// chain (xess/d3d12_host.h), on Vulkan the existing takeover (xess/vulkan/vk_host.h) runs behind the same switch.
// Its inputs come from Streamline's DLSS Frame Generation tags and constants or, in games without them, from the
// upscaler the game evaluates (DLSS Super Resolution, XeSS, FSR 3.1+). The game's own XeSS FG tags are never used.
//
// The decisions below need no graphics API: which source a frame takes, the frame ids XeFG and XeLL see, and how
// each source's conventions become XeFG's constants. Header only, so tests/xemfg_standalone checks them on the CPU.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace xemfg_standalone
{
// The input sources, best first.
enum class Source : uint32_t { eNone = 0, eStreamline = 1, eDlss = 2, eXess = 3, eFsr = 4 };

inline const char* SourceName(Source source) noexcept
{
    switch (source) {
    case Source::eStreamline: return "streamline";
    case Source::eDlss: return "dlss";
    case Source::eXess: return "xess";
    case Source::eFsr: return "fsr";
    default: return "none";
    }
}

// Streamline's tags keep the frames to themselves for this long after their last depth and motion vectors: the
// upscaler behind them (sl.dlss evaluating DLSS through NGX) carries the same images under another frame id.
constexpr uint64_t kStreamlineHoldMs = 1000;

inline bool SourceAdmitted(Source source, uint64_t nowMs, uint64_t streamlineMs) noexcept
{
    if (source == Source::eStreamline) return true;
    if (source == Source::eNone) return false;
    return !streamlineMs || nowMs < streamlineMs || nowMs - streamlineMs >= kStreamlineHoldMs;
}

// The ids XeFG (presentId) and XeLL (frame_id) see: one increasing sequence per process. Upscaler frames take the
// next id after the last presented one. Streamline frames keep their token's spacing, shifted by an offset fixed
// when the token sequence starts (or starts again), so ids never go back when the source changes.
class FrameIds
{
public:
    // A token this far behind the last presented id restarted the game's counter.
    static constexpr uint32_t kRestartGap = 64;

    uint32_t Last() const noexcept { return last_; }
    bool Tokens() const noexcept { return tokens_; }
    // The token -> id offset (for the log).
    int64_t Offset() const noexcept { return offset_; }
    // The frame an upscaler evaluation belongs to: the one after the last present.
    uint32_t Next() const noexcept { return last_ + 1; }

    // The id of a Streamline frame token; 0 for a token whose frame was already presented.
    uint32_t FromToken(uint32_t token) noexcept
    {
        if (!tokens_) {
            tokens_ = true;
            offset_ = int64_t(last_) + 1 - int64_t(token);
        }
        int64_t id = int64_t(token) + offset_;
        if (id + kRestartGap <= int64_t(last_) || id > int64_t(last_) + 0x100000) {
            offset_ = int64_t(last_) + 1 - int64_t(token);
            id = int64_t(last_) + 1;
        }
        return id > int64_t(last_) && id <= int64_t(UINT32_MAX) ? uint32_t(id) : 0;
    }
    // idanchor.1: a Streamline tag or constant of this token mapped to a frame already presented (its inputs were lost).
    void Refused(uint32_t token) noexcept
    {
        if (!refused_ || int32_t(token - refusedToken_) > 0) refusedToken_ = token;
        refused_ = true;
    }
    // idanchor.1: the id of the frame the game's present-start marker names, the frame presented now. A token newer
    // than the last such marker's, whose id was already presented and whose inputs were refused, means the offset is
    // off (fixed by a token the render thread started ahead of the present, or before a hitch's fills): every present
    // would take the next id and every Streamline input arrive after its id was presented. The map anchors again on
    // this frame. A marker sent after its present (its frame's inputs taken) anchors nothing.
    uint32_t FromPresentToken(uint32_t token) noexcept
    {
        uint32_t id = FromToken(token);
        const bool newer = !presentToken_ || int32_t(token - presentToken_) > 0;
        if (!id && newer && refused_ && int32_t(refusedToken_ - token) >= 0 && last_ < UINT32_MAX) {
            offset_ = int64_t(last_) + 1 - int64_t(token);
            id = last_ + 1;
            ++anchors_;
        }
        if (id && newer) presentToken_ = token;
        return id;
    }
    // How often FromPresentToken anchored the map again (for the log).
    uint64_t Anchors() const noexcept { return anchors_; }
    // The upscaler path took over again (no Streamline frames any more): tokens map afresh when they come back.
    void LeaveTokens() noexcept { tokens_ = false; }
    // XeFG presented this id. Ids only increase.
    void Presented(uint32_t id) noexcept { if (id > last_) last_ = id; }

private:
    uint32_t last_ = 0;
    bool tokens_ = false;
    int64_t offset_ = 0;
    uint32_t presentToken_ = 0, refusedToken_ = 0;
    bool refused_ = false;
    uint64_t anchors_ = 0;
};

// The id the present belongs to: the frame the game's present-start marker named, else the oldest frame with
// inputs not yet presented, else the next one.
inline uint32_t PresentId(uint32_t lastPresented, uint32_t markerId, const uint32_t* pending, size_t count) noexcept
{
    if (markerId > lastPresented) return markerId;
    uint32_t oldest = 0;
    for (size_t i = 0; i < count; ++i)
        if (pending[i] > lastPresented && (!oldest || pending[i] < oldest)) oldest = pending[i];
    return oldest ? oldest : lastPresented + 1;
}

// What a frame's inputs said about it, by any source.
struct FrameRecord
{
    uint32_t id = 0;
    Source source = Source::eNone;
    bool depth = false, motion = false;
    uint32_t depthWidth = 0, depthHeight = 0;
    uint32_t motionWidth = 0, motionHeight = 0;
    float jitterX = 0, jitterY = 0;
    float motionScaleX = 1, motionScaleY = 1;   // motion vectors times these are render-resolution pixels
    bool motionNormalized = false;              // Streamline's mvecScale (StreamlineMotionScale at present)
    bool reset = false;
    bool depthInvertedKnown = false, depthInverted = false;
    float cameraNear = 0, cameraFar = 0, cameraVerticalFov = 0;
    float frameTimeMs = 0;
};

// A small ring of the frames in flight (tagged, not yet presented).
template<size_t N> class FrameRing
{
public:
    FrameRecord* Find(uint32_t id) noexcept
    {
        for (auto& record : records_) if (id && record.id == id) return &record;
        return nullptr;
    }
    // The record for id, reusing the oldest slot.
    FrameRecord& At(uint32_t id) noexcept
    {
        if (auto* found = Find(id)) return *found;
        FrameRecord* oldest = &records_[0];
        for (auto& record : records_) if (record.id < oldest->id) oldest = &record;
        *oldest = FrameRecord{};
        oldest->id = id;
        return *oldest;
    }
    size_t Pending(uint32_t lastPresented, uint32_t* ids, size_t capacity) const noexcept
    {
        size_t count = 0;
        for (const auto& record : records_)
            if (record.id > lastPresented && record.depth && record.motion && count < capacity) ids[count++] = record.id;
        return count;
    }
    void Clear() noexcept { records_ = {}; }
private:
    std::array<FrameRecord, N> records_{};
};

// The first upscaler to deliver a frame's depth and motion vectors keeps that frame; a second one (the FidelityFX
// loader and its upscaler module, or another evaluation of the same frame) is skipped. Streamline's DLSS-G inputs
// outrank an upscaler's for the same frame: they carry the game's HUD-less image and UI, and in an engine
// that renders a frame while it simulates the next (Unreal), the upscaler's evaluation can reach a frame id first.
inline bool Claim(FrameRecord& record, Source source) noexcept
{
    if (source == Source::eStreamline) { record.source = source; return true; }
    if (record.source != Source::eNone && record.source != source) return false;
    record.source = source;
    return true;
}

// tagframe.1: the Streamline frame token a tag call without one (the legacy slSetTag) belongs to. Such tags are set
// while the game renders a frame: the token its latest render-submit-start marker named, while those markers are
// recent; else the latest token seen (constants and markers of any kind). The latest token alone puts every other
// frame's inputs on its successor in an engine that names the next frame before it tags the current one (Cyberpunk
// 2077: XeFG then generates on alternate frames only).
inline uint32_t LegacyTagToken(uint32_t latestToken, uint32_t renderToken, bool renderMarkersRecent) noexcept
{
    return renderMarkersRecent && renderToken ? renderToken : latestToken;
}

// Streamline's resource lifecycles (sl::ResourceLifecycle) as XeFG validity. A resource valid only now is copied
// by XeFG on the command list it was tagged on; without a list it cannot be used.
enum class Validity { eSkip, eUntilPresent, eOnlyNow };

inline Validity StreamlineValidity(uint32_t lifecycle, bool hasList) noexcept
{
    switch (lifecycle) {
    case 0: return hasList ? Validity::eOnlyNow : Validity::eSkip;          // eOnlyValidNow
    case 1: return Validity::eUntilPresent;                                   // eValidUntilPresent
    case 2: return hasList ? Validity::eOnlyNow : Validity::eSkip;          // eValidUntilEvaluate
    default: return Validity::eSkip;
    }
}

// A tag's extent (left, top, width, height; empty = the whole resource) inside the resource.
struct Area { uint32_t x = 0, y = 0, width = 0, height = 0; };

inline Area TagArea(uint32_t left, uint32_t top, uint32_t width, uint32_t height, uint32_t resourceWidth,
    uint32_t resourceHeight) noexcept
{
    Area area;
    if (!width || !height) { area.width = resourceWidth; area.height = resourceHeight; return area; }
    area.x = std::min(left, resourceWidth);
    area.y = std::min(top, resourceHeight);
    area.width = std::min(width, resourceWidth - area.x);
    area.height = std::min(height, resourceHeight - area.y);
    return area;
}

// Streamline's mvecScale normalizes motion vectors (to [-1, 1] of the motion vectors' extent); XeFG's scale makes
// pixels of that extent.
inline std::pair<float, float> StreamlineMotionScale(float mvecScaleX, float mvecScaleY, uint32_t width, uint32_t height) noexcept
{
    return {mvecScaleX * float(width), mvecScaleY * float(height)};
}

// NGX DLSS-G's DLSSG.MvecScaleX/Y, unlike Streamline's mvecScale, is already a pixel scale of the motion vectors'
// extent: sl.dlss_g hands NGX its normalized mvecScale times that extent (1.0 for pixel motion vectors, the extent
// for normalized ones), and OptiScaler's own FFX provider passes it straight to FFX's motionVectorScale. XeFG wants
// pixels of the copy it reads (target), so only the copy's resampling applies. An empty extent keeps the scale.
inline std::pair<float, float> NgxDlssgMotionScale(float mvecScaleX, float mvecScaleY, uint32_t motionWidth, uint32_t motionHeight,
                                                   uint32_t targetWidth, uint32_t targetHeight) noexcept
{
    return {motionWidth ? mvecScaleX * float(targetWidth) / float(motionWidth) : mvecScaleX,
            motionHeight ? mvecScaleY * float(targetHeight) / float(motionHeight) : mvecScaleY};
}

// XeSS's velocity: pixels times its velocity scale, or NDC (USE_NDC_VELOCITY: [-1, 1], y up) times the scale.
inline std::pair<float, float> XessMotionScale(float scaleX, float scaleY, bool ndc, uint32_t width, uint32_t height) noexcept
{
    if (!ndc) return {scaleX, scaleY};
    return {scaleX * 0.5f * float(width), -scaleY * 0.5f * float(height)};
}

// D3D12_RESOURCE_STATES of the FidelityFX API's resource states (FfxApiResourceStates), as its D3D12 backend maps
// them. false for a value outside that set.
inline bool FfxStateToD3d12(uint32_t ffx, uint32_t& d3d12) noexcept
{
    switch (ffx) {
    case 1u << 0: d3d12 = 0x0; return true;                  // COMMON -> COMMON
    case 1u << 1: d3d12 = 0x8; return true;                  // UNORDERED_ACCESS
    case 1u << 2: d3d12 = 0x40; return true;                 // COMPUTE_READ -> NON_PIXEL_SHADER_RESOURCE
    case 1u << 3: d3d12 = 0x80; return true;                 // PIXEL_READ -> PIXEL_SHADER_RESOURCE
    case (1u << 2) | (1u << 3): d3d12 = 0xC0; return true;   // PIXEL_COMPUTE_READ
    case 1u << 4: d3d12 = 0x800; return true;                // COPY_SRC -> COPY_SOURCE
    case 1u << 5: d3d12 = 0x400; return true;                // COPY_DEST
    case (1u << 4) | (1u << 2): d3d12 = 0xAC3; return true;  // GENERIC_READ
    case 1u << 6: d3d12 = 0x200; return true;                // INDIRECT_ARGUMENT
    case 1u << 7: d3d12 = 0x0; return true;                  // PRESENT
    case 1u << 8: d3d12 = 0x4; return true;                  // RENDER_TARGET
    default: return false;
    }
}

// XeFG's camera from inputs without matrices (all of them here): identity view and a left-handed perspective
// projection over the output aspect, as xess/vulkan/vk_host.cpp builds it. The source's camera when it has one,
// else 60 degrees vertical, near 0.1, far 10000; near and far swapped for inverted depth, an infinite plane taken to
// its limit.
inline void Projection(float aspect, float cameraNear, float cameraFar, float verticalFov, bool depthInverted, float out[16]) noexcept
{
    const bool known = cameraNear > 0 && cameraFar > 0 && verticalFov > 0.0001f && verticalFov < 3.13f && cameraNear != cameraFar;
    float nearPlane = known ? cameraNear : 0.1f, farPlane = known ? cameraFar : 10000.0f;
    if (depthInverted) std::swap(nearPlane, farPlane);
    const float y = 1.0f / std::tan(0.5f * (known ? verticalFov : 1.0471976f));
    const float x = y / (aspect > 0 ? aspect : 1.0f);
    float depth = 0, offset = 0;
    if (std::isinf(farPlane)) { depth = 1.0f; offset = -nearPlane; }
    else if (std::isinf(nearPlane)) { depth = 0.0f; offset = farPlane; }
    else { depth = farPlane / (farPlane - nearPlane); offset = -nearPlane * depth; }
    const float m[16] = {x, 0, 0, 0, 0, y, 0, 0, 0, 0, depth, 1, 0, 0, offset, 0};
    std::memcpy(out, m, sizeof(m));
}

// Why a frame is presented without generated frames, or nullptr when XeFG generates for it.
struct GenerationInputs
{
    bool enabled = false;          // the menu's switch
    bool depth = false, motion = false;
    bool sizesMatch = true;        // depth and motion vectors at one size (XeFG reads them at one size)
    bool dlssgSeen = false;        // the game drives Streamline's DLSS Frame Generation
    bool gameFrameGeneration = false;   // and has it on (XeMFG replaces it)
    bool fsrFrameGeneration = false;    // the game's FSR frame generation is on (never stacked)
};

inline const char* WhyNotGenerating(const GenerationInputs& in) noexcept
{
    if (!in.enabled) return "off";
    if (in.fsrFrameGeneration) return "game_fsr_frame_generation";
    if (in.dlssgSeen && !in.gameFrameGeneration) return "game_frame_generation_off";
    if (!in.depth || !in.motion) return "no_inputs";
    if (!in.sizesMatch) return "motion_vector_size";
    return nullptr;
}

// The menu's FPS row and pinned readout for XeMFG. The game's frames (its presents on the host's chain) and
// XeFG's output (every frame it presents) over a window of at least kWindowMs, sampled on a base present. Counters that
// go back (a new chain) restart the window.
struct FpsWindow
{
    static constexpr uint64_t kWindowMs = 1000;
    uint64_t startMs = 0, startBase = 0, startOutput = 0, sampleMs = 0;
    uint32_t baseMilli = 0, outputMilli = 0;

    void Add(uint64_t nowMs, uint64_t base, uint64_t output) noexcept
    {
        if (!startMs || nowMs < startMs || base < startBase || output < startOutput) {
            startMs = nowMs; startBase = base; startOutput = output;
            return;
        }
        const uint64_t elapsed = nowMs - startMs;
        if (elapsed < kWindowMs) return;
        const auto rate = [elapsed](uint64_t frames) {
            return uint32_t((std::min<uint64_t>)(frames * 1000000ull / elapsed, UINT32_MAX));
        };
        baseMilli = rate(base - startBase);
        outputMilli = rate(output - startOutput);
        sampleMs = nowMs;
        startMs = nowMs; startBase = base; startOutput = output;
    }
};

// The menu's FPS limit (presented frames per second, as for DLSS-G) as XeLL's minimum interval. XeLL counts
// the frames XeFG presents (it paces the game's frame to the interval times XeFG's count), so the interval is the
// presented frames' own: in the takeover harness 66,667 us at 4x held the game at 3.7 FPS and XeFG at 14.8, not 60.
// The driver's Reflex (vk_host.cpp) knows no generated frames and takes the interval times the count. 0 = no limit.
inline uint32_t LimitIntervalUs(uint32_t outputFps) noexcept
{
    if (!outputFps) return 0;
    return uint32_t((std::min)(1e6 / double(outputFps), 1e7) + 0.5);
}

// pacerfix.2: the output rate both standalone hosts hold while the flip queue presents with V-Sync and neither the menu's
// FPS limit nor Dynamic MFG's cap is set (sdk_pacing::VsyncRefreshNs): just under the refresh, as NVIDIA's Reflex limit
// under V-Sync + G-SYNC does (refresh - refresh^2 / 3600: 60 Hz 59, 120 Hz 116, 144 Hz 138), whole FPS rounded down, so
// the game does not run ahead of the vblanks and fill the queue (0.01 FPS of slack absorbs the period's nanosecond
// rounding: 16,666,667 ns is 59.9999988 Hz). 0 (no cap) without a refresh period or outside 23-1000 Hz.
inline uint32_t AutoCapFps(int64_t refreshNs) noexcept
{
    if (refreshNs <= 0) return 0;
    const double hz = 1e9 / double(refreshNs);
    if (hz < 23.0 || hz > 1000.0) return 0;
    const double cap = hz - hz * hz / 3600.0 + 0.01;
    return cap >= 1.0 ? uint32_t(cap) : 0;
}

// sapacing.1: whether a device's XeFG owners get the flip queue (xess_mod.cpp FlipQueueEnabled, which reads the
// inputs). An OptiScaler.ini that sets XeFGFlipQueue decides on every device; on either standalone host the control key
// xemfgFlipQueue then decides when on (1) or off (2); otherwise auto, which is on everywhere but Wine/Proton
// (winequeue.1: there XeFG's presenter is the limit and the queue's holds only add uneven waits to a serial present
// queue). The ini and the key stay as the escape hatch (off: XeFG's own pacing). setting: what flip_queue_config logs,
// nullptr to keep ReadXefgFlipQueue's.
enum class QueueHost : uint32_t { eOther = 0, eD3d12 = 1, eVulkan = 2 };
struct FlipQueueInputs
{
    bool iniSet = false;   // OptiScaler.ini [XeFG] XeFGFlipQueue present, not empty, not auto
    bool iniOn = true;     // ReadXefgFlipQueue: the ini's value when set, else on
    QueueHost host = QueueHost::eOther;
    uint32_t key = 0;      // xemfgFlipQueue: 0 auto, 1 on, 2 off
    bool wine = false;     // optiscaler_host::RunningUnderWine
};
struct FlipQueueChoice
{
    bool enabled;
    const wchar_t* setting;
};
inline FlipQueueChoice ChooseFlipQueue(const FlipQueueInputs& in) noexcept
{
    if (in.iniSet) return {in.iniOn, nullptr};
    if (in.host != QueueHost::eOther && in.key == 1) return {true, L"on (xemfgFlipQueue)"};
    if (in.host != QueueHost::eOther && in.key == 2) return {false, L"off (xemfgFlipQueue)"};
    if (!in.iniOn) return {false, nullptr};
    if (in.wine) return {false, L"auto (Wine/Proton)"};
    return {true, in.host == QueueHost::eD3d12 ? L"auto (XeMFG standalone)"
        : in.host == QueueHost::eVulkan ? L"auto (Vulkan host)" : nullptr};
}

// metergate.1: whether the flip queue's owners use NVIDIA's driver flip metering (xess_mod.cpp
// ConfigureHostFlipMetering, sdk_pacing.h SetFlipMetering). A metered batch gates only its first output and leaves the
// rest to the driver, so metering is used only where the driver reports SetFlipConfig V2 support through
// NvAPI_NGX_GetDriverFeatureSupport (R595+, NV_NGX_DRIVER_FEATURE_ID_SET_FLIP_CONFIG_V2), the query sl.dlss_g makes
// before it meters. SetFlipConfig returning NVAPI_OK is not proof: a GTX 1660 accepts it without metering. Without
// metering every output is gated at the clock's slots, as on AMD. XeFGFlipMetering off (off, 0, false, no,
// disabled) turns it off; on (on, 1, true, yes, enabled) uses it whatever the driver reports; absent, empty, auto and
// anything else follow the report. resolved: the driver's own NvAPI_D3D12_SetFlipConfig was found.
enum class FlipMeteringReport : uint32_t { eSupported = 0, eUnsupported = 1, eQueryMissing = 2, eQueryFailed = 3 };
struct FlipMeteringChoice
{
    bool enabled;
    const char* reason; // nullptr when enabled by the report
};
inline FlipMeteringChoice ChooseFlipMetering(bool resolved, bool iniOn, bool iniForced, FlipMeteringReport report) noexcept
{
    if (!resolved) return {false, nullptr};
    if (!iniOn) return {false, "setting_off"};
    if (report == FlipMeteringReport::eSupported) return {true, nullptr};
    if (iniForced) return {true, "forced"};
    return {false, report == FlipMeteringReport::eUnsupported ? "driver_unsupported"
        : report == FlipMeteringReport::eQueryMissing ? "support_query_missing" : "support_query_failed"};
}

inline const char* FlipMeteringReportName(FlipMeteringReport report) noexcept
{
    switch (report) {
    case FlipMeteringReport::eSupported: return "supported";
    case FlipMeteringReport::eUnsupported: return "unsupported";
    case FlipMeteringReport::eQueryMissing: return "query_missing";
    default: return "query_failed";
    }
}

// hosthandle.1: whether an XeFG call is the host's (universal_hooks.cpp Gateway). OptiScaler's calls are recognised by
// the module they return into, but another mod's hook between OptiScaler and XeFG (e.g. REFramework's detours of
// InitFromSwapChainDesc and GetSwapChainPtr) returns into its own module, leaving the context untracked. A context the
// host created (xess_mod hostFg; never one of the game's tracked contexts) is the host's whoever calls on it.
inline bool HostRoutedCall(bool hostCaller, bool hostOwnedFgHandle) noexcept
{
    return hostCaller || hostOwnedFgHandle;
}

// optisleep.1: whether a call reaching libxell's sleep or marker body (universal_hooks.cpp probes) is OptiScaler's, so
// that the probe acts on it: the sleep after it is latency.1's sleep point, and the Present markers time the game's
// Present. OptiScaler calls libxell through its own export detours, so Hook_xellSleep never runs there. Without
// OptiScaler the game's XeLL calls reach Hook_xellSleep, and the standalone hosts' own calls (inside a
// VulkanHostCallScope) already sleep and time the Present themselves. ownCall: one of those, or a call through our own
// Hook_xellSleep (GatewaySleepScope), which sleeps after releasing the context's lease.
inline bool HostProbeActs(bool optiscalerDetected, bool ownCall) noexcept
{
    return optiscalerDetected && !ownCall;
}

// optisleep.1: the XeLL latency markers that bracket the game's Present (xell.h: XELL_PRESENT_START 4,
// XELL_PRESENT_END 5): +1 start, -1 end, 0 any other marker.
inline int PresentMarkerEdge(uint32_t marker) noexcept
{
    return marker == 4 ? 1 : marker == 5 ? -1 : 0;
}
}
