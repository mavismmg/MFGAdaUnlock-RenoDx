/*
 * RenoDX MFG Unlock
 * SPDX-License-Identifier: MIT
 *
 * Makes DLSS multi-frame generation (3x and above) available on Ada (RTX 40),
 * which NVIDIA ships gated to Blackwell (RTX 50) only -- and corrects the
 * interpolation so the extra frames carry new motion instead of repeats.
 *
 * Nothing on disk is modified. Every patch is applied to the mapped image and
 * reverted on unload.
 *
 * ---------------------------------------------------------------------------
 * LAYOUT
 *
 *   this file      config, ReShade events, and the fallback parameter override
 *   addon_provider.inl
 *                  arch gates, midpoint and kernel patches of the provider
 *   addon_streamline.inl
 *                  flip metering and the plugin's frame ceiling
 *   addon_discovery.inl
 *                  the bounded discovery worker
 *   addon_latency.inl
 *                  DXGI/Reflex latency policy and the Latency Guard
 *   addon_overlay.inl
 *                  the DLSS-G panel and the diagnostics report
 *                  (addon_*.inl are fragments of this file, included in its
 *                  namespace)
 *   midpoint.hpp   the temporal fix -- fatbin/PTX rewrite
 *   framecount.hpp forcing numFramesToGenerate through slDLSSGSetOptions
 *   loadhook.hpp   catching the snippet as it is mapped
 *   ngx_hook.hpp   thread-safe Detours installer
 *
 * Each of those carries its own commentary. What follows is this file only.
 *
 * ---------------------------------------------------------------------------
 * HOW THE CAPABILITY IS DECIDED -- three gates, outermost first
 *
 * 1. Which GPUs the snippet claims at all. nvngx_dlssg.dll exports
 *
 *        NVSDK_NGX_GetGPUArchitecture:   mov eax, 0x190   ; Ada
 *                                        ret
 *
 *    a hardcoded minimum architecture that NGX reads before anything else. It
 *    matches each snippet's published hardware requirement exactly --
 *    nvngx_dlss 0x160 (Turing), nvngx_dlssg 0x190 (Ada), nvngx_dlssnr 0x1b0
 *    (Blackwell). A 40-series card already clears this one, so it is left
 *    alone; it is documented because it is the first thing to read when a
 *    feature is missing *entirely* rather than merely limited.
 *
 * 2. How many frames the snippet advertises, in
 *    DLSSGInstanceManager::PopulateParameters:
 *
 *        cmp ebp, 0x1b0        ; NVAPI arch id, 0x1b0 == GB20x (RTX 50)
 *        jl  <not supported>   ; anything below -->
 *        mov edi, 5            ;   Blackwell: max frame count 5
 *        ...
 *        <not supported>: mov edi, 1
 *        ... Set("DLSSG.MultiFrameCountMax", edi)
 *
 * 3. A second compare against the same constant, feeding a runtime capability
 *    flag that drives generation itself:
 *
 *        cmp   eax, 0x1b0
 *        setae al
 *        mov   byte ptr [rdi+0x28], al
 *
 *    Patching (2) without (3) is the worst of both: the options appear, the
 *    runtime accepts the request, and the game renders black.
 *
 * So this addon rewrites 0x1b0 -> 0x190 at every *compare*, in both encodings
 * (3D imm32 and 81 /7 imm32), and deliberately leaves `mov r32, 0x1b0` alone --
 * that is the arch-id lookup table, not a gate.
 *
 * ---------------------------------------------------------------------------
 * WHY THE MAPPED IMAGE AND NEVER THE FILE
 *
 * NGX verifies the snippet's Authenticode signature when it LOADS it, so the
 * same bytes changed on disk make frame generation disappear altogether. The
 * mapped copy is never re-checked.
 *
 * ---------------------------------------------------------------------------
 * THE PARAMETER OVERRIDE (kept, but not what does the work)
 *
 * NVSDK_NGX_*_GetParameters / GetCapabilityParameters are also hooked, and slot
 * 11 of the returned object's vtable -- Get(const char*, unsigned int*) -- is
 * replaced so "DLSSG.MultiFrameCountMax" can be answered directly.
 * NVSDK_NGX_Parameter declares 8 Set overloads before its 8 Get overloads,
 * which is where that slot number comes from.
 *
 * This was the original approach, and it does not work with Streamline:
 * sl.dlss_g builds its own NVSDK_NGX_Parameter rather than passing NGX's along,
 * so the patch arms and never fires. It is kept because it costs nothing and is
 * the only lever for an NGX consumer that is not Streamline. The arch gates
 * above are what actually does the job in every game tested.
 *
 * ---------------------------------------------------------------------------
 * PACING
 *
 * Current Streamline runtimes provide working native pacing on Ada after the
 * capability gates are opened. A separate legacy software-flip fallback is
 * retained only for integrations that freeze at 3x+; it is opt-in and derives
 * the provider field's offset and polarity at runtime because both move
 * between plugin builds.
 *
 * None of this makes multi-frame generation correct by NVIDIA's standards on
 * hardware they did not ship it for. It makes it run, and midpoint.hpp makes it
 * look right; the rest is judged by eye.
 */

#include "binary_patch.hpp"
#include "backend_selection.hpp"
#include "panel_ui.hpp"
#include "xemfg/backend_api.hpp"
#ifdef MFGUNLOCK_NATIVE_XEMFG
#include "xemfg/native_bridge.hpp"
#include "xemfg/dependency_manager.hpp"
#include "dlssg_display.hpp"
#include "dlssg_latency.hpp"
#endif

#define ImTextureID ImU64

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <nvsdk_ngx.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "./framecount.hpp"
#include "./loadhook.hpp"
#include "./midpoint.hpp"
#include "./nvapi_status.hpp"
#include "./ngx_hook.hpp"
#include "./ngx_param_vtable.hpp"
#include "./module_scan.hpp"
#include "./cuda_temporal.hpp"
#include "./quality_build_policy.hpp"
#include "./pacing_policy.hpp"
#include "./thin_geometry.hpp"
#include "./validation_status.hpp"
#include "./blackwell.hpp"
#include "./gpu_arch.hpp"

// The addon_*.inl fragments below are parts of this file (plan 6.2-6.6).
#define MFGUNLOCK_ADDON_FRAGMENTS

namespace {

constexpr const char* kConfigSection = "RenoDX.MFGUnlock";
constexpr const char* kParamName = "DLSSG.MultiFrameCountMax";

constexpr unsigned int kMinCount = 2;
constexpr unsigned int kMaxCount = 5;

std::atomic_bool g_enabled{true};
std::atomic_bool g_quality_refinement{false};
std::atomic_bool g_configured_quality_refinement{false};
// Single user-facing switch for the coordinated quality path. New installs use
// the validated unified profile, while LoadConfig still honors an explicit
// saved opt-out from existing users.
std::atomic_bool g_adaptive_quality{true};
std::atomic_bool g_configured_adaptive_quality{true};
std::atomic<unsigned int> g_adaptive_quality_profile{
    static_cast<unsigned int>(mfgunlock::adaptivequality::kDefaultProfile)};
std::atomic<unsigned int> g_configured_adaptive_quality_profile{
    static_cast<unsigned int>(mfgunlock::adaptivequality::kDefaultProfile)};
std::atomic_bool g_adaptive_quality_v3_photometric{true};
std::atomic_bool g_configured_adaptive_quality_v3_photometric{true};
std::atomic_bool g_adaptive_quality_v3_directional_border{true};
std::atomic_bool g_configured_adaptive_quality_v3_directional_border{true};
std::atomic_bool g_adaptive_quality_v3_oriented_geometry{true};
std::atomic_bool g_configured_adaptive_quality_v3_oriented_geometry{true};
std::atomic<unsigned int> g_adaptive_quality_v3_stability_mode{
    mfgunlock::qualitybuild::StabilityMode(static_cast<unsigned int>(
        mfgunlock::cudatemporal::StabilityMode::kTemporal))};
std::atomic<unsigned int> g_configured_adaptive_quality_v3_stability_mode{
    mfgunlock::qualitybuild::StabilityMode(static_cast<unsigned int>(
        mfgunlock::cudatemporal::StabilityMode::kTemporal))};
std::atomic<unsigned int> g_adaptive_quality_v3_inpaint_mode{
    mfgunlock::qualitybuild::InpaintMode(static_cast<unsigned int>(
        mfgunlock::cudatemporal::kDefaultInpaintMode))};
std::atomic<unsigned int> g_configured_adaptive_quality_v3_inpaint_mode{
    mfgunlock::qualitybuild::InpaintMode(static_cast<unsigned int>(
        mfgunlock::cudatemporal::kDefaultInpaintMode))};
std::atomic<unsigned int> g_source_cap_config_origin{
    static_cast<unsigned int>(
        mfgunlock::pacing::SourceCapConfigOrigin::kNone)};
// Research-only attenuation of the refinement's extra blend weight near the
// screen boundary. Never changes the provider's native candidate weight.
std::atomic_bool g_border_confidence{false};
std::atomic_bool g_configured_border_confidence{false};
std::atomic_bool g_geometry_confidence_v2{false};
std::atomic_bool g_configured_geometry_confidence_v2{false};
std::atomic_bool g_configured_enabled{true};
std::atomic<unsigned int> g_max_count{4};
std::atomic_bool g_force_flip_meter_off{false};
std::atomic_bool g_configured_force_flip_meter_off{false};
std::atomic_bool g_temporal_fix{true};
std::atomic_bool g_configured_temporal_fix{true};
std::atomic_bool g_blackwell_framework_kernels{true};
std::atomic_bool g_configured_blackwell_framework_kernels{true};
// The two complementary thin-geometry mechanisms are the experimental quality
// default. Persisted user choices still take precedence over these defaults.
std::atomic_bool g_thin_geometry_validated_warp_blend{true};
// 1.7 (plan C.4): apply the validated warp blend on RTX 50 by rewriting the
// provider's sm_120 PTX. Off until RTX 50 field testing confirms it.
std::atomic_bool g_rtx50_quality_fixes{false};
std::atomic_bool g_configured_thin_geometry_validated_warp_blend{true};
std::atomic_bool g_thin_geometry_previous_scatter{false};
std::atomic_bool g_configured_thin_geometry_previous_scatter{false};
// The independently developed intermediate-scatter variant remains paired with
// validated warp blend by default; either mechanism can still be tested alone.
std::atomic_bool g_thin_geometry_intermediate_scatter{true};
std::atomic_bool g_configured_thin_geometry_intermediate_scatter{true};
// Optional replacement for unconditional intermediate retention. It conditions
// only our additional relaxation on a local same-depth, motion-coherent cluster
// and otherwise returns to the provider's native rejection behavior.
// Fresh configurations use the recommended Balanced mode. LoadConfig preserves
// every valid current or legacy selection, including an explicitly saved Off.
std::atomic<unsigned int> g_silhouette_guard_mode{
    static_cast<unsigned int>(
        mfgunlock::blackwell::SilhouetteGuardMode::Balanced)};
std::atomic<unsigned int> g_configured_silhouette_guard_mode{
    static_cast<unsigned int>(
        mfgunlock::blackwell::SilhouetteGuardMode::Balanced)};
// Raising the plugin's own clamp broke GTA V Enhanced -- its 2.9.1.0 plugin was
// only ever shipped bounded at 3, and lifting that is not the same as it being
// able to cope. Off by default; updating the plugin is the sound fix.
std::atomic_bool g_raise_ceiling{false};
std::atomic<unsigned int> g_configured_runtime_selection_mode{
    static_cast<unsigned int>(
        mfgunlock::framecount::RuntimeSelectionMode::kGameDefault)};

enum class DetectedRenderApi : unsigned int {
  kUnknown,
  kD3D11,
  kD3D12,
  kVulkan,
  kOther,
};

std::atomic<DetectedRenderApi> g_render_api{DetectedRenderApi::kUnknown};
std::atomic<reshade::api::swapchain*> g_primary_swapchain{nullptr};
std::atomic<uint64_t> g_primary_swapchain_area{0};
std::atomic_bool g_latency_guard_dxgi_observed{false};
std::atomic_bool g_latency_guard_waitable_swapchain{false};
std::atomic<unsigned int> g_latency_guard_dxgi_max_latency{0};
struct ReflexWaitableSwapchainControl {
  SRWLOCK lock = SRWLOCK_INIT;
  reshade::api::swapchain* owner = nullptr;
  uint64_t generation = 1;
  UINT native_maximum_latency = 0;
  bool maximum_latency_forced = false;
  bool fallback_latched = false;
  bool last_requested = false;
};
ReflexWaitableSwapchainControl g_reflex_waitable;
// Published only after the owner has been retired and request bookkeeping
// returned to native. Native waitable state alone does not prove retirement.
std::atomic_bool g_reflex_waitable_idle{true};
std::atomic<unsigned long long> g_vram_ui_heartbeat_ms{0};
std::atomic_bool g_vram_capture_owned{false};
std::atomic_bool g_vram_dxgi_seen{false};
std::atomic<uint64_t> g_vram_local_usage{0};
std::atomic<uint64_t> g_vram_local_budget{0};
std::atomic<uint64_t> g_vram_local_available{0};
std::atomic<uint64_t> g_vram_nonlocal_usage{0};
std::atomic<uint64_t> g_vram_nonlocal_budget{0};
std::atomic<unsigned int> g_vram_swapchain_buffers{0};
HMODULE g_self_module = nullptr;
bool g_is_star_wars_outlaws = false;
bool g_is_monster_hunter_wilds = false;
std::atomic_bool g_monster_hunter_restart_notice_pending{false};
constexpr int kMonsterHunterRestartNoticeRevision = 1;

void ClearDxgiDiagnostics() {
  g_latency_guard_dxgi_observed.store(false, std::memory_order_release);
  g_latency_guard_waitable_swapchain.store(false, std::memory_order_relaxed);
  g_latency_guard_dxgi_max_latency.store(0, std::memory_order_relaxed);
  g_vram_dxgi_seen.store(false, std::memory_order_release);
  g_vram_local_usage.store(0, std::memory_order_relaxed);
  g_vram_local_budget.store(0, std::memory_order_relaxed);
  g_vram_local_available.store(0, std::memory_order_relaxed);
  g_vram_nonlocal_usage.store(0, std::memory_order_relaxed);
  g_vram_nonlocal_budget.store(0, std::memory_order_relaxed);
  g_vram_swapchain_buffers.store(0, std::memory_order_relaxed);
}

bool CurrentProcessNameIs(const wchar_t* expected) {
  if (expected == nullptr || expected[0] == L'\0') return false;
  wchar_t process_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, process_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  const wchar_t* basename = std::wcsrchr(process_path, L'\\');
  return _wcsicmp(basename != nullptr ? basename + 1 : process_path,
                  expected) == 0;
}

std::string SelfModuleFileName() {
  std::vector<char> path(32768);
  const DWORD length = GetModuleFileNameA(
      g_self_module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return "renodx-mfgunlock.addon64";
  const char* slash = std::strrchr(path.data(), '\\');
  const char* forward = std::strrchr(path.data(), '/');
  const char* name = slash == nullptr ? forward
                                     : (forward == nullptr || slash > forward ? slash
                                                                              : forward);
  return name == nullptr ? std::string(path.data()) : std::string(name + 1);
}

std::vector<std::string> ReadConfigArray(const char* section, const char* key) {
  size_t size = 0;
  if (!reshade::get_config_value(nullptr, section, key, nullptr, &size) ||
      size == 0) {
    return {};
  }
  std::string raw(size, '\0');
  if (!reshade::get_config_value(nullptr, section, key, raw.data(), &size)) {
    return {};
  }
  std::vector<std::string> values;
  for (size_t offset = 0; offset < raw.size();) {
    const char* entry = raw.c_str() + offset;
    const size_t length = std::strlen(entry);
    if (length == 0) break;
    values.emplace_back(entry);
    offset += length + 1;
  }
  return values;
}

bool HasEarlyLoadEntry() {
  const std::string self = SelfModuleFileName();
  for (const auto& entry : ReadConfigArray("ADDON", "LoadFromDllMain")) {
    const char* slash = std::strrchr(entry.c_str(), '\\');
    const char* forward = std::strrchr(entry.c_str(), '/');
    const char* name = slash == nullptr ? forward
                                       : (forward == nullptr || slash > forward ? slash
                                                                                : forward);
    if (_stricmp(name == nullptr ? entry.c_str() : name + 1, self.c_str()) == 0)
      return true;
  }
  return false;
}

bool EnsureEarlyLoadEntry() {
  if (HasEarlyLoadEntry()) return true;
  auto values = ReadConfigArray("ADDON", "LoadFromDllMain");
  values.push_back(SelfModuleFileName());
  std::string serialized;
  for (const auto& value : values) {
    serialized.append(value);
    serialized.push_back('\0');
  }
  reshade::set_config_value(nullptr, "ADDON", "LoadFromDllMain",
                            serialized.c_str(), serialized.size());
  return HasEarlyLoadEntry();
}

const char* RenderApiName(DetectedRenderApi api) {
  switch (api) {
    case DetectedRenderApi::kD3D11:
      return "Direct3D 11";
    case DetectedRenderApi::kD3D12:
      return "Direct3D 12";
    case DetectedRenderApi::kVulkan:
      return "Vulkan";
    case DetectedRenderApi::kOther:
      return "unsupported/other";
    default:
      return "not detected yet";
  }
}

// The ReShade device API, as the addon classifies it.
DetectedRenderApi RenderApiOf(reshade::api::device* device) {
  switch (device->get_api()) {
    case reshade::api::device_api::d3d11: return DetectedRenderApi::kD3D11;
    case reshade::api::device_api::d3d12: return DetectedRenderApi::kD3D12;
    case reshade::api::device_api::vulkan: return DetectedRenderApi::kVulkan;
    default: return DetectedRenderApi::kOther;
  }
}

// Tells the Dynamic MFG policy which API presents: D3D12, Vulkan, or one of
// the three it supports at all.
void NotifyDynamicApi(DetectedRenderApi api) {
  mfgunlock::framecount::NotifyDynamicD3D12(
      api == DetectedRenderApi::kD3D12, api == DetectedRenderApi::kVulkan,
      api == DetectedRenderApi::kD3D11 || api == DetectedRenderApi::kD3D12 ||
          api == DetectedRenderApi::kVulkan);
}

// The device API of a new primary swapchain. DXGI diagnostics apply only to
// D3D11 and D3D12.
void ObservePrimaryApi(reshade::api::device* device) {
  const DetectedRenderApi detected = RenderApiOf(device);
  g_render_api.store(detected, std::memory_order_relaxed);
  if (detected != DetectedRenderApi::kD3D11 &&
      detected != DetectedRenderApi::kD3D12)
    ClearDxgiDiagnostics();
  NotifyDynamicApi(detected);
}

bool IsHdrColorSpace(reshade::api::color_space color_space) {
  return color_space == reshade::api::color_space::scrgb ||
         color_space == reshade::api::color_space::hdr10_pq ||
         color_space == reshade::api::color_space::hdr10_hlg;
}

// Our own image. Both marker scans look for strings that are, necessarily,
// string literals inside this very DLL -- so without excluding ourselves the
// scan happily identifies the addon as the DLSS-G plugin and then fails to
// make sense of it. Harmless where the real plugin is enumerated first;
// fatal where it is not loaded at all.

mfgunlock::ngxparam::VTablePatch g_vtable_patch;
std::atomic_bool g_override_reported{false};
std::atomic<unsigned int> g_override_hits{0};
std::atomic<unsigned int> g_runtime_reported_value{0};

using GetUInt32Fn = NVSDK_NGX_Result (*)(void* self, const char* name, unsigned int* out);

bool NgxFailed(NVSDK_NGX_Result result) {
  return (static_cast<unsigned int>(result) & 0xfff00000u) == 0xbad00000u;
}

NVSDK_NGX_Result HookedGetUInt32(void* self, const char* name, unsigned int* out) {
  const auto real = reinterpret_cast<GetUInt32Fn>(g_vtable_patch.original.load(std::memory_order_acquire));
  NVSDK_NGX_Result result = real(self, name, out);

  if (!g_enabled.load(std::memory_order_relaxed)) return result;
  if (name == nullptr || out == nullptr) return result;
  if (std::strcmp(name, kParamName) != 0) return result;

  const bool failed = NgxFailed(result);
  const unsigned int reported = failed ? 0u : *out;
  const unsigned int want = g_max_count.load(std::memory_order_relaxed);

  g_runtime_reported_value.store(reported, std::memory_order_relaxed);

  // Never lower a value the runtime already offers.
  if (!failed && reported >= want) return result;

  *out = want;
  g_override_hits.fetch_add(1, std::memory_order_relaxed);

  if (!g_override_reported.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: " << kParamName << " came back as ";
    if (failed) {
      s << "a failure (0x" << std::hex << static_cast<unsigned int>(result) << std::dec << ")";
    } else {
      s << reported;
    }
    s << "; reporting " << want << " instead.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  return NVSDK_NGX_Result_Success;
}

bool PatchParameterVTable(NVSDK_NGX_Parameter* params) {
  using mfgunlock::ngxparam::PatchResult;
  const auto result = mfgunlock::ngxparam::Patch(
      g_vtable_patch, params, reinterpret_cast<void*>(&HookedGetUInt32));
  if (result == PatchResult::kProtectFailed) {
    reshade::log::message(reshade::log::level::error,
                          "mfgunlock: could not make the NGX parameter vtable writable.");
    return false;
  }
  if (result == PatchResult::kPatched) {
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: NGX parameter vtable patched; multi-frame capability override armed.");
  }
  if (result == PatchResult::kForeignHook) {
    static std::atomic_bool warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: NGX parameter vtable already points to this hook without a recorded original; left unchanged.");
  }
  return result == PatchResult::kPatched || result == PatchResult::kAlreadyPatched;
}

void RestoreParameterVTable() { mfgunlock::ngxparam::Restore(g_vtable_patch); }

#include "./addon_provider.inl"

#include "./addon_streamline.inl"

#include "./addon_discovery.inl"

#include "./addon_latency.inl"

// ReShade may unload and reload addons while it probes temporary D3D devices.
// Starting a self-pinned worker from those callbacks keeps the first addon
// instance alive and makes its eventual worker exit unregister the instance
// ReShade adopted. The first real Present happens after that probe cycle. It
// only launches the worker; discovery and patching remain on the worker thread.
void OnPresentStartDiscovery(reshade::api::command_queue* /*queue*/,
                             reshade::api::swapchain* swapchain,
                             const reshade::api::rect* /*source_rect*/,
                             const reshade::api::rect* /*dest_rect*/,
                             uint32_t /*dirty_rect_count*/,
                             const reshade::api::rect* /*dirty_rects*/) {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
  if(swapchain==g_primary_swapchain.load(std::memory_order_acquire)) mfgunlock::dlssgdisplay::Observe(swapchain);
#endif
#if defined(MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
  // Do not pin ReShade's temporary addon/device probes during startup. Once a
  // real Present is observed, hot unload cannot establish CUDA quiescence.
  static std::atomic_bool pin_attempted{false};
  if (swapchain != nullptr && !pin_attempted.exchange(true, std::memory_order_acq_rel)) {
    HMODULE held = nullptr;
    const bool resident = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&OnPresentStartDiscovery), &held) != FALSE;
    if (!resident) pin_attempted.store(false, std::memory_order_release);
    if (resident) reshade::log::message(reshade::log::level::info,
        "mfgunlock: engineering session is process-resident after first Present; restart required, hot reload disabled.");
  }
#endif
  // A primary swapchain can disappear while an already-existing secondary
  // survives (resize, launcher/video swapchain, device recreation). Promote
  // the next presenting swapchain once instead of leaving HDR/API state stale.
  if (swapchain != nullptr &&
      g_primary_swapchain.load(std::memory_order_acquire) == nullptr) {
    reshade::api::swapchain* expected = nullptr;
    if (g_primary_swapchain.compare_exchange_strong(
            expected, swapchain, std::memory_order_acq_rel)) {
      if (auto* device = swapchain->get_device(); device != nullptr) {
        const auto back_buffer = swapchain->get_back_buffer(0);
        const auto desc = device->get_resource_desc(back_buffer);
        g_primary_swapchain_area.store(
            static_cast<uint64_t>(desc.texture.width) * desc.texture.height,
            std::memory_order_relaxed);
        ObservePrimaryApi(device);
      }
      mfgunlock::framecount::NotifySwapchainTransition();
    }
  }
  if (swapchain != nullptr &&
      swapchain == g_primary_swapchain.load(std::memory_order_acquire)) {
    const auto color_space = swapchain->get_color_space();
    if (color_space != reshade::api::color_space::unknown) {
      if (mfgunlock::inputdiag::g_enabled.load(std::memory_order_relaxed)) {
        if (auto* device = swapchain->get_device(); device != nullptr) {
          const auto back_buffer = swapchain->get_back_buffer(0);
          const auto desc = device->get_resource_desc(back_buffer);
          mfgunlock::inputdiag::ObserveOutput(
              desc.texture.width, desc.texture.height,
              static_cast<uint32_t>(desc.texture.format),
              static_cast<uint32_t>(color_space));
        }
      }
      const bool hdr = IsHdrColorSpace(color_space);
      mfgunlock::framecount::NotifyHdrState(hdr);
    }
  }
  UpdateReflexPacingLab(swapchain);
  UpdateLatencyGuard(swapchain);
  mfgunlock::framecount::PublishLatencyGuardView();
  UpdateVramDiagnostics(swapchain);
  StartDiscoveryWorker();
  RetryPresetHookFromPresent();
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  if (g_render_api.load(std::memory_order_relaxed) ==
      DetectedRenderApi::kD3D12)
    mfgunlock::cudatemporal::TryInstall();
#endif
}

void OnFinishPresent(reshade::api::command_queue* /*queue*/,
                     reshade::api::swapchain* swapchain) {
  if (swapchain != nullptr &&
      swapchain == g_primary_swapchain.load(std::memory_order_acquire))
    mfgunlock::reflexpacing::RecordPresent();
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  bool primary_transition = false;
  if (swapchain != nullptr) {
    if (auto* device = swapchain->get_device(); device != nullptr) {
      const auto back_buffer = swapchain->get_back_buffer(0);
      const auto desc = device->get_resource_desc(back_buffer);
      const uint64_t area = static_cast<uint64_t>(desc.texture.width) *
                            static_cast<uint64_t>(desc.texture.height);
      uint64_t selected_area = g_primary_swapchain_area.load(
          std::memory_order_relaxed);
      reshade::api::swapchain* selected = g_primary_swapchain.load(
          std::memory_order_acquire);
      if (selected == swapchain || selected == nullptr || area > selected_area) {
        g_primary_swapchain_area.store(area, std::memory_order_relaxed);
        g_primary_swapchain.store(swapchain, std::memory_order_release);
        ObservePrimaryApi(device);
        const auto color_space = swapchain->get_color_space();
        if (color_space != reshade::api::color_space::unknown) {
          const bool hdr = IsHdrColorSpace(color_space);
          mfgunlock::framecount::NotifyHdrState(hdr);
        }
        primary_transition = true;
      }
    }
  }
  if (primary_transition) {
    AcquireSRWLockExclusive(&g_reflex_waitable.lock);
    g_reflex_waitable.fallback_latched = false;
    if (mfgunlock::reflexpacing::WaitableRequested()) {
      mfgunlock::reflexpacing::g_fallback_reason.store(
          mfgunlock::reflexpacing::FallbackReason::kNone,
          std::memory_order_relaxed);
      mfgunlock::reflexpacing::g_waitable_state.store(
          mfgunlock::reflexpacing::WaitableState::kWaitingForSwapchain,
          std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    mfgunlock::framecount::NotifySwapchainTransition();
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
    mfgunlock::cudatemporal::RequestHistoryReset();
#endif
  }
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
  mfgunlock::dlssgdisplay::Retire(swapchain);
#endif
  AcquireSRWLockExclusive(&g_reflex_waitable.lock);
  if (swapchain != nullptr && swapchain == g_reflex_waitable.owner) {
    RestoreReflexWaitableLocked(
        swapchain, mfgunlock::reflexpacing::FallbackReason::kNotRequested);
    mfgunlock::reflexpacing::g_fallback_reason.store(
        mfgunlock::reflexpacing::FallbackReason::kSwapchainChanged,
        std::memory_order_relaxed);
    mfgunlock::reflexpacing::g_waitable_state.store(
        mfgunlock::reflexpacing::WaitableState::kWaitingForSwapchain,
        std::memory_order_release);
    g_reflex_waitable.fallback_latched = false;
  }
  ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
  reshade::api::swapchain* expected = swapchain;
  if (g_primary_swapchain.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel)) {
    g_primary_swapchain_area.store(0, std::memory_order_relaxed);
    g_render_api.store(DetectedRenderApi::kUnknown, std::memory_order_relaxed);
    ClearDxgiDiagnostics();
    mfgunlock::framecount::NotifyDynamicD3D12(false, false, false);
    mfgunlock::framecount::NotifySwapchainTransition();
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
    mfgunlock::cudatemporal::RequestHistoryReset();
#endif
  }
}

// The synchronous maintenance on device and queue creation, timed (1.5.7).
// It stays synchronous: the worker starts only at the first Present, and this
// is one of the chances to commit the gates before NGX queries the provider.
// The first passes report how long the game's thread waited for it.
void RunCreationMaintenance(const char* event) {
  static std::atomic<unsigned int> reported{0};
  LARGE_INTEGER start{}, end{}, frequency{};
  QueryPerformanceCounter(&start);
  RunProviderMaintenance();
  TryPatchFrameCountCeiling();
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
  QueryPerformanceCounter(&end);
  QueryPerformanceFrequency(&frequency);
  if (reported.fetch_add(1, std::memory_order_relaxed) < 6 && frequency.QuadPart > 0) {
    std::stringstream s;
    s << "mfgunlock: provider maintenance on " << event << " took " << std::fixed << std::setprecision(2)
      << double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart) << " ms.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
}

// These remain immediate, finite retries during graphics-device initialization. They do
// not start a thread and therefore cannot keep a temporary addon instance alive.
void OnInitDevice(reshade::api::device* device) {
  if (device != nullptr) {
    const DetectedRenderApi detected = RenderApiOf(device);
    const bool no_primary =
        g_primary_swapchain.load(std::memory_order_acquire) == nullptr;
    if (no_primary) {
      NotifyDynamicApi(detected);
    }

    const DetectedRenderApi previous = no_primary
        ? g_render_api.exchange(detected, std::memory_order_relaxed)
        : g_render_api.load(std::memory_order_relaxed);
    if (no_primary && previous != detected) {
      std::stringstream s;
      s << "mfgunlock: ReShade initialized a " << RenderApiName(detected) << " device";
      if (detected == DetectedRenderApi::kVulkan) {
        s << "; enabling the experimental Vulkan provider-discovery path.";
      } else {
        s << ".";
      }
      reshade::log::message(reshade::log::level::info, s.str().c_str());
    }
  }

  RunCreationMaintenance("device creation");
}

void OnInitCommandQueue(reshade::api::command_queue* /*queue*/) {
  RunCreationMaintenance("command queue creation");
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  if (g_render_api.load(std::memory_order_relaxed) ==
      DetectedRenderApi::kD3D12)
    mfgunlock::cudatemporal::TryInstall();
#endif
}

#include "./addon_overlay.inl"

void LoadConfig() {
  int value = 0;
  int legacy_fixed_output_cap = 0;
  bool legacy_fixed_output_cap_present = false;
  bool legacy_dynamic_source_cap = false;
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ExperimentalAdaptiveQuality", value))
    g_adaptive_quality.store(value != 0, std::memory_order_relaxed);
  g_configured_adaptive_quality.store(
      g_adaptive_quality.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityProfile", value)) {
    g_adaptive_quality_profile.store(
        static_cast<unsigned int>(
            mfgunlock::adaptivequality::NormalizeProfile(
                static_cast<unsigned int>(value))),
        std::memory_order_relaxed);
  }
  g_configured_adaptive_quality_profile.store(
      g_adaptive_quality_profile.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3Photometric", value))
    g_adaptive_quality_v3_photometric.store(value != 0,
                                             std::memory_order_relaxed);
  g_configured_adaptive_quality_v3_photometric.store(
      g_adaptive_quality_v3_photometric.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3DirectionalBorder", value))
    g_adaptive_quality_v3_directional_border.store(
        value != 0, std::memory_order_relaxed);
  g_configured_adaptive_quality_v3_directional_border.store(
      g_adaptive_quality_v3_directional_border.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3OrientedGeometry", value))
    g_adaptive_quality_v3_oriented_geometry.store(
        value != 0, std::memory_order_relaxed);
  g_configured_adaptive_quality_v3_oriented_geometry.store(
      g_adaptive_quality_v3_oriented_geometry.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3StabilityMode", value)) {
    const auto normalized = mfgunlock::cudatemporal::NormalizeMode(
        static_cast<unsigned int>(value));
    g_adaptive_quality_v3_stability_mode.store(
        mfgunlock::qualitybuild::StabilityMode(static_cast<unsigned int>(normalized)),
        std::memory_order_relaxed);
  }
  g_configured_adaptive_quality_v3_stability_mode.store(
      g_adaptive_quality_v3_stability_mode.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3InpaintMode", value)) {
    const auto normalized =
        mfgunlock::cudatemporal::NormalizeInpaintMode(
            static_cast<unsigned int>(value));
    g_adaptive_quality_v3_inpaint_mode.store(
        mfgunlock::qualitybuild::InpaintMode(static_cast<unsigned int>(normalized)),
        std::memory_order_relaxed);
  }
  g_configured_adaptive_quality_v3_inpaint_mode.store(
      g_adaptive_quality_v3_inpaint_mode.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  mfgunlock::cudatemporal::Configure(
      mfgunlock::cudatemporal::NormalizeMode(
          g_adaptive_quality_v3_stability_mode.load(
              std::memory_order_relaxed)),
      mfgunlock::cudatemporal::NormalizeInpaintMode(
          g_adaptive_quality_v3_inpaint_mode.load(
              std::memory_order_relaxed)));
#endif
  if (reshade::get_config_value(nullptr, kConfigSection, "ExperimentalQualityRefinement", value))
    g_quality_refinement.store(value != 0, std::memory_order_relaxed);
  g_configured_quality_refinement.store(g_quality_refinement.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "ExperimentalBorderConfidence", value))
    g_border_confidence.store(value != 0, std::memory_order_relaxed);
  g_configured_border_confidence.store(g_border_confidence.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "ExperimentalGeometryConfidenceV2", value))
    g_geometry_confidence_v2.store(value != 0, std::memory_order_relaxed);
  g_configured_geometry_confidence_v2.store(
      g_geometry_confidence_v2.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  const bool adaptive_quality =
      g_adaptive_quality.load(std::memory_order_relaxed);
  // Adaptive Quality owns the complete confidence path. Legacy choices remain
  // loaded and saved for later A/B use, but cannot stack another refinement,
  // border taper or geometry variant on top of the unified suite.
  mfgunlock::thingeometry::g_refinement_enabled =
      !adaptive_quality &&
      g_quality_refinement.load(std::memory_order_relaxed);
  mfgunlock::thingeometry::g_adaptive_quality_enabled =
      adaptive_quality;
  mfgunlock::thingeometry::g_adaptive_quality_profile =
      mfgunlock::adaptivequality::NormalizeProfile(
          g_adaptive_quality_profile.load(std::memory_order_relaxed));
  mfgunlock::thingeometry::g_adaptive_quality_v3_photometric =
      g_adaptive_quality_v3_photometric.load(std::memory_order_relaxed);
  mfgunlock::thingeometry::g_adaptive_quality_v3_directional_border =
      g_adaptive_quality_v3_directional_border.load(
          std::memory_order_relaxed);
  mfgunlock::thingeometry::g_border_confidence_enabled =
      !adaptive_quality &&
      g_quality_refinement.load(std::memory_order_relaxed) &&
      g_border_confidence.load(std::memory_order_relaxed);
  mfgunlock::blackwell::g_refinement_enabled =
      !adaptive_quality &&
      g_quality_refinement.load(std::memory_order_relaxed);
  mfgunlock::blackwell::g_adaptive_quality_enabled =
      adaptive_quality;
  mfgunlock::blackwell::g_adaptive_quality_profile =
      mfgunlock::adaptivequality::NormalizeProfile(
          g_adaptive_quality_profile.load(std::memory_order_relaxed));
  mfgunlock::blackwell::g_adaptive_quality_v3_oriented_geometry =
      g_adaptive_quality_v3_oriented_geometry.load(std::memory_order_relaxed);
  mfgunlock::blackwell::g_adaptive_quality_v3_temporal_geometry =
      mfgunlock::cudatemporal::NormalizeMode(
          g_adaptive_quality_v3_stability_mode.load(
              std::memory_order_relaxed)) ==
      mfgunlock::cudatemporal::StabilityMode::kTemporal;
  mfgunlock::blackwell::g_adaptive_quality_v3_inpaint_mode =
      g_adaptive_quality_v3_inpaint_mode.load(std::memory_order_relaxed);
  mfgunlock::blackwell::g_geometry_confidence_v2_enabled =
      !adaptive_quality &&
      g_quality_refinement.load(std::memory_order_relaxed) &&
      g_geometry_confidence_v2.load(std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "Enabled", value)) {
    g_enabled.store(value != 0, std::memory_order_relaxed);
  }
  g_configured_enabled.store(g_enabled.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
  mfgunlock::framecount::g_addon_enabled.store(
      g_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "MaxCount", value)) {
    if (value < static_cast<int>(kMinCount)) value = static_cast<int>(kMinCount);
    if (value > static_cast<int>(kMaxCount)) value = static_cast<int>(kMaxCount);
    g_max_count.store(static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", value)) {
    g_force_flip_meter_off.store(value != 0, std::memory_order_relaxed);
  }
  g_configured_force_flip_meter_off.store(
      g_force_flip_meter_off.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "TemporalFix", value)) {
    g_temporal_fix.store(value != 0, std::memory_order_relaxed);
  }
  g_configured_temporal_fix.store(
      g_temporal_fix.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "BlackwellFrameworkKernels", value)) {
    g_blackwell_framework_kernels.store(value != 0, std::memory_order_relaxed);
  }
  g_configured_blackwell_framework_kernels.store(
      g_blackwell_framework_kernels.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ThinGeometryValidatedWarpBlend", value)) {
    g_thin_geometry_validated_warp_blend.store(value != 0,
                                                std::memory_order_relaxed);
  }
  g_configured_thin_geometry_validated_warp_blend.store(
      g_thin_geometry_validated_warp_blend.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "Rtx50QualityFixes",
                                value)) {
    g_rtx50_quality_fixes.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ThinGeometryPreviousScatter", value)) {
    g_thin_geometry_previous_scatter.store(value != 0,
                                            std::memory_order_relaxed);
  }
  g_configured_thin_geometry_previous_scatter.store(
      g_thin_geometry_previous_scatter.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ThinGeometryIntermediateScatter", value)) {
    g_thin_geometry_intermediate_scatter.store(value != 0,
                                                std::memory_order_relaxed);
  }
  g_configured_thin_geometry_intermediate_scatter.store(
      g_thin_geometry_intermediate_scatter.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "BoundaryArtifactMitigationMode", value)) {
    if (value < static_cast<int>(
                    mfgunlock::blackwell::SilhouetteGuardMode::Off) ||
        value > static_cast<int>(
                    mfgunlock::blackwell::SilhouetteGuardMode::Aggressive)) {
      value = static_cast<int>(
          mfgunlock::blackwell::SilhouetteGuardMode::Balanced);
    }
    g_silhouette_guard_mode.store(static_cast<unsigned int>(value),
                                  std::memory_order_relaxed);
  } else if (reshade::get_config_value(nullptr, kConfigSection,
                                       "SilhouetteBoundaryGuard", value)) {
    // Preserve the boolean used by the first experimental build. Its enabled
    // value maps to Balanced; the new key takes precedence after the user
    // explicitly chooses any mode, including Off.
    g_silhouette_guard_mode.store(
        static_cast<unsigned int>(
            value != 0
                ? mfgunlock::blackwell::SilhouetteGuardMode::Balanced
                : mfgunlock::blackwell::SilhouetteGuardMode::Off),
        std::memory_order_relaxed);
  }
  g_configured_silhouette_guard_mode.store(
      g_silhouette_guard_mode.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "RaiseFrameCeiling", value)) {
    g_raise_ceiling.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "RuntimeSelectionMode", value)) {
    if (value < static_cast<int>(
                    mfgunlock::framecount::RuntimeSelectionMode::kGameDefault) ||
        value > static_cast<int>(
                    mfgunlock::framecount::RuntimeSelectionMode::kForceOta)) {
      value = static_cast<int>(
          mfgunlock::framecount::RuntimeSelectionMode::kGameDefault);
    }
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  } else if (reshade::get_config_value(nullptr, kConfigSection,
                                       "ForceOTAPlugins", value) && value != 0) {
    // One-way compatibility with the older boolean setting. Once the new key
    // is saved it takes precedence, including when explicitly set to default.
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(
            mfgunlock::framecount::RuntimeSelectionMode::kForceOta),
        std::memory_order_relaxed);
  }
  g_configured_runtime_selection_mode.store(
      mfgunlock::framecount::g_runtime_selection_mode.load(
          std::memory_order_relaxed),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceMultiplier", value)) {
    if (value != 0 && (value < 2 || value > 6)) value = 0;
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(value),
                                                    std::memory_order_relaxed);
    mfgunlock::framecount::g_fixed_override_status.store(
        static_cast<unsigned int>(
            mfgunlock::forcepolicy::IsFixedMultiplier(
                static_cast<unsigned int>(value))
                ? mfgunlock::forcepolicy::FixedOverrideStatus::kPending
                : mfgunlock::forcepolicy::FixedOverrideStatus::kNative),
        std::memory_order_relaxed);
  }
  // 1.6 (plan B): 0 = the game's presets, else an sl::DLSSPreset value (J-M).
  if (reshade::get_config_value(nullptr, kConfigSection, "DlssSuperResolutionPreset", value) &&
      value >= 0 && mfgunlock::dlsspreset::IsSelectable(static_cast<uint32_t>(value)))
    mfgunlock::dlsspreset::g_selected.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
  // 1.6 (plan B.6): 0 = the game's presets, else an sl::DLSSDPreset value (D, E).
  if (reshade::get_config_value(nullptr, kConfigSection, "DlssRayReconstructionPreset", value) &&
      value >= 0 && mfgunlock::dlsspreset::IsSelectableRr(static_cast<uint32_t>(value)))
    mfgunlock::dlsspreset::g_rr_selected.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
  legacy_fixed_output_cap_present = reshade::get_config_value(
      nullptr, kConfigSection, "FixedOutputFpsCap",
      legacy_fixed_output_cap);
  if (reshade::get_config_value(nullptr, kConfigSection, "DynamicMFG", value)) {
    mfgunlock::framecount::g_dynamic_mfg_enabled.store(value != 0,
                                                        std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "DynamicTargetFPS", value)) {
    if (value < 0 || value > 1000) value = 0;
    mfgunlock::framecount::g_dynamic_target_fps.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection,
                               "DynamicReflexSourceCap", value)) {
    legacy_dynamic_source_cap = value != 0;
  }
  int configured_source_cap = 0;
  const bool source_cap_key_present = reshade::get_config_value(
      nullptr, kConfigSection, "ReflexSourceFpsCap", configured_source_cap);
  const auto source_cap = mfgunlock::pacing::ResolveSourceCapConfig(
      source_cap_key_present,
      configured_source_cap > 0
          ? static_cast<unsigned int>(configured_source_cap)
          : 0u,
      mfgunlock::framecount::g_dynamic_mfg_enabled.load(
          std::memory_order_relaxed),
      legacy_dynamic_source_cap,
      mfgunlock::framecount::g_dynamic_target_fps.load(
          std::memory_order_relaxed),
      legacy_fixed_output_cap_present,
      legacy_fixed_output_cap > 0
          ? static_cast<unsigned int>(legacy_fixed_output_cap)
          : 0u,
      mfgunlock::framecount::g_force_multiplier.load(
          std::memory_order_relaxed));
  mfgunlock::framecount::g_reflex_source_fps_cap.store(
      source_cap.output_fps, std::memory_order_relaxed);
  g_source_cap_config_origin.store(
      static_cast<unsigned int>(source_cap.origin),
      std::memory_order_relaxed);
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ReflexModeOverride", value)) {
    mfgunlock::reflexpacing::g_mode_override.store(
        static_cast<uint32_t>(
            mfgunlock::pacing::NormalizeReflexModeOverride(
                static_cast<uint32_t>(value))),
        std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ReflexPacingMethod", value)) {
    mfgunlock::reflexpacing::g_pacing_method.store(
        static_cast<uint32_t>(
            mfgunlock::pacing::NormalizeReflexPacingMethod(
                static_cast<uint32_t>(value))),
        std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ReflexVrrHeadroomEnabled", value)) {
    mfgunlock::reflexpacing::g_headroom_enabled.store(
        value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "ReflexVrrHeadroomBasisPoints", value)) {
    mfgunlock::reflexpacing::g_headroom_basis_points.store(
        mfgunlock::pacing::NormalizeHeadroomBasisPoints(
            static_cast<uint32_t>(value)),
        std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection,
                                "LatencyGuardMode", value)) {
    if (value < static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kOff) ||
        value > static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)) {
      value = static_cast<int>(
          mfgunlock::pacing::LatencyGuardMode::kMonitor);
    }
    mfgunlock::framecount::g_latency_guard_mode.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "HDRCompatibilityMode", value)) {
    if (value < static_cast<int>(mfgunlock::framecount::HdrCompatibilityMode::kNative) ||
        value > static_cast<int>(
                    mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
      value = static_cast<int>(
          mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid);
    }
    mfgunlock::framecount::g_hdr_compatibility_mode.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  } else if (reshade::get_config_value(nullptr, kConfigSection,
                                       "HDRUIRecomposition", value)) {
    // Preserve an explicit legacy opt-in. A legacy false value represented the
    // absence of that experiment, not a deliberate request to bypass every
    // quality guard, so migrate it to the new recommended automatic mode.
    mfgunlock::framecount::g_hdr_compatibility_mode.store(
        static_cast<unsigned int>(
            value != 0 ? mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition
                       : mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid),
        std::memory_order_relaxed);
  }
  // When neither the current nor legacy key exists, this is a fresh
  // configuration and the atomic's Native default remains active.
  if (reshade::get_config_value(nullptr, kConfigSection, "DepthEdgeGuardLevel", value)) {
    if (value < 0 || value > 4) value = 0;
    mfgunlock::framecount::g_depth_edge_guard_level.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  } else if (reshade::get_config_value(nullptr, kConfigSection,
                                       "DisocclusionGuardLevel", value)) {
    // Preserve the selection made by the earlier experimental build while the
    // renamed key makes its purpose clearer going forward.
    if (value < 0 || value > 4) value = 0;
    mfgunlock::framecount::g_depth_edge_guard_level.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  const auto stability_mode = mfgunlock::cudatemporal::NormalizeMode(
      g_adaptive_quality_v3_stability_mode.load(std::memory_order_relaxed));
  std::ostringstream stability_log;
  stability_log << "mfgunlock: Adaptive Quality V3.2 stability requested=";
  if (stability_mode ==
      mfgunlock::cudatemporal::StabilityMode::kTemporal) {
    stability_log
        << "Temporal Stable; effective=Local Stable until exact-provider CUDA probe validation.";
  } else {
    stability_log << "Local Stable; effective=Local Stable; CUDA history disabled.";
  }
  reshade::log::message(reshade::log::level::info,
                        stability_log.str().c_str());
  const auto inpaint_mode =
      mfgunlock::cudatemporal::NormalizeInpaintMode(
          g_adaptive_quality_v3_inpaint_mode.load(
              std::memory_order_relaxed));
  std::ostringstream inpaint_log;
  inpaint_log << "mfgunlock: Adaptive Quality V3.4 inpaint requested="
              << (inpaint_mode ==
                          mfgunlock::cudatemporal::InpaintMode::kTemporal
                      ? "Temporal V3; effective=Local V3 until exact kernel/ABI/phase validation."
                      : inpaint_mode ==
                                mfgunlock::cudatemporal::InpaintMode::kLocal
                            ? "Local V3; confidence history disabled."
                            : "V2 Compatibility; explicit compatibility mode or invalid-value fallback.");
  reshade::log::message(reshade::log::level::info,
                        inpaint_log.str().c_str());
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: Local low-overhead build; V2 inpaint; confidence history, CUDA/NVAPI launch hooks and backend barriers disabled. Saved temporal keys preserved but ignored; native pacing remains the default. Output caps require a native sleep path; Game controlled accepts fresh driver-confirmed MFG.");
#if defined(MFGUNLOCK_RELEASE_1_4_5)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: Release 1.4.5 Engineering; exact content profiles; offline validated only; restart required after first Present.");
#elif defined(MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: Experimental 1.4.4 Engineering; exact content profiles; offline validated only; restart required after first Present.");
#elif defined(MFGUNLOCK_RELEASE_1_4_4)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: Release 1.4.4; DLSS-G 310.9.16 + Streamline 2.14.16 hotfix; exact provider and kernel validation; Local Stable geometry; V2 inpaint.");
#elif defined(MFGUNLOCK_RELEASE_1_4_3)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: Release 1.4.3; continuous border axes (48 registers, zero local/spill); paired diagonal consensus; V2 inpaint; no new resource reads.");
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: 1.4.2-base Border Axis Candidate; both border axes interpolated continuously; no new resource reads; in-game validation pending.");
#elif defined(MFGUNLOCK_LOCAL_STABILITY)
  reshade::log::message(reshade::log::level::info,
      "mfgunlock: Release 1.4.2; continuous subpixel border (48 registers, zero local/spill); paired diagonal consensus (40 registers, 39680 B text, 7776 B shared, zero local/spill); InputMvecProcessing unchanged.");
#endif
#endif
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG Unlock";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    "MFG Unlock 1.7.0 - DLSS-G and XeMFG backends";
#elif defined(MFGUNLOCK_DUAL_BACKEND)
    "Dual Backend Preview X1 - official 1.4.5 base; DLSS-G and XeMFG via OptiScaler; gameplay validation pending";
#elif defined(MFGUNLOCK_RELEASE_1_4_5)
    "Release 1.4.5 - Engineering & Reliability; Local Stable; V2 Compatibility; in-game validation pending";
#elif defined(MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
    "Experimental 1.4.4 Engineering - content profiles, transactional patches, resource lifetime and acknowledged pacing trials; in-game validation pending";
#elif defined(MFGUNLOCK_RELEASE_1_4_4)
    "Release 1.4.4 - DLSS-G 310.9.16 Hotfix; Streamline 2.14.16; Local low-overhead; V2 inpaint; no confidence history or CUDA/NVAPI launch interception";
#elif defined(MFGUNLOCK_RELEASE_1_4_3)
    "Release 1.4.3 - Continuous Border Stability; Local low-overhead; V2 inpaint; no confidence history or CUDA/NVAPI launch interception";
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
    "1.4.2-base - Border Axis Candidate; Local low-overhead; V2 inpaint; no confidence history or CUDA/NVAPI launch interception";
#elif defined(MFGUNLOCK_LOCAL_STABILITY)
    "Release 1.4.2 - Local Stability & CPU Overhead; Local low-overhead; V2 inpaint; no confidence history or CUDA/NVAPI launch interception";
#elif defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
    "Local low-overhead; V2 inpaint; no confidence history or CUDA/NVAPI launch interception";
#else
    "Reports DLSSG.MultiFrameCountMax so Streamline offers multi-frame generation";
#endif

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH: {
      g_self_module = h_module;
      if (!reshade::register_addon(h_module)) return FALSE;
      const auto saved_backend = mfgunlock::backend::Read();
      mfgunlock::backend::active = mfgunlock::backend::Effective(saved_backend);
      if (mfgunlock::backend::active != mfgunlock::backend::Kind::DlssG) {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
        if(mfgunlock::backend::active==mfgunlock::backend::Kind::XeMfg) {
          // XeMFG pins this module, so DllMain never runs again in this process.
          // A game that unloads ReShade (RDR2 with another addon present) would
          // leave a later ReShade instance without this addon; ReShade stays too.
          HMODULE reshade_module = nullptr;
          if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                  reinterpret_cast<LPCWSTR>(reshade::internal::get_reshade_module_handle()), &reshade_module)) {
            reshade::log::message(reshade::log::level::warning,
                "mfgunlock: could not keep ReShade resident; the addon may not survive a ReShade reload.");
          }
          mfgunlock::xemfg::native::Attach(h_module);
        }
#endif
        reshade::register_overlay("MFG Unlock", OnRegisterOverlay);
        reshade::log::message(reshade::log::level::info,
            "mfgunlock: XeMFG/disabled startup; DLSS-G writers, discovery and Reflex controls bypassed.");
        break;
      }
      // 1.5.2: the load context for reports (the DLSS-G route maps only the
      // 1.4.5 static imports; XeMFG's DLLs are delay-loaded).
      reshade::log::message(reshade::log::level::info,
          saved_backend == mfgunlock::backend::Kind::Invalid
              ? (HasEarlyLoadEntry() ? "mfgunlock: DLSS-G backend (saved backend preference invalid; DLSS-G used); loaded from ReShade DllMain"
                                     : "mfgunlock: DLSS-G backend (saved backend preference invalid; DLSS-G used); normal addon loading")
              : (HasEarlyLoadEntry() ? "mfgunlock: DLSS-G backend; loaded from ReShade DllMain"
                                     : "mfgunlock: DLSS-G backend; normal addon loading"));
      LoadConfig();
      g_is_star_wars_outlaws = CurrentProcessNameIs(L"Outlaws.exe");
      g_is_monster_hunter_wilds =
          CurrentProcessNameIs(L"MonsterHunterWilds.exe");
      if (g_is_star_wars_outlaws) {
        mfgunlock::framecount::g_outlaws_get_state_compat.store(
            true, std::memory_order_relaxed);
        mfgunlock::framecount::g_dynamic_game_compat_blocked.store(
            true, std::memory_order_relaxed);
        reshade::log::message(
            reshade::log::level::info,
            "mfgunlock: Star Wars Outlaws compatibility path active: native startup state and telemetry retained; Dynamic MFG is blocked while fixed/game-controlled MFG remains available.");
      }
      if (g_is_monster_hunter_wilds) {
        int acknowledged_revision = 0;
        const bool acknowledged = reshade::get_config_value(
            nullptr, kConfigSection,
            "MonsterHunterRestartNoticeRevision", acknowledged_revision);
        const bool pending =
            !acknowledged ||
            acknowledged_revision < kMonsterHunterRestartNoticeRevision;
        g_monster_hunter_restart_notice_pending.store(
            pending, std::memory_order_release);
        if (pending) {
          reshade::log::message(
              reshade::log::level::warning,
              "mfgunlock: Monster Hunter Wilds first-launch notice pending: let shader.cache2 compilation finish, then fully restart the game before evaluating frame pacing.");
        }
      }
      if (mfgunlock::framecount::g_runtime_selection_mode.load(
              std::memory_order_relaxed) !=
              static_cast<unsigned int>(
                  mfgunlock::framecount::RuntimeSelectionMode::kGameDefault) &&
          !HasEarlyLoadEntry()) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: a non-default Streamline runtime policy is selected, but the addon is not in ADDON.LoadFromDllMain. Games that call slInit before normal addon loading require early loading; use the overlay button and restart.");
      }

      // Current providers keep their native pacing unless the user explicitly
      // requests the legacy software-flip compatibility path. Only that opt-in
      // path requires a verified field patch before a raised count is sent.
      mfgunlock::framecount::g_ensure_pacing = []() { TryPatchFlipMetering(); };
      mfgunlock::framecount::g_pacing_ready = []() {
        return mfgunlock::pacing::IsReady(
            g_force_flip_meter_off.load(std::memory_order_relaxed),
            g_flip_meter_patched.load(std::memory_order_acquire));
      };

      // Install load-time discovery before the bootstrap scan. Already mapped
      // providers are found by that one shared scan; anything mapped later is
      // patched directly here without enumerating modules from Present.
      mfgunlock::loadhook::g_on_interposer_loaded = []() { mfgunlock::framecount::TryInstall(); };
      mfgunlock::loadhook::g_on_dlssg_loaded = ProcessLoadedDlssgModule;
      mfgunlock::loadhook::g_on_dlssg_plugin_loaded = ProcessLoadedStreamlineDlssgPlugin;
      mfgunlock::loadhook::g_on_ngx_loaded = []() { TryInstallPresetHook(); };
      mfgunlock::loadhook::TryInstall();
      RunProviderMaintenance();
      TryPatchFrameCountCeiling();
      if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
      mfgunlock::framecount::TryInstall();

      reshade::register_overlay("MFG Unlock", OnRegisterOverlay);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::register_event<reshade::addon_event::present>(OnPresentStartDiscovery);
      reshade::register_event<reshade::addon_event::finish_present>(OnFinishPresent);
      break;
    }
    case DLL_PROCESS_DETACH:
      // During process termination Windows is already reclaiming every mapped
      // image. Avoid detour transactions and stale provider-memory restores
      // under the loader lock; explicit addon unload still performs cleanup.
      if (lpv_reserved != nullptr) break;
      if (mfgunlock::backend::active != mfgunlock::backend::Kind::DlssG) {
        reshade::unregister_overlay("MFG Unlock", OnRegisterOverlay);
        reshade::unregister_addon(h_module);
        break;
      }
      reshade::unregister_event<reshade::addon_event::finish_present>(OnFinishPresent);
      reshade::unregister_event<reshade::addon_event::present>(OnPresentStartDiscovery);
      reshade::unregister_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_overlay("MFG Unlock", OnRegisterOverlay);
      AcquireSRWLockExclusive(&g_reflex_waitable.lock);
      if (g_reflex_waitable.owner != nullptr) {
        RestoreReflexWaitableLocked(
            g_reflex_waitable.owner,
            mfgunlock::reflexpacing::FallbackReason::kNotRequested);
      }
      ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
      mfgunlock::cudatemporal::Uninstall();
#endif
      mfgunlock::loadhook::Uninstall();
      mfgunlock::framecount::Uninstall();
      RestoreThinGeometry();
      RestoreMidpoint();
      RestoreDlssgArchGate();
      RestoreFrameCountCeiling();
      RestoreFlipMetering();
      reshade::unregister_addon(h_module);
      break;
    default:
      break;
  }
  return TRUE;
}
