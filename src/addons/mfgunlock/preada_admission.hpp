// SPDX-License-Identifier: MIT
#pragma once
// 1.8.3: XMX on RTX 20 and 30. XMX replaces only NVIDIA's generator, so the
// game must still see DLSS Frame Generation as available and NVIDIA's runtime
// must create the feature. On these GPUs three answers say no:
//
//   - the provider declares Ada as its minimum: NVSDK_NGX_GetGPUArchitecture
//     is `mov eax, 0x190; ret` in every 310.x copy, and the NGX core then
//     reports FrameGeneration.Available=0, so Streamline never creates it;
//   - NGX's GetFeatureRequirements for frame generation reports the adapter
//     unsupported;
//   - Streamline's slIsFeatureSupported for DLSS-G returns 6 (no supported
//     adapter) or 7, as an RTX 3060 Ti log showed.
//
// Adapted from nefh's MFGAmpereUnlock-RenoDx (MIT, a fork of this project):
// the scoped architecture answer, the requirements and Streamline admission,
// the provider minimum and (1.8.7) the provider's kernels for this GPU.
// Everything here acts only while xmx::preada::Admitted(): every NVIDIA
// adapter predates Ada (1.9.0: with XMX or NVIDIA's own generator). Nothing
// changes on RTX 40 or 50.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <nvsdk_ngx.h>
#include <sl.h>

#include <include/reshade.hpp>

#include "./ampere/midpoint.hpp"
#include "./ampere/provider.hpp"
#include "./gpu_arch.hpp"
#include "./module_session.hpp"
#include "./ngx_hook.hpp"
#include "./patch_transaction.hpp"
#include "./xmx_generator.hpp"
#include "./xmx_preada.hpp"

namespace mfgunlock::xmx::preada {
constexpr uint32_t kAdaArchitecture = 0x190;
// Turing: the lowest architecture this path accepts, so RTX 20 and 30 pass.
constexpr uint32_t kLoweredMinimum = 0x160;

// XMX selected, its files present, and this GPU older than Ada.
inline bool Engaged() {
  return Active() && generator::Enabled() && generator::Preflight();
}
// 1.9.0: NVIDIA's own generator on RTX 20/30 too. The admission, the provider
// minimum, the multi-frame gate and the kernel preparation act on every
// pre-Ada GPU, with XMX or with DLSS-G; [DLSSG] PreAdaAdmission=0 turns them
// off. Only XMX's own pieces (its generator, its failure) still read Engaged().
inline bool AdmissionDisabled() {
  return GetPrivateProfileIntW(L"DLSSG", L"PreAdaAdmission", 1,
                               (backend::Directory() / L"MFGAdaUnlock.ini").c_str()) == 0;
}
inline bool Admitted() {
  static const bool value = Active() && !AdmissionDisabled();
  return value;
}
inline void Log(const std::string& text, bool warning = false) {
  reshade::log::message(warning ? reshade::log::level::warning : reshade::log::level::info,
                        ("mfgunlock: pre-Ada " + text).c_str());
}

// ------------------------------------------------- the architecture answer
// NVAPI's GetArchInfo answers Ada, but only on a thread inside one of the
// admission queries below and only for an architecture older than Ada.
struct ArchInfo {
  uint32_t version, architecture, implementation, revision;
};
using GetArchInfoFn = int(__cdecl*)(void* gpu, ArchInfo* info);
inline GetArchInfoFn g_real_arch_info = nullptr;
inline std::atomic_bool g_arch_hooked{false};
inline std::once_flag g_arch_once;
inline thread_local bool g_in_admission = false;
struct AdmissionScope {
  bool previous = g_in_admission;
  AdmissionScope() { g_in_admission = true; }
  ~AdmissionScope() { g_in_admission = previous; }
};
inline int __cdecl HookedArchInfo(void* gpu, ArchInfo* info) {
  const int result = g_real_arch_info(gpu, info);
  if (g_in_admission && result == 0 && info != nullptr && info->architecture >= kLoweredMinimum &&
      info->architecture < kAdaArchitecture)
    info->architecture = kAdaArchitecture;
  return result;
}
// Never from the loader callback: it may load nvapi64.dll.
inline void EnsureArchHook() {
  std::call_once(g_arch_once, [] {
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr) nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using QueryFn = void*(__cdecl*)(uint32_t);
    const auto query = nvapi ? reinterpret_cast<QueryFn>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
    void* target = query ? query(0xd8265d24) : nullptr;  // NvAPI_GPU_GetArchInfo
    if (target == nullptr) {
      Log("NVAPI architecture query not found; admission relies on the other answers", true);
      return;
    }
    g_real_arch_info = reinterpret_cast<GetArchInfoFn>(target);
    if (hook::InstallOne(reinterpret_cast<void**>(&g_real_arch_info), reinterpret_cast<void*>(&HookedArchInfo)))
      g_arch_hooked.store(true, std::memory_order_release);
    else
      Log("NVAPI architecture hook failed", true);
  });
}

// --------------------------------------------------------------- Streamline
using SupportFn = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
inline SupportFn g_real_support = nullptr;
inline std::atomic_bool g_support_hooked{false};
inline std::atomic_bool g_support_relaxed{false};

// Only an NVIDIA adapter is admitted. Vulkan queries carry no LUID.
inline bool IsNvidiaAdapter(const sl::AdapterInfo& adapter) {
  if (adapter.deviceLUID == nullptr || adapter.deviceLUIDSizeInBytes != sizeof(LUID))
    return adapter.vkPhysicalDevice != nullptr;
  LUID luid{};
  std::memcpy(&luid, adapter.deviceLUID, sizeof(luid));
  // No new static import: the game has dxgi.dll mapped by the time it asks.
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  const auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> found;
  if (create == nullptr || FAILED(create(IID_PPV_ARGS(&factory))) ||
      FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&found))))
    return false;
  DXGI_ADAPTER_DESC1 desc{};
  return SUCCEEDED(found->GetDesc1(&desc)) && desc.VendorId == 0x10DE;
}

inline sl::Result HookedIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter) {
  if (g_real_support == nullptr) return sl::Result::eErrorNotInitialized;
  const sl::Result native = g_real_support(feature, adapter);
  if (feature != sl::kFeatureDLSS_G || !Admitted() ||
      (native != sl::Result::eErrorNoSupportedAdapterFound && native != sl::Result::eErrorAdapterNotSupported))
    return native;
  // Streamline asks NVAPI for the architecture itself: ask again inside the scope.
  EnsureArchHook();
  sl::Result result = native;
  {
    AdmissionScope scope;
    result = g_real_support(feature, adapter);
  }
  if ((result == sl::Result::eErrorNoSupportedAdapterFound || result == sl::Result::eErrorAdapterNotSupported) &&
      IsNvidiaAdapter(adapter))
    result = sl::Result::eOk;
  if (result == sl::Result::eOk && !g_support_relaxed.exchange(true))
    Log("Streamline DLSS-G admission: " + std::to_string(static_cast<int>(native)) + " -> ok");
  return result;
}

inline const std::vector<hook::HookItem> kStreamlineHooks = {
    {"slIsFeatureSupported", reinterpret_cast<void**>(&g_real_support),
     reinterpret_cast<void*>(&HookedIsFeatureSupported)},
};
// Installed when sl.interposer.dll maps; slIsFeatureSupported comes long before
// the first Present. Removed on an explicit unload.
inline void TryInstallStreamline() {
  if (g_support_hooked.load(std::memory_order_acquire) || !Admitted()) return;
  static SRWLOCK lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&lock);
  if (!g_support_hooked.load(std::memory_order_relaxed)) {
    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    if (interposer != nullptr && GetProcAddress(interposer, "slIsFeatureSupported") != nullptr &&
        hook::Install(interposer, kStreamlineHooks, "sl.interposer.dll"))
      g_support_hooked.store(true, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&lock);
}

// ---------------------------------------------------- NGX feature requirements
using RequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                      NVSDK_NGX_FeatureRequirement*);
inline RequirementsFn g_real_requirements = nullptr;
inline std::atomic_bool g_requirements_hooked{false};
inline std::atomic_bool g_requirements_relaxed{false};

inline NVSDK_NGX_Result NVSDK_CONV HookedRequirements(IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* info,
                                                       NVSDK_NGX_FeatureRequirement* out) {
  if (g_real_requirements == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (info == nullptr || info->FeatureID != NVSDK_NGX_Feature_FrameGeneration || !Admitted())
    return g_real_requirements(adapter, info, out);
  EnsureArchHook();
  NVSDK_NGX_Result result;
  {
    AdmissionScope scope;
    result = g_real_requirements(adapter, info, out);
  }
  // FeatureSupported is a bit mask. Only the adapter bit is cleared; a driver
  // or OS refusal stays and keeps the feature unsupported.
  if (!NVSDK_NGX_FAILED(result) && out != nullptr) {
    const auto bits = static_cast<uint32_t>(out->FeatureSupported);
    constexpr auto kAdapter = static_cast<uint32_t>(NVSDK_NGX_FeatureSupportResult_AdapterUnsupported);
    if ((bits & kAdapter) != 0) {
      out->FeatureSupported = static_cast<NVSDK_NGX_Feature_Support_Result>(bits & ~kAdapter);
      out->MinHWArchitecture = kLoweredMinimum;
      if (!g_requirements_relaxed.exchange(true)) {
        Log("NGX frame generation requirements: adapter admitted" +
            ((bits & ~kAdapter) != 0 ? " (other refusal bits remain: " + std::to_string(bits & ~kAdapter) + ")"
                                     : std::string()));
      }
    }
  }
  return result;
}

// ------------------------------------------------------------------ slInit
// Streamline loads its plugins inside slInit and drops sl.dlss_g there when
// NVAPI or NGX say the GPU predates Ada. framecount's slInit hook calls these
// around the whole call, so every architecture query on that thread answers
// Ada. The hook exists only if the addon loaded before the game called
// slInit (ReShade's LoadFromDllMain); the panel warns when it did not.
inline std::atomic_bool g_init_observed{false};
inline thread_local bool g_init_previous = false;
inline bool InitHookWanted() { return Admitted(); }
inline void InitScope(bool begin) {
  if (!Admitted()) return;
  if (begin) {
    EnsureArchHook();
    g_init_previous = g_in_admission;
    g_in_admission = true;
    if (!g_init_observed.exchange(true)) Log("slInit runs inside the admission scope");
  } else {
    g_in_admission = g_init_previous;
  }
}

inline const std::vector<hook::HookItem> kRequirementHooks = {
    {"NVSDK_NGX_D3D12_GetFeatureRequirements", reinterpret_cast<void**>(&g_real_requirements),
     reinterpret_cast<void*>(&HookedRequirements)},
};
inline void TryInstallRequirements() {
  if (g_requirements_hooked.load(std::memory_order_acquire) || !Admitted()) return;
  static SRWLOCK lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&lock);
  for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    if (g_requirements_hooked.load(std::memory_order_relaxed)) break;
    HMODULE ngx = GetModuleHandleW(name);
    if (ngx == nullptr || GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetFeatureRequirements") == nullptr) continue;
    if (hook::Install(ngx, kRequirementHooks, "NGX core"))
      g_requirements_hooked.store(true, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&lock);
}

// ------------------------------------- NVIDIA's kernels for this GPU (nefh)
// 1.8.7: on RTX 20/30 NVIDIA's CreateFeature failed with PlatformError: the
// provider carries CUDA code for Ada and Blackwell only, and creating the
// feature loads it even though XMX never runs it. nefh's preparation (vendored
// in ampere/, MIT) rewrites the provider's sm_89 PTX for this GPU (sm_86 or
// sm_75) and hides the sm_89 cubins, so the driver compiles kernels this GPU
// can load. Only while Admitted(), and only on one Turing or Ampere family.
inline bool RetargetConfigured() {
  static const bool configured = [] {
    if (!Admitted()) return false;
    auto family = gpuarch::NvidiaPreAdaFamily();
    // A simulated pre-Ada run on Ada takes the Ampere path: sm_86 code runs there.
    if (Simulated()) family = gpuarch::PreAdaFamily::kAmpere;
    switch (family) {
      case gpuarch::PreAdaFamily::kAmpere:
        ampere::architecture::Configure(ampere::Architecture::kAmpere);
        return true;
      case gpuarch::PreAdaFamily::kTuring:
        ampere::architecture::Configure(ampere::Architecture::kTuring);
        return true;
      default:
        Log("NVIDIA's kernels are not prepared: the NVIDIA GPUs are not all one Turing or Ampere family", true);
        return false;
    }
  }();
  return configured;
}

// The provider's own quality transforms replace the same sm_89 cubins and PTX
// descriptors the preparation rewrites, so they are skipped on these GPUs;
// nefh's temporal correction below takes the midpoint fix's place.
inline bool SkipQualityPatches() {
  if (!RetargetConfigured()) return false;
  static std::atomic_bool logged{false};
  if (!logged.exchange(true)) Log("NVIDIA's kernels are prepared for this GPU; the provider quality patches are not applied");
  return true;
}

// 1.9.0: the slowest preparation of this session, for the panel. On Turing
// it recompresses the provider's kernels (about 3 s per file on the
// development PC), during the game's start.
inline std::atomic<uint64_t> g_slowest_prepare_ms{0};
constexpr uint64_t kSlowPrepareMs = 1000;
inline void RetargetProvider(HMODULE module) {
  if (module == nullptr || !RetargetConfigured()) return;
  const auto* session = modulesession::Acquire(module);
  if (session == nullptr) return;
  const auto start = GetTickCount64();
  if (!ampere::provider::PrepareProvider(module)) return;  // nefh logs why
  // Code bytes it changed (the Turing endpoint and load sites) belong to the
  // addon, like our own gate writes, so the provider's identity still holds.
  std::vector<patching::Write> code;
  AcquireSRWLockShared(&ampere::provider::internal::g_lock);
  for (const auto& provider : ampere::provider::internal::g_providers) {
    if (provider.module != module) continue;
    for (const auto& patch : provider.patches) {
      MEMORY_BASIC_INFORMATION memory{};
      if (!VirtualQuery(patch.address, &memory, sizeof(memory))) continue;
      const DWORD protection = memory.Protect & 0xFF;
      if (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
          protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY)
        code.push_back({patch.address, {patch.before}, {patch.after}, true});
    }
  }
  ReleaseSRWLockShared(&ampere::provider::internal::g_lock);
  if (!code.empty()) modulesession::RecordCode(*session, code);
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  const uint64_t elapsed = GetTickCount64() - start;
  uint64_t slowest = g_slowest_prepare_ms.load(std::memory_order_relaxed);
  while (elapsed > slowest && !g_slowest_prepare_ms.compare_exchange_weak(slowest, elapsed)) {}
  Log("NVIDIA's kernels prepared for this GPU in " + std::filesystem::path(path).filename().string() + " (" +
          std::to_string(elapsed) + " ms" +
          (elapsed >= kSlowPrepareMs ? "; this slows the game's start on RTX 20 and is expected" : "") + ")",
      elapsed >= kSlowPrepareMs);
}

// ------------------------------------------ the temporal correction (nefh)
// 1.9.0: without it, 3x and above put every generated frame at the midpoint
// between the two real frames (the Ada midpoint problem), so 4x looked worse
// than 2x in a simulated RTX 30 test. The addon's own midpoint fix edits sm_89
// PTX and is skipped here; nefh's version (vendored in ampere/midpoint.hpp)
// edits the PTX the preparation just retargeted for this GPU, then redirects
// the kernel's descriptors to the rebuilt container. Descriptor slots are data,
// not code, so the provider's identity is unaffected.
struct TemporalModule {
  HMODULE module = nullptr;
  std::vector<ampere::midpoint::Patch> patches;
  void* allocation = nullptr;
};
inline SRWLOCK g_temporal_lock = SRWLOCK_INIT;
inline std::vector<TemporalModule> g_temporal_modules;
inline std::vector<HMODULE> g_temporal_rejected;
inline std::atomic<uint32_t> g_temporal_applied{0};
inline std::atomic<uint32_t> g_temporal_missing{0};

inline void ApplyTemporalCorrection(HMODULE module) {
  if (module == nullptr || !RetargetConfigured()) return;
  if (ampere::provider::g_create_seen.load(std::memory_order_acquire)) return;
  AcquireSRWLockExclusive(&g_temporal_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_temporal_lock); }
  } unlock;
  for (const auto& done : g_temporal_modules)
    if (done.module == module) return;
  for (HMODULE rejected : g_temporal_rejected)
    if (rejected == module) return;
  TemporalModule entry{module};
  std::string detail;
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  const std::string name = std::filesystem::path(path).filename().string();
  if (!ampere::midpoint::Apply(module, entry.patches, entry.allocation, detail)) {
    g_temporal_rejected.push_back(module);
    g_temporal_missing.fetch_add(1, std::memory_order_relaxed);
    Log("temporal correction not applied to " + name + " (" + detail +
            "); above 2x, generated frames from this file land at the midpoint",
        true);
    return;
  }
  g_temporal_modules.push_back(std::move(entry));
  g_temporal_applied.fetch_add(1, std::memory_order_relaxed);
  Log("temporal correction applied to " + name + ": " + detail);
}

// ---------------------------------------------------- the provider minimum
// Rewrites `mov eax, 0x190` to Turing in each provider, before the NGX core
// asks (the loader hook sees the provider before its loader continues). The
// write counts as the addon's own code, so the provider's identity holds.
inline SRWLOCK g_minimum_lock = SRWLOCK_INIT;
inline std::vector<patching::Write> g_minimum_writes;
inline void LowerProviderMinimum(HMODULE module) {
  if (module == nullptr || !Admitted()) return;
  auto* entry = reinterpret_cast<uint8_t*>(GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture"));
  if (entry == nullptr || !patching::Readable(entry, 6) || entry[0] != 0xB8 || entry[5] != 0xC3) return;
  uint32_t declared = 0;
  std::memcpy(&declared, entry + 1, sizeof(declared));
  if (declared != kAdaArchitecture) return;  // already lowered, or not an Ada minimum
  const auto* session = modulesession::Acquire(module);
  if (session == nullptr) return;
  const uint32_t lowered = kLoweredMinimum;
  std::vector<patching::Write> writes{patching::Bytes(entry + 1, &lowered, sizeof(lowered))};
  if (!patching::Apply(writes, true)) {
    Log("could not lower the provider's minimum architecture", true);
    return;
  }
  modulesession::RecordCode(*session, writes);
  AcquireSRWLockExclusive(&g_minimum_lock);
  g_minimum_writes.insert(g_minimum_writes.end(), writes.begin(), writes.end());
  ReleaseSRWLockExclusive(&g_minimum_lock);
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  Log("provider minimum architecture 0x190 -> 0x160 in " + std::filesystem::path(path).filename().string());
}

// The multi-frame gate (cmp against 0x1b0, Blackwell) is rewritten to Turing
// instead of Ada, so the provider advertises 3x-6x to this GPU as it does to
// RTX 40. Called on the gate writes before they are applied.
inline void LowerGateWrites(std::vector<patching::Write>& writes) {
  if (!Admitted()) return;
  for (auto& write : writes)
    if (write.original.size() == 1 && write.original[0] == 0xB0 && write.replacement.size() == 1 &&
        write.replacement[0] == 0x90)
      write.replacement[0] = static_cast<uint8_t>(kLoweredMinimum & 0xFF);
}

// --------------------------------------------------- what the panel reports
// NGX CreateFeature for frame generation, seen by the NGX preset hook.
inline std::atomic<int64_t> g_create_result{-1};  // -1: not seen yet
inline void ObserveCreate(NVSDK_NGX_Result result) {
  const auto previous = g_create_result.exchange(static_cast<int64_t>(static_cast<uint32_t>(result)));
  if (previous == static_cast<int64_t>(static_cast<uint32_t>(result))) return;
  if (NVSDK_NGX_FAILED(result)) {
    std::ostringstream text;
    text << "NVIDIA's frame generation could not be created on this GPU (0x" << std::hex
         << static_cast<uint32_t>(result) << ")";
    if (Engaged()) generator::Fail(text.str());
    Log(text.str(), true);
  } else {
    Log("NVIDIA's frame generation feature was created on this GPU");
  }
}

// 1.10.0 (plan 1.3): when the game first turned frame generation on, so the
// panel and the log can say that NVIDIA's runtime never created the feature
// afterwards (OW2 and Keeper on an RTX 3060 Ti: SetOptions accepted, no
// CreateFeature, zero frames).
inline std::atomic<uint64_t> g_first_on_tick{0};
inline void ObserveFrameGenerationOn() {
  uint64_t expected = 0;
  g_first_on_tick.compare_exchange_strong(expected, GetTickCount64());
}
constexpr uint64_t kCreateGraceMs = 5000;
inline bool CreateMissing(uint64_t now) {
  const uint64_t on = g_first_on_tick.load(std::memory_order_acquire);
  return Admitted() && on != 0 && now - on >= kCreateGraceMs && g_create_result.load() < 0;
}

// Explicit unload only: these hooks go in before the first Present.
inline void Uninstall() {
  if (g_support_hooked.load() && hook::Uninstall(kStreamlineHooks)) g_support_hooked.store(false);
  if (g_requirements_hooked.load() && hook::Uninstall(kRequirementHooks)) g_requirements_hooked.store(false);
  if (g_arch_hooked.load() &&
      hook::Uninstall({{"NvAPI_GPU_GetArchInfo", reinterpret_cast<void**>(&g_real_arch_info),
                        reinterpret_cast<void*>(&HookedArchInfo)}}))
    g_arch_hooked.store(false);
  AcquireSRWLockExclusive(&g_minimum_lock);
  patching::Restore(g_minimum_writes, true);
  ReleaseSRWLockExclusive(&g_minimum_lock);
  // Only before NVIDIA's runtime created the feature: afterwards its kernels
  // may still reference the rebuilt containers (nefh's rule: leak, never dangle).
  if (!ampere::provider::g_create_seen.load(std::memory_order_acquire)) {
    AcquireSRWLockExclusive(&g_temporal_lock);
    for (auto& entry : g_temporal_modules) ampere::midpoint::Restore(entry.patches, entry.allocation);
    std::erase_if(g_temporal_modules, [](const auto& e) { return e.patches.empty() && e.allocation == nullptr; });
    ReleaseSRWLockExclusive(&g_temporal_lock);
  }
}
}  // namespace mfgunlock::xmx::preada
