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
 * Blackwell paces multi-frame output with hardware flip metering that Ada does
 * not have; left enabled, 3x+ freezes the presented image while audio keeps
 * running. Streamline already ships the software fallback, so the addon only
 * has to force the plugin down it -- see TryPatchFlipMeteringInModule, which
 * derives the field's offset AND its polarity at runtime because both move
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

#include "./framecount.hpp"
#include "./loadhook.hpp"
#include "./midpoint.hpp"
#include "./ngx_hook.hpp"

namespace {

constexpr const char* kConfigSection = "RenoDX.MFGUnlock";
constexpr const char* kParamName = "DLSSG.MultiFrameCountMax";

// Slot 11 of NVSDK_NGX_Parameter == Get(const char*, unsigned int*).
constexpr size_t kGetUInt32Slot = 11;

constexpr unsigned int kMinCount = 2;
constexpr unsigned int kMaxCount = 5;

std::atomic_bool g_enabled{true};
std::atomic<unsigned int> g_max_count{4};
std::atomic_bool g_force_flip_meter_off{false};
std::atomic_bool g_temporal_fix{true};
// Raising the plugin's own clamp broke GTA V Enhanced -- its 2.9.1.0 plugin was
// only ever shipped bounded at 3, and lifting that is not the same as it being
// able to cope. Off by default; updating the plugin is the sound fix.
std::atomic_bool g_raise_ceiling{false};

enum class DetectedRenderApi : unsigned int {
  kUnknown,
  kD3D11,
  kD3D12,
  kVulkan,
  kOther,
};

std::atomic<DetectedRenderApi> g_render_api{DetectedRenderApi::kUnknown};

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
HMODULE g_self_module = nullptr;

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
  wchar_t module_path[32768] = {};
  const DWORD length = GetModuleFileNameW(mod, module_path, ARRAYSIZE(module_path));
  if (length == 0 || length >= ARRAYSIZE(module_path)) return false;
  for (DWORD i = 0; i < length; ++i) {
    if (module_path[i] >= L'A' && module_path[i] <= L'Z') {
      module_path[i] = static_cast<wchar_t>(module_path[i] - L'A' + L'a');
    }
  }
  return std::wcsstr(module_path, L"nvngx_dlssg") != nullptr ||
         std::wcsstr(module_path, L"\\models\\dlssg\\") != nullptr;
}

bool IsDlssgProvider(HMODULE mod) {
  // A renamed provider may expose either graphics backend. Streamline's
  // slDLSSGSetOptions/slDLSSGGetState interface is renderer-independent, so
  // discovery must not discard Vulkan snippets before the shared patch path
  // gets a chance to inspect them.
  const bool has_d3d12_entry =
      GetProcAddress(mod, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") != nullptr;
  const bool has_vulkan_entry =
      GetProcAddress(mod, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") != nullptr;

  // These paths are unambiguous and include NVIDIA's opaque OTA .bin, which
  // does not carry the older "dlfg_kernel" marker in current driver builds.
  if (HasKnownDlssgPath(mod)) return true;

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
  unsigned char* address;  // the byte holding the arch id's low octet
  unsigned char original;
};

std::atomic_bool g_gate_patched{false};
std::vector<GateSite> g_gate_sites;
std::vector<HMODULE> g_gate_modules;
std::vector<HMODULE> g_gate_rejected_modules;
int g_gate_attempts = 0;

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
    g_gate_sites.push_back({site, *site});
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
struct MidpointModulePatch {
  HMODULE module;
  std::vector<mfgunlock::midpoint::Patch> patches;
  void* allocation;
};
std::vector<MidpointModulePatch> g_midpoint_modules;
std::vector<HMODULE> g_midpoint_rejected_modules;
std::string g_midpoint_detail;
int g_midpoint_attempts = 0;

void PatchMidpointInModule(HMODULE mod) {
  if (mod == nullptr) return;
  const auto already_patched = std::find_if(
      g_midpoint_modules.begin(), g_midpoint_modules.end(),
      [mod](const MidpointModulePatch& patch) { return patch.module == mod; });
  if (already_patched != g_midpoint_modules.end()) return;
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

void TryPatchMidpoint() {
  ++g_midpoint_attempts;
  for (HMODULE mod : g_dlssg_modules) PatchMidpointInModule(mod);
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
  if (g_temporal_fix.load(std::memory_order_relaxed)) PatchMidpointInModule(mod);
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
}

void RunProviderMaintenance() {
  AcquireSRWLockExclusive(&g_provider_maintenance_lock);
  DiscoverDlssgModules();
  if (g_enabled.load(std::memory_order_relaxed)) TryPatchDlssgArchGate();
  if (g_temporal_fix.load(std::memory_order_relaxed)) TryPatchMidpoint();
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
}

void RestoreMidpoint() {
  if (!g_midpoint_patched.load(std::memory_order_acquire)) return;
  for (auto& module : g_midpoint_modules) {
    mfgunlock::midpoint::Restore(module.patches, module.allocation);
  }
  g_midpoint_modules.clear();
  g_midpoint_rejected_modules.clear();
  g_midpoint_patched.store(false, std::memory_order_release);
}

// ------------------------------------------------- flip metering (sl.dlss_g)
//
// With the gate open, 2x works but 3x/4x freeze the display while audio keeps
// running -- frames are generated and never reach the screen. Blackwell paces
// multi-frame output with hardware flip metering; Ada has none, so the present
// queue waits on something that never happens.
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
int g_flip_meter_attempts = 0;
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
unsigned char* g_ceiling_site = nullptr;
unsigned char g_ceiling_original = 0;
unsigned char g_ceiling_cmov_original = 0;
unsigned int g_ceiling_compiled = 0;
unsigned int g_ceiling_effective = 0;

void PatchFrameCountCeiling(HMODULE mod) {
  if (g_ceiling_patched.load(std::memory_order_acquire)) return;

  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(mod, &base, &nt)) return;

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
    std::stringstream s;
    s << "mfgunlock: found " << hits
      << " frame-count clamps in the DLSS-G plugin (expected 1); leaving them alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    return;
  }

  DWORD old_protect = 0;
  if (VirtualProtect(found, 10, PAGE_EXECUTE_READWRITE, &old_protect) == 0) return;
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
  if (VirtualProtect(g_ceiling_site, 10, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
    g_ceiling_site[9] = g_ceiling_cmov_original;
    g_ceiling_site[1] = g_ceiling_original;
    DWORD ignored = 0;
    VirtualProtect(g_ceiling_site, 10, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_ceiling_site, 10);
  }
  g_ceiling_site = nullptr;
  g_ceiling_original = 0;
  g_ceiling_cmov_original = 0;
  g_ceiling_compiled = 0;
  g_ceiling_effective = 0;
  mfgunlock::framecount::g_advertised_max_generated.store(0, std::memory_order_release);
  g_ceiling_patched.store(false, std::memory_order_release);
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

void TryPatchFlipMetering() {
  if (g_flip_meter_patched.load(std::memory_order_acquire)) return;
  if (g_flip_meter_attempts >= kMaxFlipMeterAttempts) return;

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

void RestoreFlipMetering() {
  if (!g_flip_meter_patched.load(std::memory_order_acquire)) return;
  for (const auto& site : g_flip_meter_sites) {
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
      (!g_temporal_fix.load(std::memory_order_relaxed) ||
       g_midpoint_patched.load(std::memory_order_acquire));
  const bool pacing_ready =
      !g_force_flip_meter_off.load(std::memory_order_relaxed) ||
      g_flip_meter_patched.load(std::memory_order_acquire) ||
      g_flip_meter_attempts >= kMaxFlipMeterAttempts;
  return provider_ready && pacing_ready &&
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
                             reshade::api::swapchain* /*swapchain*/,
                             const reshade::api::rect* /*source_rect*/,
                             const reshade::api::rect* /*dest_rect*/,
                             uint32_t /*dirty_rect_count*/,
                             const reshade::api::rect* /*dirty_rects*/) {
  StartDiscoveryWorker();
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

    const DetectedRenderApi previous =
        g_render_api.exchange(detected, std::memory_order_relaxed);
    if (previous != detected) {
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
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
}

void OnInitCommandQueue(reshade::api::command_queue* /*queue*/) {
  RunProviderMaintenance();
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
}

// ---------------------------------------------------------------- overlay

void OnRegisterOverlay(reshade::api::effect_runtime* /*runtime*/) {
  bool enabled = g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Enable multi-frame override", &enabled)) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "Enabled", enabled ? 1 : 0);
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

  bool flip_off = g_force_flip_meter_off.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Force legacy software flip pacing (compatibility)", &flip_off)) {
    g_force_flip_meter_off.store(flip_off, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", flip_off ? 1 : 0);
  }
  ImGui::TextDisabled(
      "Leave off with current Streamline builds. Enable only if 3x/4x freezes;\n"
      "applied once per session, so restart the game after changing it.");

  ImGui::Separator();
  if (g_flip_meter_patched.load(std::memory_order_acquire)) {
    ImGui::Text("Flip-metering forced off: +0x%x pinned to %u, %zu site(s).",
                g_flip_meter_offset.load(std::memory_order_relaxed),
                g_flip_meter_value.load(std::memory_order_relaxed),
                g_flip_meter_sites.size());
  } else if (g_flip_meter_attempts >= kMaxFlipMeterAttempts) {
    // The counter only advances once the plugin HAS been found, and both give-up
    // paths slam it to the maximum -- so this state is "found it, could not patch
    // it", which is the opposite of what this line used to say.
    ImGui::TextDisabled("DLSS-G plugin found, but flip metering could not be patched.");
    ImGui::TextDisabled("See the ReShade log for the module and the step that failed.");
  } else if (g_flip_meter_attempts > 0) {
    ImGui::TextDisabled("DLSS-G plugin found; flip-metering patch pending (%d).",
                        g_flip_meter_attempts);
  } else {
    ImGui::TextDisabled("DLSS-G plugin not located yet -- turn frame generation on.");
  }
  if (g_ceiling_patched.load(std::memory_order_acquire)) {
    ImGui::Text("Streamline device-limit bypassed: compiled %ux, effective %ux.",
                g_ceiling_compiled + 1, g_ceiling_effective + 1);
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
      "override your in-game choice. Use it where frame gen is only on/off.");
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
  bool temporal = g_temporal_fix.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Temporal fix (stops all frames landing at the midpoint)", &temporal)) {
    g_temporal_fix.store(temporal, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "TemporalFix", temporal ? 1 : 0);
  }
  ImGui::TextDisabled("Applied once at load; restart the game to change it.");

  ImGui::Separator();
  if (g_midpoint_patched.load(std::memory_order_acquire)) {
    ImGui::TextWrapped("Temporal fix: %zu provider(s); last result: %s.",
                       g_midpoint_modules.size(), g_midpoint_detail.c_str());
  } else {
    ImGui::TextDisabled("Temporal fix not applied yet (attempt %d).", g_midpoint_attempts);
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
  if (g_gate_patched.load(std::memory_order_acquire)) {
    ImGui::Text("DLSS-G arch gates rewritten: %zu provider(s), %zu site(s).",
                g_gate_modules.size(), g_gate_sites.size());
  } else {
    ImGui::TextDisabled("DLSS-G snippet not located yet (attempt %d) -- enable frame generation.",
                        g_gate_attempts);
  }

  ImGui::Separator();
  ImGui::TextDisabled("Legacy NGX parameter-vtable override disabled; using verified code gates.");
}

void LoadConfig() {
  int value = 0;
  if (reshade::get_config_value(nullptr, kConfigSection, "Enabled", value)) {
    g_enabled.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "MaxCount", value)) {
    if (value < static_cast<int>(kMinCount)) value = static_cast<int>(kMinCount);
    if (value > static_cast<int>(kMaxCount)) value = static_cast<int>(kMaxCount);
    g_max_count.store(static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", value)) {
    g_force_flip_meter_off.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "TemporalFix", value)) {
    g_temporal_fix.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "RaiseFrameCeiling", value)) {
    g_raise_ceiling.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceOTAPlugins", value)) {
    mfgunlock::framecount::g_force_ota.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceMultiplier", value)) {
    if (value != 0 && (value < 2 || value > 6)) value = 0;
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(value),
                                                    std::memory_order_relaxed);
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG Unlock";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Reports DLSSG.MultiFrameCountMax so Streamline offers multi-frame generation";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID /*lpv_reserved*/) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      g_self_module = h_module;
      if (!reshade::register_addon(h_module)) return FALSE;
      LoadConfig();

      // The frame-count override must never ask for more generated frames than
      // the pacing can deliver, and it is the only code that runs at a moment
      // when the DLSS-G plugin is guaranteed loaded. Give it both a way to
      // force the pacing patch and a way to check whether it took.
      mfgunlock::framecount::g_ensure_pacing = []() { TryPatchFlipMetering(); };
      mfgunlock::framecount::g_pacing_ready = []() {
        return g_flip_meter_patched.load(std::memory_order_acquire);
      };

      // Install load-time discovery before the bootstrap scan. Already mapped
      // providers are found by that one shared scan; anything mapped later is
      // patched directly here without enumerating modules from Present.
      mfgunlock::loadhook::g_on_interposer_loaded = []() { mfgunlock::framecount::TryInstall(); };
      mfgunlock::loadhook::g_on_dlssg_loaded = ProcessLoadedDlssgModule;
      mfgunlock::loadhook::TryInstall();
      RunProviderMaintenance();
      if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
      mfgunlock::framecount::TryInstall();

      reshade::register_overlay("MFG Unlock", OnRegisterOverlay);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::register_event<reshade::addon_event::present>(OnPresentStartDiscovery);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresentStartDiscovery);
      reshade::unregister_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
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
