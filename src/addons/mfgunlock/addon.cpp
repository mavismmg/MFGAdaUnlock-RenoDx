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
 *   this file      arch gates, flip metering, the plugin's frame ceiling,
 *                  config, overlay, and the fallback parameter override
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
#include "./cuda_temporal.hpp"
#include "./quality_build_policy.hpp"
#include "./pacing_policy.hpp"
#include "./thin_geometry.hpp"
#include "./validation_status.hpp"
#include "./blackwell.hpp"

namespace {

constexpr const char* kConfigSection = "RenoDX.MFGUnlock";
constexpr const char* kParamName = "DLSSG.MultiFrameCountMax";

// Slot 11 of NVSDK_NGX_Parameter == Get(const char*, unsigned int*).
constexpr size_t kGetUInt32Slot = 11;

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

// Our own image. Both marker scans look for strings that are, necessarily,
// string literals inside this very DLL -- so without excluding ourselves the
// scan happily identifies the addon as the DLSS-G plugin and then fails to
// make sense of it. Harmless where the real plugin is enumerated first;
// fatal where it is not loaded at all.

std::atomic_bool g_vtable_patched{false};
std::atomic_bool g_override_reported{false};
std::atomic<unsigned int> g_override_hits{0};
std::atomic<unsigned int> g_runtime_reported_value{0};

void** g_patched_slot = nullptr;
void* g_original_slot_value = nullptr;

using GetUInt32Fn = NVSDK_NGX_Result (*)(void* self, const char* name, unsigned int* out);
GetUInt32Fn g_real_get_uint32 = nullptr;

bool NgxFailed(NVSDK_NGX_Result result) {
  return (static_cast<unsigned int>(result) & 0xfff00000u) == 0xbad00000u;
}

NVSDK_NGX_Result HookedGetUInt32(void* self, const char* name, unsigned int* out) {
  NVSDK_NGX_Result result = g_real_get_uint32(self, name, out);

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
  if (params == nullptr) return false;
  if (g_vtable_patched.load(std::memory_order_acquire)) return true;

  auto** vtable = *reinterpret_cast<void***>(params);
  if (vtable == nullptr) return false;
  void** slot = &vtable[kGetUInt32Slot];

  DWORD old_protect = 0;
  if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect) == 0) {
    reshade::log::message(reshade::log::level::error,
                          "mfgunlock: could not make the NGX parameter vtable writable.");
    return false;
  }

  g_original_slot_value = *slot;
  g_real_get_uint32 = reinterpret_cast<GetUInt32Fn>(g_original_slot_value);
  *slot = reinterpret_cast<void*>(&HookedGetUInt32);
  g_patched_slot = slot;

  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void*), old_protect, &ignored);

  g_vtable_patched.store(true, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "mfgunlock: NGX parameter vtable patched; multi-frame capability override armed.");
  return true;
}

void RestoreParameterVTable() {
  if (!g_vtable_patched.load(std::memory_order_acquire)) return;
  if (g_patched_slot == nullptr || g_original_slot_value == nullptr) return;

  DWORD old_protect = 0;
  if (VirtualProtect(g_patched_slot, sizeof(void*), PAGE_READWRITE, &old_protect) != 0) {
    *g_patched_slot = g_original_slot_value;
    DWORD ignored = 0;
    VirtualProtect(g_patched_slot, sizeof(void*), old_protect, &ignored);
  }
  g_vtable_patched.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------- NGX entries

using NgxParamsOutFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);

NgxParamsOutFn g_real_allocate_parameters = nullptr;
NgxParamsOutFn g_real_get_capability_parameters = nullptr;
NgxParamsOutFn g_real_get_device_capability_parameters = nullptr;
NgxParamsOutFn g_real_get_parameters = nullptr;

NVSDK_NGX_Result NVSDK_CONV HookedAllocateParameters(NVSDK_NGX_Parameter** out_parameters) {
  NVSDK_NGX_Result result = g_real_allocate_parameters(out_parameters);
  if (!NgxFailed(result) && out_parameters != nullptr) PatchParameterVTable(*out_parameters);
  return result;
}

NVSDK_NGX_Result NVSDK_CONV HookedGetCapabilityParameters(NVSDK_NGX_Parameter** out_parameters) {
  NVSDK_NGX_Result result = g_real_get_capability_parameters(out_parameters);
  if (!NgxFailed(result) && out_parameters != nullptr) PatchParameterVTable(*out_parameters);
  return result;
}

NVSDK_NGX_Result NVSDK_CONV HookedGetDeviceCapabilityParameters(
    NVSDK_NGX_Parameter** out_parameters) {
  NVSDK_NGX_Result result = g_real_get_device_capability_parameters(out_parameters);
  if (!NgxFailed(result) && out_parameters != nullptr) PatchParameterVTable(*out_parameters);
  return result;
}

NVSDK_NGX_Result NVSDK_CONV HookedGetParameters(NVSDK_NGX_Parameter** out_parameters) {
  NVSDK_NGX_Result result = g_real_get_parameters(out_parameters);
  if (!NgxFailed(result) && out_parameters != nullptr) PatchParameterVTable(*out_parameters);
  return result;
}

// All four hand out a parameter block, and which one Streamline uses for the
// capability query is not something we can know from outside. They all live in
// the NGX loader, so hooking the set costs nothing extra -- and missing the one
// that is actually used would look exactly like the addon doing nothing.
const std::vector<mfgunlock::hook::HookItem> kNgxHooks = {
    {"NVSDK_NGX_D3D12_AllocateParameters",
     reinterpret_cast<void**>(&g_real_allocate_parameters),
     reinterpret_cast<void*>(&HookedAllocateParameters)},
    {"NVSDK_NGX_D3D12_GetCapabilityParameters",
     reinterpret_cast<void**>(&g_real_get_capability_parameters),
     reinterpret_cast<void*>(&HookedGetCapabilityParameters)},
    {"NVSDK_NGX_D3D12_GetDeviceCapabilityParameters",
     reinterpret_cast<void**>(&g_real_get_device_capability_parameters),
     reinterpret_cast<void*>(&HookedGetDeviceCapabilityParameters)},
    {"NVSDK_NGX_D3D12_GetParameters",
     reinterpret_cast<void**>(&g_real_get_parameters),
     reinterpret_cast<void*>(&HookedGetParameters)},
};

// Only the NGX loader hands out parameter blocks; the feature snippets do not
// export these.
constexpr const wchar_t* kNgxModules[] = {L"_nvngx.dll", L"nvngx.dll"};

std::atomic_bool g_hooked{false};
int g_hook_attempts = 0;
constexpr int kMaxHookAttempts = 8;

// Resolved, not hooked -- used only to hand back the block we allocate below.
NgxParamsOutFn g_real_destroy_parameters = nullptr;

void TryInstallHooks() {
  if (g_hooked.load(std::memory_order_acquire)) return;
  if (g_hook_attempts >= kMaxHookAttempts) return;

  for (const auto* name : kNgxModules) {
    if (name == nullptr) continue;
    HMODULE mod = GetModuleHandleW(name);
    if (mod == nullptr) continue;
    if (GetProcAddress(mod, "NVSDK_NGX_D3D12_AllocateParameters") == nullptr) continue;

    ++g_hook_attempts;
    char narrow[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    if (!mfgunlock::hook::Install(mod, kNgxHooks, narrow)) continue;

    g_real_destroy_parameters = reinterpret_cast<NgxParamsOutFn>(
        reinterpret_cast<void*>(GetProcAddress(mod, "NVSDK_NGX_D3D12_DestroyParameters")));

    g_hooked.store(true, std::memory_order_release);
    return;
  }
}

// Every parameter object shares one vtable, so we do not have to wait for the
// game to hand us one: allocate a throwaway block ourselves, take the vtable
// from it, and give it straight back. Before NGX is initialised this just
// returns an error and we retry on the next present.
//
// Waiting passively would mean the override arms only once DLSS-G is already
// initialising, which is a race against the very query we want to answer.
int g_bootstrap_attempts = 0;
constexpr int kMaxBootstrapAttempts = 2000;

void TryBootstrapVTable() {
  if (g_vtable_patched.load(std::memory_order_acquire)) return;
  if (!g_hooked.load(std::memory_order_acquire)) return;
  if (g_real_allocate_parameters == nullptr) return;
  if (g_bootstrap_attempts >= kMaxBootstrapAttempts) return;
  ++g_bootstrap_attempts;

  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Result result = g_real_allocate_parameters(&params);
  if (NgxFailed(result) || params == nullptr) return;

  PatchParameterVTable(params);

  if (g_real_destroy_parameters != nullptr) {
    reinterpret_cast<NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*)>(
        reinterpret_cast<void*>(g_real_destroy_parameters))(params);
  }
}

// ------------------------------------------------------- in-memory arch gate
//
// The read-side override assumes Streamline reads the capability through the
// NGX loader's parameter object. It does not: the vtable patch arms fine and
// then never fires, because sl.* carries its own NVSDK_NGX_Parameter
// implementation and we never see that vtable.
//
// So patch the decision instead of the answer. NGX verifies the snippet's
// Authenticode signature when it LOADS the file -- which is why the on-disk
// byte patch made frame generation disappear entirely. Editing the same bytes
// in the mapped image afterwards is never re-checked, so the signed DLL loads
// and then behaves like the patched one.
//
// The instruction, in DLSSGInstanceManager::PopulateParameters:
//     81 FD B0 01 00 00     cmp ebp, 0x1b0     ; arch id, 0x1b0 == GB20x
// Exactly one occurrence in .text of 310.8, which is what makes this safe to
// find by pattern. 0x1b0 -> 0x190 lets AD10x take the Blackwell path.

// nvngx_dlssg.dll gates multi-frame on the NVAPI arch id in more than one
// place, and they do different jobs:
//
//   DLSSGInstanceManager::PopulateParameters   -- decides what to advertise
//     81 FD B0 01 00 00   cmp ebp, 0x1b0     ; 0x1b0 == GB20x (RTX 50)
//     jl  <report max = 1>
//
//   ...and a separate runtime capability flag that drives generation itself:
//     3D B0 01 00 00      cmp eax, 0x1b0
//     0F 93 C0            setae al
//     88 47 28            mov byte ptr [rdi+0x28], al
//
// Patching only the first is what produced 3x/4x rendering black: the runtime
// advertised multi-frame, accepted the request, and then took the non-Blackwell
// path when actually generating, so the extra frames were presented empty.
//
// So rewrite every comparison against the Blackwell arch id, in both encodings.
// 0x1b0 is a specific NVAPI arch constant, and any compare against it in this
// DLL is an arch gate -- but a `mov r32, 0x1b0` is the arch-id lookup table
// returning Blackwell's own id, which must be left alone. Only `cmp` forms are
// rewritten.
//
// NGX verifies the snippet's Authenticode signature when it LOADS the file --
// which is why patching the same bytes on disk made frame generation vanish.
// The mapped image is never re-checked, so the signed DLL loads and then
// behaves as patched.

// ------------------------------------------------ locating the DLSS-G snippet
//
// Finding it as GetModuleHandleW(L"nvngx_dlssg.dll") is the same mistake that
// already cost us a silent no-op on the Streamline side: NGX can load the
// snippet from the driver's OTA store, and a game may stage it under another
// path. The name is a fast path, not a contract.
//
// The game-folder DLL and the driver's ...\models\dlssg\... OTA path are
// unambiguous. For renamed providers elsewhere, fall back to the NGX provider
// export plus the older "dlfg_kernel" descriptor. This matters in STALKER 2,
// whose active provider is an opaque .bin from the driver cache and whose
// current build no longer contains that descriptor.

constexpr char kDlssgMarker[] = "dlfg_kernel";

std::vector<HMODULE> g_inspected_modules;
std::vector<HMODULE> g_dlssg_modules;
SRWLOCK g_provider_maintenance_lock = SRWLOCK_INIT;
std::atomic_bool g_provider_rescan_requested{false};

bool ModuleContains(HMODULE mod, const char* needle, size_t needle_len) {
  auto* base = reinterpret_cast<unsigned char*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER)) != 0) return false;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < needle_len) continue;
    for (size_t off = 0; off + needle_len <= size; ++off) {
      if (std::memcmp(start + off, needle, needle_len) == 0) return true;
    }
  }
  return false;
}

void RememberDlssgModule(HMODULE mod) {
  if (mod == nullptr || mod == g_self_module) return;
  if (std::find(g_dlssg_modules.begin(), g_dlssg_modules.end(), mod) == g_dlssg_modules.end()) {
    g_dlssg_modules.push_back(mod);
  }
}

bool HasKnownDlssgPath(HMODULE mod) {
  std::vector<wchar_t> module_path(32768);
  const DWORD length = GetModuleFileNameW(
      mod, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length == 0 || length >= module_path.size()) return false;
  for (DWORD i = 0; i < length; ++i) {
    if (module_path[i] >= L'A' && module_path[i] <= L'Z') {
      module_path[i] = static_cast<wchar_t>(module_path[i] - L'A' + L'a');
    }
  }
  return std::wcsstr(module_path.data(), L"nvngx_dlssg") != nullptr ||
         std::wcsstr(module_path.data(), L"\\models\\dlssg\\") != nullptr;
}

bool IsDlssgProvider(HMODULE mod) {
  // Keep the established D3D/OTA path as the fast path. Vulkan-specific export
  // checks are only needed when a game has renamed or relocated the provider.
  if (HasKnownDlssgPath(mod)) return true;

  // A renamed provider may expose either graphics backend. Streamline's
  // slDLSSGSetOptions/slDLSSGGetState interface is renderer-independent, so
  // discovery must not discard Vulkan snippets before the shared patch path
  // gets a chance to inspect them.
  const bool has_d3d12_entry =
      GetProcAddress(mod, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") != nullptr;
  const bool has_vulkan_entry =
      GetProcAddress(mod, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") != nullptr;

  // Retain content-based discovery for games that rename or relocate the
  // snippet, but only scan modules exposing an NGX provider entry point.
  if (!has_d3d12_entry && !has_vulkan_entry) return false;
  return ModuleContains(mod, kDlssgMarker, sizeof(kDlssgMarker) - 1);
}

// Perform one shared bootstrap pass for providers that were mapped before this
// addon. Providers mapped later are handled directly by the loader hook, so
// module enumeration never has to run from the presentation thread.
const std::vector<HMODULE>& DiscoverDlssgModules() {
  if (HMODULE fast = GetModuleHandleW(L"nvngx_dlssg.dll")) RememberDlssgModule(fast);

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return g_dlssg_modules;
  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      if (me.hModule == g_self_module) continue;
      if (std::find(g_inspected_modules.begin(), g_inspected_modules.end(), me.hModule) !=
          g_inspected_modules.end()) {
        continue;
      }
      g_inspected_modules.push_back(me.hModule);
      if (IsDlssgProvider(me.hModule)) RememberDlssgModule(me.hModule);
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  return g_dlssg_modules;
}

constexpr unsigned char kArchOld = 0xB0;  // 0x1b0 GB20x
constexpr unsigned char kArchNew = 0x90;  // 0x190 AD10x

struct GateSite {
  HMODULE module;
  unsigned char* address;  // the byte holding the arch id's low octet
  unsigned char original;
};

std::atomic_bool g_gate_patched{false};
std::vector<GateSite> g_gate_sites;
std::vector<HMODULE> g_gate_modules;
std::vector<HMODULE> g_gate_rejected_modules;
std::atomic<int> g_gate_attempts{0};

// Split out so the load-time trigger can patch a module it already holds a
// handle to. That path runs under the loader lock, where CreateToolhelp32Snapshot
// (which FindDlssgModule uses) would deadlock -- so it must never scan.
void PatchArchGatesInModule(HMODULE mod) {
  if (mod == nullptr) return;
  if (std::find(g_gate_modules.begin(), g_gate_modules.end(), mod) != g_gate_modules.end()) return;
  if (std::find(g_gate_rejected_modules.begin(), g_gate_rejected_modules.end(), mod) !=
      g_gate_rejected_modules.end()) {
    return;
  }

  auto* base = reinterpret_cast<unsigned char*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;

  std::vector<unsigned char*> found;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 6) continue;
    for (size_t off = 0; off + 6 <= size; ++off) {
      // 3D id32            cmp eax, imm32
      if (start[off] == 0x3D && start[off + 1] == kArchOld && start[off + 2] == 0x01 &&
          start[off + 3] == 0x00 && start[off + 4] == 0x00) {
        found.push_back(start + off + 1);
        continue;
      }
      // 81 /7 id32         cmp r32, imm32
      if (start[off] == 0x81 && start[off + 1] >= 0xF8 && start[off + 1] <= 0xFF &&
          start[off + 2] == kArchOld && start[off + 3] == 0x01 && start[off + 4] == 0x00 &&
          start[off + 5] == 0x00) {
        found.push_back(start + off + 2);
      }
    }
  }

  if (found.empty() || found.size() > 4) {
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: found " << found.size() << " arch-gate comparisons in " << module_path
      << " (expected 1-4); leaving this provider alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_gate_rejected_modules.push_back(mod);
    return;
  }

  const size_t sites_before = g_gate_sites.size();
  for (unsigned char* site : found) {
    DWORD old_protect = 0;
    if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &old_protect) == 0) continue;
    g_gate_sites.push_back({mod, site, *site});
    *site = kArchNew;
    DWORD ignored = 0;
    VirtualProtect(site, 1, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 1);
  }

  const size_t sites_written = g_gate_sites.size() - sites_before;
  if (sites_written == 0) {
    reshade::log::message(reshade::log::level::error,
                          "mfgunlock: could not make the nvngx_dlssg.dll arch gates writable.");
    g_gate_rejected_modules.push_back(mod);
    return;
  }
  g_gate_modules.push_back(mod);
  g_gate_patched.store(true, std::memory_order_release);

  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream s;
  s << "mfgunlock: rewrote " << sites_written << " arch gate(s) (0x1b0 -> 0x190) in "
    << module_path << "; multi-frame should report as supported AND generate.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

void TryPatchDlssgArchGate() {
  ++g_gate_attempts;
  for (HMODULE mod : g_dlssg_modules) PatchArchGatesInModule(mod);
}

void RestoreDlssgArchGate() {
  if (!g_gate_patched.load(std::memory_order_acquire)) return;
  for (const auto& site : g_gate_sites) {
    if (!mfgunlock::runtimeversion::IsMappedImage(site.module)) continue;
    DWORD old_protect = 0;
    if (VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
      *site.address = site.original;
      DWORD ignored = 0;
      VirtualProtect(site.address, 1, old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), site.address, 1);
    }
  }
  g_gate_sites.clear();
  g_gate_modules.clear();
  g_gate_rejected_modules.clear();
  g_gate_patched.store(false, std::memory_order_release);
}

// ------------------------------------------------------ temporal (midpoint)
//
// Unlocking the multipliers gets the right NUMBER of generated frames; this
// gets the right CONTENT. Without it every generated frame is the same 0.5
// blend, so 4x shows three identical half-way frames and the motion is no
// smoother than 2x despite double the counter. See midpoint.hpp.

std::atomic_bool g_midpoint_patched{false};
std::atomic_bool g_blackwell_patched{false};
struct MidpointModulePatch {
  HMODULE module;
  std::vector<mfgunlock::midpoint::Patch> patches;
  void* allocation;
};
std::vector<MidpointModulePatch> g_midpoint_modules;
struct BlackwellModulePatch {
  HMODULE module;
  std::vector<mfgunlock::blackwell::Patch> patches;
  std::vector<void*> allocations;
  mfgunlock::blackwell::Result result;
};
std::vector<BlackwellModulePatch> g_blackwell_modules;
std::vector<HMODULE> g_midpoint_rejected_modules;
std::string g_midpoint_detail;
std::string g_blackwell_detail;
std::string g_blackwell_applied_detail;
std::atomic<int> g_midpoint_attempts{0};

struct ThinGeometryModulePatch {
  HMODULE module = nullptr;
  std::string provider_version;
  std::vector<mfgunlock::thingeometry::Redirect> redirects;
  mfgunlock::thingeometry::Result result;
  mfgunlock::thingeometry::MechanismResult intermediate_scatter;
  mfgunlock::thingeometry::MechanismResult silhouette_boundary_guard;
  bool geometry_confidence_v2_applied = false;
  bool geometry_v3_redirected = false;
};

mfgunlock::blackwell::Result AggregateBlackwellResults(
    const std::vector<BlackwellModulePatch>& modules) {
  mfgunlock::blackwell::Result result{};
  for (const auto& module : modules) {
    result.motion_vector |= module.result.motion_vector;
    result.inpaint |= module.result.inpaint;
    result.inpaint_decision |= module.result.inpaint_decision;
    result.intermediate_scatter |= module.result.intermediate_scatter;
    result.silhouette_guard |= module.result.silhouette_guard;
    result.refined_geometry |= module.result.refined_geometry;
    result.geometry_confidence_v2 |= module.result.geometry_confidence_v2;
    result.adaptive_quality_requested |= module.result.adaptive_quality_requested;
    result.adaptive_geometry |= module.result.adaptive_geometry;
    result.adaptive_geometry_redirected |=
        module.result.adaptive_geometry_redirected;
    result.adaptive_geometry_version =
        mfgunlock::adaptivequality::MergeComponentVersions(
            result.adaptive_geometry_version,
            module.result.adaptive_geometry_version);
    if (result.adaptive_geometry_variant ==
        mfgunlock::blackwell::AdaptiveGeometryVariant::kNone) {
      result.adaptive_geometry_variant =
          module.result.adaptive_geometry_variant;
    } else if (module.result.adaptive_geometry_variant !=
                   mfgunlock::blackwell::AdaptiveGeometryVariant::kNone &&
               result.adaptive_geometry_variant !=
                   module.result.adaptive_geometry_variant) {
      result.adaptive_geometry_variant =
          mfgunlock::blackwell::AdaptiveGeometryVariant::kMixed;
    }
    result.adaptive_inpaint_decision |= module.result.adaptive_inpaint_decision;
    result.adaptive_inpaint_version =
        mfgunlock::adaptivequality::MergeComponentVersions(
            result.adaptive_inpaint_version,
            module.result.adaptive_inpaint_version);
    result.adaptive_inpaint_redirected |=
        module.result.adaptive_inpaint_redirected;
    if (result.adaptive_inpaint_variant ==
        mfgunlock::blackwell::AdaptiveInpaintVariant::kNone) {
      result.adaptive_inpaint_variant =
          module.result.adaptive_inpaint_variant;
    } else if (module.result.adaptive_inpaint_variant !=
                   mfgunlock::blackwell::AdaptiveInpaintVariant::kNone &&
               result.adaptive_inpaint_variant !=
                   module.result.adaptive_inpaint_variant) {
      result.adaptive_inpaint_variant =
          mfgunlock::blackwell::AdaptiveInpaintVariant::kMixed;
    }
    result.adaptive_directional_scatter |= module.result.adaptive_directional_scatter;
    result.adaptive_fallback |= module.result.adaptive_fallback;
    result.kernels += module.result.kernels;
  }
  return result;
}
std::vector<ThinGeometryModulePatch> g_thin_geometry_modules;
std::atomic_bool g_thin_geometry_patched{false};
std::atomic<int> g_thin_geometry_attempts{0};

bool ThinGeometryRequested() {
  return g_adaptive_quality.load(std::memory_order_relaxed) ||
         g_thin_geometry_validated_warp_blend.load(std::memory_order_relaxed) ||
         g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
         g_thin_geometry_intermediate_scatter.load(std::memory_order_relaxed) ||
         g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
             static_cast<unsigned int>(
                 mfgunlock::blackwell::SilhouetteGuardMode::Off);
}

bool ModuleHasThinGeometryResult(HMODULE mod) {
  return std::any_of(
      g_thin_geometry_modules.begin(), g_thin_geometry_modules.end(),
      [mod](const ThinGeometryModulePatch& patch) { return patch.module == mod; });
}

bool ModuleHasTemporalPatch(HMODULE mod) {
  return std::any_of(g_midpoint_modules.begin(), g_midpoint_modules.end(),
                     [mod](const MidpointModulePatch& patch) { return patch.module == mod; }) ||
         std::any_of(g_blackwell_modules.begin(), g_blackwell_modules.end(),
                     [mod](const BlackwellModulePatch& patch) { return patch.module == mod; });
}

bool PatchBlackwellInModule(HMODULE mod) {
  if (mod == nullptr || ModuleHasTemporalPatch(mod)) return false;
  std::vector<mfgunlock::blackwell::Patch> patches;
  std::vector<void*> allocations;
  mfgunlock::blackwell::Result result;
  std::string detail;
  std::string provider_version;
  std::string provider_reason;
  const bool supported_thin_geometry_provider =
      mfgunlock::thingeometry::IsSupportedProvider(
          mod, provider_version, provider_reason);
  const bool adaptive_quality =
      g_adaptive_quality.load(std::memory_order_relaxed);
  const bool enable_intermediate_scatter =
      !adaptive_quality &&
      g_thin_geometry_intermediate_scatter.load(std::memory_order_relaxed) &&
      supported_thin_geometry_provider;
  const auto silhouette_guard_mode = supported_thin_geometry_provider
                                         ? (adaptive_quality
                                                ? mfgunlock::blackwell::SilhouetteGuardMode::Balanced
                                                : static_cast<mfgunlock::blackwell::SilhouetteGuardMode>(
                                                      g_silhouette_guard_mode.load(
                                                          std::memory_order_relaxed)))
                                         : mfgunlock::blackwell::SilhouetteGuardMode::Off;
  if (!mfgunlock::blackwell::Apply(mod, patches, allocations, result, detail,
                                   enable_intermediate_scatter,
                                   silhouette_guard_mode,
                                   adaptive_quality &&
                                       supported_thin_geometry_provider)) {
    g_blackwell_detail = detail;
    return false;
  }

  g_blackwell_detail = detail;
  g_blackwell_applied_detail = detail;
  g_blackwell_modules.push_back(
      {mod, std::move(patches), std::move(allocations), result});
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  if (supported_thin_geometry_provider && adaptive_quality &&
      ((result.adaptive_geometry &&
        result.adaptive_geometry_version ==
            mfgunlock::adaptivequality::ComponentVersion::kV3 &&
        result.adaptive_geometry_variant ==
            mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal) ||
       (result.adaptive_inpaint_decision &&
        result.adaptive_inpaint_variant ==
            mfgunlock::blackwell::AdaptiveInpaintVariant::kTemporal))) {
    // Hook installation is intentionally deferred to a ReShade callback,
    // outside the loader/provider-maintenance critical section.
    mfgunlock::cudatemporal::AuthorizeExactProvider(true);
  } else if (supported_thin_geometry_provider && adaptive_quality &&
             result.adaptive_geometry_version ==
                 mfgunlock::adaptivequality::ComponentVersion::kV3 &&
             result.adaptive_geometry_variant ==
                 mfgunlock::blackwell::AdaptiveGeometryVariant::kLocal &&
             mfgunlock::cudatemporal::NormalizeMode(
                 g_adaptive_quality_v3_stability_mode.load(
                     std::memory_order_relaxed)) ==
                 mfgunlock::cudatemporal::StabilityMode::kTemporal) {
    mfgunlock::cudatemporal::SetDetail(
        "Temporal Stable requested; dedicated Local cubin installed as fallback",
        true);
  }
#endif
  g_blackwell_patched.store(true, std::memory_order_release);
  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream stream;
  stream << "mfgunlock: full Blackwell framework kernels applied to " << module_path << " -- "
         << detail << "; the separate midpoint rewrite is not needed for this provider.";
  reshade::log::message(reshade::log::level::info, stream.str().c_str());
  return true;
}

void LogThinGeometryMechanism(
    const char* module_path, const std::string& provider_version,
    const char* mechanism, const char* application_path,
    const mfgunlock::thingeometry::MechanismResult& result) {
  std::ostringstream stream;
  stream << "mfgunlock: Enhanced thin-geometry interpolation: provider="
         << (module_path[0] == '\0' ? "<unknown>" : module_path)
         << ", version="
         << (provider_version.empty() ? "unsupported/unknown" : provider_version)
         << ", mechanism=" << mechanism
         << ", detected=" << (result.detected ? "yes" : "no")
         << ", path=" << application_path
         << ", applied=" << (result.applied ? "yes" : "no")
         << ", validation="
         << (result.detail.empty()
                 ? (result.applied ? "exact provider and payload match" : "not applied")
                 : result.detail);
  reshade::log::message(result.applied ? reshade::log::level::info
                                      : reshade::log::level::warning,
                        stream.str().c_str());
}

void PatchThinGeometryInModule(HMODULE mod) {
  if (mod == nullptr || !ThinGeometryRequested() ||
      ModuleHasThinGeometryResult(mod)) {
    return;
  }
  ++g_thin_geometry_attempts;

  ThinGeometryModulePatch module_result;
  module_result.module = mod;
  const bool adaptive_quality =
      g_adaptive_quality.load(std::memory_order_relaxed);
  const mfgunlock::thingeometry::Options options{
      adaptive_quality ||
          g_thin_geometry_validated_warp_blend.load(std::memory_order_relaxed),
      !adaptive_quality &&
          g_thin_geometry_previous_scatter.load(std::memory_order_relaxed)};
  mfgunlock::thingeometry::Apply(mod, options, module_result.redirects,
                                module_result.result,
                                module_result.provider_version);

  const auto silhouette_guard_mode =
      g_adaptive_quality.load(std::memory_order_relaxed)
          ? mfgunlock::blackwell::SilhouetteGuardMode::Balanced
          : static_cast<mfgunlock::blackwell::SilhouetteGuardMode>(
                g_silhouette_guard_mode.load(std::memory_order_relaxed));
  const bool silhouette_guard_requested =
      silhouette_guard_mode !=
      mfgunlock::blackwell::SilhouetteGuardMode::Off;
  module_result.intermediate_scatter.requested =
      !adaptive_quality &&
      g_thin_geometry_intermediate_scatter.load(std::memory_order_relaxed) &&
      !silhouette_guard_requested;
  module_result.silhouette_boundary_guard.requested =
      silhouette_guard_requested;
  if (module_result.intermediate_scatter.requested) {
    std::string provider_reason;
    if (!mfgunlock::thingeometry::IsSupportedProvider(
            mod, module_result.provider_version, provider_reason)) {
      module_result.intermediate_scatter.detail = provider_reason;
    } else {
      const auto blackwell = std::find_if(
          g_blackwell_modules.begin(), g_blackwell_modules.end(),
          [mod](const BlackwellModulePatch& patch) { return patch.module == mod; });
      if (blackwell == g_blackwell_modules.end()) {
        module_result.intermediate_scatter.detail =
            "requires the exact full Blackwell motion-vector path; current temporal fallback retained";
      } else {
        module_result.intermediate_scatter.detected =
            blackwell->result.intermediate_scatter;
        module_result.intermediate_scatter.applied =
            blackwell->result.intermediate_scatter;
        module_result.intermediate_scatter.detail =
            blackwell->result.intermediate_scatter
                ? "exact original Ada cubin hash matched the generated bounded-retention variant"
                : "exact intermediate-scatter variant did not match; baseline Blackwell cubin retained";
      }
    }
  }

  if (module_result.silhouette_boundary_guard.requested) {
    std::string provider_reason;
    if (!mfgunlock::thingeometry::IsSupportedProvider(
            mod, module_result.provider_version, provider_reason)) {
      module_result.silhouette_boundary_guard.detail = provider_reason;
    } else {
      const auto blackwell = std::find_if(
          g_blackwell_modules.begin(), g_blackwell_modules.end(),
          [mod](const BlackwellModulePatch& patch) { return patch.module == mod; });
      if (blackwell == g_blackwell_modules.end()) {
        module_result.silhouette_boundary_guard.detail =
            "requires the exact full Blackwell motion-vector path; current temporal fallback retained";
      } else {
        module_result.silhouette_boundary_guard.detected =
            blackwell->result.silhouette_guard ||
            blackwell->result.silhouette_guard_fallback;
        module_result.silhouette_boundary_guard.applied =
            blackwell->result.silhouette_guard;
        module_result.geometry_confidence_v2_applied =
            blackwell->result.geometry_confidence_v2;
        module_result.geometry_v3_redirected =
            blackwell->result.adaptive_geometry_redirected;
        if (blackwell->result.silhouette_guard) {
          std::ostringstream detail;
          detail << "requested "
                 << mfgunlock::blackwell::SilhouetteGuardName(
                        blackwell->result.silhouette_guard_mode_requested)
                 << "; selected "
                 << mfgunlock::blackwell::SilhouetteGuardName(
                        blackwell->result.silhouette_guard_mode_selected)
                 << " same-depth local-support variant";
          if (blackwell->result.adaptive_geometry)
            detail << "; asymmetric disocclusion confidence; native directional scatter gating";
          else if (blackwell->result.geometry_confidence_v2)
            detail << "; geometry confidence V2";
          else if (blackwell->result.refined_geometry)
            detail << "; refined confidence V1";
          if (blackwell->result.silhouette_guard_fallback) {
            detail << " as a compatibility fallback";
          }
          if (blackwell->result.adaptive_geometry_redirected) {
            detail << "; full V3 ptxas cubin through exact fatbin descriptor redirect";
          }
          module_result.silhouette_boundary_guard.detail = detail.str();
        } else if (blackwell->result.silhouette_guard_fallback) {
          module_result.silhouette_boundary_guard.detail =
              "guard variant unavailable; released 0.9 intermediate retention was applied as a safe fallback";
        } else {
          module_result.silhouette_boundary_guard.detail =
              "exact guard variant did not match; baseline Blackwell cubin retained";
        }
      }
    }
  }

  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  if (module_result.result.validated_warp_blend.requested) {
    LogThinGeometryMechanism(
        module_path, module_result.provider_version,
        "validated warp blend", "BlendCandidatesFused PTX descriptor redirect",
        module_result.result.validated_warp_blend);
  }
  if (module_result.result.previous_scatter.requested) {
    LogThinGeometryMechanism(
        module_path, module_result.provider_version,
        "previous-to-current scatter retention",
        "EstimatePrev2CurrScatter PTX descriptor redirect",
        module_result.result.previous_scatter);
  }
  if (module_result.intermediate_scatter.requested) {
    LogThinGeometryMechanism(
        module_path, module_result.provider_version,
        "intermediate scatter retention",
        "Blackwell EstimateIntermMvecsScatter in-place cubin selection",
        module_result.intermediate_scatter);
  }
  if (module_result.silhouette_boundary_guard.requested) {
    LogThinGeometryMechanism(
        module_path, module_result.provider_version,
        "silhouette disocclusion guard",
        module_result.geometry_v3_redirected
            ? "Blackwell EstimateIntermMvecsScatter oversized ptxas cubin descriptor redirect"
            : "Blackwell EstimateIntermMvecsScatter same-depth local-support selection",
        module_result.silhouette_boundary_guard);
  }

  if (module_result.result.validated_warp_blend.applied ||
      module_result.result.previous_scatter.applied ||
      module_result.intermediate_scatter.applied ||
      module_result.silhouette_boundary_guard.applied) {
    g_thin_geometry_patched.store(true, std::memory_order_release);
  }
  g_thin_geometry_modules.push_back(std::move(module_result));
}

void PatchMidpointInModule(HMODULE mod) {
  if (mod == nullptr) return;
  if (ModuleHasTemporalPatch(mod)) return;
  if (std::find(g_midpoint_rejected_modules.begin(), g_midpoint_rejected_modules.end(), mod) !=
      g_midpoint_rejected_modules.end()) {
    return;
  }

  std::vector<mfgunlock::midpoint::Patch> patches;
  void* allocation = nullptr;
  std::string detail;
  if (!mfgunlock::midpoint::Apply(mod, patches, allocation, detail)) {
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: temporal fix not applied to " << module_path << " -- " << detail << ".";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_midpoint_rejected_modules.push_back(mod);
    return;
  }

  g_midpoint_detail = detail;
  g_midpoint_modules.push_back({mod, std::move(patches), allocation});
  g_midpoint_patched.store(true, std::memory_order_release);
  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream s;
  s << "mfgunlock: temporal fix applied to " << module_path << " -- " << detail
    << "; generated frames should now land at their own time, not all at the midpoint.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

void PatchTemporalInModule(HMODULE mod) {
  if (mod == nullptr || ModuleHasTemporalPatch(mod)) return;
  // NGX may map a DriverStore fallback only long enough to inspect it and then
  // unload it. Once both full-kernel and midpoint validation rejected a module,
  // never dereference that cached HMODULE again: it may no longer name mapped
  // memory by the next bounded discovery pass.
  if (std::find(g_midpoint_rejected_modules.begin(), g_midpoint_rejected_modules.end(), mod) !=
      g_midpoint_rejected_modules.end()) {
    return;
  }
  const bool prefer_blackwell =
      g_blackwell_framework_kernels.load(std::memory_order_relaxed);
  if (prefer_blackwell) {
    if (PatchBlackwellInModule(mod)) return;
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream stream;
    stream << "mfgunlock: full Blackwell framework path not available for " << module_path
           << " -- " << g_blackwell_detail << "; trying the 0.7 midpoint fallback.";
    reshade::log::message(reshade::log::level::warning, stream.str().c_str());
  }
  PatchMidpointInModule(mod);
}

void TryPatchMidpoint() {
  ++g_midpoint_attempts;
  for (HMODULE mod : g_dlssg_modules) PatchTemporalInModule(mod);
}

// Provider state is normally updated synchronously by the loader hook. The
// bounded fallback worker below may inspect the process at the same time, so
// serialize vector updates and patch bookkeeping. A loader callback must not
// wait here: if maintenance is already in progress it requests another pass
// and returns, avoiding a lock-order inversion with the Windows loader lock.
void ProcessLoadedDlssgModule(HMODULE mod) {
  if (!TryAcquireSRWLockExclusive(&g_provider_maintenance_lock)) {
    g_provider_rescan_requested.store(true, std::memory_order_release);
    return;
  }
  RememberDlssgModule(mod);
  if (g_enabled.load(std::memory_order_relaxed)) PatchArchGatesInModule(mod);
  if (g_enabled.load(std::memory_order_relaxed) &&
      g_temporal_fix.load(std::memory_order_relaxed)) {
    PatchTemporalInModule(mod);
  }
  if (g_enabled.load(std::memory_order_relaxed) && ThinGeometryRequested()) {
    PatchThinGeometryInModule(mod);
  }
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
}

void RunProviderMaintenance() {
  std::vector<HMODULE> version_candidates;
  AcquireSRWLockExclusive(&g_provider_maintenance_lock);
  DiscoverDlssgModules();
  if (g_enabled.load(std::memory_order_relaxed)) TryPatchDlssgArchGate();
  if (g_enabled.load(std::memory_order_relaxed) &&
      g_temporal_fix.load(std::memory_order_relaxed)) {
    TryPatchMidpoint();
  }
  if (g_enabled.load(std::memory_order_relaxed) && ThinGeometryRequested()) {
    for (HMODULE mod : g_dlssg_modules) PatchThinGeometryInModule(mod);
  }
  version_candidates = g_gate_modules;
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
  // File-version APIs are deliberately kept out of the loader callback. Only
  // candidates whose architecture gates were validated are reported here.
  for (HMODULE module : version_candidates) {
    mfgunlock::framecount::ObserveDlssgProviderVersion(module);
  }
}

void RestoreMidpoint() {
  for (auto& module : g_midpoint_modules) {
    if (mfgunlock::runtimeversion::IsMappedImage(module.module)) {
      mfgunlock::midpoint::Restore(module.patches, module.allocation);
    } else {
      module.patches.clear();
      if (module.allocation != nullptr) {
        VirtualFree(module.allocation, 0, MEM_RELEASE);
        module.allocation = nullptr;
      }
    }
  }
  for (auto& module : g_blackwell_modules) {
    if (mfgunlock::runtimeversion::IsMappedImage(module.module)) {
      mfgunlock::blackwell::Restore(module.patches, module.allocations);
    } else {
      module.patches.clear();
      module.allocations.clear();
    }
  }
  g_midpoint_modules.clear();
  g_blackwell_modules.clear();
  g_midpoint_rejected_modules.clear();
  g_blackwell_applied_detail.clear();
  g_midpoint_patched.store(false, std::memory_order_release);
  g_blackwell_patched.store(false, std::memory_order_release);
}

void RestoreThinGeometry() {
  for (auto& module : g_thin_geometry_modules) {
    if (mfgunlock::runtimeversion::IsMappedImage(module.module)) {
      mfgunlock::thingeometry::Restore(module.redirects);
    } else {
      for (auto& redirect : module.redirects) {
        redirect.descriptors.clear();
        if (redirect.allocation != nullptr) {
          VirtualFree(redirect.allocation, 0, MEM_RELEASE);
          redirect.allocation = nullptr;
        }
      }
      module.redirects.clear();
    }
  }
  g_thin_geometry_modules.clear();
  g_thin_geometry_patched.store(false, std::memory_order_release);
}

// ------------------------------------------------- flip metering (sl.dlss_g)
//
// Some older integrations can freeze at 3x/4x after the capability gate is
// opened because their selected flip-metering path never completes on Ada.
// Current Streamline providers normally pace correctly without any mutation,
// so this compatibility path is opt-in rather than part of the unlock.
//
// Streamline already ships the fallback. sl.dlss_g/ngx.cpp logs
// "FG1 DLL has been detected: forcing flip-metering off." and writes a flag on
// the DLSS-G context, dropping it onto the software RSYNC pacer in rsync.cpp.
// That is the path Ada needs.
//
// Two things make this impossible to hardcode, both learned the hard way:
//
//  1. The plugin in bin/x64 is usually NOT the one running. Streamline
//     OTA-updates its plugins into
//     C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\<n>\files\<hash>.dll
//     so GetModuleHandleW(L"sl.dlss_g.dll") finds nothing and a name-based patch
//     silently does nothing at all -- no error, no log line, no effect.
//
//  2. The flag's offset AND ITS POLARITY differ between builds. The game-folder
//     build clears [ctx+0x38bc] to mean "flip metering off"; the OTA build sets
//     [ctx+0x44f8] to 1 to mean the same thing. A hardcoded value is a coin flip
//     that silently does the opposite half the time.
//
// So derive everything from the binary: find the module carrying the marker
// string, find the code that logs it, and read the (offset, value) the fallback
// itself writes. That pair IS the wanted state, whatever its polarity. Then flip
// every other site writing that offset to match.

constexpr char kFlipMarker[] = "FG1 DLL has been detected";

struct FlipSite {
  unsigned char* address;
  unsigned char original[7];
  unsigned char length;
};

std::atomic_bool g_flip_meter_patched{false};
std::vector<FlipSite> g_flip_meter_sites;
std::atomic<unsigned int> g_flip_meter_offset{0};
std::atomic<unsigned int> g_flip_meter_value{0};
std::atomic<int> g_flip_meter_attempts{0};
constexpr int kMaxFlipMeterAttempts = 4000;

// Records the original bytes before writing, so the instruction can be put back
// exactly as it was. Patches here are either one byte (an immediate flipped in
// place) or seven (a whole store rewritten), never anything else.
bool WriteFlipSite(unsigned char* at, const unsigned char* bytes, size_t length) {
  if (length == 0 || length > sizeof(FlipSite::original)) return false;
  DWORD old_protect = 0;
  if (VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old_protect) == 0) return false;
  FlipSite site = {};
  site.address = at;
  site.length = static_cast<unsigned char>(length);
  std::memcpy(site.original, at, length);
  g_flip_meter_sites.push_back(site);
  std::memcpy(at, bytes, length);
  DWORD ignored = 0;
  VirtualProtect(at, length, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), at, length);
  return true;
}

bool ModuleImage(HMODULE mod, unsigned char** out_base, const IMAGE_NT_HEADERS64** out_nt) {
  if (mod == nullptr) return false;
  auto* base = reinterpret_cast<unsigned char*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  *out_base = base;
  *out_nt = nt;
  return true;
}

// ------------------------------------------- Streamline's own frame ceiling
//
// The plugin starts with its own compiled maximum, then lowers it to the value
// reported by NGX:
//
//     BA 03 00 00 00   mov   edx, 3
//     3B CA            cmp   ecx, edx
//     0F 42 D1         cmovb edx, ecx      ; edx = min(count, 3)
//
// `ecx` is the device maximum cached by the Streamline wrapper, not the game's
// current request. Most games observe the rewritten NGX gates early enough for
// it to be 5. STALKER 2 does not: its wrapper caches 1, so this CMOV reduces the
// otherwise-valid compiled maximum back to one generated frame. The native UI
// can then expose 3x/4x, but slDLSSGSetOptions rejects either with
// eErrorInvalidState (38).
//
// Turn the conditional move into `cmovb edx, edx` by changing only its ModRM
// byte (D1 -> D2). This is an atomic one-byte code patch and leaves the
// instruction boundary intact. The immediate stays in place as a hard bound:
// old plugins remain capped at their compiled 3 generated frames (4x), while
// newer plugins keep their compiled 5 (6x). The
// opt-in RaiseFrameCeiling setting may still raise an old plugin's immediate,
// but is deliberately separate because doing so is not safe in every game.

constexpr unsigned char kCeilingTarget = 5;  // generated frames == 6x

std::atomic_bool g_ceiling_patched{false};
std::atomic_bool g_ceiling_plugin_seen{false};
HMODULE g_ceiling_module = nullptr;
std::wstring g_ceiling_module_path;
unsigned char* g_ceiling_site = nullptr;
unsigned char g_ceiling_original = 0;
unsigned char g_ceiling_cmov_original = 0;
unsigned int g_ceiling_compiled = 0;
unsigned int g_ceiling_effective = 0;
std::vector<HMODULE> g_ceiling_rejected_modules;
SRWLOCK g_streamline_maintenance_lock = SRWLOCK_INIT;

bool CeilingPatchStillOwned() {
  if (!g_ceiling_patched.load(std::memory_order_acquire) ||
      g_ceiling_module == nullptr || g_ceiling_site == nullptr ||
      !mfgunlock::runtimeversion::IsMappedImage(g_ceiling_module)) {
    return false;
  }
  std::vector<wchar_t> current_path(32768);
  const DWORD length = GetModuleFileNameW(
      g_ceiling_module, current_path.data(),
      static_cast<DWORD>(current_path.size()));
  if (length == 0 || length >= current_path.size() ||
      _wcsicmp(current_path.data(), g_ceiling_module_path.c_str()) != 0) {
    return false;
  }
  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(g_ceiling_module, &base, &nt)) return false;
  const uintptr_t offset = reinterpret_cast<uintptr_t>(g_ceiling_site) -
                           reinterpret_cast<uintptr_t>(base);
  if (offset > nt->OptionalHeader.SizeOfImage ||
      nt->OptionalHeader.SizeOfImage - offset < 10) {
    return false;
  }
  return g_ceiling_site[0] == 0xBA && g_ceiling_site[2] == 0 &&
         g_ceiling_site[3] == 0 && g_ceiling_site[4] == 0 &&
         g_ceiling_site[5] == 0x3B && g_ceiling_site[6] == 0xCA &&
         g_ceiling_site[7] == 0x0F && g_ceiling_site[8] == 0x42 &&
         g_ceiling_site[9] == 0xD2;
}

void AbandonStaleFrameCountCeiling() {
  g_ceiling_module = nullptr;
  g_ceiling_module_path.clear();
  g_ceiling_site = nullptr;
  g_ceiling_original = 0;
  g_ceiling_cmov_original = 0;
  g_ceiling_compiled = 0;
  g_ceiling_effective = 0;
  g_ceiling_patched.store(false, std::memory_order_release);
  mfgunlock::framecount::g_advertised_max_generated.store(
      0, std::memory_order_release);
}

void PatchFrameCountCeiling(HMODULE mod) {
  if (g_ceiling_patched.load(std::memory_order_acquire)) {
    if (CeilingPatchStillOwned()) return;
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: the previously patched temporary Streamline wrapper was unloaded; discarding its stale address and waiting for the active wrapper.");
    AbandonStaleFrameCountCeiling();
  }
  if (std::find(g_ceiling_rejected_modules.begin(),
                g_ceiling_rejected_modules.end(), mod) !=
      g_ceiling_rejected_modules.end()) {
    return;
  }

  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(mod, &base, &nt)) return;
  g_ceiling_plugin_seen.store(true, std::memory_order_release);

  const unsigned char tail[] = {0x3B, 0xCA, 0x0F, 0x42, 0xD1};
  unsigned char* found = nullptr;
  size_t hits = 0;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 10) continue;
    for (size_t off = 0; off + 10 <= size; ++off) {
      if (start[off] != 0xBA) continue;
      if (start[off + 2] != 0 || start[off + 3] != 0 || start[off + 4] != 0) continue;
      if (std::memcmp(start + off + 5, tail, sizeof(tail)) != 0) continue;
      const unsigned char ceiling = start[off + 1];
      if (ceiling == 0 || ceiling > 8) continue;
      if (found == nullptr) found = start + off;
      ++hits;
    }
  }

  if (hits != 1 || found == nullptr) {
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: found " << hits
      << " frame-count clamps in the DLSS-G plugin " << module_path
      << " (expected 1); leaving it alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_ceiling_rejected_modules.push_back(mod);
    return;
  }

  DWORD old_protect = 0;
  if (VirtualProtect(found, 10, PAGE_EXECUTE_READWRITE, &old_protect) == 0) return;
  g_ceiling_module = mod;
  std::vector<wchar_t> module_path(32768);
  const DWORD module_path_length = GetModuleFileNameW(
      mod, module_path.data(), static_cast<DWORD>(module_path.size()));
  g_ceiling_module_path =
      module_path_length != 0 && module_path_length < module_path.size()
          ? std::wstring(module_path.data(), module_path_length)
          : std::wstring();
  g_ceiling_site = found;
  g_ceiling_original = found[1];
  g_ceiling_cmov_original = found[9];
  g_ceiling_compiled = found[1];
  g_ceiling_effective = g_ceiling_compiled;
  if (g_raise_ceiling.load(std::memory_order_relaxed) && found[1] < kCeilingTarget) {
    found[1] = kCeilingTarget;
    g_ceiling_effective = kCeilingTarget;
  }
  // cmovb edx, ecx -> cmovb edx, edx: same three-byte instruction, no lowering.
  found[9] = 0xD2;
  DWORD ignored = 0;
  VirtualProtect(found, 10, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), found, 10);
  g_ceiling_patched.store(true, std::memory_order_release);
  mfgunlock::framecount::g_advertised_max_generated.store(g_ceiling_effective,
                                                           std::memory_order_release);

  std::stringstream s;
  s << "mfgunlock: stopped the DLSS-G plugin from lowering its compiled ceiling of "
    << g_ceiling_compiled << " generated frame(s) to the stale NGX device value";
  if (g_ceiling_effective != g_ceiling_compiled) {
    s << "; RaiseFrameCeiling also changed the hard bound to " << g_ceiling_effective;
  }
  s << " (effective maximum " << (g_ceiling_effective + 1) << "x).";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

void RestoreFrameCountCeiling() {
  if (!g_ceiling_patched.load(std::memory_order_acquire)) return;
  if (g_ceiling_site == nullptr) return;
  DWORD old_protect = 0;
  if (CeilingPatchStillOwned() &&
      VirtualProtect(g_ceiling_site, 10, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
    g_ceiling_site[9] = g_ceiling_cmov_original;
    g_ceiling_site[1] = g_ceiling_original;
    DWORD ignored = 0;
    VirtualProtect(g_ceiling_site, 10, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_ceiling_site, 10);
  }
  g_ceiling_module = nullptr;
  g_ceiling_module_path.clear();
  g_ceiling_site = nullptr;
  g_ceiling_original = 0;
  g_ceiling_cmov_original = 0;
  g_ceiling_compiled = 0;
  g_ceiling_effective = 0;
  g_ceiling_rejected_modules.clear();
  g_ceiling_plugin_seen.store(false, std::memory_order_release);
  mfgunlock::framecount::g_advertised_max_generated.store(0, std::memory_order_release);
  g_ceiling_patched.store(false, std::memory_order_release);
}

void TryPatchFrameCountCeiling() {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  if (g_ceiling_patched.load(std::memory_order_acquire)) {
    if (CeilingPatchStillOwned()) {
      mfgunlock::framecount::ObserveStreamlinePluginVersion(g_ceiling_module);
      return;
    }
    AbandonStaleFrameCountCeiling();
  }
  AcquireSRWLockExclusive(&g_streamline_maintenance_lock);
  if (!g_ceiling_patched.load(std::memory_order_relaxed)) {
    if (HMODULE fast = GetModuleHandleW(L"sl.dlss_g.dll")) {
      PatchFrameCountCeiling(fast);
    }
    if (!g_ceiling_patched.load(std::memory_order_relaxed)) {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
          do {
            if (me.hModule == g_self_module ||
                !ModuleContains(me.hModule, kFlipMarker, sizeof(kFlipMarker) - 1)) {
              continue;
            }
            PatchFrameCountCeiling(me.hModule);
            if (g_ceiling_patched.load(std::memory_order_relaxed)) break;
          } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
      }
    }
  }
  ReleaseSRWLockExclusive(&g_streamline_maintenance_lock);
  if (g_ceiling_patched.load(std::memory_order_acquire)) {
    mfgunlock::framecount::ObserveStreamlinePluginVersion(g_ceiling_module);
  }
}

// Handles the DLSS-G Streamline plugin wherever it was loaded from. Returns true
// once a module has been dealt with, so the caller stops scanning.
bool TryPatchFlipMeteringInModule(HMODULE mod) {
  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(mod, &base, &nt)) return false;

  // 1. Is this the DLSS-G plugin? The marker string identifies it regardless of
  //    what the OTA layer decided to call the file.
  const size_t marker_len = sizeof(kFlipMarker) - 1;
  const unsigned char* marker = nullptr;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections && marker == nullptr; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < marker_len) continue;
    for (size_t off = 0; off + marker_len <= size; ++off) {
      if (std::memcmp(start + off, kFlipMarker, marker_len) == 0) {
        marker = start + off;
        break;
      }
    }
  }
  if (marker == nullptr) return false;

  ++g_flip_meter_attempts;

  // 2. Find the code referencing it, then read the (offset, value) the fallback
  //    writes: C6 /r disp32 imm8 == mov byte ptr [reg+disp32], imm8.
  unsigned int want_offset = 0;
  int want_value = -1;
  section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections && want_value < 0; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 8) continue;
    for (size_t off = 0; off + 8 <= size && want_value < 0; ++off) {
      // lea reg, [rip+disp32] pointing at the marker string
      if (!(start[off] == 0x48 || start[off] == 0x4C)) continue;
      if (start[off + 1] != 0x8D) continue;
      if ((start[off + 2] & 0xC7) != 0x05) continue;
      int disp = 0;
      std::memcpy(&disp, start + off + 3, sizeof(disp));
      if (start + off + 7 + disp != marker) continue;

      const size_t window = 0x200;
      const size_t limit = (off + window < size) ? (off + window) : size;
      for (size_t w = off; w + 7 <= limit; ++w) {
        if (start[w] != 0xC6) continue;
        if (start[w + 1] < 0x80 || start[w + 1] > 0xBF) continue;  // mod=10, disp32
        unsigned int field = 0;
        std::memcpy(&field, start + w + 2, sizeof(field));
        const unsigned char imm = start[w + 6];
        if (field <= 0x100 || field >= 0x20000) continue;
        if (imm > 1) continue;
        want_offset = field;
        want_value = imm;
        break;
      }
    }
  }

  if (want_value < 0) {
    // Name the module. Which DLSS-G plugin is actually loaded varies wildly --
    // game-bundled, driver OTA, or an NVIDIA App override copy -- and without
    // the path this warning says nothing actionable.
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: located a DLSS-G plugin (" << module_path
      << ") but could not read its flip-metering fallback state; leaving it alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_flip_meter_attempts = kMaxFlipMeterAttempts;
    return true;
  }

  // 3. Pin the field to that value everywhere it is written.
  //
  //    Two encodings appear in the wild, and sl.dlss_g 2.13.0.0 introduced the
  //    second:
  //
  //      C6 /0 disp32 imm8    mov byte ptr [reg+disp32], imm8    (7 bytes)
  //      40 88 /r  disp32     mov byte ptr [reg+disp32], reg8    (7 bytes)
  //
  //    The first is flipped by rewriting its immediate. The second stores a
  //    runtime value, so there is no immediate to change -- but its REX prefix
  //    is present only to name a byte register (spl/bpl/sil/dil), which makes it
  //    exactly seven bytes: the same length as the C6 form with that same base
  //    register. That equivalence is the only reason it is patchable in place,
  //    so it is deliberately the only register store handled. A bare
  //    88 /r disp32 is six bytes and a REX.B one needs eight, and rewriting
  //    either would run past the end of the instruction.
  const unsigned char opposite = static_cast<unsigned char>(1 - want_value);
  section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 7) continue;
    for (size_t off = 0; off + 7 <= size; ++off) {
      // C6 /0 disp32 imm8 -- flip the immediate. Unchanged from the form that
      // has been working; every other encoding is handled after it.
      if (start[off] == 0xC6) {
        if (start[off + 1] < 0x80 || start[off + 1] > 0xBF) continue;
        unsigned int field = 0;
        std::memcpy(&field, start + off + 2, sizeof(field));
        if (field != want_offset) continue;
        if (start[off + 6] != opposite) continue;
        const unsigned char imm = static_cast<unsigned char>(want_value);
        WriteFlipSite(start + off + 6, &imm, 1);
        continue;
      }

      // 40 88 /r disp32 -- rewrite the whole store into the C6 form, keeping the
      // same base register. REX must be exactly 0x40: any of the B/R/X/W bits
      // set would change either the encoding length or the base register.
      if (start[off] != 0x40 || start[off + 1] != 0x88) continue;
      const unsigned char modrm = start[off + 2];
      if (modrm < 0x80 || modrm > 0xBF) continue;  // mod=10, disp32
      const unsigned char rm = static_cast<unsigned char>(modrm & 7);
      if (rm == 4) continue;                       // rm=100 means a SIB byte follows
      unsigned int field = 0;
      std::memcpy(&field, start + off + 3, sizeof(field));
      if (field != want_offset) continue;

      unsigned char replacement[7] = {0xC6, static_cast<unsigned char>(0x80 | rm),
                                      0,    0,
                                      0,    0,
                                      static_cast<unsigned char>(want_value)};
      std::memcpy(replacement + 2, &want_offset, sizeof(want_offset));
      WriteFlipSite(start + off, replacement, sizeof(replacement));
    }
  }

  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);

  // Deriving the field is not the same as changing anything. If no site wrote
  // the opposite value there was nothing to flip, and claiming success here
  // would report a patch that never happened -- which is exactly how a stale
  // DLL once looked like a working one.
  if (g_flip_meter_sites.empty()) {
    std::stringstream s;
    s << "mfgunlock: flip-metering field +0x" << std::hex << want_offset << std::dec
      << " derived from " << module_path
      << ", but nothing writes it in a form this can patch -- no immediate store of "
      << (1 - want_value) << ", and no 7-byte register store. Nothing changed.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_flip_meter_attempts = kMaxFlipMeterAttempts;
    return true;
  }

  g_flip_meter_offset.store(want_offset, std::memory_order_relaxed);
  g_flip_meter_value.store(static_cast<unsigned int>(want_value), std::memory_order_relaxed);
  g_flip_meter_patched.store(true, std::memory_order_release);

  std::stringstream s;
  s << "mfgunlock: forced flip-metering off in " << module_path << " -- field +0x" << std::hex
    << want_offset << std::dec << " pinned to " << want_value << " at "
    << g_flip_meter_sites.size() << " site(s); multi-frame should pace in software (RSYNC).";
  reshade::log::message(reshade::log::level::info, s.str().c_str());

  // Same module, and by now we know it is the right one.
  PatchFrameCountCeiling(mod);
  return true;
}

void TryPatchFlipMeteringUnlocked() {
  if (g_flip_meter_patched.load(std::memory_order_acquire)) return;
  if (g_flip_meter_attempts.load(std::memory_order_relaxed) >=
      kMaxFlipMeterAttempts) return;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;

  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      if (me.hModule == g_self_module) continue;
      if (TryPatchFlipMeteringInModule(me.hModule)) break;
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
}

void TryPatchFlipMetering() {
  AcquireSRWLockExclusive(&g_streamline_maintenance_lock);
  TryPatchFlipMeteringUnlocked();
  ReleaseSRWLockExclusive(&g_streamline_maintenance_lock);
}

void ProcessLoadedStreamlineDlssgPlugin(HMODULE mod) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  if (!TryAcquireSRWLockExclusive(&g_streamline_maintenance_lock)) {
    g_provider_rescan_requested.store(true, std::memory_order_release);
    return;
  }
  PatchFrameCountCeiling(mod);
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) {
    TryPatchFlipMeteringInModule(mod);
  }
  ReleaseSRWLockExclusive(&g_streamline_maintenance_lock);
}

void RestoreFlipMetering() {
  if (!g_flip_meter_patched.load(std::memory_order_acquire)) return;
  for (const auto& site : g_flip_meter_sites) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(site.address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || info.Type != MEM_IMAGE) {
      continue;
    }
    DWORD old_protect = 0;
    if (VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
      std::memcpy(site.address, site.original, site.length);
      DWORD ignored = 0;
      VirtualProtect(site.address, site.length, old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
    }
  }
  g_flip_meter_sites.clear();
  g_flip_meter_patched.store(false, std::memory_order_release);
}

// Keep compatibility retries away from Present. The load-time hook remains the
// primary path; this short-lived worker only covers unusual loaders, hook
// conflicts, and modules that appear during startup through an unobserved API.
// Once the required pieces have been verified it exits and performs no further
// work for the rest of the session.
constexpr DWORD kDiscoveryRetryIntervalMs = 250;
constexpr unsigned int kDiscoveryRetryLimit = 40;
std::atomic_bool g_discovery_worker_started{false};
std::atomic_bool g_discovery_worker_running{false};
std::atomic_bool g_discovery_worker_finished{false};
std::atomic<unsigned int> g_discovery_worker_passes{0};

bool DiscoveryRequirementsMet() {
  const bool provider_ready =
      (!g_enabled.load(std::memory_order_relaxed) ||
       g_gate_patched.load(std::memory_order_acquire)) &&
      (!g_enabled.load(std::memory_order_relaxed) ||
       !g_temporal_fix.load(std::memory_order_relaxed) ||
       g_midpoint_patched.load(std::memory_order_acquire) ||
       g_blackwell_patched.load(std::memory_order_acquire));
  const bool pacing_ready =
      !g_force_flip_meter_off.load(std::memory_order_relaxed) ||
      g_flip_meter_patched.load(std::memory_order_acquire) ||
      g_flip_meter_attempts.load(std::memory_order_relaxed) >=
          kMaxFlipMeterAttempts;
  const bool wrapper_ready =
      !g_enabled.load(std::memory_order_relaxed) ||
      g_ceiling_patched.load(std::memory_order_acquire) ||
      g_ceiling_plugin_seen.load(std::memory_order_acquire);
  return provider_ready && pacing_ready && wrapper_ready &&
         mfgunlock::loadhook::g_hooked.load(std::memory_order_acquire) &&
         mfgunlock::framecount::g_hooked.load(std::memory_order_acquire);
}

void RunBoundedDiscoveryWorker() {
  g_discovery_worker_running.store(true, std::memory_order_release);
  unsigned int quiet_ready_passes = 0;
  for (unsigned int pass = 1; pass <= kDiscoveryRetryLimit; ++pass) {
    Sleep(kDiscoveryRetryIntervalMs);
    g_provider_rescan_requested.store(false, std::memory_order_release);
    RunProviderMaintenance();
    TryPatchFrameCountCeiling();
    if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
    mfgunlock::framecount::TryInstall();
    mfgunlock::loadhook::TryInstall();
    g_discovery_worker_passes.store(pass, std::memory_order_relaxed);

    if (DiscoveryRequirementsMet() &&
        !g_provider_rescan_requested.load(std::memory_order_acquire)) {
      // A short quiet period closes the race where another module is loaded as
      // the first successful pass finishes.
      if (++quiet_ready_passes >= 4) break;
    } else {
      quiet_ready_passes = 0;
    }
  }

  const bool ready = DiscoveryRequirementsMet();
  g_discovery_worker_running.store(false, std::memory_order_release);
  g_discovery_worker_finished.store(true, std::memory_order_release);
  reshade::log::message(
      ready ? reshade::log::level::info : reshade::log::level::warning,
      ready ? "mfgunlock: bounded startup discovery completed; no Present polling is active."
            : "mfgunlock: bounded startup discovery ended without verifying every component; "
              "the load-time trigger remains armed for late modules.");
}

DWORD WINAPI DiscoveryWorkerEntry(LPVOID pinned_module) {
  RunBoundedDiscoveryWorker();
  FreeLibraryAndExitThread(static_cast<HMODULE>(pinned_module), 0);
}

void StartDiscoveryWorker() {
  if (g_discovery_worker_started.load(std::memory_order_acquire)) return;
  bool expected = false;
  if (!g_discovery_worker_started.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }

  HMODULE pinned_module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
          reinterpret_cast<LPCWSTR>(&DiscoveryWorkerEntry), &pinned_module)) {
    g_discovery_worker_started.store(false, std::memory_order_release);
    return;
  }
  HANDLE thread = CreateThread(nullptr, 0, DiscoveryWorkerEntry, pinned_module, 0, nullptr);
  if (thread == nullptr) {
    FreeLibrary(pinned_module);
    g_discovery_worker_started.store(false, std::memory_order_release);
    return;
  }
  CloseHandle(thread);
}

uint32_t DetectDisplayRefreshFps(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr || swapchain->get_hwnd() == nullptr) return 0;
  const HMONITOR monitor = MonitorFromWindow(
      static_cast<HWND>(swapchain->get_hwnd()), MONITOR_DEFAULTTONEAREST);
  if (monitor == nullptr) return 0;
  static HMONITOR cached_monitor = nullptr;
  static uint64_t cached_epoch = 0, checked_ms = 0;
  static uint32_t cached_refresh = 0;
  const auto epoch = mfgunlock::framecount::g_latency_guard_epoch.load(std::memory_order_acquire);
  const auto now = GetTickCount64();
  if (monitor == cached_monitor && epoch == cached_epoch && now - checked_ms < 5000)
    return cached_refresh;
  cached_monitor = monitor; cached_epoch = epoch; checked_ms = now; cached_refresh = 0;
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    return 0;
  cached_refresh = mode.dmDisplayFrequency > 1 && mode.dmDisplayFrequency <= 1000
             ? mode.dmDisplayFrequency
             : 0;
  return cached_refresh;
}

void ObserveDxgiLatencyPolicy(reshade::api::swapchain* swapchain) {
  auto* device = swapchain == nullptr ? nullptr : swapchain->get_device();
  if (device == nullptr ||
      (device->get_api() != reshade::api::device_api::d3d11 &&
       device->get_api() != reshade::api::device_api::d3d12) ||
      swapchain->get_native() == 0) {
    g_latency_guard_dxgi_observed.store(false, std::memory_order_release);
    g_latency_guard_waitable_swapchain.store(false,
                                              std::memory_order_relaxed);
    g_latency_guard_dxgi_max_latency.store(0, std::memory_order_relaxed);
    return;
  }
  auto* native = reinterpret_cast<IUnknown*>(
      static_cast<uintptr_t>(swapchain->get_native()));
  IDXGISwapChain2* swapchain2 = nullptr;
  if (FAILED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) ||
      swapchain2 == nullptr)
    return;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  const bool desc_ok = SUCCEEDED(swapchain2->GetDesc1(&desc));
  const bool waitable =
      desc_ok &&
      (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
  UINT maximum_latency = 0;
  if (!waitable || FAILED(swapchain2->GetMaximumFrameLatency(&maximum_latency)))
    maximum_latency = 0;
  swapchain2->Release();

  g_latency_guard_waitable_swapchain.store(waitable,
                                            std::memory_order_relaxed);
  g_latency_guard_dxgi_max_latency.store(maximum_latency,
                                         std::memory_order_relaxed);
  g_latency_guard_dxgi_observed.store(desc_ok, std::memory_order_release);
}

uint32_t DetectDisplayRefreshMilliHz(reshade::api::swapchain* swapchain) {
  const uint32_t fallback_fps = DetectDisplayRefreshFps(swapchain);
  const uint32_t fallback_millihz =
      fallback_fps >= 10 && fallback_fps <= 1000 ? fallback_fps * 1000u : 0u;
  if (swapchain != nullptr && swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain* dxgi_swapchain = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&dxgi_swapchain))) &&
        dxgi_swapchain != nullptr) {
      DXGI_SWAP_CHAIN_DESC desc{};
      const bool valid = SUCCEEDED(dxgi_swapchain->GetDesc(&desc)) &&
          desc.BufferDesc.RefreshRate.Numerator != 0 &&
          desc.BufferDesc.RefreshRate.Denominator != 0;
      const uint64_t millihz = valid
          ? (static_cast<uint64_t>(
                 desc.BufferDesc.RefreshRate.Numerator) * 1000ull +
             desc.BufferDesc.RefreshRate.Denominator / 2u) /
                desc.BufferDesc.RefreshRate.Denominator
          : 0;
      dxgi_swapchain->Release();
      if (millihz >= 10000 && millihz <= 1000000) {
        // Flip-model borderless swapchains often retain a stale nominal
        // BufferDesc refresh (commonly 60 Hz) on a 120-480 Hz desktop. Use the
        // rational value only when it agrees with the active monitor mode.
        const uint32_t rational = static_cast<uint32_t>(millihz);
        const uint32_t difference = rational > fallback_millihz
            ? rational - fallback_millihz
            : fallback_millihz - rational;
        if (fallback_millihz == 0 || difference <= 1500)
          return rational;
      }
    }
  }
  return fallback_millihz;
}

const char* ReflexWaitableFallbackText(
    mfgunlock::reflexpacing::FallbackReason reason) {
  using Reason = mfgunlock::reflexpacing::FallbackReason;
  switch (reason) {
    case Reason::kNotRequested: return "Native sleep requested";
    case Reason::kUnsupportedApi: return "Renderer is not D3D11/D3D12";
    case Reason::kMfgInactive: return "MFG is not active at 2x-6x";
    case Reason::kNoWaitableFlag: return "Swapchain has no waitable-object flag";
    case Reason::kNoWaitableHandle: return "DXGI returned no waitable handle";
    case Reason::kHandleDuplicationFailed: return "Waitable handle duplication failed";
    case Reason::kMaximumLatencyQueryFailed: return "MaximumFrameLatency query failed";
    case Reason::kMaximumLatencySetFailed: return "Could not set MaximumFrameLatency to 1";
    case Reason::kSwapchainChanged: return "Primary swapchain changed";
    case Reason::kTokenDuplicate: return "Duplicate Reflex frame token";
    case Reason::kTokenRegressed: return "Regressing Reflex frame token";
    case Reason::kConcurrentSleep: return "Concurrent slReflexSleep calls";
    case Reason::kNativeSleepFailed: return "Native slReflexSleep failed during probe";
    case Reason::kWaitTimeout: return "DXGI waitable object timed out";
    case Reason::kWaitFailed: return "DXGI waitable-object wait failed";
    default: return "None";
  }
}

void RestoreReflexWaitableLocked(
    reshade::api::swapchain* swapchain,
    mfgunlock::reflexpacing::FallbackReason reason) {
  const HANDLE duplicate = mfgunlock::reflexpacing::RetireWaitable(reason);
  if (duplicate != nullptr) CloseHandle(duplicate);
  if (g_reflex_waitable.maximum_latency_forced && swapchain != nullptr &&
      swapchain == g_reflex_waitable.owner && swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain2* swapchain2 = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) &&
        swapchain2 != nullptr) {
      UINT current = 0;
      if (SUCCEEDED(swapchain2->GetMaximumFrameLatency(&current)) &&
          current == 1 && g_reflex_waitable.native_maximum_latency != 0) {
        swapchain2->SetMaximumFrameLatency(
            g_reflex_waitable.native_maximum_latency);
      }
      swapchain2->Release();
    }
  }
  g_reflex_waitable.owner = nullptr;
  g_reflex_waitable.native_maximum_latency = 0;
  g_reflex_waitable.maximum_latency_forced = false;
}

bool PublishReflexWaitableLocked(reshade::api::swapchain* swapchain,
                                 uint32_t refresh_millihz) {
  using namespace mfgunlock::reflexpacing;
  if (swapchain == nullptr || swapchain->get_native() == 0) {
    SetFallback(FallbackReason::kNoWaitableHandle);
    return false;
  }
  auto* device = swapchain->get_device();
  if (device == nullptr ||
      (device->get_api() != reshade::api::device_api::d3d11 &&
       device->get_api() != reshade::api::device_api::d3d12)) {
    SetFallback(FallbackReason::kUnsupportedApi);
    return false;
  }
  auto* native = reinterpret_cast<IUnknown*>(
      static_cast<uintptr_t>(swapchain->get_native()));
  IDXGISwapChain2* swapchain2 = nullptr;
  if (FAILED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) ||
      swapchain2 == nullptr) {
    SetFallback(FallbackReason::kNoWaitableHandle);
    return false;
  }
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (FAILED(swapchain2->GetDesc1(&desc)) ||
      (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0) {
    swapchain2->Release();
    SetFallback(FallbackReason::kNoWaitableFlag);
    return false;
  }
  const HANDLE borrowed = swapchain2->GetFrameLatencyWaitableObject();
  if (borrowed == nullptr) {
    swapchain2->Release();
    SetFallback(FallbackReason::kNoWaitableHandle);
    return false;
  }
  UINT maximum_latency = 0;
  if (FAILED(swapchain2->GetMaximumFrameLatency(&maximum_latency)) ||
      maximum_latency == 0) {
    swapchain2->Release();
    SetFallback(FallbackReason::kMaximumLatencyQueryFailed);
    return false;
  }
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    swapchain2->Release();
    SetFallback(FallbackReason::kHandleDuplicationFailed);
    return false;
  }
  swapchain2->Release();
  const uint32_t two_periods_ms = refresh_millihz != 0
      ? static_cast<uint32_t>((2000000ull + refresh_millihz - 1ull) /
                              refresh_millihz)
      : 48u;
  const uint32_t timeout_ms =
      (std::clamp)(two_periods_ms + 2u, 8u, 50u);
  const uint64_t generation = ++g_reflex_waitable.generation;
  g_reflex_waitable.owner = swapchain;
  g_reflex_waitable.native_maximum_latency = maximum_latency;
  g_reflex_waitable.maximum_latency_forced = false;
  PublishWaitable(duplicate, generation, timeout_ms, maximum_latency);
  std::stringstream log;
  log << "mfgunlock: DXGI Waitable probe started (generation " << generation
      << ", native MaximumFrameLatency=" << maximum_latency
      << ", timeout=" << timeout_ms << " ms).";
  reshade::log::message(reshade::log::level::info, log.str().c_str());
  return true;
}

void UpdateReflexPacingLab(reshade::api::swapchain* swapchain) {
  using namespace mfgunlock;
  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire))
    return;
  const bool cap_poll_requested = framecount::g_addon_enabled.load(std::memory_order_relaxed) &&
      pacing::IsValidReflexOutputFpsCap(framecount::g_reflex_source_fps_cap.load());
  if (!cap_poll_requested && !reflexpacing::WaitableRequested() &&
      !reflexpacing::LabMeasurementEnabled() &&
      reflexpacing::g_sleep_dispatch.load(std::memory_order_acquire) ==
          reflexpacing::SleepDispatch::kNative &&
      g_reflex_waitable_idle.load(std::memory_order_acquire) &&
      reflexpacing::g_headroom_limit_us.load(std::memory_order_relaxed) == 0)
    return;
  static SRWLOCK update_lock = SRWLOCK_INIT;
  if (!TryAcquireSRWLockExclusive(&update_lock)) return;
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockExclusive(lock); }
  } unlock{&update_lock};

  const uint32_t estimated_multiplier =
      framecount::internal::ActiveMultiplierForEstimate();
  reflexpacing::g_mfg_multiplier.store(estimated_multiplier,
                                       std::memory_order_relaxed);

  static ULONGLONG next_status_sample = 0;
  const ULONGLONG now = GetTickCount64();
  const bool addon_enabled =
      framecount::g_addon_enabled.load(std::memory_order_relaxed);
  const uint64_t ui_heartbeat = reflexpacing::g_ui_heartbeat_ms.load(
      std::memory_order_acquire);
  const bool measure_native = addon_enabled &&
      (reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed) ||
       pacing::NormalizeReflexModeOverride(
           reflexpacing::g_mode_override.load(std::memory_order_relaxed)) !=
           pacing::ReflexModeOverride::kGame ||
       (ui_heartbeat != 0 && now - ui_heartbeat <= 1500));
  const bool fg_safe_off = addon_enabled &&
      pacing::NormalizeReflexModeOverride(
          reflexpacing::g_mode_override.load(std::memory_order_relaxed)) ==
          pacing::ReflexModeOverride::kOff;
  const auto next_dispatch =
      addon_enabled && reflexpacing::WaitableRequested()
          ? reflexpacing::SleepDispatch::kWaitable
          : (fg_safe_off
                 ? reflexpacing::SleepDispatch::kBypass
                 : (measure_native
                        ? reflexpacing::SleepDispatch::kMeasureNative
                        : reflexpacing::SleepDispatch::kNative));
  const auto previous_dispatch = reflexpacing::g_sleep_dispatch.exchange(
      next_dispatch, std::memory_order_acq_rel);
  if (previous_dispatch != next_dispatch &&
      (previous_dispatch == reflexpacing::SleepDispatch::kBypass ||
       next_dispatch == reflexpacing::SleepDispatch::kBypass)) {
    reshade::log::message(
        reshade::log::level::info,
        next_dispatch == reflexpacing::SleepDispatch::kBypass
            ? "mfgunlock: FG-safe Reflex Off active; Streamline mode remains On and slReflexSleep is bypassed."
            : "mfgunlock: FG-safe Reflex sleep bypass released.");
  }
  const bool lab_requested = addon_enabled &&
      (cap_poll_requested || reflexpacing::WaitableRequested() ||
       reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed));
  if (lab_requested && now >= next_status_sample) {
    next_status_sample = now + 500;
    bool vrr = false;
    uint32_t driver_multiplier = 0;
    if (auto* device = swapchain->get_device();
        device != nullptr && device->get_native() != 0 &&
        (device->get_api() == reshade::api::device_api::d3d11 ||
         device->get_api() == reshade::api::device_api::d3d12)) {
      nvapistatus::SleepStatus sleep{};
      sleep.version = nvapistatus::StructVersion<nvapistatus::SleepStatus, 1>();
      const auto get_sleep = nvapistatus::GetSleepStatus();
      if (get_sleep != nullptr && nvapistatus::Initialize() ==
                                      nvapistatus::kOk &&
          get_sleep(reinterpret_cast<IUnknown*>(
                        static_cast<uintptr_t>(device->get_native())),
                    &sleep) == nvapistatus::kOk) {
        vrr = sleep.fullscreen_vrr != 0;
        driver_multiplier = sleep.frame_generation_multiplier;
      }
    }
    reflexpacing::g_vrr_active.store(vrr, std::memory_order_relaxed);
    const uint32_t previous_cap_multiplier = framecount::g_cap_driver_multiplier.exchange(
        driver_multiplier, std::memory_order_relaxed);
    framecount::g_cap_driver_observed_ms.store(now, std::memory_order_release);
    if (previous_cap_multiplier != driver_multiplier) {
      framecount::g_latency_guard_epoch.fetch_add(1, std::memory_order_acq_rel);
      framecount::g_latency_guard_refresh_pending.store(true, std::memory_order_release);
    }
    if (driver_multiplier >= 2 && driver_multiplier <= 6)
      reflexpacing::g_mfg_multiplier.store(driver_multiplier,
                                           std::memory_order_relaxed);
    const uint32_t refresh_millihz = DetectDisplayRefreshMilliHz(swapchain);
    reflexpacing::g_refresh_millihz.store(refresh_millihz,
                                          std::memory_order_relaxed);
    const uint32_t headroom_limit =
        addon_enabled &&
                reflexpacing::g_headroom_enabled.load(
                    std::memory_order_relaxed) &&
                vrr
            ? pacing::VrrHeadroomFrameLimitUs(
                  refresh_millihz,
                  pacing::NormalizeHeadroomBasisPoints(
                      reflexpacing::g_headroom_basis_points.load(
                          std::memory_order_relaxed)))
            : 0;
    const uint32_t previous = reflexpacing::g_headroom_limit_us.exchange(
        headroom_limit, std::memory_order_acq_rel);
    if (previous != headroom_limit) {
      framecount::g_latency_guard_epoch.fetch_add(1,
                                                   std::memory_order_acq_rel);
      framecount::g_latency_guard_refresh_pending.store(
          true, std::memory_order_release);
    }
  } else if (!lab_requested &&
             reflexpacing::g_headroom_limit_us.exchange(
                 0, std::memory_order_acq_rel) != 0) {
    framecount::g_latency_guard_epoch.fetch_add(1, std::memory_order_acq_rel);
    framecount::g_latency_guard_refresh_pending.store(
        true, std::memory_order_release);
  }

  const bool requested =
      framecount::g_addon_enabled.load(std::memory_order_relaxed) &&
      reflexpacing::WaitableRequested();
  if (!requested && g_reflex_waitable_idle.load(std::memory_order_acquire))
    return;
  g_reflex_waitable_idle.store(false, std::memory_order_release);
  AcquireSRWLockExclusive(&g_reflex_waitable.lock);
  if (requested != g_reflex_waitable.last_requested) {
    g_reflex_waitable.last_requested = requested;
    g_reflex_waitable.fallback_latched = false;
    if (requested) {
      framecount::g_latency_guard_auto_cap_ready.store(
          false, std::memory_order_release);
      framecount::g_latency_guard_active_source_cap_fps.store(
          0, std::memory_order_relaxed);
      framecount::g_latency_guard_multiplier_override.store(
          0, std::memory_order_release);
      framecount::g_latency_guard_multiplier_trial_accepted.store(
          false, std::memory_order_relaxed);
      framecount::g_latency_guard_refresh_pending.store(
          true, std::memory_order_release);
    }
    if (!requested && g_reflex_waitable.owner != nullptr)
      RestoreReflexWaitableLocked(g_reflex_waitable.owner,
                                  reflexpacing::FallbackReason::kNotRequested);
  }
  if (!requested) {
    reflexpacing::g_waitable_state.store(
        reflexpacing::WaitableState::kNative, std::memory_order_release);
    g_reflex_waitable_idle.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  const uint32_t live_multiplier = reflexpacing::g_mfg_multiplier.load(
      std::memory_order_relaxed);
  if (live_multiplier < 2 || live_multiplier > 6) {
    if (g_reflex_waitable.owner != nullptr) {
      RestoreReflexWaitableLocked(
          g_reflex_waitable.owner,
          reflexpacing::FallbackReason::kNotRequested);
    }
    reflexpacing::g_fallback_reason.store(
        reflexpacing::FallbackReason::kMfgInactive,
        std::memory_order_relaxed);
    reflexpacing::g_waitable_state.store(
        reflexpacing::WaitableState::kWaitingForSwapchain,
        std::memory_order_release);
    g_reflex_waitable.fallback_latched = false;
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  const auto state = reflexpacing::g_waitable_state.load(
      std::memory_order_acquire);
  if (state == reflexpacing::WaitableState::kFallback) {
    const auto reason = reflexpacing::g_fallback_reason.load(
        std::memory_order_relaxed);
    if (g_reflex_waitable.owner != nullptr)
      RestoreReflexWaitableLocked(g_reflex_waitable.owner, reason);
    if (!g_reflex_waitable.fallback_latched) {
      std::stringstream log;
      log << "mfgunlock: DXGI Waitable fell back to native Reflex sleep: "
          << ReflexWaitableFallbackText(reason) << ".";
      reshade::log::message(reshade::log::level::warning,
                            log.str().c_str());
      g_reflex_waitable.fallback_latched = true;
    }
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  if (g_reflex_waitable.owner != nullptr &&
      g_reflex_waitable.owner != swapchain) {
    RestoreReflexWaitableLocked(g_reflex_waitable.owner,
                                reflexpacing::FallbackReason::kSwapchainChanged);
    g_reflex_waitable.fallback_latched = true;
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  if (g_reflex_waitable.owner == nullptr &&
      !g_reflex_waitable.fallback_latched) {
    PublishReflexWaitableLocked(
        swapchain,
        reflexpacing::g_refresh_millihz.load(std::memory_order_relaxed));
  } else if (state == reflexpacing::WaitableState::kActivationPending &&
             g_reflex_waitable.owner == swapchain &&
             swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain2* swapchain2 = nullptr;
    bool success = false;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) &&
        swapchain2 != nullptr) {
      success = g_reflex_waitable.native_maximum_latency == 1 ||
                SUCCEEDED(swapchain2->SetMaximumFrameLatency(1));
      swapchain2->Release();
    }
    g_reflex_waitable.maximum_latency_forced =
        success && g_reflex_waitable.native_maximum_latency != 1;
    reflexpacing::MarkActivationResult(g_reflex_waitable.generation, success);
    if (success) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: DXGI Waitable probe passed; alternate pacing is active with MaximumFrameLatency=1.");
    }
  }
  ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
}

bool QueryVideoMemory(reshade::api::device* device,
                      DXGI_MEMORY_SEGMENT_GROUP group,
                      DXGI_QUERY_VIDEO_MEMORY_INFO& info) {
  if (device == nullptr || device->get_native() == 0) return false;
  IDXGIAdapter3* adapter3 = nullptr;
  if (device->get_api() == reshade::api::device_api::d3d12) {
    auto* d3d12 = reinterpret_cast<ID3D12Device*>(
        static_cast<uintptr_t>(device->get_native()));
    IDXGIFactory4* factory = nullptr;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    const auto create_factory = dxgi == nullptr
        ? nullptr
        : reinterpret_cast<CreateFactoryFn>(
              GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (create_factory != nullptr &&
        SUCCEEDED(create_factory(IID_PPV_ARGS(&factory))) &&
        factory != nullptr) {
      factory->EnumAdapterByLuid(d3d12->GetAdapterLuid(),
                                 IID_PPV_ARGS(&adapter3));
      factory->Release();
    }
  } else if (device->get_api() == reshade::api::device_api::d3d11) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(device->get_native()));
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
        dxgi_device != nullptr) {
      if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr) {
        adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        adapter->Release();
      }
      dxgi_device->Release();
    }
  }
  if (adapter3 == nullptr) return false;
  const bool ok = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, group, &info));
  adapter3->Release();
  return ok;
}

void UpdateVramDiagnostics(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire))
    return;
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG heartbeat =
      g_vram_ui_heartbeat_ms.load(std::memory_order_acquire);
  if (heartbeat == 0 || now - heartbeat > 1500) {
    if (g_vram_capture_owned.exchange(false, std::memory_order_acq_rel))
      mfgunlock::inputdiag::g_enabled.store(false,
                                            std::memory_order_release);
    return;
  }

  static ULONGLONG next_sample = 0;
  if (now < next_sample) return;
  next_sample = now + 1000;

  auto* device = swapchain->get_device();
  const auto api = device == nullptr
      ? mfgunlock::memorypolicy::GraphicsApi::kUnknown
      : (device->get_api() == reshade::api::device_api::d3d11
             ? mfgunlock::memorypolicy::GraphicsApi::kD3D11
             : (device->get_api() == reshade::api::device_api::d3d12
                    ? mfgunlock::memorypolicy::GraphicsApi::kD3D12
                    : (device->get_api() == reshade::api::device_api::vulkan
                           ? mfgunlock::memorypolicy::GraphicsApi::kVulkan
                           : mfgunlock::memorypolicy::GraphicsApi::kUnknown)));
  const bool supports_dxgi =
      mfgunlock::memorypolicy::SupportsDxgiBudget(api);
  if (!supports_dxgi) {
    // Vulkan native handles are not COM objects. Clear any values retained
    // from an earlier D3D swapchain, but leave provider/resource telemetry on.
    ClearDxgiDiagnostics();
  } else if (swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain1* swapchain1 = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain1))) &&
        swapchain1 != nullptr) {
      DXGI_SWAP_CHAIN_DESC1 desc{};
      if (SUCCEEDED(swapchain1->GetDesc1(&desc)))
        g_vram_swapchain_buffers.store(desc.BufferCount,
                                       std::memory_order_relaxed);
      swapchain1->Release();
    }
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
  const bool local_ok = supports_dxgi && QueryVideoMemory(
      device, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, local);
  const bool nonlocal_ok = supports_dxgi && QueryVideoMemory(
      device, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, nonlocal);
  if (local_ok) {
    g_vram_local_usage.store(local.CurrentUsage, std::memory_order_relaxed);
    g_vram_local_budget.store(local.Budget, std::memory_order_relaxed);
    g_vram_local_available.store(local.AvailableForReservation,
                                 std::memory_order_relaxed);
    g_vram_dxgi_seen.store(true, std::memory_order_release);
  }
  if (nonlocal_ok) {
    g_vram_nonlocal_usage.store(nonlocal.CurrentUsage,
                                std::memory_order_relaxed);
    g_vram_nonlocal_budget.store(nonlocal.Budget,
                                 std::memory_order_relaxed);
  }
}

mfgunlock::pacing::MarkerHealth AssessReflexMarkers(
    const mfgunlock::nvapistatus::GuardObservation& observation) {
  using mfgunlock::pacing::MarkerHealth;
  if (observation.latency_status != mfgunlock::nvapistatus::kOk)
    return MarkerHealth::kUnavailable;
  if (observation.valid_latency_frames < 48)
    return MarkerHealth::kWaiting;
  const auto& frame = observation.latest;
  const bool complete = frame.simulation_start_time != 0 &&
                        frame.simulation_end_time != 0 &&
                        frame.render_submit_start_time != 0 &&
                        frame.render_submit_end_time != 0 &&
                        frame.present_start_time != 0 &&
                        frame.present_end_time != 0 &&
                        frame.gpu_render_start_time != 0 &&
                        frame.gpu_render_end_time != 0;
  if (!complete) return MarkerHealth::kIncomplete;
  const bool ordered =
      frame.simulation_start_time <= frame.simulation_end_time &&
      frame.render_submit_start_time <= frame.render_submit_end_time &&
      frame.present_start_time <= frame.present_end_time &&
      frame.gpu_render_start_time <= frame.gpu_render_end_time;
  if (!ordered) return MarkerHealth::kInvalidOrder;
  if (observation.sleep_status == mfgunlock::nvapistatus::kOk &&
      observation.sleep.game_sleep == 0)
    return MarkerHealth::kMissingSleep;
  if (!observation.source_timing_confident)
    return MarkerHealth::kUnstableTiming;
  return MarkerHealth::kHealthy;
}

void UpdateLatencyGuard(reshade::api::swapchain* swapchain) {
  using mfgunlock::pacing::LatencyGuardMode;
  const auto configured_guard_mode = static_cast<LatencyGuardMode>(
      mfgunlock::framecount::g_latency_guard_mode.load(
          std::memory_order_relaxed));
  const auto waitable_state =
      mfgunlock::reflexpacing::g_waitable_state.load(
          std::memory_order_acquire);
  const bool waitable_controls_pacing =
      waitable_state == mfgunlock::reflexpacing::WaitableState::kProbing ||
      waitable_state ==
          mfgunlock::reflexpacing::WaitableState::kActivationPending ||
      waitable_state == mfgunlock::reflexpacing::WaitableState::kActive;
  const auto guard_mode =
      configured_guard_mode == LatencyGuardMode::kAutomatic &&
              waitable_controls_pacing
          ? LatencyGuardMode::kMonitor
          : configured_guard_mode;

  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire)) return;
  if (!mfgunlock::framecount::g_latency_guard_refresh_pending.load(std::memory_order_acquire) &&
      !mfgunlock::framecount::g_latency_guard_auto_cap_ready.load(std::memory_order_acquire)) {
    const uint64_t heartbeat = mfgunlock::framecount::g_latency_guard_ui_heartbeat_ms.load(
        std::memory_order_acquire);
    if (guard_mode == LatencyGuardMode::kOff ||
        (guard_mode == LatencyGuardMode::kMonitor &&
         (heartbeat == 0 || GetTickCount64() - heartbeat > 1500)))
      return;
  }
  // Prevent two presenting threads from racing the sampler/controller.
  static SRWLOCK sample_lock = SRWLOCK_INIT;
  if (!TryAcquireSRWLockExclusive(&sample_lock)) return;
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&sample_lock); } } unlock;
  auto* device = swapchain->get_device();
  if (device == nullptr ||
      (device->get_api() != reshade::api::device_api::d3d11 &&
       device->get_api() != reshade::api::device_api::d3d12) || device->get_native() == 0) {
    if (mfgunlock::framecount::g_latency_guard_auto_cap_ready.exchange(false)) {
      mfgunlock::framecount::g_latency_guard_active_source_cap_fps.store(0);
      mfgunlock::framecount::g_latency_guard_epoch.fetch_add(1);
      mfgunlock::framecount::g_latency_guard_refresh_pending.store(true);
    }
    return; // Never replay Reflex options through an unavailable device/session.
  }
  // Restoring the native Reflex options is also deferred to Present so the UI
  // never invokes Streamline directly. This executes only on a mode/cap state
  // transition, not once per frame.
  const bool refresh_pending =
      mfgunlock::framecount::g_latency_guard_refresh_pending.exchange(
          false, std::memory_order_acq_rel);
  if (refresh_pending) {
    mfgunlock::framecount::internal::RefreshReflexTarget();
  }
  if (guard_mode == LatencyGuardMode::kOff || swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire))
    return;

  static ULONGLONG next_sample = 0;
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG ui_heartbeat =
      mfgunlock::framecount::g_latency_guard_ui_heartbeat_ms.load(
          std::memory_order_acquire);
  if (guard_mode == LatencyGuardMode::kMonitor &&
      (ui_heartbeat == 0 || now - ui_heartbeat > 1500))
    return;
  if (now < next_sample) return;
  next_sample = now + 500;

  LARGE_INTEGER sample_begin{}, sample_end{};
  const uint64_t sample_frequency =
      mfgunlock::nvapistatus::QpcFrequency();
  QueryPerformanceCounter(&sample_begin);
  const auto observation = mfgunlock::nvapistatus::ObserveGuard(
      reinterpret_cast<IUnknown*>(
          static_cast<uintptr_t>(device->get_native())),
      mfgunlock::framecount::g_latency_guard_epoch.load(std::memory_order_acquire));
  QueryPerformanceCounter(&sample_end);
  const uint32_t sample_cost_us = sample_frequency > 0 &&
          sample_end.QuadPart >= sample_begin.QuadPart
      ? static_cast<uint32_t>((std::min)(
            uint64_t{UINT32_MAX},
            static_cast<uint64_t>(sample_end.QuadPart - sample_begin.QuadPart) *
                 1000000ull / sample_frequency))
      : 0;
  ObserveDxgiLatencyPolicy(swapchain);
  const uint32_t refresh_fps = DetectDisplayRefreshFps(swapchain);
  const bool sleep_available =
      observation.sleep_status == mfgunlock::nvapistatus::kOk;
  const uint32_t live_multiplier =
      sleep_available ? observation.sleep.frame_generation_multiplier : 0;
  const bool vsync_active =
      sleep_available && observation.sleep.control_panel_vsync != 0;
  const auto marker_health = AssessReflexMarkers(observation);
  const uint32_t queue_wait_us = observation.median_queue_wait_us;
  const uint32_t gpu_frame_time_us = observation.median_gpu_frame_time_us;
  const uint32_t pipeline_latency_us =
      observation.median_pipeline_latency_us;
  const uint32_t observed_source_interval_us =
      observation.source_interval_us;
  const bool queue_action_timing_confident =
      observation.source_timing_confident &&
      observation.queue_timing_confident;

  const auto recommendation =
      mfgunlock::pacing::BuildLatencyGuardRecommendation(
          vsync_active, refresh_fps,
          mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed)
              ? mfgunlock::framecount::g_dynamic_target_fps.load(std::memory_order_relaxed) : 0,
          sleep_available ? observation.sleep.dynamic_frame_time_target_us : 0,
          live_multiplier, observed_source_interval_us, queue_wait_us,
          queue_action_timing_confident);

  mfgunlock::framecount::g_latency_guard_units.store(
      static_cast<unsigned int>(observation.timestamp_units), std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_new_frames.store(observation.new_frames, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_display_refresh_fps.store(
      refresh_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_live_multiplier.store(
      live_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_output_target_fps.store(
      recommendation.output_target_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_recommended_source_cap_fps.store(
      recommendation.source_cap_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_estimated_source_fps.store(
      recommendation.estimated_source_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_simulation_fps.store(
      mfgunlock::pacing::FrameLimitUsToFps(
          observation.simulation_interval_us),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_projected_output_fps.store(
      recommendation.projected_output_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_suggested_multiplier.store(
      recommendation.suggested_total_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_high.store(
      recommendation.multiplier_may_be_higher_than_needed,
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_marker_health.store(
      static_cast<unsigned int>(marker_health), std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_queue_wait_us.store(
      queue_wait_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_queue_p95_us.store(
      observation.p95_queue_wait_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_gpu_frame_time_us.store(
      gpu_frame_time_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_gpu_frame_p95_us.store(
      observation.p95_gpu_frame_time_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_gpu_active_us.store(
      observation.median_gpu_active_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_pipeline_latency_us.store(
      pipeline_latency_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_pipeline_p95_us.store(
      observation.p95_pipeline_latency_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_input_to_gpu_end_us.store(
      observation.median_input_to_gpu_end_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_input_to_simulation_us.store(
      observation.median_input_to_simulation_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_simulation_cpu_us.store(
      observation.median_simulation_cpu_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_submit_cpu_us.store(
      observation.median_submit_cpu_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_ai_frame_time_us.store(
      observation.median_ai_frame_time_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_sample_cost_us.store(
      sample_cost_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_iflip_known.store(
      sleep_available, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_iflip_active.store(
      sleep_available && observation.sleep.fullscreen_independent_flip != 0,
      std::memory_order_relaxed);
  const auto latency_bottleneck =
      mfgunlock::pacing::ClassifyLatencyBottleneck(
          observation.source_timing_confident,
          recommendation.source_oversubscribed,
          observation.queue_timing_confident
              ? observation.p95_queue_wait_us : 0,
          observed_source_interval_us,
          observation.median_gpu_active_us,
          observation.median_ai_frame_time_us);
  mfgunlock::framecount::g_latency_guard_bottleneck.store(
      static_cast<unsigned int>(latency_bottleneck),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_timing_samples.store(
      observation.consecutive_timing_samples, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_timing_confident.store(
      observation.source_timing_confident, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_queue_timing_confident.store(
      observation.queue_timing_confident, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_timing_issue_mask.store(
      observation.source_timing_issue_mask, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_oversubscribed.store(
      recommendation.source_oversubscribed, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_sample_seen.store(
      true, std::memory_order_release);

  const bool pressure_candidate =
      mfgunlock::pacing::ShouldApplyAutomaticLatencyCap(
          guard_mode, marker_health,
          mfgunlock::framecount::g_reflex_hooked.load(
              std::memory_order_acquire),
          mfgunlock::framecount::g_reflex_options_seen.load(
              std::memory_order_acquire),
          recommendation);
  static mfgunlock::latency::QueueTrial trial;
  static mfgunlock::latency::MultiplierTrial multiplier_trial;
  const uint32_t configured_multiplier =
      mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed);
  const unsigned int current_limit_source =
      mfgunlock::framecount::g_reflex_limit_source.load(
          std::memory_order_relaxed);
  const auto user_source_cap_state =
      mfgunlock::framecount::internal::ResolveUserSourceCapState();
  const bool user_source_cap_requested = user_source_cap_state.requested;
  const bool user_source_cap_ready =
      mfgunlock::framecount::internal::UserSourceCapReady();
  const bool user_source_cap_rejected =
      user_source_cap_state.status ==
      mfgunlock::framecount::internal::UserSourceCapStatus::kRejected;
  if (user_source_cap_requested && !user_source_cap_ready &&
      !user_source_cap_rejected) {
    // Apply the user-requested final/output cap once at the established safe
    // Reflex boundary. It is invariant across multiplier trials, so later
    // 6x->5x->4x->3x transitions never need another Reflex replay.
    mfgunlock::framecount::g_latency_guard_refresh_pending.store(
        true, std::memory_order_release);
    mfgunlock::framecount::internal::RefreshReflexTarget();
  }
  const bool base_safe = guard_mode == LatencyGuardMode::kAutomatic &&
      marker_health == mfgunlock::pacing::MarkerHealth::kHealthy &&
      observation.source_timing_confident && sleep_available &&
      observation.sleep.game_sleep &&
      !observation.sleep.dynamic_frame_generation_control &&
      !mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed) &&
      mfgunlock::framecount::g_reflex_options_seen.load(std::memory_order_acquire) &&
      mfgunlock::forcepolicy::IsFixedMultiplier(configured_multiplier);
  const uint32_t old_multiplier_override =
      mfgunlock::framecount::g_latency_guard_multiplier_override.load(
          std::memory_order_relaxed);
  const bool multiplier_safe = base_safe &&
      mfgunlock::framecount::g_latency_guard_active_source_cap_fps.load(
          std::memory_order_relaxed) == 0;
  const auto responsive_reason = multiplier_safe
      ? mfgunlock::latency::ClassifyResponsiveTrialReason(
            recommendation.source_oversubscribed,
            observed_source_interval_us,
            observation.queue_timing_confident
                ? observation.p95_queue_wait_us : 0,
            observation.median_input_to_gpu_end_us,
            pipeline_latency_us,
            observation.median_gpu_active_us,
            observation.median_ai_frame_time_us)
      : mfgunlock::latency::ResponsiveTrialReason::kNone;

  using mfgunlock::latency::ResponsiveTrialBlocker;
  ResponsiveTrialBlocker trial_blocker = ResponsiveTrialBlocker::kNone;
  if (guard_mode != LatencyGuardMode::kAutomatic) {
    trial_blocker = ResponsiveTrialBlocker::kMonitorOnly;
  } else if (marker_health ==
                 mfgunlock::pacing::MarkerHealth::kUnstableTiming ||
             (marker_health == mfgunlock::pacing::MarkerHealth::kHealthy &&
              !observation.source_timing_confident)) {
    trial_blocker = ResponsiveTrialBlocker::kSourceTimingUnverified;
  } else if (marker_health != mfgunlock::pacing::MarkerHealth::kHealthy) {
    trial_blocker = ResponsiveTrialBlocker::kReflexUnhealthy;
  } else if (!sleep_available ||
             observation.sleep.dynamic_frame_generation_control ||
             mfgunlock::framecount::g_dynamic_applied.load(
                 std::memory_order_relaxed)) {
    trial_blocker = ResponsiveTrialBlocker::kDynamicActive;
  } else if (configured_multiplier < 4 || configured_multiplier > 6) {
    trial_blocker = ResponsiveTrialBlocker::kFixedMultiplierRequired;
  } else if (multiplier_trial.phase ==
                 mfgunlock::latency::MultiplierTrialPhase::kIdle &&
             live_multiplier != configured_multiplier) {
    trial_blocker = ResponsiveTrialBlocker::kLiveMultiplierUnconfirmed;
  } else if (!mfgunlock::framecount::g_reflex_options_seen.load(
                 std::memory_order_acquire)) {
    trial_blocker = ResponsiveTrialBlocker::kReflexOptionsUnavailable;
  } else if (user_source_cap_rejected) {
    trial_blocker = ResponsiveTrialBlocker::kUserCapRejected;
  } else if (user_source_cap_requested && !user_source_cap_ready) {
    trial_blocker = ResponsiveTrialBlocker::kUserCapPending;
  } else if (responsive_reason ==
             mfgunlock::latency::ResponsiveTrialReason::kNone) {
    trial_blocker = ResponsiveTrialBlocker::kNoTrigger;
  }
  mfgunlock::framecount::g_latency_guard_trial_blocker.store(
      static_cast<unsigned int>(trial_blocker), std::memory_order_relaxed);
  const mfgunlock::latency::MultiplierTrialSample multiplier_sample{
      observed_source_interval_us,
      queue_wait_us,
      observation.p95_queue_wait_us,
      pipeline_latency_us,
      observation.p95_pipeline_latency_us,
      observation.median_ai_frame_time_us};
  const uint32_t multiplier_override = multiplier_trial.Update(
      now,
      mfgunlock::framecount::g_latency_guard_epoch.load(
          std::memory_order_acquire),
      configured_multiplier, live_multiplier, multiplier_safe,
      responsive_reason, multiplier_sample, user_source_cap_ready);
  mfgunlock::framecount::g_latency_guard_multiplier_override.store(
      multiplier_override, std::memory_order_release);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_accepted.store(
      multiplier_trial.accepted, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_phase.store(
      static_cast<unsigned int>(multiplier_trial.phase),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_reason.store(
      static_cast<unsigned int>(multiplier_trial.reason),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_approved.store(
      multiplier_trial.approved_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_candidate.store(
      multiplier_trial.candidate_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_baseline_samples.store(
      static_cast<unsigned int>(multiplier_trial.baseline_window.count),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_samples.store(
      static_cast<unsigned int>(multiplier_trial.candidate_window.count),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_baseline_pipeline_us.store(
      multiplier_trial.last_compared_baseline.samples != 0
          ? multiplier_trial.last_compared_baseline.pipeline_us
          : multiplier_trial.baseline.pipeline_us,
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_baseline_p95_us.store(
      multiplier_trial.last_compared_baseline.samples != 0
          ? multiplier_trial.last_compared_baseline.pipeline_p95_us
          : multiplier_trial.baseline.pipeline_p95_us,
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_pipeline_us.store(
      multiplier_trial.latest_trial.pipeline_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_p95_us.store(
      multiplier_trial.latest_trial.pipeline_p95_us,
      std::memory_order_relaxed);
  if (old_multiplier_override != multiplier_override) {
    // The next normal game SetOptions submission picks up (or releases) the
    // bounded runtime override. The user output-FPS cap is unchanged, so do
    // not replay Reflex or add a second reconfiguration at this transition.
    mfgunlock::framecount::g_force_failed_for.store(0,
                                                    std::memory_order_relaxed);
  }
  const bool multiplier_fallback_allowed = multiplier_override == 0 &&
      multiplier_trial.attempted &&
      multiplier_trial.phase ==
          mfgunlock::latency::MultiplierTrialPhase::kCooldown;
  const bool multiplier_sequence_active =
      multiplier_trial.phase !=
          mfgunlock::latency::MultiplierTrialPhase::kIdle &&
      multiplier_trial.phase !=
          mfgunlock::latency::MultiplierTrialPhase::kCooldown;
  const bool queue_trim_safe = base_safe &&
      observation.queue_timing_confident && !user_source_cap_requested &&
      mfgunlock::framecount::g_reflex_native_limit_us.load(
          std::memory_order_relaxed) == 0 &&
      (observation.sleep.sleep_interval_us == 0 ||
       current_limit_source == static_cast<unsigned int>(
           mfgunlock::framecount::internal::ReflexTargetSource::kLatencyGuard));
  const auto old_cap = mfgunlock::framecount::g_latency_guard_active_source_cap_fps.load(std::memory_order_relaxed);
  const auto cap = trial.Update(now,
      mfgunlock::framecount::g_latency_guard_epoch.load(std::memory_order_acquire), live_multiplier,
      queue_trim_safe && multiplier_override == 0 && !multiplier_sequence_active &&
          (responsive_reason ==
               mfgunlock::latency::ResponsiveTrialReason::kNone ||
           multiplier_fallback_allowed),
      pressure_candidate && multiplier_override == 0 &&
              !multiplier_sequence_active &&
              (responsive_reason ==
                   mfgunlock::latency::ResponsiveTrialReason::kNone ||
               multiplier_fallback_allowed)
          ? recommendation.source_cap_fps : 0,
      observed_source_interval_us, queue_wait_us, pipeline_latency_us);
  mfgunlock::framecount::g_latency_guard_active_source_cap_fps.store(cap, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_auto_cap_ready.store(cap != 0, std::memory_order_release);
  if (cap != old_cap) {
    mfgunlock::framecount::g_latency_guard_refresh_pending.store(true, std::memory_order_release);
    mfgunlock::framecount::internal::RefreshReflexTarget();
  }
}

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
        DetectedRenderApi detected = DetectedRenderApi::kOther;
        switch (device->get_api()) {
          case reshade::api::device_api::d3d11:
            detected = DetectedRenderApi::kD3D11;
            break;
          case reshade::api::device_api::d3d12:
            detected = DetectedRenderApi::kD3D12;
            break;
          case reshade::api::device_api::vulkan:
            detected = DetectedRenderApi::kVulkan;
            break;
          default:
            break;
        }
        g_render_api.store(detected, std::memory_order_relaxed);
        if (detected != DetectedRenderApi::kD3D11 &&
            detected != DetectedRenderApi::kD3D12)
          ClearDxgiDiagnostics();
        mfgunlock::framecount::NotifyDynamicD3D12(
            detected == DetectedRenderApi::kD3D12,
            detected == DetectedRenderApi::kVulkan,
            detected == DetectedRenderApi::kD3D11 ||
                detected == DetectedRenderApi::kD3D12 ||
                detected == DetectedRenderApi::kVulkan);
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
      const bool hdr = color_space == reshade::api::color_space::scrgb ||
                       color_space == reshade::api::color_space::hdr10_pq ||
                       color_space == reshade::api::color_space::hdr10_hlg;
      mfgunlock::framecount::NotifyHdrState(hdr);
    }
  }
  UpdateReflexPacingLab(swapchain);
  UpdateLatencyGuard(swapchain);
  UpdateVramDiagnostics(swapchain);
  StartDiscoveryWorker();
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
        DetectedRenderApi detected = DetectedRenderApi::kOther;
        switch (device->get_api()) {
          case reshade::api::device_api::d3d11:
            detected = DetectedRenderApi::kD3D11;
            break;
          case reshade::api::device_api::d3d12:
            detected = DetectedRenderApi::kD3D12;
            break;
          case reshade::api::device_api::vulkan:
            detected = DetectedRenderApi::kVulkan;
            break;
          default:
            break;
        }
        g_render_api.store(detected, std::memory_order_relaxed);
        if (detected != DetectedRenderApi::kD3D11 &&
            detected != DetectedRenderApi::kD3D12)
          ClearDxgiDiagnostics();
        mfgunlock::framecount::NotifyDynamicD3D12(
            detected == DetectedRenderApi::kD3D12,
            detected == DetectedRenderApi::kVulkan,
            detected == DetectedRenderApi::kD3D11 ||
                detected == DetectedRenderApi::kD3D12 ||
                detected == DetectedRenderApi::kVulkan);
        const auto color_space = swapchain->get_color_space();
        if (color_space != reshade::api::color_space::unknown) {
          const bool hdr = color_space == reshade::api::color_space::scrgb ||
                           color_space == reshade::api::color_space::hdr10_pq ||
                           color_space == reshade::api::color_space::hdr10_hlg;
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

// These remain immediate, finite retries during graphics-device initialization. They do
// not start a thread and therefore cannot keep a temporary addon instance alive.
void OnInitDevice(reshade::api::device* device) {
  if (device != nullptr) {
    DetectedRenderApi detected = DetectedRenderApi::kOther;
    switch (device->get_api()) {
      case reshade::api::device_api::d3d11:
        detected = DetectedRenderApi::kD3D11;
        break;
      case reshade::api::device_api::d3d12:
        detected = DetectedRenderApi::kD3D12;
        break;
      case reshade::api::device_api::vulkan:
        detected = DetectedRenderApi::kVulkan;
        break;
      default:
        break;
    }
    const bool no_primary =
        g_primary_swapchain.load(std::memory_order_acquire) == nullptr;
    if (no_primary) {
      mfgunlock::framecount::NotifyDynamicD3D12(
          detected == DetectedRenderApi::kD3D12,
          detected == DetectedRenderApi::kVulkan,
          detected == DetectedRenderApi::kD3D11 ||
              detected == DetectedRenderApi::kD3D12 ||
              detected == DetectedRenderApi::kVulkan);
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

  RunProviderMaintenance();
  TryPatchFrameCountCeiling();
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
}

void OnInitCommandQueue(reshade::api::command_queue* /*queue*/) {
  RunProviderMaintenance();
  TryPatchFrameCountCeiling();
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
#if !defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
  if (g_render_api.load(std::memory_order_relaxed) ==
      DetectedRenderApi::kD3D12)
    mfgunlock::cudatemporal::TryInstall();
#endif
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kUiPositive{0.35f, 0.82f, 0.47f, 1.0f};
constexpr ImVec4 kUiWarning{1.0f, 0.76f, 0.25f, 1.0f};
constexpr ImVec4 kUiError{1.0f, 0.38f, 0.34f, 1.0f};
constexpr ImVec4 kUiMuted{0.62f, 0.65f, 0.70f, 1.0f};

void HelpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("[?]");
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void SetNextItemWidthWithHelp(const char* visible_label) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float trailing_width =
      ImGui::CalcTextSize(visible_label).x + style.ItemInnerSpacing.x +
      style.ItemSpacing.x + ImGui::CalcTextSize("[?]").x;
  ImGui::SetNextItemWidth(
      (std::max)(1.0f, ImGui::GetContentRegionAvail().x - trailing_width));
}

void TextDisabledWrapped(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void StatusRow(const char* label, const char* value, const ImVec4& color) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", value);
  ImGui::PopStyleColor();
}

void StatusSectionRow(const char* label) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::Separator();
}

const char* SourceCapConfigOriginText(
    mfgunlock::pacing::SourceCapConfigOrigin origin) {
  using Origin = mfgunlock::pacing::SourceCapConfigOrigin;
  switch (origin) {
    case Origin::kConfigured:
      return "Reflex output cap (compatibility key ReflexSourceFpsCap)";
    case Origin::kLegacyDynamic:
      return "migrated in memory from DynamicReflexSourceCap";
    case Origin::kLegacyFixedOutput:
      return "migrated in memory from FixedOutputFpsCap";
    default:
      return "default/off";
  }
}

const char* UserSourceCapStatusText(
    mfgunlock::framecount::internal::UserSourceCapStatus status) {
  using Status =
      mfgunlock::framecount::internal::UserSourceCapStatus;
  switch (status) {
    case Status::kInactiveGameControlled:
      return "Waiting for fresh driver-confirmed MFG (or select fixed/Dynamic MFG)";
    case Status::kSleepNotNative:
      return "Unavailable: Reflex sleep bypassed/replaced; configured cap is not guaranteed";
    case Status::kWaitingForDynamic:
      return "Waiting for Dynamic MFG to become active";
    case Status::kWaitingForReflex:
      return "Pending: game has not exposed Reflex options";
    case Status::kPending:
      return "Pending application at the next safe Reflex update";
    case Status::kActive:
      return "Active";
    case Status::kNativeLimitStricter:
      return "Active; the game's stricter native cap wins";
    case Status::kRejected:
      return "Rejected by Reflex; native settings restored";
    default:
      return "Off";
  }
}

const ImVec4& UserSourceCapColor(
    mfgunlock::framecount::internal::UserSourceCapStatus status) {
  using Status =
      mfgunlock::framecount::internal::UserSourceCapStatus;
  if (status == Status::kActive ||
      status == Status::kNativeLimitStricter)
    return kUiPositive;
  if (status == Status::kRejected) return kUiError;
  if (status == Status::kOff) return kUiMuted;
  return kUiWarning;
}

std::string UserSourceCapSummary(
    const mfgunlock::framecount::internal::UserSourceCapState& state) {
  if (state.configured_fps == 0) return "0 is Off.";
  std::ostringstream summary;
  summary << "Targets up to ~" << state.configured_fps
          << " displayed FPS";
  if (state.estimated_multiplier >= 2 &&
      state.estimated_multiplier <= 6) {
    const float estimated_source =
        static_cast<float>(state.configured_fps) /
        static_cast<float>(state.estimated_multiplier);
    summary << "; ~" << std::fixed << std::setprecision(1)
            << estimated_source << " game-rendered FPS at "
            << state.estimated_multiplier << "x";
  } else {
    summary << "; base FPS depends on the active multiplier";
  }
  return summary.str();
}

std::string AdaptiveComponentStatus(
    mfgunlock::adaptivequality::ComponentVersion expected,
    mfgunlock::adaptivequality::ComponentVersion actual, bool applied) {
  using Version = mfgunlock::adaptivequality::ComponentVersion;
  if (!applied || actual == Version::kNative) return "Native / not applied";
  if (actual == Version::kMixed)
    return "Mixed versions across providers; execution unverified";
  const std::string version =
      mfgunlock::adaptivequality::ComponentVersionName(actual);
  return version + (actual == expected ? " applied; execution unverified"
                                       : " fallback; execution unverified");
}

const ImVec4& AdaptiveComponentColor(
    mfgunlock::adaptivequality::ComponentVersion expected,
    mfgunlock::adaptivequality::ComponentVersion actual, bool applied) {
  using Version = mfgunlock::adaptivequality::ComponentVersion;
  if (!applied || actual == Version::kNative || actual == Version::kMixed)
    return kUiWarning;
  return actual == expected ? kUiPositive : kUiWarning;
}

const char* InputFormatApiName(uint32_t api) {
  switch (api) {
    case 1: return "DXGI";
    case 2: return "Vulkan";
    default: return "Unknown";
  }
}

const char* InputNativeApiName(mfgunlock::inputdiag::NativeApi api) {
  switch (api) {
    case mfgunlock::inputdiag::NativeApi::D3D11: return "D3D11";
    case mfgunlock::inputdiag::NativeApi::D3D12: return "D3D12";
    case mfgunlock::inputdiag::NativeApi::Vulkan: return "Vulkan";
    default: return "Unknown";
  }
}

const char* InputColorSpaceName(uint32_t value) {
  switch (static_cast<reshade::api::color_space>(value)) {
    case reshade::api::color_space::srgb: return "sRGB";
    case reshade::api::color_space::scrgb: return "scRGB linear";
    case reshade::api::color_space::hdr10_pq: return "HDR10 PQ";
    case reshade::api::color_space::hdr10_hlg: return "HDR10 HLG";
    default: return "Unknown";
  }
}

const char* InputBooleanName(int value) {
  switch (value) {
    case 0: return "No";
    case 1: return "Yes";
    default: return "Unspecified";
  }
}

const char* InputDlssgModeName(uint32_t value) {
  switch (static_cast<sl::DLSSGMode>(value)) {
    case sl::DLSSGMode::eOff: return "Off";
    case sl::DLSSGMode::eOn: return "On";
    case sl::DLSSGMode::eAuto: return "Auto";
    case sl::DLSSGMode::eDynamic: return "Dynamic";
    default: return "Unknown";
  }
}

const char* InputLifecycleName(uint32_t value) {
  switch (value) {
    case sl::eOnlyValidNow: return "Now";
    case sl::eValidUntilPresent: return "Until Present";
    case sl::eValidUntilEvaluate: return "Until Evaluate";
    default: return "Unknown";
  }
}

bool KnownInputFloat(float value) {
  return std::isfinite(value) && value != sl::INVALID_FLOAT;
}

std::string FormatMemoryBytes(uint64_t bytes) {
  if (bytes == 0) return "Not reported";
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(bytes >= (1ull << 30) ? 2 : 0)
         << (bytes >= (1ull << 30)
                 ? static_cast<double>(bytes) / static_cast<double>(1ull << 30)
                 : static_cast<double>(bytes) / static_cast<double>(1ull << 20))
         << (bytes >= (1ull << 30) ? " GiB" : " MiB");
  return stream.str();
}

const char* VramReadinessText(mfgunlock::memorypolicy::EstimateReadiness value) {
  using mfgunlock::memorypolicy::EstimateReadiness;
  switch (value) {
    case EstimateReadiness::kReady: return "Ready";
    case EstimateReadiness::kMissingOptions: return "Waiting for DLSS-G options";
    case EstimateReadiness::kMissingColor: return "Waiting for output metadata";
    case EstimateReadiness::kMissingMotionVectors: return "Waiting for motion-vector metadata";
    case EstimateReadiness::kMissingDepth: return "Waiting for depth metadata";
    case EstimateReadiness::kMissingBackBuffers: return "Waiting for swapchain metadata";
    case EstimateReadiness::kMissingUiInputs: return "Waiting for UI resource metadata";
    default: return "Waiting for inputs";
  }
}

mfgunlock::memorypolicy::TextureInfo MemoryTextureInfo(
    const mfgunlock::inputdiag::TagSummary& tag) {
  mfgunlock::memorypolicy::TextureInfo result{};
  result.present = tag.has_resource;
  result.width = tag.native_desc_seen ? tag.native_desc_width
                                      : (tag.width != 0 ? tag.width
                                                        : tag.resource_width);
  result.height = tag.native_desc_seen ? tag.native_desc_height
                                       : (tag.height != 0 ? tag.height
                                                          : tag.resource_height);
  result.format = tag.native_desc_seen ? tag.native_desc_format
                                       : tag.native_format;
  result.lifecycle = tag.lifecycle;
  return result;
}

void QueueProviderVramEstimateFromCapture() {
  // NVIDIA documents this as an occasional, relatively expensive query. Keep
  // it user-visible and exact-stack-only; never run it continuously in Present.
  if (g_is_star_wars_outlaws ||
      !mfgunlock::framecount::g_dlssg_310_9_1_seen.load(
          std::memory_order_acquire) ||
      !mfgunlock::framecount::g_streamline_2_14_1_active.load(
          std::memory_order_acquire)) {
    mfgunlock::framecount::g_vram_estimate_status.store(
        static_cast<unsigned int>(
            mfgunlock::framecount::VramEstimateStatus::kUnsupported),
        std::memory_order_release);
    return;
  }

  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  mfgunlock::inputdiag::OutputSnapshot output{};
  const bool output_valid = mfgunlock::inputdiag::SnapshotOutput(output);
  for (size_t index = 0; index < count; ++index) {
    const auto& snapshot = snapshots[index];
    const auto& options = snapshot.forwarded_options;
    if (!options.valid || options.calls == 0) continue;
    mfgunlock::memorypolicy::EstimateInputs input{};
    input.viewport = snapshot.viewport;
    input.options_valid = true;
    input.mode = options.mode;
    input.generated_frames = options.generated_frames;
    input.flags = options.flags;
    input.dynamic_width = options.dynamic_width;
    input.dynamic_height = options.dynamic_height;
    input.back_buffers = options.back_buffers != 0
        ? options.back_buffers
        : g_vram_swapchain_buffers.load(std::memory_order_relaxed);
    input.mvec_depth_width = options.mvec_depth_width;
    input.mvec_depth_height = options.mvec_depth_height;
    input.color_width = options.color_width;
    input.color_height = options.color_height;
    input.color_format = options.color_format;
    input.mvec_format = options.mvec_format;
    input.depth_format = options.depth_format;
    input.hudless_format = options.hudless_format;
    input.ui_format = options.ui_format;
    input.queue_parallelism_mode = options.queue_parallelism_mode;
    input.ui_recomposition = options.ui_recomposition;
    input.dynamic_target_fps = KnownInputFloat(options.dynamic_target_fps)
        ? options.dynamic_target_fps : 0.0f;
    if (output_valid) {
      input.swapchain = {output.width, output.height, output.format,
                         static_cast<uint32_t>(sl::eValidUntilPresent), true};
    }
    input.motion = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::MotionVectors)]);
    input.depth = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::Depth)]);
    input.hudless = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::Hudless)]);
    input.ui = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::UiColorAlpha)]);
    if (!input.ui.present) {
      input.ui = MemoryTextureInfo(snapshot.tags[
          static_cast<size_t>(mfgunlock::inputdiag::Kind::UiAlpha)]);
    }
    const auto plan = mfgunlock::memorypolicy::BuildEstimatePlan(input);
    if (plan.readiness == mfgunlock::memorypolicy::EstimateReadiness::kReady)
      mfgunlock::framecount::QueueVramEstimate(snapshot.viewport, plan);
    else
      mfgunlock::framecount::SetVramEstimateWaiting(plan.readiness,
                                                    plan.volatile_inputs);
    return;
  }
  mfgunlock::framecount::SetVramEstimateWaiting(
      mfgunlock::memorypolicy::EstimateReadiness::kMissingOptions);
}

void AppendInputDiagnosticsReport(std::ostringstream& report) {
  report << "Streamline input resource capture: "
         << (mfgunlock::inputdiag::g_enabled.load(std::memory_order_relaxed)
                 ? "Enabled" : "Disabled")
         << " (original game tags before addon filtering; no pixel data)\n";
  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  report << "Input capture skipped/truncated: "
         << mfgunlock::inputdiag::g_dropped_contention.load(std::memory_order_relaxed)
         << '/' << mfgunlock::inputdiag::g_truncated_batches.load(
                        std::memory_order_relaxed) << '\n';
  mfgunlock::inputdiag::OutputSnapshot output{};
  if (mfgunlock::inputdiag::SnapshotOutput(output)) {
    report << "Primary swapchain: " << output.width << 'x' << output.height
           << ", format " << output.format << ", color space "
           << InputColorSpaceName(output.color_space) << " ("
           << output.color_space << "), samples/changes " << output.samples
           << '/' << output.changes << '\n';
  } else {
    report << "Primary swapchain: no complete sample\n";
  }
  for (size_t viewport = 0; viewport < count; ++viewport) {
    const auto& snapshot = snapshots[viewport];
    if (snapshot.tag_batches == 0 && snapshot.constants.calls == 0) continue;
    report << "Input viewport " << snapshot.viewport << " ("
           << InputNativeApiName(snapshot.native_api) << "; "
           << InputFormatApiName(snapshot.format_api) << " formats): "
           << snapshot.tag_batches << " tag batches, "
           << snapshot.frame_aware_batches << " frame-aware\n";
    const auto append_options = [&report](
        const char* label,
        const mfgunlock::inputdiag::OptionsSummary& options) {
      if (options.calls == 0) return;
      report << "  " << label << " DLSS-G options: " << options.calls
             << " calls, " << options.changes << " changes, "
             << options.invalid_base_metadata << " invalid";
      if (!options.valid) {
        report << ", no valid sample\n";
        return;
      }
      report << ", struct v" << options.struct_version
             << ", mode " << InputDlssgModeName(options.mode)
             << ", generated frames " << options.generated_frames
             << " (" << (options.generated_frames + 1) << "x)"
             << ", flags 0x" << std::hex << options.flags << std::dec
             << ", color " << options.color_width << 'x'
             << options.color_height << " format " << options.color_format
             << ", MV/depth " << options.mvec_depth_width << 'x'
             << options.mvec_depth_height
             << ", formats MV/depth/HUD-less/UI "
             << options.mvec_format << '/' << options.depth_format << '/'
             << options.hudless_format << '/' << options.ui_format
             << ", UI recomposition "
             << InputBooleanName(options.ui_recomposition);
      if (KnownInputFloat(options.dynamic_target_fps))
        report << ", dynamic target " << options.dynamic_target_fps;
      report << '\n';
    };
    append_options("Game", snapshot.game_options);
    append_options("Forwarded", snapshot.forwarded_options);
    for (size_t index = 0; index < mfgunlock::inputdiag::kKindCount; ++index) {
      const auto& tag = snapshot.tags[index];
      if (tag.sets == 0 && tag.clears == 0) continue;
      report << "  " << mfgunlock::inputdiag::KindName(
          static_cast<mfgunlock::inputdiag::Kind>(index)) << ": "
          << "tag extent " << tag.width << 'x' << tag.height
          << ", Streamline resource " << tag.resource_width << 'x'
          << tag.resource_height << " format " << tag.native_format;
      if (tag.native_desc_seen) {
        report << ", native desc " << tag.native_desc_width << 'x'
               << tag.native_desc_height << " format "
               << tag.native_desc_format;
      } else {
        report << ", native desc unavailable";
      }
      report << ", lifecycle " << InputLifecycleName(tag.lifecycle)
          << ", last state " << (tag.has_resource ? "set" : "cleared")
          << ", sets/clears " << tag.sets << '/' << tag.clears
          << ", invalid base metadata " << tag.invalid_base_metadata
          << ", metadata/native-desc/identity changes "
          << tag.metadata_changes << '/' << tag.native_desc_changes << '/'
          << tag.native_identity_changes;
      if (tag.frame_known) report << ", last frame " << tag.last_frame;
      report << '\n';
    }
    const auto& constants = snapshot.constants;
    if (constants.calls != 0) {
      report << "  Constants: " << constants.calls << " calls, game resets "
             << constants.game_reset_true << ", invalid base metadata "
             << constants.invalid_base_metadata
             << ", struct v" << constants.struct_version
             << ", last frame " << constants.last_frame
             << ", depth inverted " << InputBooleanName(constants.depth_inverted)
             << ", camera motion " << InputBooleanName(constants.camera_motion_included)
             << ", MV 3D/dilated/jittered "
             << InputBooleanName(constants.motion_vectors_3d) << '/'
             << InputBooleanName(constants.motion_vectors_dilated) << '/'
             << InputBooleanName(constants.motion_vectors_jittered) << '\n';
      if (KnownInputFloat(constants.mvec_scale_x) &&
          KnownInputFloat(constants.mvec_scale_y))
        report << "  Motion-vector scale: " << constants.mvec_scale_x
               << ", " << constants.mvec_scale_y << '\n';
      if (KnownInputFloat(constants.jitter_x) &&
          KnownInputFloat(constants.jitter_y))
        report << "  Jitter: " << constants.jitter_x << ", "
               << constants.jitter_y << '\n';
    }
  }
}

void DrawInputDiagnosticsPanel() {
  if (!ImGui::TreeNode("Streamline input resources (read-only)")) return;
  bool enabled = mfgunlock::inputdiag::g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Capture resource metadata this session", &enabled)) {
    if (enabled) mfgunlock::inputdiag::Clear();
    mfgunlock::inputdiag::g_enabled.store(enabled, std::memory_order_release);
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear input samples")) mfgunlock::inputdiag::Clear();
  ImGui::TextDisabled(
      "Game submissions before Quality Guard; no pixel reads or saved addresses.");
  ImGui::TextDisabled(
      "Relevant tag types may also be used by other Streamline features.");
  ImGui::TextDisabled(
      "Native format is API-specific and does not establish scRGB, PQ or SDR encoding.");
  ImGui::TextDisabled("Skipped lock/truncated batches: %llu / %llu.",
      mfgunlock::inputdiag::g_dropped_contention.load(std::memory_order_relaxed),
      mfgunlock::inputdiag::g_truncated_batches.load(std::memory_order_relaxed));
  mfgunlock::inputdiag::OutputSnapshot output{};
  if (mfgunlock::inputdiag::SnapshotOutput(output)) {
    ImGui::TextDisabled(
        "Primary swapchain: %ux%u, format %u, %s; samples/changes %llu/%llu.",
        output.width, output.height, output.format,
        InputColorSpaceName(output.color_space), output.samples,
        output.changes);
  } else {
    ImGui::TextDisabled("Primary swapchain: waiting for a complete sample.");
  }
  if (mfgunlock::inputdiag::g_viewport_overflow.load(std::memory_order_relaxed))
    ImGui::TextColored(kUiWarning, "More than eight viewports; capture incomplete.");
  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  for (size_t viewport = 0; viewport < count; ++viewport) {
    const auto& snapshot = snapshots[viewport];
    if (snapshot.tag_batches == 0 && snapshot.constants.calls == 0) continue;
    ImGui::PushID(static_cast<int>(snapshot.viewport));
    if (ImGui::TreeNode("viewport", "Viewport %u (%s; %s formats): %llu tag batches",
                        snapshot.viewport,
                        InputNativeApiName(snapshot.native_api),
                        InputFormatApiName(snapshot.format_api),
                        snapshot.tag_batches)) {
      ImGui::TextDisabled("Frame-aware tag batches: %llu", snapshot.frame_aware_batches);
      const auto draw_options = [](
          const char* label,
          const mfgunlock::inputdiag::OptionsSummary& options) {
        if (options.calls == 0) return;
        if (!options.valid) {
          ImGui::TextDisabled("%s options: %llu calls; no valid sample (%llu invalid).",
                              label, options.calls,
                              options.invalid_base_metadata);
          return;
        }
        ImGui::Text(
            "%s options v%u: %s, %u generated (%ux), flags 0x%X",
            label, options.struct_version, InputDlssgModeName(options.mode),
            options.generated_frames, options.generated_frames + 1,
            options.flags);
        ImGui::TextDisabled(
            "Color %ux%u fmt %u | MV/depth %ux%u | formats MV/depth/HUD/UI %u/%u/%u/%u",
            options.color_width, options.color_height, options.color_format,
            options.mvec_depth_width, options.mvec_depth_height,
            options.mvec_format, options.depth_format,
            options.hudless_format, options.ui_format);
        ImGui::TextDisabled(
            "UI recomposition %s | calls %llu | changes %llu | invalid %llu",
            InputBooleanName(options.ui_recomposition), options.calls,
            options.changes, options.invalid_base_metadata);
        if (KnownInputFloat(options.dynamic_target_fps))
          ImGui::TextDisabled("Dynamic target: %.4g FPS",
                              options.dynamic_target_fps);
      };
      draw_options("Game", snapshot.game_options);
      draw_options("Forwarded", snapshot.forwarded_options);
      for (size_t index = 0; index < mfgunlock::inputdiag::kKindCount; ++index) {
        const auto& tag = snapshot.tags[index];
        if (tag.sets == 0 && tag.clears == 0) continue;
        ImGui::Separator();
        ImGui::Text("%s: tag %ux%u; Streamline resource %ux%u, format %u",
                    mfgunlock::inputdiag::KindName(
                        static_cast<mfgunlock::inputdiag::Kind>(index)),
                    tag.width, tag.height, tag.resource_width,
                    tag.resource_height, tag.native_format);
        if (tag.native_desc_seen) {
          ImGui::TextDisabled("Native desc: %ux%u, format %u",
                              tag.native_desc_width, tag.native_desc_height,
                              tag.native_desc_format);
        } else {
          ImGui::TextDisabled("Native desc unavailable for this API/resource.");
        }
        ImGui::TextDisabled(
            "Sets %llu | clears %llu | last state %s | lifecycle %s | base concerns %llu",
            tag.sets, tag.clears, tag.has_resource ? "set" : "cleared",
            InputLifecycleName(tag.lifecycle), tag.invalid_base_metadata);
        ImGui::TextDisabled(
            "Metadata changes %llu | native-desc changes %llu | native identity changes %llu",
            tag.metadata_changes, tag.native_desc_changes,
            tag.native_identity_changes);
        if (tag.frame_known)
          ImGui::TextDisabled("Last frame token: %u", tag.last_frame);
      }
      const auto& constants = snapshot.constants;
      if (constants.calls != 0) {
        ImGui::Separator();
        ImGui::Text("Constants v%u: %llu calls, %llu game reset flags",
                    constants.struct_version, constants.calls,
                    constants.game_reset_true);
        ImGui::TextDisabled(
            "Frame %u | depth inverted %s | camera motion %s | MV 3D/dilated/jittered %s/%s/%s",
            constants.last_frame, InputBooleanName(constants.depth_inverted),
            InputBooleanName(constants.camera_motion_included),
            InputBooleanName(constants.motion_vectors_3d),
            InputBooleanName(constants.motion_vectors_dilated),
            InputBooleanName(constants.motion_vectors_jittered));
        if (KnownInputFloat(constants.mvec_scale_x) &&
            KnownInputFloat(constants.mvec_scale_y))
          ImGui::TextDisabled("MV scale: %.4g, %.4g", constants.mvec_scale_x,
                              constants.mvec_scale_y);
        if (KnownInputFloat(constants.jitter_x) &&
            KnownInputFloat(constants.jitter_y))
          ImGui::TextDisabled("Jitter: %.4g, %.4g", constants.jitter_x,
                              constants.jitter_y);
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  ImGui::TreePop();
}

void SettingLabel(const char* label, const char* summary, const char* tooltip,
                  const char* badge = nullptr,
                  const ImVec4& badge_color = kUiMuted) {
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr) HelpMarker(tooltip);
  if (badge != nullptr) {
    ImGui::SameLine();
    ImGui::TextColored(badge_color, "%s", badge);
  }
  if (summary != nullptr) ImGui::TextDisabled("%s", summary);
}

std::string PackedVersionString(uint64_t version, bool seen) {
  if (!seen) return "Not detected";
  std::ostringstream stream;
  stream << ((version >> 48u) & 0xffffu) << '.'
         << ((version >> 32u) & 0xffffu) << '.'
         << ((version >> 16u) & 0xffffu) << '.'
         << (version & 0xffffu);
  return stream.str();
}

const mfgunlock::nvapistatus::Snapshot& ObserveNvapiUiStatus(
    reshade::api::effect_runtime* runtime) {
  static mfgunlock::nvapistatus::Snapshot cached{};
  static ULONGLONG next_sample = 0;
  const ULONGLONG now = GetTickCount64();
  if (now < next_sample) return cached;
  next_sample = now + 500;

  IUnknown* native_device = nullptr;
  if (runtime != nullptr) {
    if (auto* device = runtime->get_device(); device != nullptr) {
      const auto api = device->get_api();
      if ((api == reshade::api::device_api::d3d11 ||
           api == reshade::api::device_api::d3d12) &&
          device->get_native() != 0) {
        native_device = reinterpret_cast<IUnknown*>(
            static_cast<uintptr_t>(device->get_native()));
      }
    }
  }
  cached = mfgunlock::nvapistatus::Observe(native_device,
                                            GetCurrentProcessId());
  return cached;
}

std::string NgxPresetText(uint32_t preset) {
  std::string result;
  if (preset >= 1 && preset <= 26) {
    result = "Preset ";
    result.push_back(static_cast<char>('A' + preset - 1));
  } else if (preset == 0x00fffffeu) {
    result = "Default preset";
  } else if (preset == 0x00ffffffu) {
    result = "Latest preset";
  } else if (preset == 0) {
    result = "Game/provider controlled";
  } else {
    result = "Unknown (" + std::to_string(preset) + ')';
  }
  return result;
}

const char* NgxPerformanceModeText(uint32_t mode) {
  switch (mode) {
    case 0:
      return "Performance";
    case 1:
      return "Balanced";
    case 2:
      return "Quality";
    case 3:
      return "Game controlled";
    case 4:
      return "DLAA";
    case 5:
      return "Ultra Performance";
    case 6:
      return "Custom";
    default:
      return "Unknown mode";
  }
}

std::string ReconstructionPresetText(
    const mfgunlock::nvapistatus::Snapshot& status, bool ray_reconstruction) {
  using namespace mfgunlock::nvapistatus;
  if (status.ngx_override_status != kOk) return "Not reported";

  const uint64_t feedback = ray_reconstruction
                                ? status.ngx.feedback_ray_reconstruction
                                : status.ngx.feedback_super_resolution;
  if (feedback == 0) return "Not detected";
  const bool active = NgxFeatureActive(feedback);
  const bool configured = NgxFeatureConfigured(feedback);
  if (!active && !configured) return "Not active";

  std::ostringstream stream;
  bool wrote_detail = false;
  if (!active) {
    stream << "Configured";
    wrote_detail = true;
  }
  if ((feedback & kNgxOverridePerformanceMode) != 0) {
    stream << NgxPerformanceModeText(status.ngx.performance_mode);
    wrote_detail = true;
  }
  if ((feedback & kNgxOverrideScalingRatio) != 0 &&
      std::isfinite(status.ngx.scaling_ratio) &&
      status.ngx.scaling_ratio > 0.0f) {
    if (wrote_detail) stream << " | ";
    stream << std::fixed << std::setprecision(1)
           << status.ngx.scaling_ratio * 100.0f << "% render scale";
    wrote_detail = true;
  }
  if ((feedback & kNgxOverridePreset) != 0) {
    if (wrote_detail) stream << " | ";
    stream << NgxPresetText(status.ngx.render_preset)
           << " (NVIDIA override)";
    wrote_detail = true;
  }
  if (!wrote_detail) return "Active; game/provider controlled";
  return stream.str();
}

std::string FrameGenerationPresetText(
    const mfgunlock::nvapistatus::Snapshot& status, bool dlssg_seen) {
  using namespace mfgunlock::nvapistatus;
  if (status.ngx_override_status != kOk) return "Not reported";

  const uint64_t feedback = status.ngx.feedback_frame_generation;
  if (feedback == 0 && !dlssg_seen) return "Not detected";
  if ((feedback & kNgxOverridePreset) == 0) {
    if (NgxFeatureActive(feedback) || dlssg_seen)
      return "Game/provider controlled";
    return NgxFeatureConfigured(feedback)
               ? "Configured; waiting for Frame Generation"
               : "Not detected";
  }

  return NgxPresetText(status.ngx.frame_generation_preset) +
         " (NVIDIA override)";
}

std::string DynamicMultiplierText(
    const mfgunlock::nvapistatus::Snapshot& status,
    unsigned int maximum_generated_frames) {
  if (status.sleep_status != mfgunlock::nvapistatus::kOk ||
      status.sleep.frame_generation_multiplier < 2) {
    return "Dynamic (waiting for live multiplier)";
  }
  std::string result =
      std::to_string(status.sleep.frame_generation_multiplier) + "x live";
  if (maximum_generated_frames != 0) {
    result += " / " + std::to_string(maximum_generated_frames + 1) +
              "x max";
  }
  return result;
}

void DrawAdvancedQualityControls(bool adaptive_quality_managed) {
  constexpr const char* kDepthEdgeModes[] = {
      "Game Default", "Mild", "Balanced", "Strong", "Aggressive"};
  int depth_edge_level = static_cast<int>(
      mfgunlock::framecount::g_depth_edge_guard_level.load(
          std::memory_order_relaxed));
  SetNextItemWidthWithHelp("Depth-edge compatibility");
  if (ImGui::Combo("Depth-edge compatibility", &depth_edge_level,
                   kDepthEdgeModes,
                   static_cast<int>(std::size(kDepthEdgeModes)))) {
    mfgunlock::framecount::g_depth_edge_guard_level.store(
        static_cast<unsigned int>(depth_edge_level),
        std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDepthEdgeTuningChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "DepthEdgeGuardLevel", depth_edge_level);
  }
  HelpMarker(
      "Optional compatibility override for the game's linear-depth separation. Leave on Game Default unless a specific game shows unresolved depth-edge artifacts; stronger values may hurt pacing or thin-detail stability.");

  const auto configured_profile =
      mfgunlock::adaptivequality::NormalizeProfile(
          g_configured_adaptive_quality_profile.load(
              std::memory_order_relaxed));
  if (adaptive_quality_managed &&
      configured_profile ==
          mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
      ImGui::TreeNode("Adaptive Quality V3 A/B controls")) {
    ImGui::TextDisabled(
        "Developer comparisons inside the single V3 profile. Requires restart.");
    bool photometric =
        g_configured_adaptive_quality_v3_photometric.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox("Relative luma/chroma confidence", &photometric)) {
      g_configured_adaptive_quality_v3_photometric.store(
          photometric, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3Photometric",
                                photometric ? 1 : 0);
    }
    HelpMarker(
        "Off retains V2 absolute-RGB confidence and arbitration while the rest of V3 remains available.");

    bool directional_border =
        g_configured_adaptive_quality_v3_directional_border.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox("Motion-directional border taper",
                        &directional_border)) {
      g_configured_adaptive_quality_v3_directional_border.store(
          directional_border, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3DirectionalBorder",
                                directional_border ? 1 : 0);
    }
    HelpMarker(
        "Off returns the warp path to V2's symmetric two-pixel border taper.");

    bool oriented_geometry =
        g_configured_adaptive_quality_v3_oriented_geometry.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox("Motion-oriented diagonal geometry",
                        &oriented_geometry)) {
      g_configured_adaptive_quality_v3_oriented_geometry.store(
          oriented_geometry, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3OrientedGeometry",
                                oriented_geometry ? 1 : 0);
    }
    HelpMarker(
        "Off deliberately requests the validated V2 geometry cubin. No additional texture or shared-memory reads are introduced when enabled.");

#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
    ImGui::TextWrapped(
        "Local low-overhead build: Local Stable geometry + V2 Compatibility inpaint. "
        "Confidence history and launch hooks are disabled. Saved temporal research settings are preserved but ignored.");
#else
    constexpr const char* kStabilityModes[] = {
        "Local Stable (3x3 tile)", "Temporal Stable (confidence history)"};
    int stability_mode = static_cast<int>(
        mfgunlock::cudatemporal::NormalizeMode(
            g_configured_adaptive_quality_v3_stability_mode.load(
                std::memory_order_relaxed))) - 1;
    SetNextItemWidthWithHelp("V3.2 silhouette stability");
    if (ImGui::Combo("V3.2 silhouette stability", &stability_mode,
                     kStabilityModes,
                     static_cast<int>(std::size(kStabilityModes)))) {
      const auto selected = mfgunlock::cudatemporal::NormalizeMode(
          static_cast<unsigned int>(stability_mode + 1));
      g_configured_adaptive_quality_v3_stability_mode.store(
          static_cast<unsigned int>(selected), std::memory_order_relaxed);
      reshade::set_config_value(
          nullptr, kConfigSection, "AdaptiveQualityV3StabilityMode",
          static_cast<int>(selected));
    }
    HelpMarker(
        "Temporal Stable stores only one byte of geometry confidence per pixel, direction and symmetric phase bucket. It starts in read-only probe mode and automatically falls back to Local Stable on any CUDA API, module, phase, stream, allocation or 64 MiB validation failure. Requires restart.");
    if (g_configured_adaptive_quality_v3_stability_mode.load(
            std::memory_order_relaxed) !=
        g_adaptive_quality_v3_stability_mode.load(
            std::memory_order_relaxed)) {
      ImGui::TextDisabled("Saved for next launch; restart the game.");
    }

    constexpr const char* kInpaintModes[] = {
        "V2 Compatibility", "Local V3", "Temporal V3"};
    int inpaint_mode = static_cast<int>(
        mfgunlock::cudatemporal::NormalizeInpaintMode(
            g_configured_adaptive_quality_v3_inpaint_mode.load(
                std::memory_order_relaxed)));
    SetNextItemWidthWithHelp("V3.4 inpaint stability");
    if (ImGui::Combo("V3.4 inpaint stability", &inpaint_mode,
                     kInpaintModes,
                     static_cast<int>(std::size(kInpaintModes)))) {
      const auto selected =
          mfgunlock::cudatemporal::NormalizeInpaintMode(
              static_cast<unsigned int>(inpaint_mode));
      g_configured_adaptive_quality_v3_inpaint_mode.store(
          static_cast<unsigned int>(selected), std::memory_order_relaxed);
      reshade::set_config_value(
          nullptr, kConfigSection, "AdaptiveQualityV3InpaintMode",
          static_cast<int>(selected));
    }
    HelpMarker(
        "Temporal V3 is the default when no saved setting exists. Local V3 separates hard rejects without history. Temporal V3 stores one confidence byte per pixel, direction and symmetric phase bucket; color always comes from the current frame. Invalid saved values fall back to V2 Compatibility. Requires restart.");
    if (g_configured_adaptive_quality_v3_inpaint_mode.load(
            std::memory_order_relaxed) !=
        g_adaptive_quality_v3_inpaint_mode.load(
            std::memory_order_relaxed)) {
      ImGui::TextDisabled("Inpaint mode saved for next launch; restart the game.");
    }
#endif
    ImGui::TreePop();
  }

  if (!ImGui::TreeNode("Legacy quality A/B controls")) return;
  ImGui::TextDisabled(
      "For regression testing only. Adaptive Quality preserves these saved choices but ignores them while it is enabled.");
  if (adaptive_quality_managed) {
    ImGui::TextColored(kUiPositive,
                       "Managed by Adaptive Quality Suite");
    ImGui::BeginDisabled();
  }

  bool refined =
      g_configured_quality_refinement.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Legacy smooth-confidence refinement", &refined)) {
    g_configured_quality_refinement.store(refined,
                                          std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalQualityRefinement",
                              refined ? 1 : 0);
  }
  HelpMarker(
      "Earlier smooth-confidence experiment retained for A/B comparisons. Requires restart.");

  bool intermediate =
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Intermediate scatter retention", &intermediate)) {
    g_configured_thin_geometry_intermediate_scatter.store(
        intermediate, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryIntermediateScatter",
                              intermediate ? 1 : 0);
  }
  HelpMarker(
      "Legacy unconditional retention for fences, foliage and other thin geometry. It can increase trails or flicker and is superseded by Adaptive Quality. Requires restart.");

  int boundary_mode = static_cast<int>(
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed));
  constexpr const char* kBoundaryModes[] = {"Off", "Balanced", "Aggressive"};
  SetNextItemWidthWithHelp("Boundary mitigation A/B");
  if (ImGui::Combo("Boundary mitigation A/B", &boundary_mode,
                   kBoundaryModes,
                   static_cast<int>(std::size(kBoundaryModes)))) {
    g_configured_silhouette_guard_mode.store(
        static_cast<unsigned int>(boundary_mode),
        std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "BoundaryArtifactMitigationMode",
                              boundary_mode);
  }
  HelpMarker(
      "Standalone legacy boundary experiment. Adaptive Quality always uses its integrated Balanced asymmetric path instead. Requires restart.");

  bool validated =
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Validated warp blend A/B", &validated)) {
    g_configured_thin_geometry_validated_warp_blend.store(
        validated, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryValidatedWarpBlend",
                              validated ? 1 : 0);
  }
  HelpMarker(
      "Standalone legacy warp validation. Adaptive Quality uses an integrated version with smooth confidence, border safety and candidate arbitration. Requires restart.");

  bool geometry_v2 =
      g_configured_geometry_confidence_v2.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Geometry confidence V2 A/B", &geometry_v2)) {
    g_configured_geometry_confidence_v2.store(geometry_v2,
                                              std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalGeometryConfidenceV2",
                              geometry_v2 ? 1 : 0);
  }
  HelpMarker(
      "Developer comparison for the prior symmetric geometry-confidence path. Requires the legacy refinement and Balanced boundary settings. Requires restart.");

  bool border =
      g_configured_border_confidence.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Border confidence A/B", &border)) {
    g_configured_border_confidence.store(border, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalBorderConfidence",
                              border ? 1 : 0);
  }
  HelpMarker(
      "Developer comparison for the previous standalone two-pixel screen-border taper. Requires restart.");

  bool previous =
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Previous-to-current scatter (unstable)", &previous)) {
    g_configured_thin_geometry_previous_scatter.store(
        previous, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryPreviousScatter",
                              previous ? 1 : 0);
  }
  HelpMarker(
      "Unstable research path retained only for regression testing. It crashed after restart in earlier testing and is never combined with Adaptive Quality. Requires restart.");

  if (adaptive_quality_managed) ImGui::EndDisabled();
  ImGui::TreePop();
}

void DrawLatencyGuardControl() {
  int mode = static_cast<int>(
      mfgunlock::framecount::g_latency_guard_mode.load(
          std::memory_order_relaxed));
  constexpr const char* kModes[] = {
      "Off", "Monitor Only (Recommended)",
      "Automatic Latency Guard (Advanced)"};

  if (ImGui::BeginTable("##latency_guard_control", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch,
                            0.62f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch,
                            0.38f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Latency Guard",
        mode == static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)
            ? "Tests 6x -> 5x -> 4x -> 3x one step at a time when justified."
            : (mode == static_cast<int>(
                          mfgunlock::pacing::LatencyGuardMode::kMonitor)
                   ? "Read-only Reflex and render-queue monitoring."
                   : "No latency sampling or automatic cap."),
        "Monitor Only is read-only and remains the recommended default. Automatic mode requires healthy, fresh Reflex timing and starts only for sustained queue pressure, output saturation, significant DLSS-G workload or at least 60 ms of marker-to-GPU pipeline time. That 60-ms signal is not end-to-end display latency and only starts a measured trial. With an addon-forced 4x-6x selection it tests one lower multiplier at a time, never below 3x, and keeps only measured improvements. DLSS-G provider reconfiguration can cause a brief hitch whenever a trial or periodic recheck changes the multiplier. A small source-FPS trim remains a queue-only fallback. The saved multiplier, Reflex mode, VSync, G-SYNC, Dynamic MFG and Reflex markers are never rewritten.",
        mode == static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)
            ? "Advanced"
            : nullptr,
        kUiWarning);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##latency_guard", &mode, kModes,
                     static_cast<int>(std::size(kModes)))) {
      mfgunlock::framecount::g_latency_guard_mode.store(
          static_cast<unsigned int>(mode), std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_stable_samples.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_clear_samples.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_candidate_cap_fps.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_active_source_cap_fps.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_multiplier_override.store(
          0, std::memory_order_release);
      mfgunlock::framecount::g_latency_guard_multiplier_trial_accepted.store(
          false, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_auto_cap_ready.store(
          false, std::memory_order_release);
      mfgunlock::framecount::g_latency_guard_refresh_pending.store(
          true, std::memory_order_release);
      mfgunlock::framecount::g_latency_guard_epoch.fetch_add(
          1, std::memory_order_acq_rel);
      reshade::set_config_value(nullptr, kConfigSection, "LatencyGuardMode",
                                mode);
    }
    ImGui::EndTable();
  }
}

const char* ReflexWaitableStateText(
    mfgunlock::reflexpacing::WaitableState state) {
  using State = mfgunlock::reflexpacing::WaitableState;
  switch (state) {
    case State::kWaitingForSwapchain: return "Waiting for compatible swapchain";
    case State::kProbing: return "Native-sleep probe";
    case State::kActivationPending: return "Probe passed; activation pending";
    case State::kActive: return "DXGI Waitable active";
    case State::kFallback: return "Native-sleep fallback";
    default: return "Native Reflex sleep";
  }
}

const char* ReflexModeText(uint32_t mode) {
  switch (mode) {
    case static_cast<uint32_t>(sl::ReflexMode::eOff): return "Off";
    case static_cast<uint32_t>(sl::ReflexMode::eLowLatency): return "On";
    case static_cast<uint32_t>(sl::ReflexMode::eLowLatencyWithBoost):
      return "On + Boost";
    default: return "Unknown";
  }
}

void NotifyReflexPacingSettingChanged() {
  mfgunlock::reflexpacing::ResetCadence();
  mfgunlock::framecount::g_latency_guard_epoch.fetch_add(
      1, std::memory_order_acq_rel);
  mfgunlock::framecount::g_latency_guard_refresh_pending.store(
      true, std::memory_order_release);
}

void DrawReflexPacingLabControl() {
  using namespace mfgunlock;
  reflexpacing::g_ui_heartbeat_ms.store(GetTickCount64(),
                                        std::memory_order_release);
  constexpr const char* kModeOverrides[] = {
      "Game controlled", "Off (FG-safe sleep bypass)", "Force On",
      "Force On + Boost"};
  int mode_override = static_cast<int>(pacing::NormalizeReflexModeOverride(
      reflexpacing::g_mode_override.load(std::memory_order_relaxed)));
  SetNextItemWidthWithHelp("Reflex mode override");
  if (ImGui::Combo("Reflex mode override", &mode_override, kModeOverrides,
                   static_cast<int>(std::size(kModeOverrides)))) {
    reflexpacing::g_mode_override.store(
        static_cast<uint32_t>(mode_override), std::memory_order_relaxed);
    reflexpacing::g_mode_rejected.store(false, std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection, "ReflexModeOverride",
                              mode_override);
  }
  HelpMarker(
      "FG-safe Off keeps Streamline's internal Reflex mode On because several DLSS-G integrations disable Frame Generation when mode Off is submitted. It bypasses slReflexSleep instead, while preserving frame tokens and markers. With DXGI Waitable selected, the waitable pacer replaces that sleep after its safety probe. On + Boost may improve clock stability at the cost of power and temperature.");

  constexpr const char* kPacingMethods[] = {
      "Native Reflex Sleep", "DXGI Waitable (Experimental)"};
  int pacing_method = static_cast<int>(pacing::NormalizeReflexPacingMethod(
      reflexpacing::g_pacing_method.load(std::memory_order_relaxed)));
  SetNextItemWidthWithHelp("Pacing method");
  if (ImGui::Combo("Pacing method", &pacing_method, kPacingMethods,
                   static_cast<int>(std::size(kPacingMethods)))) {
    reflexpacing::g_pacing_method.store(
        static_cast<uint32_t>(pacing_method), std::memory_order_release);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection, "ReflexPacingMethod",
                              pacing_method);
  }
  HelpMarker(
      "Waitable mode activates only after a 120-frame native probe and only when the game already created a D3D11/D3D12 waitable swapchain. It never injects the flag or recreates the swapchain.");

  bool headroom =
      reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("VRR headroom cap", &headroom)) {
    reflexpacing::g_headroom_enabled.store(headroom,
                                           std::memory_order_relaxed);
    if (!headroom)
      reflexpacing::g_headroom_limit_us.store(0,
                                              std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "ReflexVrrHeadroomEnabled", headroom ? 1 : 0);
  }
  int basis_points = static_cast<int>(pacing::NormalizeHeadroomBasisPoints(
      reflexpacing::g_headroom_basis_points.load(std::memory_order_relaxed)));
  float headroom_percent = static_cast<float>(basis_points) / 100.0f;
  if (!headroom) ImGui::BeginDisabled();
  SetNextItemWidthWithHelp("VRR headroom");
  if (ImGui::SliderFloat("VRR headroom", &headroom_percent, 0.5f, 3.0f,
                         "%.1f%%")) {
    basis_points = std::clamp(
        static_cast<int>(std::lround(headroom_percent * 100.0f)), 50, 300);
    reflexpacing::g_headroom_basis_points.store(
        static_cast<uint32_t>(basis_points), std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "ReflexVrrHeadroomBasisPoints", basis_points);
  }
  if (!headroom) ImGui::EndDisabled();
  HelpMarker(
      "Uses Reflex's final/output limiter and preserves every stricter game, user or Latency Guard cap. It activates only while NVAPI reports VRR/G-SYNC.");

  const auto state = reflexpacing::g_waitable_state.load(
      std::memory_order_acquire);
  const auto reason = reflexpacing::g_fallback_reason.load(
      std::memory_order_relaxed);
  const auto sleep_intervals = reflexpacing::g_sleep_intervals.Stats();
  const auto sleep_durations = reflexpacing::g_sleep_durations.Stats();
  const auto present_intervals = reflexpacing::g_present_intervals.Stats();
  ImGui::Spacing();
  if (ImGui::BeginTable("##reflex_pacing_lab_status", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("Effective", ImGuiTableColumnFlags_WidthStretch,
                            0.55f);
    const bool fg_safe_off =
        pacing::NormalizeReflexModeOverride(
            reflexpacing::g_mode_override.load(std::memory_order_relaxed)) ==
        pacing::ReflexModeOverride::kOff;
    const auto sleep_dispatch = reflexpacing::g_sleep_dispatch.load(
        std::memory_order_relaxed);
    std::string pacing_status = ReflexWaitableStateText(state);
    if (sleep_dispatch == reflexpacing::SleepDispatch::kBypass) {
      pacing_status = "FG-safe Reflex sleep bypass active";
    } else if (sleep_dispatch == reflexpacing::SleepDispatch::kWaitable &&
               fg_safe_off &&
               (state == reflexpacing::WaitableState::kNative ||
                state == reflexpacing::WaitableState::kWaitingForSwapchain ||
                state == reflexpacing::WaitableState::kFallback)) {
      pacing_status += "; FG-safe bypass while waitable unavailable";
    }
    StatusRow("Pacing", pacing_status.c_str(),
              state == reflexpacing::WaitableState::kActive
                  ? kUiPositive
                  : (sleep_dispatch == reflexpacing::SleepDispatch::kBypass
                         ? kUiPositive
                         : (state == reflexpacing::WaitableState::kFallback
                         ? kUiWarning
                         : kUiMuted)));
    const uint32_t native_mode = reflexpacing::g_native_mode.load(
        std::memory_order_relaxed);
    const uint32_t forwarded_mode = reflexpacing::g_forwarded_mode.load(
        std::memory_order_relaxed);
    const std::string reflex_mode = fg_safe_off
        ? std::string(ReflexModeText(native_mode)) + " -> " +
              ReflexModeText(forwarded_mode) +
              " internally; Reflex sleep bypass requested"
        : std::string(ReflexModeText(native_mode)) + " -> " +
              ReflexModeText(forwarded_mode) +
              (reflexpacing::g_mode_rejected.load(std::memory_order_relaxed)
                   ? " (override rejected)"
                   : "");
    StatusRow("Reflex game -> effective", reflex_mode.c_str(),
              reflexpacing::g_mode_rejected.load(std::memory_order_relaxed)
                  ? kUiWarning
                  : (reflexpacing::g_mode_applied.load(
                         std::memory_order_relaxed)
                         ? kUiPositive
                         : kUiMuted));
    if (state == reflexpacing::WaitableState::kFallback)
      StatusRow("Fallback reason", ReflexWaitableFallbackText(reason),
                kUiWarning);
    const std::string probe = std::to_string(
        reflexpacing::g_probe_samples.load(std::memory_order_relaxed)) +
        "/120";
    StatusRow("Waitable probe", probe.c_str(), kUiMuted);
    const std::string dxgi_latency = std::to_string(
        reflexpacing::g_native_maximum_latency.load(
            std::memory_order_relaxed)) +
        " -> " + std::to_string(
            reflexpacing::g_effective_maximum_latency.load(
                std::memory_order_relaxed));
    StatusRow("DXGI MaximumFrameLatency", dxgi_latency.c_str(), kUiMuted);
    const std::string wait_health =
        std::string(reflexpacing::g_waitable_handle.load(
                        std::memory_order_acquire) != nullptr
                        ? "Handle ready"
                        : "No active handle") +
        "; timeouts=" +
        std::to_string(reflexpacing::g_timeout_count.load(
            std::memory_order_relaxed)) +
        "; failures=" +
        std::to_string(reflexpacing::g_wait_failure_count.load(
            std::memory_order_relaxed));
    StatusRow("Waitable health", wait_health.c_str(), kUiMuted);
    const uint32_t refresh = reflexpacing::g_refresh_millihz.load(
        std::memory_order_relaxed);
    std::ostringstream display;
    display << std::fixed << std::setprecision(2)
            << static_cast<double>(refresh) / 1000.0 << " Hz; VRR "
            << (reflexpacing::g_vrr_active.load(std::memory_order_relaxed)
                    ? "active"
                    : "inactive/unavailable");
    StatusRow("Display", display.str().c_str(), kUiMuted);
    const uint32_t headroom_limit = reflexpacing::g_headroom_limit_us.load(
        std::memory_order_relaxed);
    const std::string cap = headroom_limit == 0
        ? "Inactive"
        : std::to_string(headroom_limit) + " us (" +
              std::to_string(pacing::FrameLimitUsToFps(headroom_limit)) +
              " FPS approx.)";
    StatusRow("VRR headroom", cap.c_str(),
              headroom_limit != 0 ? kUiPositive : kUiMuted);
    auto cadence_text = [](const reflexpacing::SampleStats& stats) {
      if (stats.count == 0) return std::string("No samples");
      std::ostringstream text;
      text << stats.median_us / 1000.0 << "/" << stats.p95_us / 1000.0
           << "/" << stats.p99_us / 1000.0 << " ms; delta "
           << stats.mean_delta_us / 1000.0 << " ms; n=" << stats.count;
      return text.str();
    };
    const auto sleep_interval_text = cadence_text(sleep_intervals);
    const auto sleep_duration_text = cadence_text(sleep_durations);
    const auto present_interval_text = cadence_text(present_intervals);
    StatusRow("Sleep interval med/p95/p99", sleep_interval_text.c_str(),
              kUiMuted);
    StatusRow("Sleep duration med/p95/p99", sleep_duration_text.c_str(),
              kUiMuted);
    StatusRow("App Present med/p95/p99", present_interval_text.c_str(),
              kUiMuted);
    ImGui::EndTable();
  }
  ImGui::TextDisabled(
      "These are application-cadence proxies, not generated/displayed frames. Validate final output with PresentMon or FrameView.");
}

const char* MarkerHealthText(mfgunlock::pacing::MarkerHealth health) {
  using mfgunlock::pacing::MarkerHealth;
  switch (health) {
    case MarkerHealth::kWaiting:
      return "Waiting for latency frames";
    case MarkerHealth::kHealthy:
      return "Healthy";
    case MarkerHealth::kMissingSleep:
      return "Game is not calling Reflex sleep";
    case MarkerHealth::kIncomplete:
      return "Incomplete Reflex markers";
    case MarkerHealth::kInvalidOrder:
      return "Invalid marker order";
    case MarkerHealth::kUnstableTiming:
      return "Source timing validation failed";
    default:
      return "Unavailable";
  }
}

const char* TimingValidationText(uint32_t issues) {
  using namespace mfgunlock::latency;
  if (issues == kTimingOk) return "Verified from simulation + Present markers";
  if ((issues & kTimingUnitsUnknown) != 0)
    return "Timestamp scale could not be validated";
  if ((issues & kTimingSimulationCadenceInvalid) != 0)
    return "Simulation cadence is missing or outside the valid range";
  if ((issues & kTimingPresentCadenceMissing) != 0)
    return "Not enough consecutive Present markers";
  if ((issues & kTimingPresentCadenceMismatch) != 0)
    return "Present cadence does not match simulation cadence";
  if ((issues & kTimingSimulationCadenceUnstable) != 0)
    return "Simulation cadence is too unstable for automatic control";
  if ((issues & kTimingPresentCadenceUnstable) != 0)
    return "Present cadence is too unstable for automatic control";
  if ((issues & kTimingInsufficientConsecutiveFrames) != 0)
    return "Waiting for 49 consecutive Reflex frames";
  if ((issues & kTimingNotFresh) != 0)
    return "No sufficiently fresh completed-frame window";
  return "Unclassified timing validation failure";
}

const char* ValidationStageName(mfgunlock::validation::Stage stage) {
  using mfgunlock::validation::Stage;
  switch (stage) {
    case Stage::kNativeRequestObserved:
      return "native request observed";
    case Stage::kRequestAccepted:
      return "request accepted";
    case Stage::kDriverMultiplierActive:
      return "driver multiplier active";
    case Stage::kProviderOutputConfirmed:
      return "provider output confirmed";
    case Stage::kRequestRejected:
      return "request rejected; native fallback preserved";
    case Stage::kRuntimeError:
      return "runtime error";
    case Stage::kWaitingForGameRequest:
    default:
      return "waiting for game request";
  }
}

std::string ValidationStatusText(mfgunlock::validation::Stage stage,
                                 bool dynamic_accepted,
                                 bool effective_seen,
                                 unsigned int effective_multiplier,
                                 unsigned int driver_multiplier,
                                 unsigned int provider_presentations) {
  using mfgunlock::validation::Stage;
  switch (stage) {
    case Stage::kRuntimeError:
      return "DLSS-G runtime reported an error";
    case Stage::kRequestRejected:
      return "Addon request rejected; game fallback preserved";
    case Stage::kProviderOutputConfirmed:
      return std::to_string(provider_presentations) +
             " presentation(s) reported by provider";
    case Stage::kDriverMultiplierActive:
      return std::to_string(driver_multiplier) +
             "x live reported by driver";
    case Stage::kRequestAccepted:
      if (dynamic_accepted)
        return "Dynamic request accepted; output telemetry unavailable";
      if (effective_seen)
        return std::to_string(effective_multiplier) +
               "x request accepted; output telemetry unavailable";
      return "Request accepted; output telemetry unavailable";
    case Stage::kNativeRequestObserved:
      return "Game request observed; waiting for runtime result";
    case Stage::kWaitingForGameRequest:
    default:
      return "Waiting for game Frame Generation request";
  }
}

const char* LatencyBottleneckText(
    mfgunlock::pacing::LatencyBottleneck bottleneck) {
  using mfgunlock::pacing::LatencyBottleneck;
  switch (bottleneck) {
    case LatencyBottleneck::kBalanced:
      return "No dominant avoidable stage observed";
    case LatencyBottleneck::kDisplayOversubscription:
      return "Output oversubscription + render queue";
    case LatencyBottleneck::kRenderQueue:
      return "Render queue";
    case LatencyBottleneck::kFrameGeneration:
      return "DLSS-G workload";
    case LatencyBottleneck::kGpuWork:
      return "Base GPU workload";
    case LatencyBottleneck::kCpuOrSource:
      return "CPU/source-frame interval";
    default:
      return "Insufficient fresh timing data";
  }
}

const char* ResponsiveTrialReasonText(
    mfgunlock::latency::ResponsiveTrialReason reason) {
  using mfgunlock::latency::ResponsiveTrialReason;
  switch (reason) {
    case ResponsiveTrialReason::kDisplayOversubscription:
      return "Output saturation";
    case ResponsiveTrialReason::kRenderQueue:
      return "Sustained render queue";
    case ResponsiveTrialReason::kHighInputLatency:
      return "Input-to-GPU latency at or above 60 ms";
    case ResponsiveTrialReason::kHighPipelineLatency:
      return "Estimated marker-to-GPU pipeline at or above 60 ms";
    case ResponsiveTrialReason::kGpuSaturatedLatencyProxy:
      return "GPU saturation + long marker pipeline proxy";
    case ResponsiveTrialReason::kFrameGenerationWorkload:
      return "Significant DLSS-G workload";
    default:
      return "No responsive trial requested";
  }
}

const char* ResponsiveTrialBlockerText(
    mfgunlock::latency::ResponsiveTrialBlocker blocker) {
  using mfgunlock::latency::ResponsiveTrialBlocker;
  switch (blocker) {
    case ResponsiveTrialBlocker::kNone:
      return "Eligible; collecting a stable trigger";
    case ResponsiveTrialBlocker::kMonitorOnly:
      return "Automatic mode is not enabled";
    case ResponsiveTrialBlocker::kReflexUnhealthy:
      return "Waiting for 48 healthy Reflex timing frames";
    case ResponsiveTrialBlocker::kSourceTimingUnverified:
      return "Source timing is stale or unverified";
    case ResponsiveTrialBlocker::kDynamicActive:
      return "Dynamic MFG is active or driver-controlled";
    case ResponsiveTrialBlocker::kFixedMultiplierRequired:
      return "Select a fixed 4x, 5x or 6x multiplier";
    case ResponsiveTrialBlocker::kLiveMultiplierUnconfirmed:
      return "Waiting for the driver to confirm the fixed multiplier";
    case ResponsiveTrialBlocker::kReflexOptionsUnavailable:
      return "The game has not exposed slReflexSetOptions";
    case ResponsiveTrialBlocker::kUserCapPending:
      return "Waiting for the Reflex output cap to be applied once";
    case ResponsiveTrialBlocker::kUserCapRejected:
      return "Output-FPS cap was rejected; native Reflex settings restored";
    case ResponsiveTrialBlocker::kNoTrigger:
      return "No sustained latency, saturation, queue or workload trigger";
    default:
      return "Eligibility unavailable";
  }
}

const char* MultiplierTrialPhaseText(
    mfgunlock::latency::MultiplierTrialPhase phase) {
  using mfgunlock::latency::MultiplierTrialPhase;
  switch (phase) {
    case MultiplierTrialPhase::kCollectingBaseline:
      return "Collecting current-multiplier baseline";
    case MultiplierTrialPhase::kWaitingForCandidate:
      return "Waiting for the next lower multiplier";
    case MultiplierTrialPhase::kMeasuringCandidate:
      return "Measuring the next lower multiplier";
    case MultiplierTrialPhase::kAccepted:
      return "Last lower multiplier measured beneficial";
    case MultiplierTrialPhase::kWaitingForOriginal:
      return "Restoring saved multiplier for periodic check";
    case MultiplierTrialPhase::kMeasuringOriginal:
      return "Rechecking the saved multiplier";
    case MultiplierTrialPhase::kCooldown:
      return "No benefit measured; cooldown active";
    default:
      return "Monitoring; no responsive trial active";
  }
}

ThinGeometryModulePatch SelectRelevantThinGeometryResult(
    const std::vector<ThinGeometryModulePatch>& modules) {
  if (modules.empty()) return {};
  const auto applied = std::find_if(
      modules.rbegin(), modules.rend(), [](const ThinGeometryModulePatch& item) {
        return item.result.validated_warp_blend.applied ||
               item.result.previous_scatter.applied ||
               item.intermediate_scatter.applied ||
               item.silhouette_boundary_guard.applied;
      });
  return applied != modules.rend() ? *applied : modules.back();
}

void DrawLegacyOverlay(reshade::api::effect_runtime* /*runtime*/) {
  bool gate_patched = false;
  bool midpoint_patched = false;
  bool blackwell_patched = false;
  bool thin_geometry_patched = false;
  size_t gate_provider_count = 0;
  size_t gate_site_count = 0;
  size_t midpoint_provider_count = 0;
  size_t blackwell_provider_count = 0;
  size_t thin_geometry_provider_count = 0;
  std::string midpoint_detail;
  std::string blackwell_detail;
  std::string blackwell_applied_detail;
  mfgunlock::blackwell::Result blackwell_active_result{};
  ThinGeometryModulePatch thin_geometry_last;
  bool ceiling_patched = false;
  unsigned int ceiling_compiled = 0;
  unsigned int ceiling_effective = 0;
  size_t flip_meter_site_count = 0;
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    ImGui::TextDisabled("Diagnostics updating; retry next frame.");
    return;
  }
  gate_patched = g_gate_patched.load(std::memory_order_relaxed);
  midpoint_patched = g_midpoint_patched.load(std::memory_order_relaxed);
  blackwell_patched = g_blackwell_patched.load(std::memory_order_relaxed);
  thin_geometry_patched = g_thin_geometry_patched.load(std::memory_order_relaxed);
  gate_provider_count = g_gate_modules.size();
  gate_site_count = g_gate_sites.size();
  midpoint_provider_count = g_midpoint_modules.size();
  blackwell_provider_count = g_blackwell_modules.size();
  thin_geometry_provider_count = g_thin_geometry_modules.size();
  midpoint_detail = g_midpoint_detail;
  blackwell_detail = g_blackwell_detail;
  blackwell_applied_detail = g_blackwell_applied_detail;
  blackwell_active_result = AggregateBlackwellResults(g_blackwell_modules);
  thin_geometry_last =
      SelectRelevantThinGeometryResult(g_thin_geometry_modules);
  ReleaseSRWLockShared(&g_provider_maintenance_lock);
  if (!TryAcquireSRWLockShared(&g_streamline_maintenance_lock)) {
    ImGui::TextDisabled("Diagnostics updating; retry next frame.");
    return;
  }
  ceiling_patched = g_ceiling_patched.load(std::memory_order_relaxed);
  ceiling_compiled = g_ceiling_compiled;
  ceiling_effective = g_ceiling_effective;
  flip_meter_site_count = g_flip_meter_sites.size();
  ReleaseSRWLockShared(&g_streamline_maintenance_lock);

  bool configured_enabled = g_configured_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Enable addon on next restart", &configured_enabled)) {
    g_configured_enabled.store(configured_enabled, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "Enabled",
                              configured_enabled ? 1 : 0);
  }
  const bool enabled = g_enabled.load(std::memory_order_relaxed);
  if (configured_enabled != enabled) {
    ImGui::TextDisabled("Restart required; this session remains %s.",
                        enabled ? "enabled" : "disabled");
  }

  int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
  if (ImGui::SliderInt("Reported MultiFrameCountMax", &count,
                       static_cast<int>(kMinCount), static_cast<int>(kMaxCount))) {
    if (count < static_cast<int>(kMinCount)) count = static_cast<int>(kMinCount);
    if (count > static_cast<int>(kMaxCount)) count = static_cast<int>(kMaxCount);
    g_max_count.store(static_cast<unsigned int>(count), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
  }
  ImGui::TextDisabled(
      "Takes effect when DLSS-G next queries the runtime -- toggle frame\n"
      "generation off and on in the game if the option does not appear.");

  const DetectedRenderApi render_api = g_render_api.load(std::memory_order_relaxed);
  ImGui::Text("Renderer detected by ReShade: %s%s", RenderApiName(render_api),
              render_api == DetectedRenderApi::kVulkan ? " (experimental)" : "");

  constexpr const char* kRuntimeModes[] = {
      "Game default (recommended)",
      "Prefer local runtime - disable OTA",
      "Force NVIDIA OTA runtime"};
  int runtime_mode = static_cast<int>(
      g_configured_runtime_selection_mode.load(std::memory_order_relaxed));
  if (ImGui::Combo("Streamline runtime selection", &runtime_mode, kRuntimeModes,
                   static_cast<int>(std::size(kRuntimeModes)))) {
    g_configured_runtime_selection_mode.store(
        static_cast<unsigned int>(runtime_mode), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "RuntimeSelectionMode",
                              runtime_mode);
  }
  ImGui::TextDisabled(
      "Restart required. Local mode clears Streamline's OTA flags; OTA mode sets them.\n"
      "Use a complete, version-matched Streamline package rather than mixing DLL versions.");
  const int active_runtime_mode = static_cast<int>(
      mfgunlock::framecount::g_runtime_selection_mode.load(
          std::memory_order_relaxed));
  if (runtime_mode != active_runtime_mode) {
    ImGui::TextDisabled("Runtime-selection change is saved for the next restart.");
  }
  if (active_runtime_mode != static_cast<int>(
                                 mfgunlock::framecount::RuntimeSelectionMode::kGameDefault)) {
    if (mfgunlock::framecount::g_runtime_selection_observed.load(
            std::memory_order_acquire)) {
      ImGui::Text("slInit policy: flags 0x%llx -> 0x%llx; result %u.",
                  mfgunlock::framecount::g_runtime_flags_before.load(
                      std::memory_order_relaxed),
                  mfgunlock::framecount::g_runtime_flags_after.load(
                      std::memory_order_relaxed),
                  mfgunlock::framecount::g_runtime_selection_result.load(
                      std::memory_order_relaxed));
    } else {
      ImGui::TextDisabled(
          "The active runtime policy did not reach slInit during this launch.");
      if (!HasEarlyLoadEntry()) {
        ImGui::TextWrapped(
            "This game initialized Streamline before normal ReShade addon loading. "
            "Early loading is required for the selected local/OTA policy.");
        if (ImGui::Button("Enable early addon loading for next restart")) {
          if (EnsureEarlyLoadEntry()) {
            reshade::log::message(
                reshade::log::level::info,
                "mfgunlock: added this addon to ADDON.LoadFromDllMain; restart required for the Streamline runtime-selection hook.");
          } else {
            reshade::log::message(
                reshade::log::level::error,
                "mfgunlock: could not update ADDON.LoadFromDllMain; add renodx-mfgunlock.addon64 manually and restart.");
          }
        }
      } else {
        ImGui::TextDisabled(
            "Early loading is configured, but slInit was not observed; verify that the "
            "configured addon filename matches the loaded file, then restart fully.");
      }
    }
  }

  bool flip_off =
      g_configured_force_flip_meter_off.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Force legacy software flip pacing on next restart (compatibility)",
                      &flip_off)) {
    g_configured_force_flip_meter_off.store(flip_off,
                                             std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", flip_off ? 1 : 0);
  }
  ImGui::TextDisabled(
      "Leave off with current Streamline builds. Enable only if 3x/4x freezes;\n"
      "applied once per session, so restart the game after changing it.");

  ImGui::Separator();
  const bool active_flip_off =
      g_force_flip_meter_off.load(std::memory_order_relaxed);
  if (flip_off != active_flip_off) {
    ImGui::TextDisabled("Legacy pacing change is saved for the next restart.");
  }
  if (!active_flip_off) {
    ImGui::TextDisabled("Native Streamline pacing active; no legacy metering patch requested.");
  } else if (g_flip_meter_patched.load(std::memory_order_acquire)) {
    ImGui::Text("Flip-metering forced off: +0x%x pinned to %u, %zu site(s).",
                g_flip_meter_offset.load(std::memory_order_relaxed),
                g_flip_meter_value.load(std::memory_order_relaxed),
                flip_meter_site_count);
  } else if (g_flip_meter_attempts.load(std::memory_order_relaxed) >=
             kMaxFlipMeterAttempts) {
    // The counter only advances once the plugin HAS been found, and both give-up
    // paths slam it to the maximum -- so this state is "found it, could not patch
    // it", which is the opposite of what this line used to say.
    ImGui::TextDisabled("DLSS-G plugin found, but flip metering could not be patched.");
    ImGui::TextDisabled("See the ReShade log for the module and the step that failed.");
  } else if (g_flip_meter_attempts.load(std::memory_order_relaxed) > 0) {
    ImGui::TextDisabled("DLSS-G plugin found; flip-metering patch pending (%d).",
                        g_flip_meter_attempts.load(std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("DLSS-G plugin not located yet -- turn frame generation on.");
  }
  if (ceiling_patched) {
    ImGui::Text("Streamline device-limit bypassed: compiled %ux, effective %ux.",
                ceiling_compiled + 1, ceiling_effective + 1);
  }
  if (mfgunlock::framecount::g_capacity_advertised.load(std::memory_order_relaxed)) {
    ImGui::Text("Native menu maximum: runtime %ux, advertised %ux.",
                mfgunlock::framecount::g_runtime_max_generated.load(std::memory_order_relaxed) + 1,
                mfgunlock::framecount::g_advertised_max_generated.load(
                    std::memory_order_relaxed) + 1);
  }
  if (mfgunlock::framecount::g_state_seen.load(std::memory_order_relaxed)) {
    const unsigned int status =
        mfgunlock::framecount::g_dlssg_status.load(std::memory_order_relaxed);
    ImGui::Text("Actual presentations since last state query: %u.",
                mfgunlock::framecount::g_actual_frames_presented.load(std::memory_order_relaxed));
    if (status != 0) {
      ImGui::Text("DLSS-G runtime status: failure flags 0x%x.", status);
    } else {
      const unsigned int observed =
          mfgunlock::framecount::g_max_actual_frames_presented.load(std::memory_order_relaxed);
      if (observed > 1) {
        ImGui::Text("MFG validation: active; observed up to %u actual presentations.", observed);
      } else {
        ImGui::TextDisabled("DLSS-G status is OK; generated output has not been confirmed yet.");
      }
    }
  } else {
    ImGui::TextDisabled("No successful slDLSSGGetState telemetry sample yet (last result: %u).",
                        mfgunlock::framecount::g_state_result.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  constexpr const char* kHdrModes[] = {
      "Native (Default — recommended for most games)",
      "Force UI Composition (Advanced)",
      "Automatic Guard + UI Composition (HDR compatibility)",
      "Final Color Fallback (Troubleshooting)"};
  int hdr_mode = static_cast<int>(
      mfgunlock::framecount::g_hdr_compatibility_mode.load(std::memory_order_relaxed));
  if (ImGui::Combo("Frame-generation input quality", &hdr_mode, kHdrModes,
                   static_cast<int>(std::size(kHdrModes)))) {
    mfgunlock::framecount::g_hdr_compatibility_mode.store(
        static_cast<unsigned int>(hdr_mode), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyQualityModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "HDRCompatibilityMode", hdr_mode);
  }
  if (hdr_mode == static_cast<int>(
                      mfgunlock::framecount::HdrCompatibilityMode::kNative)) {
    ImGui::TextDisabled(
        "Passes the game's optional HUD-less and UI tags through unchanged.\n"
        "Recommended for most games; use this if HUD elements show artifacts.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
    ImGui::TextDisabled(
        "Forces Streamline to process the scene and UI separately. This requires\n"
        "the game to provide correctly matched buffers and color spaces.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
    ImGui::TextDisabled(
        "Rejects optional HUD/UI inputs when their metadata is invalid and uses\n"
        "final color as the safe fallback. It also resets temporal history once\n"
        "after HDR, swapchain, resolution, option or multiplier transitions.");
  } else {
    ImGui::TextDisabled(
        "Use for HDR artifacts in games such as Hogwarts Legacy, Jusant, and\n"
        "Mafia: The Old Country. If HUD elements show artifacts, use Native.\n"
        "Matched SDR inputs may use UI Composition; unsafe inputs fall back safely.");
  }
  ImGui::TextDisabled(
      "Color, depth, motion vectors, temporal kernel and frame pacing are not rewritten.");
  const bool hdr_active =
      mfgunlock::framecount::g_hdr_active.load(std::memory_order_relaxed);
  const bool hdr_seen =
      mfgunlock::framecount::g_hdr_state_seen.load(std::memory_order_acquire);
  ImGui::Text("HDR output detected: %s.",
              hdr_seen ? (hdr_active ? "yes" : "no") : "not observed yet");
  if (mfgunlock::framecount::g_ui_recomposition_applied.load(std::memory_order_relaxed)) {
    ImGui::Text("Streamline accepted the UI-capable path (source options v%u).",
                mfgunlock::framecount::g_ui_recomposition_source_version.load(
                    std::memory_order_relaxed));
    if (hdr_mode == static_cast<int>(
                        mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
      ImGui::TextDisabled(
          "Quality Guard still controls whether optional HUD-less/UI tags are used.");
    }
  } else if (mfgunlock::framecount::g_ui_recomposition_fell_back.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("UI Composition rejected (result %u); safe fallback is active.",
                mfgunlock::framecount::g_ui_recomposition_result.load(
                    std::memory_order_relaxed));
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid) &&
             hdr_active) {
    ImGui::TextDisabled(
        "Automatic HDR final-color fallback active; UI Composition is intentionally not submitted.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
    ImGui::TextDisabled(
        hdr_seen
            ? "Automatic SDR path is waiting for SetOptions and a valid HUD-less/UI pair."
            : "Automatic mode is waiting for the primary swapchain color space.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
    ImGui::TextDisabled("UI Composition has not been submitted yet; toggle FG after changing modes.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
    ImGui::TextDisabled(
        "Conservative final-color fallback active; UI Composition is intentionally not submitted.");
  }
  if (mfgunlock::framecount::g_hud_inputs_suppressed.load(std::memory_order_relaxed)) {
    ImGui::Text("Quality Guard filtered incompatible optional HUD/UI input.");
    ImGui::TextDisabled("Observed issue mask: 0x%X.",
                        mfgunlock::framecount::g_quality_issue_mask.load(
                            std::memory_order_relaxed));
  }
  if (mfgunlock::framecount::g_quality_mode_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextDisabled(
        "Quality-mode option change pending; toggle Frame Generation off/on to apply it.");
  }
  if (mfgunlock::framecount::g_quality_tag_batch_too_large.load(
          std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "Quality Guard saw a tag batch larger than 64; that batch was forwarded unchanged.");
  }
  if (mfgunlock::framecount::g_quality_viewport_capacity_exhausted.load(
          std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "More than %zu Streamline viewports were seen; extra viewports use conservative fallback.",
        mfgunlock::framecount::kMaxQualityViewports);
  }
  const auto reset_count = mfgunlock::framecount::g_quality_resets_injected.load(
      std::memory_order_relaxed);
  if (reset_count != 0) {
    ImGui::Text("Quality Guard synchronized temporal history %llu time(s).", reset_count);
  }

  constexpr const char* kDepthEdgeModes[] = {
      "Off - game value",
      "Mild (20.0)",
      "Balanced (10.0)",
      "Strong (4.0)",
      "Aggressive (1.0)"};
  int depth_edge_level = static_cast<int>(
      mfgunlock::framecount::g_depth_edge_guard_level.load(std::memory_order_relaxed));
  if (ImGui::Combo("Optional depth-edge guard", &depth_edge_level,
                   kDepthEdgeModes,
                   static_cast<int>(std::size(kDepthEdgeModes)))) {
    mfgunlock::framecount::g_depth_edge_guard_level.store(
        static_cast<unsigned int>(depth_edge_level), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDepthEdgeTuningChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DepthEdgeGuardLevel",
                              depth_edge_level);
  }
  ImGui::TextDisabled(
      "Lower values can improve nearby object/lower-screen edge separation.\n"
      "This does not perform camera-turn resets or modify frame pacing.");
  if (mfgunlock::framecount::g_depth_edge_override_applied.load(
          std::memory_order_relaxed)) {
    ImGui::Text("Depth-edge override active; game supplied %.3f.",
                mfgunlock::framecount::g_last_native_depth_separation.load(
                    std::memory_order_relaxed));
  }

  ImGui::Separator();
  bool dynamic_mfg =
      mfgunlock::framecount::g_dynamic_mfg_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Use NVIDIA Dynamic MFG (310.9.1 + SL 2.14.1)", &dynamic_mfg)) {
    mfgunlock::framecount::g_dynamic_mfg_enabled.store(dynamic_mfg,
                                                        std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG",
                              dynamic_mfg ? 1 : 0);
  }
  int dynamic_target = static_cast<int>(
      mfgunlock::framecount::g_dynamic_target_fps.load(std::memory_order_relaxed));
  if (ImGui::InputInt("Dynamic output target FPS", &dynamic_target, 1, 10)) {
    if (dynamic_target < 0) dynamic_target = 0;
    if (dynamic_target > 1000) dynamic_target = 1000;
    mfgunlock::framecount::g_dynamic_target_fps.store(
        static_cast<unsigned int>(dynamic_target), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicTargetFPS",
                              dynamic_target);
  }
  ImGui::TextDisabled(
      "Requires D3D12, driver 595.41+, DLSS-G 310.9.1 and Streamline 2.14.1.\n"
      "0 = display refresh. Uses Streamline's native eDynamic scheduler; no Present hook.\n"
      "When VSync is active, Streamline ignores this number and targets display refresh.\n"
      "Toggle frame generation off/on after changing Dynamic settings.");
  if (mfgunlock::framecount::g_dynamic_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextDisabled(
        "Dynamic setting change pending; waiting for a successful game-side SetOptions call.");
  }
  const uint64_t streamline_version =
      mfgunlock::framecount::g_active_streamline_version.load(std::memory_order_relaxed);
  const uint64_t dlssg_version =
      mfgunlock::framecount::g_last_dlssg_version.load(std::memory_order_relaxed);
  if (mfgunlock::framecount::g_streamline_version_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Observed Streamline DLSS-G: %u.%u.%u.%u%s.",
                static_cast<unsigned int>((streamline_version >> 48u) & 0xffffu),
                static_cast<unsigned int>((streamline_version >> 32u) & 0xffffu),
                static_cast<unsigned int>((streamline_version >> 16u) & 0xffffu),
                static_cast<unsigned int>(streamline_version & 0xffffu),
                mfgunlock::framecount::g_streamline_2_14_1_active.load(
                    std::memory_order_relaxed) ? " (supported)" : " (not Dynamic-supported)");
  }
  if (mfgunlock::framecount::g_dlssg_version_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Observed DLSS-G provider candidate: %u.%u.%u.%u%s.",
                static_cast<unsigned int>((dlssg_version >> 48u) & 0xffffu),
                static_cast<unsigned int>((dlssg_version >> 32u) & 0xffffu),
                static_cast<unsigned int>((dlssg_version >> 16u) & 0xffffu),
                static_cast<unsigned int>(dlssg_version & 0xffffu),
                mfgunlock::framecount::g_dlssg_310_9_1_seen.load(
                    std::memory_order_relaxed) ? " (supported candidate seen)"
                                               : " (not Dynamic-supported)");
  }
  if (render_api != DetectedRenderApi::kD3D12) {
    ImGui::TextDisabled("Dynamic MFG unavailable: the current renderer is not D3D12.");
  } else if (!mfgunlock::framecount::g_streamline_version_seen.load(
                 std::memory_order_acquire) ||
             !mfgunlock::framecount::g_dlssg_version_seen.load(
                 std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG waiting for the loaded Streamline/DLSS-G versions.");
  } else if (!mfgunlock::framecount::g_streamline_2_14_1_active.load(
                 std::memory_order_acquire) ||
             !mfgunlock::framecount::g_dlssg_310_9_1_seen.load(
                 std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG unavailable: this release requires exact versions 2.14.1 and 310.9.1.");
  } else if (!mfgunlock::framecount::g_dynamic_support_seen.load(
                 std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG support has not been reported by DLSS-G yet.");
  } else if (!mfgunlock::framecount::g_dynamic_supported.load(
                 std::memory_order_relaxed)) {
    ImGui::TextDisabled("Dynamic MFG unavailable: the active provider/driver reports unsupported.");
  } else if (mfgunlock::framecount::g_dynamic_applied.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("Dynamic MFG active; requested target: %s.",
                dynamic_target == 0 ? "display refresh" :
                                      (std::to_string(dynamic_target) + " FPS").c_str());
  } else if (mfgunlock::framecount::g_dynamic_fell_back.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("Dynamic MFG rejected (result %u); fixed MFG fallback is active.",
                mfgunlock::framecount::g_dynamic_result.load(
                    std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("Dynamic MFG supported; waiting for the next SetOptions call.");
  }
  if (!mfgunlock::framecount::g_vsync_support_seen.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("DLSS-G has not reported its VSync capability yet.");
  } else if (mfgunlock::framecount::g_vsync_supported.load(
                 std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "The active DLSS-G runtime reports VSync support. Streamline 2.14.1 adds\n"
        "VSync and frame-limiter support to Dynamic MFG on compatible D3D12 systems.");
  } else {
    ImGui::TextDisabled(
        "The active DLSS-G runtime reports VSync unavailable; this can indicate\n"
        "an older/mismatched runtime or an unsupported presentation mode.");
  }
  int source_cap = static_cast<int>(
      mfgunlock::framecount::g_reflex_source_fps_cap.load(
          std::memory_order_relaxed));
  if (ImGui::InputInt("Output FPS Cap (Reflex)", &source_cap, 1, 10)) {
    source_cap = source_cap <= 0 ? 0 : std::clamp(source_cap, 10, 1000);
    mfgunlock::framecount::g_reflex_source_fps_cap.store(
        static_cast<unsigned int>(source_cap), std::memory_order_relaxed);
    g_source_cap_config_origin.store(
        static_cast<unsigned int>(
            mfgunlock::pacing::SourceCapConfigOrigin::kConfigured),
        std::memory_order_relaxed);
    mfgunlock::framecount::NotifySourceFpsCapChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "ReflexSourceFpsCap", source_cap);
  }
  const auto source_cap_state =
      mfgunlock::framecount::internal::ResolveUserSourceCapState();
  ImGui::TextDisabled("%s", UserSourceCapSummary(source_cap_state).c_str());
  ImGui::TextDisabled("%s", UserSourceCapStatusText(source_cap_state.status));

  int force = static_cast<int>(
      mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed));
  // 6x == numFramesToGenerate 5, which is the Streamline plugin's own hard
  // ceiling (its wrapper clamps the count to 5). Whether the runtime accepts it
  // is up to that plugin -- a refusal is logged and falls back to the game's
  // own request, so asking costs nothing.
  if (ImGui::SliderInt("Force frame multiplier", &force, 0, 6,
                       force == 0 ? "off (game decides)" : "%dx")) {
    if (force != 0 && force < 2) force = 2;
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(force),
                                                    std::memory_order_relaxed);
    mfgunlock::framecount::NotifyFixedMultiplierChanged(
        static_cast<unsigned int>(force));
    reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", force);
  }
  ImGui::TextDisabled(
      "Leave off for games with their own 2x/3x/4x selector -- forcing would\n"
      "override your in-game choice. Dynamic MFG takes priority when active.");
  const bool game_request_seen =
      mfgunlock::framecount::g_game_request_seen.load(std::memory_order_acquire);
  if (game_request_seen) {
    ImGui::Text("Game request: %ux.",
                mfgunlock::framecount::g_last_requested.load(
                    std::memory_order_relaxed) + 1);
  } else {
    ImGui::TextDisabled("Game request: not observed yet.");
  }
  if (mfgunlock::forcepolicy::IsFixedMultiplier(
          static_cast<unsigned int>(force))) {
    ImGui::Text("Addon fixed request: %dx.", force);
  } else {
    ImGui::TextDisabled("Addon fixed request: off (game decides).");
  }

  const auto fixed_status =
      static_cast<mfgunlock::forcepolicy::FixedOverrideStatus>(
          mfgunlock::framecount::g_fixed_override_status.load(
              std::memory_order_acquire));
  const bool effective_seen =
      mfgunlock::framecount::g_effective_request_seen.load(
          std::memory_order_acquire);
  const unsigned int effective_multiplier =
      mfgunlock::framecount::g_last_effective_generated.load(
          std::memory_order_relaxed) + 1;
  switch (fixed_status) {
    case mfgunlock::forcepolicy::FixedOverrideStatus::kPending:
      ImGui::TextDisabled(
          "Effective request: pending the next enabled slDLSSGSetOptions call.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kApplied:
      ImGui::Text("Effective downstream request: %ux (addon override accepted).",
                  effective_multiplier);
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kRejected:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game fallback).",
                    effective_multiplier);
      }
      ImGui::TextDisabled("The runtime rejected the addon's fixed request.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kBlockedByPacing:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game fallback).",
                    effective_multiplier);
      }
      ImGui::TextDisabled(
          "Addon override blocked: legacy flip pacing was not verified.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kUnsupportedAbi:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game fallback).",
                    effective_multiplier);
      }
      ImGui::TextDisabled(
          "Addon override unsupported by the game's DLSSGOptions ABI.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kDynamicPriority:
      ImGui::TextDisabled(
          "Effective multiplier: Dynamic MFG/provider-controlled; fixed request is not applied.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kMatchedGameRequest:
      ImGui::Text("Effective downstream request: %ux (already matched addon request).",
                  effective_multiplier);
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kNative:
    default:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game controlled).",
                    effective_multiplier);
      } else if (mfgunlock::framecount::g_hooked.load(
                     std::memory_order_acquire)) {
        ImGui::TextDisabled(
            "Effective request: waiting for an enabled slDLSSGSetOptions call.");
      }
      break;
  }
  if (!mfgunlock::framecount::g_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("sl.interposer.dll not hooked (no Streamline in this game?).");
  }

  ImGui::Separator();
  bool temporal =
      g_configured_temporal_fix.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Temporal fix on next restart (stops midpoint compaction)",
                      &temporal)) {
    g_configured_temporal_fix.store(temporal, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "TemporalFix", temporal ? 1 : 0);
  }
  ImGui::TextDisabled("Applied once at load; restart the game to change it.");
  if (temporal != g_temporal_fix.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Temporal-fix change is saved for the next restart.");
  }

  bool blackwell = g_configured_blackwell_framework_kernels.load(
      std::memory_order_relaxed);
  if (ImGui::Checkbox("Prefer full Blackwell framework kernels on next restart (experimental)",
                      &blackwell)) {
    g_configured_blackwell_framework_kernels.store(
        blackwell, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "BlackwellFrameworkKernels",
                              blackwell ? 1 : 0);
  }
  ImGui::TextDisabled(
      "On: full motion-vector/inpaint path when validated. Off: 0.7 midpoint correction.\n"
      "Restart the game after changing this option.");
  if (blackwell !=
      g_blackwell_framework_kernels.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Kernel-path change is saved for the next restart.");
  }

  ImGui::Separator();
  ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.22f, 1.0f),
                     "Enhanced thin-geometry interpolation (EXPERIMENTAL)");
  ImGui::TextDisabled(
      "Complementary DLSS-G kernel experiments for thin objects and reprojection.\n"
      "Both are enabled by default but remain independently selectable. Restart\n"
      "the game after changing either option; results can vary by game.");

  bool intermediate_scatter =
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(
          "Intermediate scatter retention (Experimental - Recommended)##thin_intermediate",
          &intermediate_scatter)) {
    g_configured_thin_geometry_intermediate_scatter.store(
        intermediate_scatter, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryIntermediateScatter",
                              intermediate_scatter ? 1 : 0);
  }
  ImGui::TextWrapped(
      "Enabled by default. Relaxes one motion-consistency rejection while DLSS-G\n"
      "builds motion vectors for intermediate generated frames. The separate depth\n"
      "test stays active. May preserve fences, foliage and moving edges; disable it\n"
      "if a game shows added trails, ghosting or disocclusion artifacts.");

  int silhouette_mode = static_cast<int>(
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed));
  const char* silhouette_modes[] = {
      "Off - released 0.9 behavior",
      "Balanced - preserve supported thin silhouettes",
      "Aggressive - prefer cleaner motion/depth boundaries"};
  if (ImGui::Combo("Boundary artifact mitigation (restart required)",
                   &silhouette_mode, silhouette_modes,
                   static_cast<int>(std::size(silhouette_modes)))) {
    g_configured_silhouette_guard_mode.store(
        static_cast<unsigned int>(silhouette_mode),
        std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "BoundaryArtifactMitigationMode",
                              silhouette_mode);
  }
  if (silhouette_mode == static_cast<int>(
                             mfgunlock::blackwell::SilhouetteGuardMode::Balanced)) {
    ImGui::TextWrapped(
        "Balanced conditions extra intermediate retention on one same-depth,\n"
        "motion-coherent neighbor. It targets occlusion boundaries, foreground/\n"
        "background bleeding, silhouette stretching, motion boundaries and depth\n"
        "discontinuities while favoring narrow foliage, hair and weapon edges.");
  } else if (silhouette_mode == static_cast<int>(
                                    mfgunlock::blackwell::SilhouetteGuardMode::Aggressive)) {
    ImGui::TextWrapped(
        "Aggressive requires more than one neighbor-equivalent of coherent support,\n"
        "uses a tighter depth boundary and nonlinear confidence, and permits at\n"
        "most half of Balanced's extra relaxation. It may clean silhouettes more,\n"
        "but can reduce persistence or add flicker to very thin geometry.");
  } else {
    ImGui::TextDisabled(
        "Off keeps the released 0.9 Intermediate Scatter Retention behavior.");
  }
  if (silhouette_mode != static_cast<int>(
                             mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
    ImGui::TextDisabled(
        "The selected guard supersedes unconditional Intermediate retention.\n"
        "It cannot reconstruct genuinely hidden pixels or repair invalid game motion vectors.");
    if (intermediate_scatter) {
      ImGui::TextDisabled(
          "Intermediate retention remains enabled only as a compatibility fallback.");
    }
  }

  bool validated_warp =
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(
          "Validated warp blend (Experimental - Recommended)##thin_validated_warp",
          &validated_warp)) {
    g_configured_thin_geometry_validated_warp_blend.store(
        validated_warp, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryValidatedWarpBlend",
                              validated_warp ? 1 : 0);
  }
  ImGui::TextWrapped(
      "Enabled by default. Later-stage blend experiment inspired by Tony Joaca's\n"
      "DLSSG-Transfusion qualityValidWarp work and independently implemented here.\n"
      "It validates candidate bounds, finite color and mutual agreement before\n"
      "gradually trusting accepted warped color more. It may reduce flicker, but can\n"
      "also increase persistence or ghosting in some scenes.");

  bool previous_scatter =
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(
          "Previous-to-current scatter retention (Experimental/Unstable)##thin_previous",
          &previous_scatter)) {
    g_configured_thin_geometry_previous_scatter.store(
        previous_scatter, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryPreviousScatter",
                              previous_scatter ? 1 : 0);
  }
  ImGui::TextDisabled(
      "Advanced research control; disabled by default. It changes motion rejection\n"
      "between real frames and was unstable in initial game testing. Not recommended\n"
      "for normal use; bounds logic remains unchanged.");

  if (validated_warp !=
          g_thin_geometry_validated_warp_blend.load(std::memory_order_relaxed) ||
      previous_scatter !=
          g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
      intermediate_scatter !=
          g_thin_geometry_intermediate_scatter.load(std::memory_order_relaxed) ||
      static_cast<unsigned int>(silhouette_mode) !=
          g_silhouette_guard_mode.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "Thin-geometry selection is saved for the next restart.");
  }

  const auto show_thin_result = [](const char* label,
                                   const mfgunlock::thingeometry::MechanismResult& result) {
    if (!result.requested) return;
    if (result.applied) {
      ImGui::TextWrapped("%s: applied (%s).", label, result.detail.c_str());
    } else {
      ImGui::TextDisabled("%s: not applied (%s).", label,
                          result.detail.empty() ? "waiting for a supported provider"
                                                : result.detail.c_str());
    }
  };
  if (thin_geometry_provider_count != 0) {
    ImGui::TextDisabled("Validated provider result: %s; processed provider(s): %zu.",
                        thin_geometry_last.provider_version.empty()
                            ? "unsupported/unknown"
                            : thin_geometry_last.provider_version.c_str(),
                        thin_geometry_provider_count);
    show_thin_result("Validated warp blend",
                     thin_geometry_last.result.validated_warp_blend);
    show_thin_result("Previous scatter retention",
                     thin_geometry_last.result.previous_scatter);
    show_thin_result("Intermediate scatter retention",
                     thin_geometry_last.intermediate_scatter);
    show_thin_result("Silhouette disocclusion guard",
                     thin_geometry_last.silhouette_boundary_guard);
  } else if (g_thin_geometry_validated_warp_blend.load(
                 std::memory_order_relaxed) ||
             g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
             g_thin_geometry_intermediate_scatter.load(
                 std::memory_order_relaxed) ||
             g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                 static_cast<unsigned int>(
                     mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
    ImGui::TextDisabled("Waiting for a supported DLSS-G provider (attempt %d).",
                        g_thin_geometry_attempts.load(std::memory_order_relaxed));
  }
  if (thin_geometry_patched) {
    ImGui::TextDisabled(
        "At least one thin-geometry mechanism passed exact validation this session.");
  }

  ImGui::Separator();
  if (blackwell_patched) {
    ImGui::Text("Blackwell framework kernels active: %zu provider(s).",
                blackwell_provider_count);
    ImGui::TextDisabled(
        "Patched roles: motion-vector %s, inpaint %s, decision %s (%zu slots).",
        blackwell_active_result.motion_vector ? "yes" : "no",
        blackwell_active_result.inpaint ? "yes" : "no",
        blackwell_active_result.inpaint_decision ? "yes" : "no",
        blackwell_active_result.kernels);
    if (!blackwell_applied_detail.empty()) {
      ImGui::TextDisabled("Applied provider result: %s.",
                          blackwell_applied_detail.c_str());
    }
    if (!blackwell_detail.empty() &&
        blackwell_detail != blackwell_applied_detail) {
      ImGui::TextDisabled(
          "Separate rejected candidate (does not replace the result above): %s.",
          blackwell_detail.c_str());
    }
  } else if (midpoint_patched) {
    ImGui::TextWrapped("Temporal fix: %zu provider(s); last result: %s.",
                       midpoint_provider_count, midpoint_detail.c_str());
    if (g_blackwell_framework_kernels.load(std::memory_order_relaxed) &&
        !blackwell_detail.empty()) {
      ImGui::TextDisabled("Full Blackwell path fell back safely: %s.",
                          blackwell_detail.c_str());
    }
  } else {
    ImGui::TextDisabled("Temporal fix not applied yet (attempt %d).",
                        g_midpoint_attempts.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  if (mfgunlock::loadhook::g_hooked.load(std::memory_order_acquire)) {
    ImGui::Text("Load-time trigger armed (%u snippet load(s) caught).",
                mfgunlock::loadhook::g_catches.load(std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("Load-time trigger not installed.");
  }
  if (g_discovery_worker_running.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("Bounded fallback discovery running (%u/%u passes; no Present polling).",
                        g_discovery_worker_passes.load(std::memory_order_relaxed),
                        kDiscoveryRetryLimit);
  } else if (g_discovery_worker_finished.load(std::memory_order_acquire)) {
    if (DiscoveryRequirementsMet()) {
      ImGui::TextDisabled("Bounded fallback discovery complete; no Present polling is active.");
    } else {
      ImGui::TextDisabled("Bounded fallback discovery finished; waiting on load-time triggers.");
    }
  }

  ImGui::Separator();
  if (gate_patched) {
    ImGui::Text("DLSS-G arch gates rewritten: %zu provider(s), %zu site(s).",
                gate_provider_count, gate_site_count);
  } else {
    ImGui::TextDisabled("DLSS-G snippet not located yet (attempt %d) -- enable frame generation.",
                        g_gate_attempts.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  ImGui::TextDisabled("Legacy NGX parameter-vtable override disabled; using verified code gates.");
}

void OnRegisterOverlay(reshade::api::effect_runtime* runtime) {
  mfgunlock::framecount::g_latency_guard_ui_heartbeat_ms.store(
      GetTickCount64(), std::memory_order_release);
  const bool enabled = g_enabled.load(std::memory_order_relaxed);
  bool configured_enabled =
      g_configured_enabled.load(std::memory_order_relaxed);
  const auto render_api = g_render_api.load(std::memory_order_relaxed);

  const bool streamline_seen =
      mfgunlock::framecount::g_streamline_version_seen.load(
          std::memory_order_acquire);
  const bool dlssg_seen =
      mfgunlock::framecount::g_dlssg_version_seen.load(
          std::memory_order_acquire);
  const uint64_t streamline_version =
      mfgunlock::framecount::g_active_streamline_version.load(
          std::memory_order_relaxed);
  const uint64_t dlssg_version =
      mfgunlock::framecount::g_last_dlssg_version.load(
          std::memory_order_relaxed);
  const std::string streamline_text =
      PackedVersionString(streamline_version, streamline_seen);
  const std::string dlssg_text = PackedVersionString(dlssg_version, dlssg_seen);

  const bool dynamic_versions_ready =
      mfgunlock::framecount::g_streamline_2_14_1_active.load(
          std::memory_order_acquire) &&
      mfgunlock::framecount::g_dlssg_310_9_1_seen.load(
          std::memory_order_acquire);
  const bool dynamic_support_seen =
      mfgunlock::framecount::g_dynamic_support_seen.load(
          std::memory_order_acquire);
  const bool dynamic_supported =
      mfgunlock::framecount::g_dynamic_supported.load(
          std::memory_order_relaxed);
  const bool dynamic_renderer_blocked =
      render_api != DetectedRenderApi::kUnknown &&
      render_api != DetectedRenderApi::kD3D12;
  const bool dynamic_version_blocked =
      streamline_seen && dlssg_seen && !dynamic_versions_ready;
  const bool dynamic_provider_blocked =
      dynamic_support_seen && !dynamic_supported;
  const bool dynamic_game_blocked =
      mfgunlock::framecount::g_dynamic_game_compat_blocked.load(
          std::memory_order_relaxed);
  const bool dynamic_hard_unavailable =
      dynamic_renderer_blocked || dynamic_version_blocked ||
      dynamic_provider_blocked || dynamic_game_blocked;
  const bool dynamic_enabled =
      mfgunlock::framecount::g_dynamic_mfg_enabled.load(
          std::memory_order_relaxed);
  const bool dynamic_available =
      render_api == DetectedRenderApi::kD3D12 && dynamic_versions_ready &&
      dynamic_support_seen && dynamic_supported && !dynamic_game_blocked;

  const bool state_seen =
      mfgunlock::framecount::g_state_seen.load(std::memory_order_acquire);
  const unsigned int dlssg_status =
      mfgunlock::framecount::g_dlssg_status.load(std::memory_order_relaxed);
  const unsigned int observed_presentations =
      mfgunlock::framecount::g_max_actual_frames_presented.load(
          std::memory_order_relaxed);
  const bool effective_seen =
      mfgunlock::framecount::g_effective_request_seen.load(
          std::memory_order_acquire);
  const unsigned int effective_multiplier =
      mfgunlock::framecount::g_last_effective_generated.load(
          std::memory_order_relaxed) +
      1;
  const bool game_request_seen =
      mfgunlock::framecount::g_game_request_seen.load(
          std::memory_order_acquire);
  const unsigned int game_multiplier =
      mfgunlock::framecount::g_last_requested.load(std::memory_order_relaxed) +
      1;
  const bool hdr_seen =
      mfgunlock::framecount::g_hdr_state_seen.load(std::memory_order_acquire);
  const bool hdr_active =
      mfgunlock::framecount::g_hdr_active.load(std::memory_order_relaxed);
  const auto& nvapi_status = ObserveNvapiUiStatus(runtime);
  const bool sleep_status_available =
      nvapi_status.sleep_status == mfgunlock::nvapistatus::kOk;
  const unsigned int driver_multiplier =
      sleep_status_available
          ? nvapi_status.sleep.frame_generation_multiplier
          : 0;
  const bool driver_vsync_active =
      sleep_status_available && nvapi_status.sleep.control_panel_vsync != 0;
  const bool gsync_active =
      sleep_status_available && nvapi_status.sleep.fullscreen_vrr != 0;
  const std::string frame_generation_preset =
      FrameGenerationPresetText(nvapi_status, dlssg_seen);
  const std::string super_resolution_preset =
      ReconstructionPresetText(nvapi_status, false);
  const std::string ray_reconstruction_preset =
      ReconstructionPresetText(nvapi_status, true);
  const bool super_resolution_active =
      mfgunlock::nvapistatus::NgxFeatureActive(
          nvapi_status.ngx.feedback_super_resolution);
  const bool super_resolution_configured =
      mfgunlock::nvapistatus::NgxFeatureConfigured(
          nvapi_status.ngx.feedback_super_resolution);
  const bool ray_reconstruction_active =
      mfgunlock::nvapistatus::NgxFeatureActive(
          nvapi_status.ngx.feedback_ray_reconstruction);
  const bool ray_reconstruction_configured =
      mfgunlock::nvapistatus::NgxFeatureConfigured(
          nvapi_status.ngx.feedback_ray_reconstruction);
  const unsigned int dynamic_max_generated =
      mfgunlock::framecount::g_advertised_max_generated.load(
          std::memory_order_relaxed);

  const bool restart_pending =
      configured_enabled != enabled ||
      g_configured_force_flip_meter_off.load(std::memory_order_relaxed) !=
          g_force_flip_meter_off.load(std::memory_order_relaxed) ||
      g_configured_temporal_fix.load(std::memory_order_relaxed) !=
          g_temporal_fix.load(std::memory_order_relaxed) ||
      g_configured_blackwell_framework_kernels.load(
          std::memory_order_relaxed) !=
          g_blackwell_framework_kernels.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality.load(std::memory_order_relaxed) !=
          g_adaptive_quality.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality_profile.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_profile.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_photometric.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_photometric.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_directional_border.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_directional_border.load(
              std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_oriented_geometry.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_oriented_geometry.load(
              std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_stability_mode.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_stability_mode.load(
              std::memory_order_relaxed) ||
      g_configured_quality_refinement.load(std::memory_order_relaxed) !=
          g_quality_refinement.load(std::memory_order_relaxed) ||
      g_configured_geometry_confidence_v2.load(std::memory_order_relaxed) !=
          g_geometry_confidence_v2.load(std::memory_order_relaxed) ||
      g_configured_border_confidence.load(std::memory_order_relaxed) !=
          g_border_confidence.load(std::memory_order_relaxed) ||
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed) !=
          g_thin_geometry_validated_warp_blend.load(
              std::memory_order_relaxed) ||
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed) !=
          g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed) !=
          g_thin_geometry_intermediate_scatter.load(
              std::memory_order_relaxed) ||
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed) !=
          g_silhouette_guard_mode.load(std::memory_order_relaxed) ||
      g_configured_runtime_selection_mode.load(std::memory_order_relaxed) !=
          mfgunlock::framecount::g_runtime_selection_mode.load(
              std::memory_order_relaxed);

  const auto fixed_status =
      static_cast<mfgunlock::forcepolicy::FixedOverrideStatus>(
          mfgunlock::framecount::g_fixed_override_status.load(
              std::memory_order_acquire));
  const bool dynamic_accepted =
      mfgunlock::framecount::g_dynamic_applied.load(
          std::memory_order_acquire);
  const bool request_rejected =
      fixed_status ==
          mfgunlock::forcepolicy::FixedOverrideStatus::kRejected ||
      mfgunlock::framecount::g_dynamic_runtime_declined.load(
          std::memory_order_acquire);
  const bool runtime_error = state_seen && dlssg_status != 0;
  const auto validation_stage = mfgunlock::validation::Classify({
      game_request_seen,
      effective_seen || dynamic_accepted,
      request_rejected,
      runtime_error,
      sleep_status_available,
      driver_multiplier,
      state_seen && dlssg_status == 0,
      observed_presentations});
  const std::string validation_text = ValidationStatusText(
      validation_stage, dynamic_accepted, effective_seen,
      effective_multiplier, driver_multiplier, observed_presentations);
  const bool mfg_confirmed =
      validation_stage ==
      mfgunlock::validation::Stage::kProviderOutputConfirmed;
  const bool mfg_live =
      mfgunlock::validation::HasLiveRuntimeEvidence(validation_stage);
  const bool mfg_request_accepted =
      mfgunlock::validation::HasAcceptedRequest(validation_stage);
  const bool provider_ready =
      g_gate_patched.load(std::memory_order_acquire) &&
      mfgunlock::framecount::g_hooked.load(std::memory_order_acquire);

  if (g_is_monster_hunter_wilds &&
      g_monster_hunter_restart_notice_pending.load(
          std::memory_order_acquire)) {
    constexpr const char* kPopup =
        "Monster Hunter Wilds first-launch notice";
    if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(kUiWarning, "Restart once after shader compilation");
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
      ImGui::TextWrapped(
          "Monster Hunter Wilds may build shader.cache2 on the first launch "
          "after installing or updating the addon. Let the game's shader "
          "compilation finish, close the game completely, and launch it again "
          "before evaluating frame pacing or image quality.");
      ImGui::Spacing();
      ImGui::TextDisabled(
          "Do not restart while the shader compilation is still running. The "
          "addon does not delete or modify the game's cache.");
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      if (ImGui::Button("I understand", ImVec2(150.0f, 0.0f))) {
        reshade::set_config_value(nullptr, kConfigSection,
                                  "MonsterHunterRestartNoticeRevision",
                                  kMonsterHunterRestartNoticeRevision);
        g_monster_hunter_restart_notice_pending.store(
            false, std::memory_order_release);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  ImGui::TextUnformatted("MFG Unlock");
  ImGui::Separator();
  if (validation_stage == mfgunlock::validation::Stage::kRuntimeError) {
    ImGui::TextColored(kUiError, "Compatibility issue");
  } else if (!enabled) {
    ImGui::TextColored(kUiMuted, "Inactive this session");
  } else if (dynamic_enabled && dynamic_hard_unavailable) {
    ImGui::TextColored(kUiWarning,
                       "Dynamic MFG unavailable - fixed fallback remains active");
  } else if (validation_stage ==
             mfgunlock::validation::Stage::kProviderOutputConfirmed) {
    ImGui::TextColored(kUiPositive, "Working correctly");
  } else if (validation_stage ==
             mfgunlock::validation::Stage::kDriverMultiplierActive) {
    ImGui::TextColored(kUiPositive, "%ux live reported by driver",
                       driver_multiplier);
  } else if (validation_stage ==
             mfgunlock::validation::Stage::kRequestAccepted) {
    ImGui::TextColored(kUiPositive, "%s", validation_text.c_str());
  } else if (validation_stage ==
             mfgunlock::validation::Stage::kRequestRejected) {
    ImGui::TextColored(kUiWarning,
                       "Override rejected - game fallback remains active");
  } else if (validation_stage ==
             mfgunlock::validation::Stage::kNativeRequestObserved) {
    ImGui::TextColored(kUiWarning,
                       "Game request observed - waiting for runtime result");
  } else if (provider_ready) {
    ImGui::TextColored(kUiWarning, "Ready - waiting for game request");
  } else {
    ImGui::TextColored(kUiWarning, "Waiting for DLSS Frame Generation");
  }

  if (ImGui::BeginTable("##mfg_overview", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    const bool dynamic_applied =
        mfgunlock::framecount::g_dynamic_applied.load(
            std::memory_order_relaxed);
    const std::string multiplier_text =
        dynamic_applied
            ? DynamicMultiplierText(nvapi_status, dynamic_max_generated)
            : (effective_seen ? std::to_string(effective_multiplier) + "x"
                              : (game_request_seen
                                     ? std::to_string(game_multiplier) + "x (game)"
                                     : "Not observed"));
    const std::string sync_text =
        !sleep_status_available
            ? "Not reported by NVAPI"
            : (driver_vsync_active && gsync_active
                   ? "Driver VSync + G-SYNC active"
                   : (driver_vsync_active
                          ? "Driver VSync active"
                          : (gsync_active ? "G-SYNC active; driver VSync off"
                                          : "Driver VSync/G-SYNC inactive")));
    StatusRow("MFG", multiplier_text.c_str(),
              mfg_live || mfg_request_accepted
                  ? kUiPositive
                  : (runtime_error || request_rejected ? kUiWarning
                                                       : kUiMuted));
    StatusRow("Renderer", RenderApiName(render_api),
              render_api == DetectedRenderApi::kUnknown ? kUiMuted
                                                        : kUiPositive);
    StatusRow("DLSS-G", dlssg_text.c_str(),
              dlssg_seen ? kUiPositive : kUiMuted);
    StatusRow("Streamline", streamline_text.c_str(),
              streamline_seen ? kUiPositive : kUiMuted);
    const char* dynamic_status =
        dynamic_game_blocked
            ? "Blocked for this game"
            : (dynamic_available
            ? (mfgunlock::framecount::g_dynamic_applied.load(
                   std::memory_order_relaxed)
                   ? "Active"
                   : "Available")
            : (dynamic_hard_unavailable ? "Unavailable" : "Checking"));
    StatusRow("Dynamic MFG", dynamic_status,
              dynamic_available
                  ? kUiPositive
                  : (dynamic_hard_unavailable ? kUiError : kUiWarning));
    const auto overview_source_cap =
        mfgunlock::framecount::internal::ResolveUserSourceCapState();
    if (overview_source_cap.configured_fps != 0) {
      const std::string cap_overview =
          std::to_string(overview_source_cap.configured_fps) +
          " final/output FPS; " +
          UserSourceCapStatusText(overview_source_cap.status);
      StatusRow("Output FPS cap", cap_overview.c_str(),
                UserSourceCapColor(overview_source_cap.status));
    }
    StatusRow("Display sync", sync_text.c_str(),
              driver_vsync_active ? kUiWarning : kUiMuted);
    StatusRow("HDR", hdr_seen ? (hdr_active ? "Detected" : "Off")
                              : "Not observed",
              hdr_active ? kUiPositive : kUiMuted);
    ImGui::EndTable();
  }

  if (restart_pending) {
    ImGui::Spacing();
    ImGui::TextColored(kUiWarning, "Restart required");
    ImGui::TextWrapped(
        "One or more saved changes will take effect after restarting the game.");
  }
  if (dynamic_game_blocked) {
    ImGui::TextColored(
        kUiWarning,
        "Dynamic MFG is disabled for Star Wars Outlaws compatibility.");
    ImGui::TextWrapped(
        "Fixed and game-controlled MFG remain available. The saved Dynamic "
        "preference is preserved for other games.");
  } else if (dynamic_hard_unavailable) {
    ImGui::TextColored(
        kUiWarning,
        "Dynamic MFG requires DLSS-G 310.9.1 + Streamline 2.14.1.");
    ImGui::TextDisabled("Detected: DLSS-G %s | Streamline %s",
                        dlssg_text.c_str(), streamline_text.c_str());
    HelpMarker(
        "Dynamic MFG additionally requires D3D12 and NVIDIA driver 595.41 or newer. The fixed/game-controlled MFG path remains available when Dynamic cannot activate.");
  } else if (!dynamic_available) {
    ImGui::TextDisabled(
        "Dynamic MFG requirements are checked after the game loads DLSS Frame Generation.");
  }
  if (mfgunlock::framecount::g_dynamic_change_pending.load(
          std::memory_order_acquire) ||
      mfgunlock::framecount::g_quality_mode_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextColored(kUiWarning, "Apply pending");
    ImGui::TextWrapped(
        "Toggle Frame Generation off and on in the game to apply the latest runtime change.");
  }

  enum class UiPage { General, DlssInfo, Latency, Support };
  static UiPage page = UiPage::General;
  if (ImGui::BeginTabBar("##mfg_unlock_pages")) {
    if (ImGui::BeginTabItem("General")) {
      page = UiPage::General;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("DLSS Info")) {
      page = UiPage::DlssInfo;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Latency")) {
      page = UiPage::Latency;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Support")) {
      page = UiPage::Support;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  if (page == UiPage::DlssInfo) {
    g_vram_ui_heartbeat_ms.store(GetTickCount64(),
                                 std::memory_order_release);
    if (!mfgunlock::inputdiag::g_enabled.load(std::memory_order_acquire)) {
      mfgunlock::inputdiag::Clear();
      mfgunlock::inputdiag::g_enabled.store(true,
                                            std::memory_order_release);
      g_vram_capture_owned.store(true, std::memory_order_release);
    }
    QueueProviderVramEstimateFromCapture();
    ImGui::Spacing();
    ImGui::TextDisabled("DLSS RUNTIME INFORMATION");
    ImGui::Separator();
    if (ImGui::BeginTable("##dlss_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn("Active mode", ImGuiTableColumnFlags_WidthStretch,
                              0.58f);
      StatusRow("Super Resolution", super_resolution_preset.c_str(),
                super_resolution_active
                    ? kUiPositive
                    : (super_resolution_configured ? kUiWarning : kUiMuted));
      StatusRow("Frame Generation", frame_generation_preset.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow("Ray Reconstruction", ray_reconstruction_preset.c_str(),
                ray_reconstruction_active
                    ? kUiPositive
                    : (ray_reconstruction_configured ? kUiWarning : kUiMuted));
      StatusRow("DLSS-G runtime", dlssg_text.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow("Streamline runtime", streamline_text.c_str(),
                streamline_seen ? kUiPositive : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Read-only information reported by NVIDIA. Game-controlled modes may not expose a preset name.");
    HelpMarker(
        "NVIDIA exposes one shared preset/mode field for Super Resolution and Ray Reconstruction. The addon displays it only when the corresponding feature reports active or configured feedback, and never changes the selected model or render resolution.");

    ImGui::Spacing();
    ImGui::TextDisabled("VRAM / RESOURCE HEALTH");
    ImGui::Separator();
    if (ImGui::BeginTable("##vram_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn("Observation",
                              ImGuiTableColumnFlags_WidthStretch, 0.58f);
      if (g_vram_dxgi_seen.load(std::memory_order_acquire)) {
        const uint64_t usage =
            g_vram_local_usage.load(std::memory_order_relaxed);
        const uint64_t budget =
            g_vram_local_budget.load(std::memory_order_relaxed);
        const std::string usage_text =
            FormatMemoryBytes(usage) + " / " + FormatMemoryBytes(budget);
        const uint64_t headroom = budget > usage ? budget - usage : 0;
        const std::string headroom_text = FormatMemoryBytes(headroom);
        StatusRow("Process local VRAM", usage_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        StatusRow("Budget headroom", headroom_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        const std::string reservation_text = FormatMemoryBytes(
            g_vram_local_available.load(std::memory_order_relaxed));
        StatusRow("Available for reservation", reservation_text.c_str(),
                  kUiMuted);
        const uint64_t shared_usage =
            g_vram_nonlocal_usage.load(std::memory_order_relaxed);
        if (shared_usage != 0) {
          const std::string shared_text =
              FormatMemoryBytes(shared_usage) + " / " +
              FormatMemoryBytes(
                  g_vram_nonlocal_budget.load(std::memory_order_relaxed));
          StatusRow("Process shared GPU memory", shared_text.c_str(),
                    kUiMuted);
        }
      } else {
        StatusRow(
            "Process local VRAM",
            g_render_api.load(std::memory_order_relaxed) ==
                    DetectedRenderApi::kVulkan
                ? "DXGI budget unavailable on Vulkan"
                : "Waiting for DXGI budget",
            kUiMuted);
      }

      const auto estimate_status =
          static_cast<mfgunlock::framecount::VramEstimateStatus>(
              mfgunlock::framecount::g_vram_estimate_status.load(
                  std::memory_order_acquire));
      std::string estimate_text;
      ImVec4 estimate_color = kUiMuted;
      switch (estimate_status) {
        case mfgunlock::framecount::VramEstimateStatus::kReady:
          estimate_text = FormatMemoryBytes(
              mfgunlock::framecount::g_vram_estimate_bytes.load(
                  std::memory_order_relaxed));
          break;
        case mfgunlock::framecount::VramEstimateStatus::kPending:
          estimate_text = "Waiting for next DLSS-G state query";
          break;
        case mfgunlock::framecount::VramEstimateStatus::kUnsupported:
          estimate_text = "Unavailable on this runtime/game";
          break;
        case mfgunlock::framecount::VramEstimateStatus::kFailed:
          estimate_text = "Provider did not return an estimate";
          estimate_color = kUiWarning;
          break;
        default:
          estimate_text = VramReadinessText(
              static_cast<mfgunlock::memorypolicy::EstimateReadiness>(
                  mfgunlock::framecount::g_vram_estimate_readiness.load(
                      std::memory_order_relaxed)));
          break;
      }
      StatusRow("DLSS-G estimated VRAM", estimate_text.c_str(),
                estimate_color);

      const bool native_ready =
          mfgunlock::framecount::g_vram_native_estimate_ready.load(
              std::memory_order_acquire);
      const bool ui_ready =
          mfgunlock::framecount::g_vram_ui_estimate_ready.load(
              std::memory_order_acquire);
      if (native_ready && ui_ready) {
        const uint64_t native_bytes =
            mfgunlock::framecount::g_vram_native_estimate_bytes.load(
                std::memory_order_relaxed);
        const uint64_t ui_bytes =
            mfgunlock::framecount::g_vram_ui_estimate_bytes.load(
                std::memory_order_relaxed);
        const int64_t delta = static_cast<int64_t>(ui_bytes) -
                              static_cast<int64_t>(native_bytes);
        const uint64_t delta_bytes =
            static_cast<uint64_t>(delta >= 0 ? delta : -delta);
        const std::string delta_text =
            std::string(delta >= 0 ? "+" : "-") +
            (delta_bytes == 0 ? "0 MiB" : FormatMemoryBytes(delta_bytes)) +
            " with UI composition";
        StatusRow("Measured UI-composition delta", delta_text.c_str(),
                  kUiMuted);
      }

      const unsigned int volatile_inputs =
          mfgunlock::framecount::g_vram_volatile_input_count.load(
              std::memory_order_relaxed);
      const std::string lifecycle_text = volatile_inputs == 0
          ? "Optimized: relevant inputs valid until Present"
          : std::to_string(volatile_inputs) +
                " input(s) may require transient copies";
      StatusRow("Input lifetimes", lifecycle_text.c_str(),
                volatile_inputs == 0 ? kUiPositive : kUiWarning);
      const unsigned int flags =
          mfgunlock::framecount::g_vram_estimate_flags.load(
              std::memory_order_relaxed);
      const bool retained_while_off =
          (flags & static_cast<unsigned int>(
                       sl::DLSSGFlags::eRetainResourcesWhenOff)) != 0;
      StatusRow("Resources while FG is off",
                retained_while_off ? "Retained by game request"
                                   : "Not retained by game request",
                retained_while_off ? kUiWarning : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Read-only diagnostics. Process usage includes the whole game; the provider estimate is approximate.");
    HelpMarker(
        "This page temporarily captures resource dimensions, formats and lifetimes only; it never reads pixels or stores resource addresses. The addon does not reserve VRAM, rewrite resource lifetimes, or disable the game's retain-resources flag. Close this page to stop metadata capture.");
  }

  if (page == UiPage::General) {
    ImGui::Spacing();
    ImGui::TextDisabled("FRAME GENERATION");
    ImGui::Separator();
    if (ImGui::BeginTable("##frame_generation_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.38f);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Enable MFG Unlock", "Applied on the next game launch.",
        "Changes the master addon state. If you change this option, a restart-required notice will appear at the top of the panel.");
    ImGui::TableNextColumn();
    if (ImGui::Checkbox("##mfg_enabled", &configured_enabled)) {
      g_configured_enabled.store(configured_enabled,
                                 std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "Enabled",
                                configured_enabled ? 1 : 0);
    }

    int force = static_cast<int>(
        mfgunlock::framecount::g_force_multiplier.load(
            std::memory_order_relaxed));
    int force_choice = force == 0 ? 0 : force - 1;
    constexpr const char* kMultiplierModes[] = {
        "Game controlled", "2x", "3x", "4x", "5x", "6x"};
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Frame Multiplier", "Use the game's selector when possible.",
        "Game controlled preserves the multiplier selected in the game's menu. A fixed value overrides that request when supported. Dynamic MFG takes priority while active.");
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##frame_multiplier", &force_choice,
                     kMultiplierModes,
                     static_cast<int>(std::size(kMultiplierModes)))) {
      force = force_choice == 0 ? 0 : force_choice + 1;
      mfgunlock::framecount::g_force_multiplier.store(
          static_cast<unsigned int>(force), std::memory_order_relaxed);
      mfgunlock::framecount::NotifyFixedMultiplierChanged(
          static_cast<unsigned int>(force));
      reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier",
                                force);
    }

    bool dynamic_mfg = dynamic_enabled && !dynamic_game_blocked;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "NVIDIA Dynamic MFG",
        dynamic_game_blocked
            ? "Disabled for this game's compatibility."
            : (dynamic_available ? "Runtime support detected."
                                 : (dynamic_hard_unavailable
                                        ? "Required runtime is unavailable."
                                        : "Waiting for runtime detection.")),
        "Lets NVIDIA's native scheduler choose the generated-frame count. Requires D3D12, driver 595.41 or newer, DLSS-G 310.9.1 and Streamline 2.14.1. Star Wars Outlaws is kept on fixed/game-controlled MFG because its integration breaks on the Dynamic path. Toggle Frame Generation off/on after changing it.");
    ImGui::TableNextColumn();
    const bool block_dynamic_enable =
        dynamic_game_blocked || (dynamic_hard_unavailable && !dynamic_mfg);
    if (block_dynamic_enable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("##dynamic_mfg", &dynamic_mfg)) {
      mfgunlock::framecount::g_dynamic_mfg_enabled.store(
          dynamic_mfg, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyDynamicModeChanged();
      reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG",
                                dynamic_mfg ? 1 : 0);
    }
    if (block_dynamic_enable) ImGui::EndDisabled();

    if (dynamic_mfg) {
      int dynamic_target = static_cast<int>(
          mfgunlock::framecount::g_dynamic_target_fps.load(
              std::memory_order_relaxed));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "Dynamic Output Target", "0 follows the display refresh rate.",
          "Sets Dynamic MFG's requested final/output target. With VSync active, Streamline follows the display refresh rate and may ignore a non-zero target. This scheduler target is separate from Output FPS Cap (Reflex).");
      ImGui::TableNextColumn();
      if (dynamic_hard_unavailable) ImGui::BeginDisabled();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::InputInt("##dynamic_target", &dynamic_target, 1, 10)) {
        dynamic_target = std::clamp(dynamic_target, 0, 1000);
        mfgunlock::framecount::g_dynamic_target_fps.store(
            static_cast<unsigned int>(dynamic_target),
            std::memory_order_relaxed);
        mfgunlock::framecount::NotifyDynamicModeChanged();
        reshade::set_config_value(nullptr, kConfigSection,
                                  "DynamicTargetFPS", dynamic_target);
      }
      if (dynamic_hard_unavailable) ImGui::EndDisabled();

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "VSync / G-SYNC behavior",
          driver_vsync_active && gsync_active
              ? "Detected: driver VSync + G-SYNC active."
              : (driver_vsync_active
                     ? "Detected: driver VSync active."
                     : (gsync_active
                            ? "Detected: G-SYNC active; driver VSync is off."
                            : "Check the rule before choosing a target.")),
          "With VSync active, Streamline ignores Dynamic Output Target and follows the active display refresh rate. G-SYNC does not change that VSync rule. Output FPS Cap (Reflex) remains a separate final-output ceiling.");
      ImGui::TableNextColumn();
      if (driver_vsync_active) {
        ImGui::TextColored(kUiWarning, "Output target ignored; follows refresh");
      } else {
        ImGui::TextWrapped("Custom output target available when VSync is off");
      }
    }

    ImGui::EndTable();
  }

    ImGui::Spacing();
    ImGui::TextDisabled("FRAME RATE CONTROLS");
    ImGui::Separator();
    if (ImGui::BeginTable("##frame_rate_controls", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch,
                              0.62f);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch,
                              0.38f);
      const auto source_cap_state =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const std::string source_cap_summary =
          UserSourceCapSummary(source_cap_state);
      int source_cap = static_cast<int>(source_cap_state.configured_fps);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "Output FPS Cap (Reflex)", source_cap_summary.c_str(),
          "Sets the final/output FPS ceiling handled by native Reflex sleep and the DLSS-G pacer. Entering 120 targets approximately 120 displayed FPS. It applies with fixed 2x-6x, active Dynamic MFG, or fresh driver-confirmed MFG in Game controlled. A stricter native cap wins. FG-safe Off and active DXGI Waitable bypass/replace this sleep, so this cap is not guaranteed there. Use FrameView Displayed FPS to validate final output.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::InputInt("##rendered_fps_cap", &source_cap, 1, 10)) {
        source_cap = source_cap <= 0 ? 0
                                     : std::clamp(source_cap, 10, 1000);
        mfgunlock::framecount::g_reflex_source_fps_cap.store(
            static_cast<unsigned int>(source_cap),
            std::memory_order_relaxed);
        g_source_cap_config_origin.store(
            static_cast<unsigned int>(
                mfgunlock::pacing::SourceCapConfigOrigin::kConfigured),
            std::memory_order_relaxed);
        mfgunlock::framecount::NotifySourceFpsCapChanged();
        reshade::set_config_value(nullptr, kConfigSection,
                                  "ReflexSourceFpsCap", source_cap);
      }
      ImGui::EndTable();
      if (source_cap_state.configured_fps != 0) {
        ImGui::TextColored(UserSourceCapColor(source_cap_state.status), "%s",
                           UserSourceCapStatusText(source_cap_state.status));
        ImGui::TextDisabled(
            "This is the final FPS ceiling; actual displayed FPS may be lower.");
      }
    }
  }

  if (page == UiPage::Latency) {
    ImGui::Spacing();
    ImGui::TextDisabled("REFLEX / PACING LAB");
    ImGui::Separator();
    DrawReflexPacingLabControl();
    ImGui::Spacing();
    ImGui::TextDisabled("LATENCY GUARD");
    ImGui::Separator();
    DrawLatencyGuardControl();
    const auto latency_guard_mode =
        static_cast<mfgunlock::pacing::LatencyGuardMode>(
            mfgunlock::framecount::g_latency_guard_mode.load(
                std::memory_order_relaxed));
    if (latency_guard_mode ==
        mfgunlock::pacing::LatencyGuardMode::kAutomatic) {
      ImGui::TextColored(
          kUiWarning,
          "Changing the DLSS-G multiplier may cause a brief hitch during trials and periodic rechecks.");
    }
    if (latency_guard_mode == mfgunlock::pacing::LatencyGuardMode::kOff) {
      ImGui::TextDisabled(
          "Latency monitoring is off. Select Monitor Only for read-only information.");
    } else {
    ImGui::Spacing();
    ImGui::TextDisabled("CURRENT LATENCY STATUS");
    const bool guard_sample_seen =
        mfgunlock::framecount::g_latency_guard_sample_seen.load(
            std::memory_order_acquire);
    if (!guard_sample_seen) {
      ImGui::TextDisabled("Waiting for the first bounded NVAPI sample.");
    } else if (ImGui::BeginTable("##latency_guard_status", 2,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch,
                              0.48f);
      ImGui::TableSetupColumn("Observation",
                              ImGuiTableColumnFlags_WidthStretch, 0.52f);
      StatusSectionRow("INTEGRATION");
      const auto marker_health =
          static_cast<mfgunlock::pacing::MarkerHealth>(
              mfgunlock::framecount::g_latency_guard_marker_health.load(
                  std::memory_order_relaxed));
      StatusRow("Reflex integration", MarkerHealthText(marker_health),
                marker_health == mfgunlock::pacing::MarkerHealth::kHealthy
                    ? kUiPositive
                    : (marker_health ==
                               mfgunlock::pacing::MarkerHealth::kWaiting ||
                           marker_health ==
                               mfgunlock::pacing::MarkerHealth::kUnavailable
                       ? kUiMuted
                       : kUiWarning));
      const uint32_t timing_issues =
          mfgunlock::framecount::g_latency_guard_timing_issue_mask.load(
              std::memory_order_relaxed);
      const bool source_timing_confident =
          mfgunlock::framecount::g_latency_guard_timing_confident.load(
              std::memory_order_relaxed);
      const bool queue_timing_confident =
          mfgunlock::framecount::g_latency_guard_queue_timing_confident.load(
              std::memory_order_relaxed);
      StatusRow("Source timing validation",
                TimingValidationText(timing_issues),
                source_timing_confident ? kUiPositive : kUiWarning);
      StatusRow("Queue timing validation",
                !source_timing_confident
                    ? "Waiting for valid source timing"
                    : (queue_timing_confident
                           ? "Verified from at least 48 queue markers"
                           : "Queue-only actions unavailable; other latency signals remain active"),
                queue_timing_confident
                    ? kUiPositive
                    : (source_timing_confident ? kUiWarning : kUiMuted));
      const unsigned int refresh =
          mfgunlock::framecount::g_latency_guard_display_refresh_fps.load(
              std::memory_order_relaxed);
      const unsigned int live_multiplier =
          mfgunlock::framecount::g_latency_guard_live_multiplier.load(
              std::memory_order_relaxed);
      const unsigned int source_cap =
          mfgunlock::framecount::g_latency_guard_recommended_source_cap_fps.load(
              std::memory_order_relaxed);
      const unsigned int active_source_cap =
          mfgunlock::framecount::g_latency_guard_active_source_cap_fps.load(
              std::memory_order_relaxed);
      const unsigned int source_fps =
          mfgunlock::framecount::g_latency_guard_estimated_source_fps.load(
              std::memory_order_relaxed);
      const unsigned int projected_output =
          mfgunlock::framecount::g_latency_guard_projected_output_fps.load(
              std::memory_order_relaxed);
      const unsigned int queue_wait =
          mfgunlock::framecount::g_latency_guard_queue_wait_us.load(
              std::memory_order_relaxed);
      const unsigned int queue_p95 =
          mfgunlock::framecount::g_latency_guard_queue_p95_us.load(
              std::memory_order_relaxed);
      const unsigned int input_to_gpu =
          mfgunlock::framecount::g_latency_guard_input_to_gpu_end_us.load(
              std::memory_order_relaxed);
      const unsigned int gpu_active =
          mfgunlock::framecount::g_latency_guard_gpu_active_us.load(
              std::memory_order_relaxed);
      const unsigned int pipeline =
          mfgunlock::framecount::g_latency_guard_pipeline_latency_us.load(
              std::memory_order_relaxed);
      const unsigned int pipeline_p95 =
          mfgunlock::framecount::g_latency_guard_pipeline_p95_us.load(
              std::memory_order_relaxed);
      const unsigned int ai_frame_time =
          mfgunlock::framecount::g_latency_guard_ai_frame_time_us.load(
              std::memory_order_relaxed);
      const std::string refresh_text =
          refresh == 0 ? "Not detected" : std::to_string(refresh) + " Hz";
      const std::string multiplier_text =
          live_multiplier < 2 ? "Not reported"
                              : std::to_string(live_multiplier) + "x live";
      const std::string source_cap_text =
          source_cap == 0
              ? "No queue trim justified"
              : std::to_string(source_cap) + " source FPS (small trim)";
      const std::string source_text =
          source_fps == 0 ? "Not available"
                          : std::to_string(source_fps) + " real FPS estimated";
      const std::string projected_text =
          projected_output == 0
              ? "Not available"
              : std::to_string(projected_output) + " FPS estimated";
      const std::string queue_text =
          queue_wait == 0
              ? "Not reported"
              : std::to_string(queue_wait / 1000.0f).substr(0, 4) +
                    " ms median / " +
                    std::to_string(queue_p95 / 1000.0f).substr(0, 4) +
                    " ms p95";
      const std::string input_text =
          input_to_gpu == 0
              ? "Input marker not reported"
              : std::to_string(input_to_gpu / 1000.0f).substr(0, 5) +
                    " ms (input sample to GPU end)";
      const std::string ai_text =
          ai_frame_time == 0
              ? "Not reported"
              : std::to_string(ai_frame_time / 1000.0f).substr(0, 5) +
                    " ms";
      const std::string pipeline_text =
          pipeline == 0
              ? "Not reported"
              : std::to_string(pipeline / 1000.0f).substr(0, 5) +
                    " ms median / " +
                    std::to_string(pipeline_p95 / 1000.0f).substr(0, 5) +
                    " ms p95";
      const std::string gpu_active_text =
          gpu_active == 0
              ? "Not reported"
              : std::to_string(gpu_active / 1000.0f).substr(0, 5) +
                    " ms active GPU";
      StatusSectionRow("PERFORMANCE");
      StatusRow("Display refresh", refresh_text.c_str(), kUiMuted);
      StatusRow("Active FG multiplier", multiplier_text.c_str(), kUiMuted);
      const bool iflip_known =
          mfgunlock::framecount::g_latency_guard_iflip_known.load(
              std::memory_order_relaxed);
      const bool iflip_active =
          mfgunlock::framecount::g_latency_guard_iflip_active.load(
              std::memory_order_relaxed);
      StatusRow("Independent flip",
                !iflip_known ? "Not reported"
                             : (iflip_active ? "Active" : "Inactive"),
                !iflip_known || iflip_active ? kUiMuted : kUiWarning);
      StatusRow("Rendered/source FPS", source_text.c_str(), kUiMuted);
      StatusRow("Estimated displayed FPS", projected_text.c_str(),
                mfgunlock::framecount::g_latency_guard_oversubscribed.load(
                    std::memory_order_relaxed)
                    ? kUiWarning
                    : kUiMuted);
      const uint32_t observed_source_interval = source_fps == 0
          ? 0
          : mfgunlock::pacing::TargetFpsToFrameLimitUs(source_fps);
      const bool meaningful_queue =
          queue_timing_confident && queue_wait >= 1500 &&
          observed_source_interval != 0 &&
          static_cast<uint64_t>(queue_wait) * 4ull >=
              observed_source_interval;
      StatusRow("Median render-queue wait", queue_text.c_str(),
                meaningful_queue ? kUiWarning : kUiMuted);
      StatusRow("Estimated pipeline latency", pipeline_text.c_str(), kUiMuted);
      HelpMarker(
          "This is a marker-to-GPU estimate from game/Reflex timestamps, not NVIDIA overlay end-to-end system latency.");
      StatusRow("GPU active work", gpu_active_text.c_str(), kUiMuted);
      StatusRow("Input-to-GPU latency", input_text.c_str(), kUiMuted);
      StatusRow("DLSS-G workload", ai_text.c_str(), kUiMuted);
      const auto bottleneck =
          static_cast<mfgunlock::pacing::LatencyBottleneck>(
              mfgunlock::framecount::g_latency_guard_bottleneck.load(
                  std::memory_order_relaxed));
      StatusRow("Dominant observed stage",
                LatencyBottleneckText(bottleneck),
                bottleneck == mfgunlock::pacing::LatencyBottleneck::kBalanced
                    ? kUiPositive
                    : (bottleneck ==
                               mfgunlock::pacing::LatencyBottleneck::kInsufficientData
                           ? kUiMuted : kUiWarning));
      const unsigned int sample_cost =
          mfgunlock::framecount::g_latency_guard_sample_cost_us.load(
              std::memory_order_relaxed);
      const std::string sample_cost_text =
          sample_cost == 0 ? "Below timer resolution"
                           : std::to_string(sample_cost) + " us every 500 ms";
      StatusRow("Monitoring overhead", sample_cost_text.c_str(), kUiMuted);
      const unsigned int limit_source =
          mfgunlock::framecount::g_reflex_limit_source.load(
              std::memory_order_relaxed);
      const auto user_source_cap =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const bool explicit_source_cap =
          limit_source == static_cast<unsigned int>(
              mfgunlock::framecount::internal::ReflexTargetSource::
                  kUserSourceCap);
      const unsigned int multiplier_override =
          mfgunlock::framecount::g_latency_guard_multiplier_override.load(
              std::memory_order_acquire);
      const bool multiplier_accepted =
          mfgunlock::framecount::g_latency_guard_multiplier_trial_accepted.load(
              std::memory_order_relaxed);
      const auto multiplier_phase =
          static_cast<mfgunlock::latency::MultiplierTrialPhase>(
              mfgunlock::framecount::g_latency_guard_multiplier_trial_phase.load(
                  std::memory_order_relaxed));
      const auto multiplier_reason =
          static_cast<mfgunlock::latency::ResponsiveTrialReason>(
              mfgunlock::framecount::g_latency_guard_multiplier_trial_reason.load(
                  std::memory_order_relaxed));
      const unsigned int saved_multiplier =
          mfgunlock::framecount::g_force_multiplier.load(
              std::memory_order_relaxed);
      const unsigned int approved_multiplier =
          mfgunlock::framecount::g_latency_guard_multiplier_approved.load(
              std::memory_order_relaxed);
      const unsigned int candidate_multiplier =
          mfgunlock::framecount::g_latency_guard_multiplier_candidate.load(
              std::memory_order_relaxed);
      StatusSectionRow("GUARD DECISION");
      const char* action =
          explicit_source_cap
              ? "Explicit Reflex output cap has priority"
              : (multiplier_phase !=
                         mfgunlock::latency::MultiplierTrialPhase::kIdle
                     ? MultiplierTrialPhaseText(multiplier_phase)
                     : (limit_source == 2
                     ? "Queue trim active"
                     : (latency_guard_mode ==
                                mfgunlock::pacing::LatencyGuardMode::kAutomatic
                            ? "Monitoring; no safe cap change"
                            : "Read-only monitoring")));
      StatusRow("Guard action", action,
                limit_source == 2 || explicit_source_cap ||
                        multiplier_override != 0
                    ? kUiPositive : kUiMuted);
      if (user_source_cap.configured_fps != 0) {
        const std::string cap_text =
            UserSourceCapSummary(user_source_cap) + "; " +
            UserSourceCapStatusText(user_source_cap.status);
        StatusRow("Output FPS cap", cap_text.c_str(),
                  UserSourceCapColor(user_source_cap.status));
      }
      if (latency_guard_mode ==
              mfgunlock::pacing::LatencyGuardMode::kAutomatic &&
          multiplier_phase ==
              mfgunlock::latency::MultiplierTrialPhase::kIdle) {
        const auto blocker =
            static_cast<mfgunlock::latency::ResponsiveTrialBlocker>(
                mfgunlock::framecount::g_latency_guard_trial_blocker.load(
                    std::memory_order_relaxed));
        StatusRow("Responsive eligibility",
                  ResponsiveTrialBlockerText(blocker),
                  blocker ==
                          mfgunlock::latency::ResponsiveTrialBlocker::kNone
                      ? kUiPositive
                      : kUiMuted);
      }
      if (saved_multiplier >= 2) {
        const std::string saved_text =
            std::to_string(saved_multiplier) + "x (never overwritten)";
        StatusRow("Saved multiplier", saved_text.c_str(), kUiMuted);
      }
      if (multiplier_phase !=
          mfgunlock::latency::MultiplierTrialPhase::kIdle) {
        StatusRow("Responsive trial", MultiplierTrialPhaseText(multiplier_phase),
                  multiplier_accepted ? kUiPositive : kUiWarning);
        StatusRow("Trial reason", ResponsiveTrialReasonText(multiplier_reason),
                  kUiMuted);
        if (approved_multiplier >= 3 &&
            approved_multiplier < saved_multiplier) {
          const std::string approved_text =
              std::to_string(approved_multiplier) +
              "x is the last measured-beneficial step";
          StatusRow("Last approved multiplier", approved_text.c_str(),
                    kUiPositive);
        }
        if (candidate_multiplier >= 3 &&
            candidate_multiplier != saved_multiplier) {
          const std::string candidate_text =
              std::to_string(approved_multiplier) + "x -> " +
              std::to_string(candidate_multiplier) + "x";
          StatusRow("Current trial step", candidate_text.c_str(),
                    kUiWarning);
        }
        const unsigned int baseline_samples =
            mfgunlock::framecount::g_latency_guard_multiplier_baseline_samples.load(
                std::memory_order_relaxed);
        const unsigned int trial_samples =
            mfgunlock::framecount::g_latency_guard_multiplier_trial_samples.load(
                std::memory_order_relaxed);
        const std::string window_text =
            "baseline " + std::to_string(baseline_samples) +
            "/8; current " + std::to_string(trial_samples) + "/8";
        StatusRow("Measurement windows", window_text.c_str(), kUiMuted);
        const unsigned int baseline_pipeline =
            mfgunlock::framecount::g_latency_guard_multiplier_baseline_pipeline_us.load(
                std::memory_order_relaxed);
        const unsigned int baseline_p95 =
            mfgunlock::framecount::g_latency_guard_multiplier_baseline_p95_us.load(
                std::memory_order_relaxed);
        const unsigned int trial_pipeline =
            mfgunlock::framecount::g_latency_guard_multiplier_trial_pipeline_us.load(
                std::memory_order_relaxed);
        const unsigned int trial_p95 =
            mfgunlock::framecount::g_latency_guard_multiplier_trial_p95_us.load(
                std::memory_order_relaxed);
        if (baseline_pipeline != 0 && trial_pipeline != 0) {
          const std::string comparison =
              std::to_string(baseline_pipeline / 1000.0f).substr(0, 5) +
              "/" + std::to_string(baseline_p95 / 1000.0f).substr(0, 5) +
              " ms baseline -> " +
              std::to_string(trial_pipeline / 1000.0f).substr(0, 5) +
              "/" + std::to_string(trial_p95 / 1000.0f).substr(0, 5) +
              " ms measured (median/p95)";
          StatusRow("Pipeline comparison", comparison.c_str(), kUiMuted);
        }
      }
      if (source_cap != 0 && limit_source != 2 &&
          !user_source_cap.requested)
        StatusRow("Potential automatic trim", source_cap_text.c_str(),
                  kUiWarning);
      if (active_source_cap != 0) {
        const std::string active_text =
            std::to_string(active_source_cap) + " source FPS";
        StatusRow("Active queue-trim cap", active_text.c_str(), kUiPositive);
      }
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Automatic tests fixed multipliers one step at a time (6x -> 5x -> 4x -> 3x)\n"
        "when input/pipeline latency, a GPU-bound latency proxy, queueing, output saturation or DLSS-G workload justifies a trial.\n"
        "Each step uses two eight-sample windows and is kept only after lower measured latency.\n"
        "It periodically restores the saved multiplier for a new baseline and never goes below 3x.\n"
        "An explicit Reflex output-FPS cap remains unchanged across multiplier tests; queue trim never replaces it.\n"
        "A small source-rate trim is used only as a verified queue-pressure fallback when no explicit cap is set.\n"
        "It does not replace Reflex or change Dynamic MFG, VSync or G-SYNC behavior.");
    ImGui::TextDisabled(
        "Real/source FPS comes from game markers. Estimated output FPS is source FPS x the driver-reported multiplier; it is not a displayed-frame measurement.");
    HelpMarker(
        "ReShade and NVIDIA overlays may expose rendered/source FPS depending on their selected metric. Confirm actual displayed output with FrameView or PresentMon Displayed FPS.");
    }
  }

  if (page == UiPage::General) {
    ImGui::Spacing();
    ImGui::TextDisabled("IMAGE QUALITY");
    ImGui::Separator();
    if (ImGui::BeginTable("##image_quality_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.38f);

    constexpr const char* kHdrModes[] = {
        "Native (Most Games)", "Force UI Composition (Advanced)",
        "Automatic Guard + UI (HDR Compatibility)",
        "Final Color Fallback (Troubleshooting)"};
    int hdr_mode = static_cast<int>(
        mfgunlock::framecount::g_hdr_compatibility_mode.load(
            std::memory_order_relaxed));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const char* hdr_summary = "Native game inputs; recommended for most games.";
    if (hdr_mode == static_cast<int>(
                        mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
      hdr_summary = "Advanced UI separation; requires correctly matched game buffers.";
    } else if (hdr_mode == static_cast<int>(
                               mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
      hdr_summary =
          "HDR compatibility for Hogwarts Legacy, Jusant and Mafia: The Old Country.";
    } else if (hdr_mode == static_cast<int>(
                               mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
      hdr_summary = "Conservative HDR/UI troubleshooting fallback.";
    }
    SettingLabel(
        "Frame-generation Inputs", hdr_summary,
        "Native passes the game's tags unchanged and is recommended for most games. Automatic Guard + UI is intended for HDR-related Frame Generation artifacts in games such as Hogwarts Legacy, Jusant and Mafia: The Old Country. It validates HUD-less/UI inputs and falls back safely when their formats or color spaces do not match. If HUD elements show artifacts, return to Native.");
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##hdr_compatibility", &hdr_mode, kHdrModes,
                     static_cast<int>(std::size(kHdrModes)))) {
      mfgunlock::framecount::g_hdr_compatibility_mode.store(
          static_cast<unsigned int>(hdr_mode), std::memory_order_relaxed);
      mfgunlock::framecount::NotifyQualityModeChanged();
      reshade::set_config_value(nullptr, kConfigSection,
                                "HDRCompatibilityMode", hdr_mode);
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Adaptive Quality Suite",
        "Unified quality profile for silhouettes, warping and inpaint.",
        "Coordinates silhouette confidence, motion-direction scatter coverage, warp validation, candidate arbitration and inpaint decisions as one quality path. Standalone legacy experiments remain saved but do not stack on top of it. Exact kernel/provider validation is mandatory and unavailable components fall back independently. Requires a full game restart.",
        "Recommended", kUiPositive);
    ImGui::TableNextColumn();
    bool adaptive_quality =
        g_configured_adaptive_quality.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("##adaptive_quality", &adaptive_quality)) {
      g_configured_adaptive_quality.store(adaptive_quality,
                                          std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "ExperimentalAdaptiveQuality",
                                adaptive_quality ? 1 : 0);
    }
    if (adaptive_quality !=
        g_adaptive_quality.load(std::memory_order_relaxed))
      ImGui::TextDisabled("Saved for next launch; restart the game.");

    if (adaptive_quality) {
      constexpr const char* kAdaptiveProfiles[] = {
          "Stable V1 (Compatibility)",
          "Flicker-Reduced V2",
          "Luminance + Directional V3 (Recommended)"};
      int adaptive_profile = static_cast<int>(
          mfgunlock::adaptivequality::NormalizeProfile(
              g_configured_adaptive_quality_profile.load(
                  std::memory_order_relaxed))) - 1;
      const char* profile_summary =
          adaptive_profile == 2
              ? "Exposure-relative color confidence and motion-oriented edges."
              : adaptive_profile == 1
                    ? "Smoother confidence transitions for fine detail and silhouettes."
                    : "Released 1.1.5 behavior for compatibility.";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "Adaptive Quality Profile", profile_summary,
          "V3 normalizes luma/chroma disagreement by local luminance and adds motion-oriented border and geometry confidence. V3.2 Local Stable uses only the existing 3x3 tile; optional Temporal Stable stores confidence bytes only after a strict CUDA probe and uses the low-overhead launch fast path. V2 retains absolute RGB confidence; Stable V1 preserves the 1.1.5 behavior. Components fall back independently. Requires restart.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::Combo("##adaptive_quality_profile", &adaptive_profile,
                       kAdaptiveProfiles,
                       static_cast<int>(std::size(kAdaptiveProfiles)))) {
        const auto selected = mfgunlock::adaptivequality::NormalizeProfile(
            static_cast<unsigned int>(adaptive_profile + 1));
        g_configured_adaptive_quality_profile.store(
            static_cast<unsigned int>(selected),
            std::memory_order_relaxed);
        reshade::set_config_value(
            nullptr, kConfigSection, "AdaptiveQualityProfile",
            static_cast<int>(selected));
      }
      if (g_configured_adaptive_quality_profile.load(
              std::memory_order_relaxed) !=
          g_adaptive_quality_profile.load(std::memory_order_relaxed)) {
        ImGui::TextDisabled("Saved for next launch; restart the game.");
      }
    }
    ImGui::EndTable();
  }
  if (page == UiPage::General &&
      g_configured_adaptive_quality.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "Adaptive Quality manages geometry, boundary handling, warp validation and inpaint as one profile.");
  }

  if (page == UiPage::General && ImGui::CollapsingHeader("Advanced")) {
    constexpr const char* kRuntimeModes[] = {
        "Game default", "Prefer local runtime - disable OTA",
        "Force NVIDIA OTA runtime"};
    int runtime_mode = static_cast<int>(
        g_configured_runtime_selection_mode.load(std::memory_order_relaxed));
    SetNextItemWidthWithHelp("Streamline runtime selection");
    if (ImGui::Combo("Streamline runtime selection", &runtime_mode,
                     kRuntimeModes,
                     static_cast<int>(std::size(kRuntimeModes)))) {
      g_configured_runtime_selection_mode.store(
          static_cast<unsigned int>(runtime_mode),
          std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "RuntimeSelectionMode", runtime_mode);
    }
    HelpMarker(
        "Selects which complete Streamline runtime package initializes on the next launch. Do not mix DLL versions. Game default is recommended. Requires restart.");

    int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
    SetNextItemWidthWithHelp("Reported MultiFrameCountMax");
    if (ImGui::SliderInt("Reported MultiFrameCountMax", &count,
                         static_cast<int>(kMinCount),
                         static_cast<int>(kMaxCount))) {
      count = std::clamp(count, static_cast<int>(kMinCount),
                         static_cast<int>(kMaxCount));
      g_max_count.store(static_cast<unsigned int>(count),
                        std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
    }
    HelpMarker(
        "Controls the maximum generated-frame count advertised to the game. Toggle Frame Generation off/on if the game's selector does not refresh.");

    bool temporal =
        g_configured_temporal_fix.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Temporal midpoint fix", &temporal)) {
      g_configured_temporal_fix.store(temporal,
                                      std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "TemporalFix",
                                temporal ? 1 : 0);
    }
    HelpMarker(
        "Corrects Ada midpoint compaction at higher multipliers. Applied during provider loading and requires restart.");

    bool blackwell = g_configured_blackwell_framework_kernels.load(
        std::memory_order_relaxed);
    if (ImGui::Checkbox("Prefer validated Blackwell framework kernels",
                        &blackwell)) {
      g_configured_blackwell_framework_kernels.store(
          blackwell, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "BlackwellFrameworkKernels",
                                blackwell ? 1 : 0);
    }
    HelpMarker(
        "Uses the full validated motion-vector/inpaint framework path when an exact provider match exists. Otherwise the addon fails closed to its compatible fallback. Requires restart.");

    bool flip_off =
        g_configured_force_flip_meter_off.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Legacy software flip pacing", &flip_off)) {
      g_configured_force_flip_meter_off.store(flip_off,
                                               std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "ForceFlipMeteringOff", flip_off ? 1 : 0);
    }
    HelpMarker(
        "Compatibility fallback for older Streamline integrations where 3x/4x freezes. Leave disabled with current runtimes. Requires restart.");

    const int active_runtime_mode = static_cast<int>(
        mfgunlock::framecount::g_runtime_selection_mode.load(
            std::memory_order_relaxed));
    if (active_runtime_mode != static_cast<int>(
                                   mfgunlock::framecount::RuntimeSelectionMode::kGameDefault) &&
        !mfgunlock::framecount::g_runtime_selection_observed.load(
            std::memory_order_acquire) &&
        !HasEarlyLoadEntry()) {
      ImGui::TextColored(kUiWarning,
                         "Streamline initialized before normal addon loading.");
      if (ImGui::Button("Enable early addon loading")) {
        EnsureEarlyLoadEntry();
      }
      HelpMarker(
          "Adds this addon's current filename to ReShade's ADDON.LoadFromDllMain list. Required only when a selected runtime policy must intercept slInit before normal addon loading. Requires restart.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("IMAGE QUALITY COMPATIBILITY");
    DrawAdvancedQualityControls(
        g_configured_adaptive_quality.load(std::memory_order_relaxed));
  }
  }

  if (page == UiPage::Support) {
    ImGui::Spacing();
    ImGui::TextDisabled("SUPPORT DIAGNOSTICS");
    ImGui::Separator();
    bool gate_patched = false;
    bool midpoint_patched = false;
    bool blackwell_patched = false;
    bool thin_geometry_patched = false;
    size_t gate_provider_count = 0;
    size_t gate_site_count = 0;
    size_t midpoint_provider_count = 0;
    size_t blackwell_provider_count = 0;
    size_t thin_geometry_provider_count = 0;
    std::string midpoint_detail;
    std::string blackwell_detail;
    std::string blackwell_applied_detail;
    mfgunlock::blackwell::Result blackwell_active_result{};
    ThinGeometryModulePatch thin_geometry_last;
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    ImGui::TextDisabled("Diagnostics updating; retry next frame.");
    return;
  }
    gate_patched = g_gate_patched.load(std::memory_order_relaxed);
    midpoint_patched = g_midpoint_patched.load(std::memory_order_relaxed);
    blackwell_patched = g_blackwell_patched.load(std::memory_order_relaxed);
    thin_geometry_patched =
        g_thin_geometry_patched.load(std::memory_order_relaxed);
    gate_provider_count = g_gate_modules.size();
    gate_site_count = g_gate_sites.size();
    midpoint_provider_count = g_midpoint_modules.size();
    blackwell_provider_count = g_blackwell_modules.size();
    thin_geometry_provider_count = g_thin_geometry_modules.size();
    midpoint_detail = g_midpoint_detail;
    blackwell_detail = g_blackwell_detail;
    blackwell_applied_detail = g_blackwell_applied_detail;
    blackwell_active_result = AggregateBlackwellResults(g_blackwell_modules);
    thin_geometry_last =
        SelectRelevantThinGeometryResult(g_thin_geometry_modules);
    ReleaseSRWLockShared(&g_provider_maintenance_lock);

    ImGui::TextDisabled("RUNTIME");
    if (ImGui::BeginTable("##runtime_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      StatusRow("Renderer", RenderApiName(render_api), kUiMuted);
      StatusRow("Streamline", streamline_text.c_str(), kUiMuted);
      StatusRow("DLSS-G", dlssg_text.c_str(), kUiMuted);
      StatusRow("DLSS Super Resolution", super_resolution_preset.c_str(),
                kUiMuted);
      StatusRow("DLSS Frame Generation", frame_generation_preset.c_str(),
                kUiMuted);
      StatusRow("DLSS Ray Reconstruction",
                ray_reconstruction_preset.c_str(), kUiMuted);
      StatusRow("HDR", hdr_seen ? (hdr_active ? "Yes" : "No") : "Unknown",
                kUiMuted);
      const std::string diagnostic_sync =
          !sleep_status_available
              ? "Not reported"
              : (driver_vsync_active && gsync_active
                     ? "Driver VSync + G-SYNC active"
                     : (driver_vsync_active
                            ? "Driver VSync active"
                            : (gsync_active ? "G-SYNC active"
                                            : "Inactive")));
      StatusRow("Driver sync", diagnostic_sync.c_str(), kUiMuted);
      StatusRow("Dynamic MFG VSync API",
                !mfgunlock::framecount::g_vsync_support_seen.load(
                     std::memory_order_acquire)
                    ? "Not reported"
                    : (mfgunlock::framecount::g_vsync_supported.load(
                           std::memory_order_relaxed)
                           ? "Supported by active runtime"
                           : "Not supported by active runtime"),
                kUiMuted);
      ImGui::EndTable();
    }
    TextDisabledWrapped(
        "This reports the Streamline/DLSS-G Dynamic interface, not whether driver VSync is enabled.");

    ImGui::Spacing();
    ImGui::TextDisabled("MFG");
    if (ImGui::BeginTable("##mfg_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      const std::string game_request_text =
          game_request_seen ? std::to_string(game_multiplier) + "x"
                            : "Not observed";
      const std::string effective_text =
          effective_seen ? std::to_string(effective_multiplier) + "x"
                         : "Not observed";
      StatusRow("Game request", game_request_text.c_str(), kUiMuted);
      StatusRow("Effective request", effective_text.c_str(), kUiMuted);
      const std::string actual_text =
          state_seen
              ? std::to_string(mfgunlock::framecount::g_actual_frames_presented.load(
                    std::memory_order_relaxed))
              : "No sample";
      const std::string live_multiplier =
          driver_multiplier >= 2
              ? std::to_string(driver_multiplier) + "x live"
              : "Not reported";
      StatusRow("Driver live multiplier", live_multiplier.c_str(),
                driver_multiplier >= 2 ? kUiPositive : kUiMuted);
      StatusRow("Provider presentations", actual_text.c_str(),
                mfg_confirmed ? kUiPositive : kUiMuted);
      StatusRow("Validation evidence", validation_text.c_str(),
                runtime_error
                    ? kUiError
                    : (request_rejected
                           ? kUiWarning
                           : (mfg_request_accepted ? kUiPositive : kUiMuted)));
      StatusRow("Dynamic MFG",
                mfgunlock::framecount::g_dynamic_applied.load(
                    std::memory_order_relaxed)
                    ? "Active"
                    : (dynamic_available ? "Available" : "Inactive"),
                kUiMuted);
      const auto source_cap_state =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const std::string source_cap_value =
          std::to_string(source_cap_state.configured_fps) +
          " final/output FPS; " +
          UserSourceCapStatusText(source_cap_state.status);
      StatusRow("Output FPS cap", source_cap_value.c_str(),
                UserSourceCapColor(source_cap_state.status));
      std::ostringstream source_cap_intervals;
      source_cap_intervals << "requested "
                           << source_cap_state.requested_limit_us
                           << " us; native "
                           << source_cap_state.native_limit_us
                           << " us; effective "
                           << source_cap_state.effective_limit_us << " us";
      StatusRow("Reflex cap intervals", source_cap_intervals.str().c_str(),
                kUiMuted);
      StatusRow(
          "Cap configuration source",
          SourceCapConfigOriginText(
              static_cast<mfgunlock::pacing::SourceCapConfigOrigin>(
                  g_source_cap_config_origin.load(
                      std::memory_order_relaxed))),
          kUiMuted);
      ImGui::EndTable();
    }
    TextDisabledWrapped(
        "Evidence is layered: an accepted request, a driver-live multiplier and provider output are separate observations.");
    HelpMarker(
        "ReShade's FPS counter and many overlays can report game/source Presents rather than frames scanned out after DLSS-G. Estimated output FPS is never treated as a measured result here. Use FrameView or PresentMon Displayed FPS for external confirmation.");

    ImGui::Spacing();
    ImGui::TextDisabled("PATCHES");
    if (ImGui::BeginTable("##patch_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      StatusRow("Architecture gates", gate_patched ? "Active" : "Pending",
                gate_patched ? kUiPositive : kUiWarning);
      StatusRow("Temporal fix",
                blackwell_patched
                    ? "Blackwell framework active"
                    : (midpoint_patched ? "Midpoint fallback active"
                                        : "Pending"),
                blackwell_patched || midpoint_patched ? kUiPositive
                                                      : kUiWarning);
      StatusRow("Thin-geometry patches",
                thin_geometry_patched ? "At least one active" : "Pending",
                thin_geometry_patched ? kUiPositive : kUiWarning);
      if (g_adaptive_quality.load(std::memory_order_relaxed)) {
        const auto requested_profile =
            mfgunlock::adaptivequality::NormalizeProfile(
                g_adaptive_quality_profile.load(std::memory_order_relaxed));
        const bool adaptive_blend =
            thin_geometry_last.result.validated_warp_blend.applied;
        const bool adaptive_complete =
            blackwell_active_result.adaptive_geometry && adaptive_blend &&
            blackwell_active_result.adaptive_inpaint_decision;
        const auto warp_version =
            thin_geometry_last.result.validated_warp_blend.adaptive_version;
        const auto geometry_version =
            blackwell_active_result.adaptive_geometry_version;
        const auto inpaint_version =
            blackwell_active_result.adaptive_inpaint_version;
        const auto expected_warp =
            mfgunlock::adaptivequality::ExpectedComponentVersion(
                requested_profile,
                mfgunlock::adaptivequality::Component::kWarp);
        const auto expected_geometry =
            requested_profile ==
                        mfgunlock::adaptivequality::Profile::
                            kLuminanceDirectionalV3 &&
                    !g_adaptive_quality_v3_oriented_geometry.load(
                        std::memory_order_relaxed)
                ? mfgunlock::adaptivequality::ComponentVersion::kV2
                : mfgunlock::adaptivequality::ExpectedComponentVersion(
                      requested_profile,
                      mfgunlock::adaptivequality::Component::kGeometry);
        const auto requested_inpaint_mode =
            mfgunlock::cudatemporal::NormalizeInpaintMode(
                g_adaptive_quality_v3_inpaint_mode.load(
                    std::memory_order_relaxed));
        const auto expected_inpaint =
            requested_profile ==
                    mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
                requested_inpaint_mode !=
                    mfgunlock::cudatemporal::InpaintMode::kV2Compatibility
                ? mfgunlock::adaptivequality::ComponentVersion::kV3
                : mfgunlock::adaptivequality::ExpectedComponentVersion(
                      requested_profile,
                      mfgunlock::adaptivequality::Component::kInpaint);
        const bool all_requested_versions =
            warp_version == expected_warp &&
            geometry_version == expected_geometry &&
            inpaint_version == expected_inpaint;
        const std::string profile_status =
            std::string(mfgunlock::adaptivequality::ProfileName(
                requested_profile)) +
            (adaptive_complete && all_requested_versions
                 ? "; core components applied"
                 : "; partial/fallback");
        StatusRow("Adaptive Quality Suite",
                  profile_status.c_str(),
                  adaptive_complete && all_requested_versions
                      ? kUiPositive
                      : kUiWarning);
        const std::string warp_status = AdaptiveComponentStatus(
            expected_warp, warp_version, adaptive_blend);
        StatusRow("Warp confidence", warp_status.c_str(),
                  AdaptiveComponentColor(expected_warp, warp_version,
                                         adaptive_blend));
        const std::string geometry_status = AdaptiveComponentStatus(
            expected_geometry, geometry_version,
            blackwell_active_result.adaptive_geometry);
        StatusRow("Geometry confidence", geometry_status.c_str(),
                  AdaptiveComponentColor(
                      expected_geometry, geometry_version,
                      blackwell_active_result.adaptive_geometry));
        if (geometry_version ==
            mfgunlock::adaptivequality::ComponentVersion::kV3) {
          StatusRow(
              "Geometry V3 install",
              blackwell_active_result.adaptive_geometry_redirected
                  ? "Full ptxas cubin; exact fatbin redirect"
                  : "In-place cubin",
              blackwell_active_result.adaptive_geometry_redirected
                  ? kUiPositive
                  : kUiMuted);
          StatusRow("Geometry V3.2 cubin",
                    mfgunlock::blackwell::AdaptiveGeometryVariantName(
                        blackwell_active_result.adaptive_geometry_variant),
                    blackwell_active_result.adaptive_geometry_variant ==
                            mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal
                        ? kUiPositive
                        : kUiMuted);
          StatusRow("Geometry V3.2 ptxas",
                    "40 registers; 0 spills/local; 7,776 B shared",
                    kUiPositive);
        }
        const std::string inpaint_status = AdaptiveComponentStatus(
            expected_inpaint, inpaint_version,
            blackwell_active_result.adaptive_inpaint_decision);
        StatusRow("Inpaint decision", inpaint_status.c_str(),
                  AdaptiveComponentColor(
                      expected_inpaint, inpaint_version,
                      blackwell_active_result.adaptive_inpaint_decision));
        if (requested_profile ==
            mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3) {
          const char* requested_inpaint =
              requested_inpaint_mode ==
                      mfgunlock::cudatemporal::InpaintMode::kTemporal
                  ? "Temporal V3"
                  : requested_inpaint_mode ==
                            mfgunlock::cudatemporal::InpaintMode::kLocal
                        ? "Local V3"
                        : "V2 Compatibility";
          StatusRow("Inpaint V3.4 requested", requested_inpaint, kUiMuted);
          StatusRow(
              "Inpaint V3.4 cubin",
              mfgunlock::blackwell::AdaptiveInpaintVariantName(
                  blackwell_active_result.adaptive_inpaint_variant),
              blackwell_active_result.adaptive_inpaint_variant ==
                      mfgunlock::blackwell::AdaptiveInpaintVariant::kTemporal
                  ? kUiPositive
                  : kUiMuted);
          const std::string inpaint_temporal_detail =
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
              "V3 inpaint removed; V2 Compatibility requested (see installed cubin above)";
#else
              mfgunlock::cudatemporal::InpaintDetail();
#endif
          StatusRow(
              "Inpaint V3.4 effective", inpaint_temporal_detail.c_str(),
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
              kUiMuted);
#else
              mfgunlock::cudatemporal::g_inpaint_temporal_active.load(
                      std::memory_order_acquire)
                  ? kUiPositive
                  : (mfgunlock::cudatemporal::g_inpaint_fallback.load(
                             std::memory_order_acquire)
                         ? kUiWarning
                         : kUiMuted));
#endif
        }
        if (requested_profile ==
            mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3) {
          StatusRow(
              "V3 photometric A/B",
              g_adaptive_quality_v3_photometric.load(
                      std::memory_order_relaxed)
                  ? "Relative luma/chroma enabled"
                  : "Disabled; using V2 absolute RGB",
              kUiMuted);
          StatusRow(
              "V3 border A/B",
              g_adaptive_quality_v3_directional_border.load(
                      std::memory_order_relaxed)
                  ? "Motion-directional taper enabled"
                  : "Disabled; using V2 symmetric taper",
              kUiMuted);
          StatusRow(
              "V3 geometry A/B",
              g_adaptive_quality_v3_oriented_geometry.load(
                      std::memory_order_relaxed)
                  ? "Motion-oriented diagonals enabled"
                  : "Disabled; requesting geometry V2",
              kUiMuted);
        }
        StatusRow("Directional scatter",
                  blackwell_active_result.adaptive_directional_scatter
                      ? "Native motion-adaptive coverage retained"
                      : "Baseline",
                  kUiMuted);
        const auto stability_mode = mfgunlock::cudatemporal::NormalizeMode(
            g_adaptive_quality_v3_stability_mode.load(
                std::memory_order_relaxed));
        StatusRow(
            "V3.2 stability requested",
            stability_mode ==
                    mfgunlock::cudatemporal::StabilityMode::kTemporal
                ? "Temporal Stable"
                : "Local Stable",
            kUiMuted);
        const std::string temporal_detail =
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
            "History disabled; dedicated Local cubin requested (see installed variant above)";
#else
            mfgunlock::cudatemporal::Detail();
#endif
        StatusRow(
            "V3.2 stability effective", temporal_detail.c_str(),
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
            kUiMuted);
#if defined(MFGUNLOCK_LOCAL_STABILITY)
#if defined(MFGUNLOCK_RELEASE_1_4_3)
        StatusRow("Build", "Release 1.4.3; continuous border axes + paired diagonals", kUiPositive);
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
        StatusRow("Build", "1.4.2-base; continuous-axis border candidate; in-game validation pending", kUiMuted);
#else
        StatusRow("Build", "Release 1.4.2; continuous borders + paired diagonals", kUiPositive);
#endif
#else
        StatusRow("Build", "Local low-overhead; no V3 inpaint or confidence history", kUiPositive);
#endif
        StatusRow("Temporal backend", "Disabled by build; 0 B history; no self-test or barriers", kUiMuted);
        StatusRow("CUDA hook fast path", "Not installed; no CUDA/NVAPI launch interception", kUiMuted);
#else
            mfgunlock::cudatemporal::g_temporal_active.load(
                    std::memory_order_acquire)
                ? kUiPositive
                : (mfgunlock::cudatemporal::g_fallback.load(
                           std::memory_order_acquire)
                       ? kUiWarning
                       : kUiMuted));
        StatusRow(
            "CUDA hook fast path",
            mfgunlock::cudatemporal::FastPathReady()
                ? "Active; atomic CUfunction compare only for non-target kernels"
                : (mfgunlock::cudatemporal::g_hooked.load(
                           std::memory_order_acquire)
                       ? "Hook installed; target association pending"
                       : "Inactive"),
            mfgunlock::cudatemporal::FastPathReady() ? kUiPositive
                                                     : kUiMuted);
#endif
        if (mfgunlock::cudatemporal::g_history_bytes.load(
                std::memory_order_relaxed) != 0) {
          const uint64_t history_bytes =
              mfgunlock::cudatemporal::g_history_bytes.load(
                  std::memory_order_relaxed);
          std::ostringstream history_status;
          history_status << history_bytes / (1024.0 * 1024.0) << " MiB; "
                         << mfgunlock::cudatemporal::g_history_width.load(
                                std::memory_order_relaxed)
                         << 'x'
                         << mfgunlock::cudatemporal::g_history_height.load(
                                std::memory_order_relaxed)
                         << "; "
                         << mfgunlock::cudatemporal::g_history_multiplier.load(
                                std::memory_order_relaxed)
                         << 'x';
          StatusRow("V3.2 confidence history",
                    history_status.str().c_str(), kUiMuted);
        }
        if (mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                std::memory_order_relaxed) != 0) {
          std::ostringstream inpaint_history;
          inpaint_history
              << mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                     std::memory_order_relaxed) /
                     (1024.0 * 1024.0)
              << " MiB; total arena used "
              << (mfgunlock::cudatemporal::g_history_bytes.load(
                      std::memory_order_relaxed) +
                  mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                      std::memory_order_relaxed)) /
                     (1024.0 * 1024.0)
              << " / 96 MiB; resets "
              << mfgunlock::cudatemporal::g_history_resets.load(
                     std::memory_order_relaxed);
          StatusRow("V3.4 inpaint history",
                    inpaint_history.str().c_str(), kUiMuted);
        }
      }
      if (!g_adaptive_quality.load(std::memory_order_relaxed)) {
        StatusRow("Geometry confidence V2",
                  !g_geometry_confidence_v2.load(std::memory_order_relaxed)
                      ? "Off"
                      : (!g_quality_refinement.load(std::memory_order_relaxed)
                             ? "Requires Quality Refinement"
                             : (g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                                        static_cast<unsigned int>(mfgunlock::blackwell::SilhouetteGuardMode::Balanced)
                                    ? "Requires Balanced boundary mode"
                                    : (thin_geometry_last.geometry_confidence_v2_applied
                                           ? "Patch applied; visual test pending"
                                           : "Requested; V1/fallback or no patch"))),
                  kUiMuted);
        StatusRow("Border confidence trial",
                  !g_border_confidence.load(std::memory_order_relaxed)
                      ? "Off"
                      : (!g_quality_refinement.load(std::memory_order_relaxed)
                             ? "Requires Quality Refinement"
                             : (!thin_geometry_last.result.validated_warp_blend.applied
                                    ? "Requested; blend not applied"
                                    : "Patch applied; visual test pending")),
                  kUiMuted);
      }
      if (!g_adaptive_quality.load(std::memory_order_relaxed) &&
          thin_geometry_provider_count != 0) {
        const auto mechanism_state = [](const auto& result) {
          return !result.requested ? "Not requested"
                                   : (result.applied ? "Active" : "Not applied");
        };
        const bool intermediate_superseded =
            g_thin_geometry_intermediate_scatter.load(
                std::memory_order_relaxed) &&
            (g_adaptive_quality.load(std::memory_order_relaxed) ||
             g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                static_cast<unsigned int>(
                    mfgunlock::blackwell::SilhouetteGuardMode::Off));
        StatusRow("Intermediate retention",
                  intermediate_superseded
                      ? "Superseded by Boundary mitigation"
                      : mechanism_state(
                            thin_geometry_last.intermediate_scatter),
                  thin_geometry_last.intermediate_scatter.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow("Boundary mitigation",
                  mechanism_state(
                      thin_geometry_last.silhouette_boundary_guard),
                  thin_geometry_last.silhouette_boundary_guard.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow("Validated warp blend",
                  mechanism_state(
                      thin_geometry_last.result.validated_warp_blend),
                  thin_geometry_last.result.validated_warp_blend.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow("Previous scatter",
                  mechanism_state(thin_geometry_last.result.previous_scatter),
                  thin_geometry_last.result.previous_scatter.applied
                      ? kUiPositive
                      : kUiMuted);
      }
      StatusRow("Input Quality Guard",
                mfgunlock::framecount::g_hud_inputs_suppressed.load(
                    std::memory_order_relaxed)
                    ? "Filtered incompatible input"
                    : "No filtering reported",
                kUiMuted);
      const unsigned int tracked_viewports =
          mfgunlock::framecount::g_quality_viewport_count.load(
              std::memory_order_relaxed);
      const bool viewport_capacity_exhausted =
          mfgunlock::framecount::g_quality_viewport_capacity_exhausted.load(
              std::memory_order_acquire);
      const std::string viewport_text =
          std::to_string(tracked_viewports) + " / " +
          std::to_string(mfgunlock::framecount::kMaxQualityViewports) +
          (viewport_capacity_exhausted ? "; capacity exhausted" : " tracked");
      StatusRow("Quality Guard viewports", viewport_text.c_str(),
                viewport_capacity_exhausted ? kUiWarning : kUiMuted);
      const auto tag_lock_contentions =
          mfgunlock::framecount::g_quality_tag_lock_contentions.load(
              std::memory_order_relaxed);
      const std::string contention_text =
          std::to_string(tag_lock_contentions) +
          " non-blocking fallback(s)";
      StatusRow("Quality Guard contention", contention_text.c_str(),
                tag_lock_contentions == 0 ? kUiMuted : kUiWarning);
      ImGui::EndTable();
    }

    std::string intermediate_report = "Disabled";
    if (g_adaptive_quality.load(std::memory_order_relaxed)) {
      intermediate_report = "Managed by Adaptive Quality Suite";
    } else if (g_thin_geometry_intermediate_scatter.load(
            std::memory_order_relaxed)) {
      if (g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
          static_cast<unsigned int>(
              mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
        intermediate_report = "Superseded by Boundary mitigation";
      } else if (thin_geometry_last.intermediate_scatter.applied) {
        intermediate_report = "Active";
      } else {
        intermediate_report = "Requested; not applied";
      }
    }
    const auto adaptive_profile =
        mfgunlock::adaptivequality::NormalizeProfile(
            g_adaptive_quality_profile.load(std::memory_order_relaxed));
    const auto expected_warp =
        mfgunlock::adaptivequality::ExpectedComponentVersion(
            adaptive_profile, mfgunlock::adaptivequality::Component::kWarp);
    const auto expected_geometry =
        adaptive_profile ==
                    mfgunlock::adaptivequality::Profile::
                        kLuminanceDirectionalV3 &&
                !g_adaptive_quality_v3_oriented_geometry.load(
                    std::memory_order_relaxed)
            ? mfgunlock::adaptivequality::ComponentVersion::kV2
            : mfgunlock::adaptivequality::ExpectedComponentVersion(
                  adaptive_profile,
                  mfgunlock::adaptivequality::Component::kGeometry);
    const auto requested_inpaint_mode =
        mfgunlock::cudatemporal::NormalizeInpaintMode(
            g_adaptive_quality_v3_inpaint_mode.load(
                std::memory_order_relaxed));
    const auto expected_inpaint =
        adaptive_profile ==
                mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
            requested_inpaint_mode !=
                mfgunlock::cudatemporal::InpaintMode::kV2Compatibility
            ? mfgunlock::adaptivequality::ComponentVersion::kV3
            : mfgunlock::adaptivequality::ExpectedComponentVersion(
                  adaptive_profile,
                  mfgunlock::adaptivequality::Component::kInpaint);
    const std::string adaptive_warp_report = AdaptiveComponentStatus(
        expected_warp,
        thin_geometry_last.result.validated_warp_blend.adaptive_version,
        thin_geometry_last.result.validated_warp_blend.applied);
    const std::string adaptive_geometry_report = AdaptiveComponentStatus(
        expected_geometry, blackwell_active_result.adaptive_geometry_version,
        blackwell_active_result.adaptive_geometry);
    const std::string adaptive_inpaint_report = AdaptiveComponentStatus(
        expected_inpaint, blackwell_active_result.adaptive_inpaint_version,
        blackwell_active_result.adaptive_inpaint_decision);
    const auto source_cap_state =
        mfgunlock::framecount::internal::ResolveUserSourceCapState();
    const auto source_cap_origin =
        static_cast<mfgunlock::pacing::SourceCapConfigOrigin>(
            g_source_cap_config_origin.load(std::memory_order_relaxed));
    std::ostringstream report;
    report << "MFG Unlock Diagnostics\n"
           << "Renderer: " << RenderApiName(render_api) << '\n'
           << "Streamline: " << streamline_text << '\n'
           << "DLSS-G: " << dlssg_text << '\n'
           << "DLSS Super Resolution: " << super_resolution_preset << '\n'
           << "DLSS Frame Generation: " << frame_generation_preset << '\n'
           << "DLSS Ray Reconstruction: " << ray_reconstruction_preset
           << '\n'
           << "HDR: " << (hdr_seen ? (hdr_active ? "Yes" : "No") : "Unknown")
           << '\n'
           << "Game request: "
           << (game_request_seen ? std::to_string(game_multiplier) + "x"
                                 : "Not observed")
           << '\n'
           << "Effective request: "
           << (effective_seen ? std::to_string(effective_multiplier) + "x"
                              : "Not observed")
           << '\n'
           << "Driver live multiplier: "
           << (driver_multiplier >= 2
                   ? std::to_string(driver_multiplier) + "x"
                   : "Not reported")
           << '\n'
           << "Provider presentations: "
           << (state_seen ? std::to_string(observed_presentations)
                          : "Not sampled")
           << '\n'
           << "Dynamic MFG: "
           << (mfgunlock::framecount::g_dynamic_applied.load(
                   std::memory_order_relaxed)
                   ? "Active"
                   : (dynamic_available ? "Available" : "Unavailable/Pending"))
           << '\n'
           << "Dynamic target FPS: "
           << mfgunlock::framecount::g_dynamic_target_fps.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Output FPS cap (Reflex): "
           << source_cap_state.configured_fps << " final/output FPS; state "
           << UserSourceCapStatusText(source_cap_state.status)
           << "; estimated multiplier "
           << source_cap_state.estimated_multiplier
           << "x; requested/native/effective interval "
           << source_cap_state.requested_limit_us << '/'
           << source_cap_state.native_limit_us << '/'
           << source_cap_state.effective_limit_us << " us; config "
           << SourceCapConfigOriginText(source_cap_origin)
           << '\n'
           << "Latency Guard mode: "
           << mfgunlock::framecount::g_latency_guard_mode.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Timestamp units (0 unknown / 1 us / 2 QPC): "
           << mfgunlock::framecount::g_latency_guard_units.load(std::memory_order_relaxed) << '\n'
           << "Fresh completed frame count: "
           << mfgunlock::framecount::g_latency_guard_new_frames.load(std::memory_order_relaxed) << '\n'
           << "Adaptive Quality Suite: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? mfgunlock::adaptivequality::ProfileName(adaptive_profile)
                   : "Off") << '\n'
           << "V3 photometric A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_photometric.load(
                          std::memory_order_relaxed)
                          ? "relative luma/chroma"
                          : "disabled; V2 absolute RGB")
                   : "inactive outside V3") << '\n'
           << "V3 border A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_directional_border.load(
                          std::memory_order_relaxed)
                          ? "motion-directional"
                          : "disabled; V2 symmetric")
                   : "inactive outside V3") << '\n'
           << "V3 geometry A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_oriented_geometry.load(
                          std::memory_order_relaxed)
                          ? "motion-oriented diagonals"
                          : "disabled; V2 geometry")
                   : "inactive outside V3") << '\n'
           << "Adaptive warp confidence: "
           << adaptive_warp_report << '\n'
           << "Adaptive geometry confidence: "
           << adaptive_geometry_report << '\n'
           << "Geometry V3 install: "
           << (blackwell_active_result.adaptive_geometry_redirected
                   ? "Full ptxas cubin; exact fatbin descriptor redirect"
                   : "No oversized redirect active") << '\n'
           << "Geometry V3.2 cubin: "
           << mfgunlock::blackwell::AdaptiveGeometryVariantName(
                  blackwell_active_result.adaptive_geometry_variant)
           << (blackwell_active_result.adaptive_geometry_variant ==
                       mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal
                   ? "; 40 registers; 7,776 B shared; zero stack/spill/local; .text=41,216 B; 8 global loads (2 u8) + 2 u8 stores"
                   : blackwell_active_result.adaptive_geometry_variant ==
                             mfgunlock::blackwell::AdaptiveGeometryVariant::kLocal
                         ? "; 40 registers; 7,776 B shared; zero stack/spill/local; .text="
#if defined(MFGUNLOCK_LOCAL_STABILITY)
                           "39,680 B; paired diagonal consensus; zero global loads/stores"
#else
                           "39,552 B; zero global loads/stores"
#endif
                         : "; resource audit unavailable for fallback/native")
           << '\n'
           << "Adaptive inpaint decision: "
           << adaptive_inpaint_report << '\n'
           << "Inpaint V3.4 requested: "
           << (requested_inpaint_mode ==
                       mfgunlock::cudatemporal::InpaintMode::kTemporal
                   ? "Temporal V3"
                   : requested_inpaint_mode ==
                             mfgunlock::cudatemporal::InpaintMode::kLocal
                         ? "Local V3"
                         : "V2 Compatibility")
           << '\n'
           << "Inpaint V3.4 cubin: "
           << mfgunlock::blackwell::AdaptiveInpaintVariantName(
                  blackwell_active_result.adaptive_inpaint_variant)
           << (blackwell_active_result.adaptive_inpaint_variant ==
                       mfgunlock::blackwell::AdaptiveInpaintVariant::kTemporal
                   ? "; ABI 152 bytes; 48 registers; 784 B shared; zero stack/spill/local; .text=10,368 B; one u8 history load + one u8 store"
                   : blackwell_active_result.adaptive_inpaint_variant ==
                             mfgunlock::blackwell::AdaptiveInpaintVariant::kLocal
                         ? "; 48 registers; 784 B shared; zero stack/spill/local; .text=9,344 B; zero global history access"
                         : "; compatibility/fallback resources")
           << '\n'
           << "Inpaint V3.4 effective: "
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
           << "V3 inpaint removed; V2 Compatibility requested (see installed cubin above)" << '\n'
           << "Confidence history: 0 bytes; disabled by local low-overhead build" << '\n'
#else
           << mfgunlock::cudatemporal::InpaintDetail() << '\n'
           << "Inpaint V3.4 history: "
           << mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                  std::memory_order_relaxed)
           << " bytes; combined used="
           << (mfgunlock::cudatemporal::g_history_bytes.load(
                   std::memory_order_relaxed) +
               mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                   std::memory_order_relaxed))
           << "/" << mfgunlock::cudatemporal::kArenaLimit
           << "; resets="
           << mfgunlock::cudatemporal::g_history_resets.load(
                  std::memory_order_relaxed)
           << '\n'
#endif
           << "Directional scatter: "
           << (blackwell_active_result.adaptive_directional_scatter
                   ? "Provider-native signed coverage retained"
                   : "Baseline") << '\n'
           << "V3.2 Stability requested: "
           << (mfgunlock::cudatemporal::NormalizeMode(
                       g_adaptive_quality_v3_stability_mode.load(
                           std::memory_order_relaxed)) ==
                       mfgunlock::cudatemporal::StabilityMode::kTemporal
                   ? "Temporal Stable"
                   : "Local Stable") << '\n'
           << "V3.2 Stability effective: "
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
           << "History disabled; dedicated Local cubin requested (see installed variant above)" << '\n'
#if defined(MFGUNLOCK_RELEASE_1_4_3)
           << "Build: Release 1.4.3; continuous border axes; paired diagonal consensus; V2 inpaint" << '\n'
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
           << "Build: 1.4.2-base; continuous-axis border candidate; in-game validation pending" << '\n'
#elif defined(MFGUNLOCK_LOCAL_STABILITY)
           << "Build: Release 1.4.2; continuous border transition; paired diagonal consensus; V2 inpaint" << '\n'
#else
           << "Build: Local low-overhead; no V3 inpaint or confidence history" << '\n'
#endif
           << "Temporal backend: Disabled by build; no self-test, allocations or barriers" << '\n'
           << "CUDA hook fast path: Not installed; no CUDA/NVAPI launch interception" << '\n'
#else
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? mfgunlock::cudatemporal::Detail()
                   : "Off") << '\n'
           << "V3.2 CUDA probe: launches="
           << mfgunlock::cudatemporal::g_probe_launches.load(
                  std::memory_order_relaxed)
           << ", phase_mask=0x" << std::hex
           << mfgunlock::cudatemporal::g_probe_phase_mask.load(
                  std::memory_order_relaxed)
           << std::dec << ", history_bytes="
           << mfgunlock::cudatemporal::g_history_bytes.load(
                  std::memory_order_relaxed) << '\n'
           << "V3.2 CUDA hook fast path: "
           << (mfgunlock::cudatemporal::FastPathReady()
                   ? "active; one immutable dispatch-table atomic load for non-target kernels"
                   : "inactive/target association pending")
           << '\n'
#endif
           << "Quality refinement requested this session: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Legacy setting ignored; managed by Adaptive Quality"
                   : (g_quality_refinement.load(std::memory_order_relaxed)
                          ? "Yes (check patch details)"
                          : "No")) << '\n'
           << "Geometry confidence V2: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Integrated asymmetric successor managed by Adaptive Quality"
                   : (g_geometry_confidence_v2.load(std::memory_order_relaxed)
                   ? (thin_geometry_last.geometry_confidence_v2_applied
                          ? "Patch applied; execution unverified"
                          : "Requested; V1/fallback or no patch")
                   : "Off")) << '\n'
           << "Geometry patch detail: "
           << thin_geometry_last.silhouette_boundary_guard.detail << '\n'
           << "Border confidence trial: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Integrated and managed by Adaptive Quality"
                   : (g_border_confidence.load(std::memory_order_relaxed)
                   ? (!g_quality_refinement.load(std::memory_order_relaxed)
                          ? "Requires Quality Refinement"
                          : (thin_geometry_last.result.validated_warp_blend.applied
                                 ? "Patch applied; execution unverified"
                                 : "Requested; blend not applied"))
                   : "Off")) << '\n'
           << "Reflex marker health: "
           << MarkerHealthText(static_cast<mfgunlock::pacing::MarkerHealth>(
                  mfgunlock::framecount::g_latency_guard_marker_health.load(
                      std::memory_order_relaxed)))
           << '\n'
           << "Source timing validation: "
           << TimingValidationText(
                  mfgunlock::framecount::g_latency_guard_timing_issue_mask.load(
                      std::memory_order_relaxed))
           << "; queue timing "
           << (mfgunlock::framecount::g_latency_guard_queue_timing_confident.load(
                   std::memory_order_relaxed) ? "verified" : "unverified")
           << '\n'
           << "Estimated rendered/source FPS: "
           << mfgunlock::framecount::g_latency_guard_estimated_source_fps.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Estimated output FPS (not measured): "
           << mfgunlock::framecount::g_latency_guard_projected_output_fps.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Simulation marker FPS: "
           << mfgunlock::framecount::g_latency_guard_simulation_fps.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Median render-queue wait (us): "
           << mfgunlock::framecount::g_latency_guard_queue_wait_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "P95 render-queue wait (us): "
           << mfgunlock::framecount::g_latency_guard_queue_p95_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Estimated marker-to-GPU pipeline median/p95 (us; not end-to-end): "
           << mfgunlock::framecount::g_latency_guard_pipeline_latency_us.load(
                  std::memory_order_relaxed)
           << '/'
           << mfgunlock::framecount::g_latency_guard_pipeline_p95_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Median input-to-GPU latency (us): "
           << mfgunlock::framecount::g_latency_guard_input_to_gpu_end_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Median GPU active work (us): "
           << mfgunlock::framecount::g_latency_guard_gpu_active_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Dominant observed latency stage: "
           << LatencyBottleneckText(
                  static_cast<mfgunlock::pacing::LatencyBottleneck>(
                      mfgunlock::framecount::g_latency_guard_bottleneck.load(
                          std::memory_order_relaxed)))
           << '\n'
           << "Independent flip: "
           << (!mfgunlock::framecount::g_latency_guard_iflip_known.load(
                       std::memory_order_relaxed)
                   ? "Not reported"
                   : (mfgunlock::framecount::g_latency_guard_iflip_active.load(
                              std::memory_order_relaxed)
                          ? "Active" : "Inactive"))
           << '\n'
           << "Latency monitor sample cost (us): "
           << mfgunlock::framecount::g_latency_guard_sample_cost_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Automatic multiplier trial: "
           << mfgunlock::framecount::g_latency_guard_multiplier_override.load(
                  std::memory_order_relaxed)
           << "x (0 = inactive; saved selection unchanged)\n"
           << "Responsive multiplier phase: "
           << MultiplierTrialPhaseText(
                  static_cast<mfgunlock::latency::MultiplierTrialPhase>(
                      mfgunlock::framecount::g_latency_guard_multiplier_trial_phase.load(
                          std::memory_order_relaxed)))
           << '\n'
           << "Responsive trial reason: "
           << ResponsiveTrialReasonText(
                  static_cast<mfgunlock::latency::ResponsiveTrialReason>(
                      mfgunlock::framecount::g_latency_guard_multiplier_trial_reason.load(
                          std::memory_order_relaxed)))
           << '\n'
           << "Responsive trial eligibility: "
           << ResponsiveTrialBlockerText(
                  static_cast<mfgunlock::latency::ResponsiveTrialBlocker>(
                      mfgunlock::framecount::g_latency_guard_trial_blocker.load(
                          std::memory_order_relaxed)))
           << '\n'
           << "Responsive multiplier saved/approved/candidate: "
           << mfgunlock::framecount::g_force_multiplier.load(
                  std::memory_order_relaxed)
           << 'x' << '/'
           << mfgunlock::framecount::g_latency_guard_multiplier_approved.load(
                  std::memory_order_relaxed)
           << 'x' << '/'
           << mfgunlock::framecount::g_latency_guard_multiplier_candidate.load(
                  std::memory_order_relaxed)
           << "x\n"
           << "Responsive baseline/trial samples: "
           << mfgunlock::framecount::g_latency_guard_multiplier_baseline_samples.load(
                  std::memory_order_relaxed)
           << "/8; "
           << mfgunlock::framecount::g_latency_guard_multiplier_trial_samples.load(
                  std::memory_order_relaxed)
           << "/8\n"
           << "Responsive baseline pipeline median/p95 (us): "
           << mfgunlock::framecount::g_latency_guard_multiplier_baseline_pipeline_us.load(
                  std::memory_order_relaxed)
           << '/'
           << mfgunlock::framecount::g_latency_guard_multiplier_baseline_p95_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Responsive latest trial pipeline median/p95 (us): "
           << mfgunlock::framecount::g_latency_guard_multiplier_trial_pipeline_us.load(
                  std::memory_order_relaxed)
           << '/'
           << mfgunlock::framecount::g_latency_guard_multiplier_trial_p95_us.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Automatic queue-trim cap: "
           << mfgunlock::framecount::g_latency_guard_active_source_cap_fps.load(
                  std::memory_order_relaxed)
           << '\n'
           << "DXGI frame-latency policy: "
           << (render_api == DetectedRenderApi::kVulkan
                   ? "unavailable on Vulkan"
                   : (g_latency_guard_dxgi_observed.load(std::memory_order_acquire)
                   ? (g_latency_guard_waitable_swapchain.load(
                              std::memory_order_relaxed)
                          ? "waitable"
                          : "No waitable object observed")
                   : "not observed"))
           << '\n'
           << "Process local VRAM usage/budget: "
           << (render_api == DetectedRenderApi::kVulkan
                   ? std::string("DXGI budget unavailable on Vulkan")
                   : std::to_string(
                         g_vram_local_usage.load(std::memory_order_relaxed)) +
                         "/" +
                         std::to_string(
                             g_vram_local_budget.load(
                                 std::memory_order_relaxed)) +
                         " bytes")
           << '\n'
           << "DLSS-G estimated VRAM: "
           << mfgunlock::framecount::g_vram_estimate_bytes.load(
                  std::memory_order_relaxed)
           << " bytes; status "
           << mfgunlock::framecount::g_vram_estimate_status.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Potentially volatile DLSS-G inputs: "
           << mfgunlock::framecount::g_vram_volatile_input_count.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Input quality mode: "
           << mfgunlock::framecount::g_hdr_compatibility_mode.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Quality issue mask: 0x" << std::hex
           << mfgunlock::framecount::g_quality_issue_mask.load(
                  std::memory_order_relaxed)
           << std::dec << '\n'
           << "Quality Guard viewports: "
           << mfgunlock::framecount::g_quality_viewport_count.load(
                  std::memory_order_relaxed)
           << '/' << mfgunlock::framecount::kMaxQualityViewports
           << (mfgunlock::framecount::g_quality_viewport_capacity_exhausted.load(
                       std::memory_order_acquire)
                   ? " (capacity exhausted)" : "")
           << '\n'
           << "Quality Guard tag-lock contention fallbacks: "
           << mfgunlock::framecount::g_quality_tag_lock_contentions.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Validation evidence: " << ValidationStageName(validation_stage)
           << " -- " << validation_text << '\n'
           << "Architecture gates: " << (gate_patched ? "Active" : "Pending")
           << " (providers " << gate_provider_count << ", sites "
           << gate_site_count << ")\n"
           << "Blackwell kernels: " << (blackwell_patched ? "Active" : "Inactive")
           << "\nBlackwell targeted roles: motion-vector "
           << (blackwell_active_result.motion_vector ? "yes" : "no")
           << ", inpaint "
           << (blackwell_active_result.inpaint ? "yes" : "no")
           << ", decision "
           << (blackwell_active_result.inpaint_decision ? "yes" : "no")
           << "; patched slots " << blackwell_active_result.kernels
           << "\nTemporal fallback: " << (midpoint_patched ? "Active" : "Inactive")
           << "\nThin-geometry patches: "
           << (thin_geometry_patched ? "Active" : "Inactive/Pending") << '\n'
           << "Intermediate retention: " << intermediate_report
           << '\n'
           << "Boundary mitigation: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "balanced (selected by Adaptive Quality Suite)"
                   : mfgunlock::blackwell::SilhouetteGuardName(
                         static_cast<mfgunlock::blackwell::SilhouetteGuardMode>(
                             g_silhouette_guard_mode.load(
                                 std::memory_order_relaxed))))
           << '\n'
           << "Validated warp blend: "
           << ((g_adaptive_quality.load(std::memory_order_relaxed) ||
                g_thin_geometry_validated_warp_blend.load(
                    std::memory_order_relaxed))
                   ? "Enabled"
                   : "Disabled")
           << " (" << thin_geometry_last.result.validated_warp_blend.detail
           << ")"
           << '\n'
           << "Previous scatter retention: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Disabled by Adaptive Quality Suite; saved preference preserved"
                   : (g_thin_geometry_previous_scatter.load(
                          std::memory_order_relaxed)
                          ? "Enabled"
                          : "Disabled"))
           << '\n';
    if (ImGui::Button("Copy diagnostics")) {
      ImGui::SetClipboardText(report.str().c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("No local paths or credentials are included.");


    if (blackwell_patched) {
      std::ostringstream framework_status;
      framework_status << "Blackwell framework kernels are active on "
                       << blackwell_provider_count << " provider(s).";
      ImGui::TextWrapped("%s", framework_status.str().c_str());
      std::ostringstream patched_roles;
      patched_roles << "Patched roles: motion-vector "
                    << (blackwell_active_result.motion_vector ? "yes" : "no")
                    << ", inpaint "
                    << (blackwell_active_result.inpaint ? "yes" : "no")
                    << ", decision "
                    << (blackwell_active_result.inpaint_decision ? "yes" : "no")
                    << " (" << blackwell_active_result.kernels << " slots).";
      TextDisabledWrapped(patched_roles.str().c_str());
      if (!blackwell_applied_detail.empty()) {
        const std::string applied =
            "Applied provider result: " + blackwell_applied_detail;
        TextDisabledWrapped(applied.c_str());
      }
      if (!blackwell_detail.empty() &&
          blackwell_detail != blackwell_applied_detail) {
        const std::string rejected =
            "Separate rejected candidate (does not replace the result above): " +
            blackwell_detail;
        TextDisabledWrapped(rejected.c_str());
      }
    } else if (!blackwell_detail.empty()) {
      ImGui::TextWrapped("Blackwell: %s", blackwell_detail.c_str());
    }
    if (!midpoint_detail.empty())
      ImGui::TextWrapped("Temporal: %s", midpoint_detail.c_str());
    ImGui::TextDisabled(
        "Providers: gates %zu, Blackwell %zu, temporal %zu, quality %zu.",
        gate_provider_count, blackwell_provider_count,
        midpoint_provider_count, thin_geometry_provider_count);
    ImGui::TextDisabled("Load trigger: %s; caught %u provider load(s).",
                        mfgunlock::loadhook::g_hooked.load(
                            std::memory_order_acquire)
                            ? "armed"
                            : "not installed",
                        mfgunlock::loadhook::g_catches.load(
                            std::memory_order_relaxed));

  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextDisabled(
      "Changes marked for restart keep the current session unchanged.");
}

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
#if defined(MFGUNLOCK_RELEASE_1_4_3)
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
#if defined(MFGUNLOCK_RELEASE_1_4_3)
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
