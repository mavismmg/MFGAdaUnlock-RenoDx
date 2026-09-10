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
#include <cwchar>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>

#include <nvsdk_ngx.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "./blackwell.hpp"
#include "./framecount.hpp"
#include "./loadhook.hpp"
#include "./midpoint.hpp"
#include "./ngx_hook.hpp"
#include "./pacing_policy.hpp"

namespace {

constexpr const char* kConfigSection = "RenoDX.MFGUnlock";
constexpr const char* kParamName = "DLSSG.MultiFrameCountMax";

// Slot 11 of NVSDK_NGX_Parameter == Get(const char*, unsigned int*).
constexpr size_t kGetUInt32Slot = 11;

constexpr unsigned int kMinCount = 2;
constexpr unsigned int kMaxCount = 5;

std::atomic_bool g_enabled{true};
std::atomic_bool g_configured_enabled{true};
std::atomic<unsigned int> g_max_count{4};
std::atomic_bool g_force_flip_meter_off{false};
std::atomic_bool g_configured_force_flip_meter_off{false};
std::atomic_bool g_temporal_fix{true};
std::atomic_bool g_configured_temporal_fix{true};
std::atomic_bool g_blackwell_framework_kernels{true};
std::atomic_bool g_configured_blackwell_framework_kernels{true};
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
HMODULE g_self_module = nullptr;

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
std::atomic<int> g_midpoint_attempts{0};

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
  if (!mfgunlock::blackwell::Apply(mod, patches, allocations, result, detail)) {
    g_blackwell_detail = detail;
    return false;
  }

  g_blackwell_detail = detail;
  g_blackwell_modules.push_back(
      {mod, std::move(patches), std::move(allocations), result});
  g_blackwell_patched.store(true, std::memory_order_release);
  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream stream;
  stream << "mfgunlock: full Blackwell framework kernels applied to " << module_path << " -- "
         << detail << "; the separate midpoint rewrite is not needed for this provider.";
  reshade::log::message(reshade::log::level::info, stream.str().c_str());
  return true;
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
  g_midpoint_patched.store(false, std::memory_order_release);
  g_blackwell_patched.store(false, std::memory_order_release);
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
        mfgunlock::framecount::NotifyDynamicD3D12(
            detected == DetectedRenderApi::kD3D12);
      }
      mfgunlock::framecount::NotifySwapchainTransition();
    }
  }
  if (swapchain != nullptr &&
      swapchain == g_primary_swapchain.load(std::memory_order_acquire)) {
    const auto color_space = swapchain->get_color_space();
    if (color_space != reshade::api::color_space::unknown) {
      const bool hdr = color_space == reshade::api::color_space::scrgb ||
                       color_space == reshade::api::color_space::hdr10_pq ||
                       color_space == reshade::api::color_space::hdr10_hlg;
      mfgunlock::framecount::NotifyHdrState(hdr);
    }
  }
  StartDiscoveryWorker();
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
        mfgunlock::framecount::NotifyDynamicD3D12(
            detected == DetectedRenderApi::kD3D12);
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
  if (primary_transition) mfgunlock::framecount::NotifySwapchainTransition();
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  reshade::api::swapchain* expected = swapchain;
  if (g_primary_swapchain.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel)) {
    g_primary_swapchain_area.store(0, std::memory_order_relaxed);
    g_render_api.store(DetectedRenderApi::kUnknown, std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicD3D12(false);
    mfgunlock::framecount::NotifySwapchainTransition();
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
          detected == DetectedRenderApi::kD3D12);
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
}

// ---------------------------------------------------------------- overlay

void OnRegisterOverlay(reshade::api::effect_runtime* /*runtime*/) {
  bool gate_patched = false;
  bool midpoint_patched = false;
  bool blackwell_patched = false;
  size_t gate_provider_count = 0;
  size_t gate_site_count = 0;
  size_t midpoint_provider_count = 0;
  size_t blackwell_provider_count = 0;
  std::string midpoint_detail;
  std::string blackwell_detail;
  bool ceiling_patched = false;
  unsigned int ceiling_compiled = 0;
  unsigned int ceiling_effective = 0;
  size_t flip_meter_site_count = 0;
  AcquireSRWLockShared(&g_provider_maintenance_lock);
  gate_patched = g_gate_patched.load(std::memory_order_relaxed);
  midpoint_patched = g_midpoint_patched.load(std::memory_order_relaxed);
  blackwell_patched = g_blackwell_patched.load(std::memory_order_relaxed);
  gate_provider_count = g_gate_modules.size();
  gate_site_count = g_gate_sites.size();
  midpoint_provider_count = g_midpoint_modules.size();
  blackwell_provider_count = g_blackwell_modules.size();
  midpoint_detail = g_midpoint_detail;
  blackwell_detail = g_blackwell_detail;
  ReleaseSRWLockShared(&g_provider_maintenance_lock);
  AcquireSRWLockShared(&g_streamline_maintenance_lock);
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
    ImGui::Text("Actual presentations since last state query: %u (samples: %llu).",
                mfgunlock::framecount::g_actual_frames_presented.load(std::memory_order_relaxed),
                mfgunlock::framecount::g_state_samples.load(std::memory_order_relaxed));
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
      "Native - pass the game's tags unchanged",
      "UI Composition - force when inputs are known-good",
      "Automatic Guard + UI Composition (Recommended)",
      "Automatic Guard - conservative final-color fallback"};
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
    ImGui::TextDisabled("Passes the game's optional HUD-less and UI tags through unchanged.");
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
        "Uses final color in HDR because Streamline tags cannot prove color-space compatibility.\n"
        "In SDR, matched HUD-less/UI inputs may use UI Composition; any concrete\n"
        "mismatch falls back safely and synchronizes temporal history once.");
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
        "More than eight Streamline viewports were seen; extra viewports use conservative fallback.");
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
  bool reflex_source_cap =
      mfgunlock::framecount::g_dynamic_reflex_source_cap.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Advanced: cap application-rendered FPS with Reflex",
                      &reflex_source_cap)) {
    mfgunlock::framecount::g_dynamic_reflex_source_cap.store(
        reflex_source_cap, std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "DynamicReflexSourceCap",
                              reflex_source_cap ? 1 : 0);
  }
  ImGui::TextDisabled(
      "This is a source-frame limiter, not a final-output target. Leave it off unless\n"
      "you intentionally calculated a rendered-FPS cap for your multiplier/refresh setup.");
  if (dynamic_mfg && dynamic_target != 0 && reflex_source_cap) {
    if (mfgunlock::framecount::g_reflex_limit_applied.load(
            std::memory_order_acquire)) {
      const unsigned int effective_us =
          mfgunlock::framecount::g_reflex_effective_limit_us.load(
              std::memory_order_relaxed);
      ImGui::Text("Reflex source-frame cap active: %u us (~%u rendered FPS); game requested %u us.",
                  effective_us,
                  effective_us == 0 ? 0u : (1000000u + effective_us / 2u) / effective_us,
                  mfgunlock::framecount::g_reflex_native_limit_us.load(
                      std::memory_order_relaxed));
    } else if (!mfgunlock::framecount::g_reflex_hooked.load(
                   std::memory_order_acquire)) {
      ImGui::TextDisabled(
          "The game has not exposed slReflexSetOptions; the advanced source cap is unavailable.");
    } else if (!mfgunlock::framecount::g_reflex_options_seen.load(
                   std::memory_order_acquire)) {
      ImGui::TextDisabled(
          "Reflex source-cap hook is ready; waiting for the game's Reflex options.");
    } else {
      ImGui::TextDisabled("Reflex source-frame cap pending (last result: %u).",
                          mfgunlock::framecount::g_reflex_limit_result.load(
                              std::memory_order_relaxed));
    }
  }

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
    reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", force);
  }
  ImGui::TextDisabled(
      "Leave off for games with their own 2x/3x/4x selector -- forcing would\n"
      "override your in-game choice. Dynamic MFG takes priority when active.");
  if (mfgunlock::framecount::g_intercepted.load(std::memory_order_relaxed)) {
    ImGui::Text("Game asked for %ux, forced to %u generated frame(s).",
                mfgunlock::framecount::g_last_requested.load(std::memory_order_relaxed) + 1,
                mfgunlock::framecount::g_last_forced.load(std::memory_order_relaxed));
  } else if (mfgunlock::framecount::g_declined_no_pacing.load(std::memory_order_relaxed)) {
    ImGui::TextWrapped("Declined to force: flip metering was still on when DLSS-G started.");
  } else if (mfgunlock::framecount::g_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("slDLSSGSetOptions hook installed; not exercised yet.");
  } else {
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
  if (blackwell_patched) {
    ImGui::TextWrapped("Blackwell framework kernels: %zu provider(s); last result: %s.",
                       blackwell_provider_count, blackwell_detail.c_str());
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

void LoadConfig() {
  int value = 0;
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
  }
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
    mfgunlock::framecount::g_dynamic_reflex_source_cap.store(
        value != 0, std::memory_order_relaxed);
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
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG Unlock";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Reports DLSSG.MultiFrameCountMax so Streamline offers multi-frame generation";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      g_self_module = h_module;
      if (!reshade::register_addon(h_module)) return FALSE;
      LoadConfig();
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
      break;
    case DLL_PROCESS_DETACH:
      // During process termination Windows is already reclaiming every mapped
      // image. Avoid detour transactions and stale provider-memory restores
      // under the loader lock; explicit addon unload still performs cleanup.
      if (lpv_reserved != nullptr) break;
      reshade::unregister_event<reshade::addon_event::present>(OnPresentStartDiscovery);
      reshade::unregister_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_overlay("MFG Unlock", OnRegisterOverlay);
      mfgunlock::loadhook::Uninstall();
      mfgunlock::framecount::Uninstall();
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
