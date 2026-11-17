// SPDX-License-Identifier: MIT
// DLSS-G provider patches (plan 6.2): the NGX parameter entries, the in-memory
// architecture gates of nvngx_dlssg, locating the snippet, the temporal
// (midpoint) fix, the thin-geometry and Blackwell kernels, and the provider
// maintenance that applies and restores them.
//
// A fragment of addon.cpp, included inside its anonymous namespace: it uses
// the configuration and state declared there and is not a standalone header.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "addon_provider.inl is included only by addon.cpp"
#endif

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
// 1.6 (plan B.3): games that create DLSS through NGX themselves, without
// Streamline, get the selected preset as a creation hint. NGX reads it only
// when the feature is created: at startup and when the game changes
// resolution or quality. Streamline games are left to the slDLSSSetOptions
// wrapper. The game's own hints are kept per parameter block and restored
// when "Game default" is selected again.
using NgxCreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                         NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
NgxCreateFeatureFn g_real_create_feature = nullptr;
constexpr const char* kNgxPresetHints[] = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
struct GamePresetHints {
  NVSDK_NGX_Parameter* parameters;
  unsigned int values[std::size(kNgxPresetHints)];
};
SRWLOCK g_game_preset_hints_lock = SRWLOCK_INIT;
std::vector<GamePresetHints> g_game_preset_hints;

// 1.10.0 (XMX video memory): NVIDIA's frame generation feature allocates
// ~406 MiB at 1440p (48 full- and half-resolution intermediates, weights and
// scratch; vram_trace.hpp) when DLSS-G creates it, and XMX never evaluates it.
// With XMX ready, NGX's CreateFeature hands back XMX's own handle instead, and
// NGX's Evaluate and Release for that handle go straight to XMX: on the
// Streamline Sample, XMX went from 2242 to 1818 MiB, as little as NVIDIA's own
// frame generation (Streamline 2.7.32 to 2.14.16). [DLSSG] XmxSaveVram=0
// creates NVIDIA's feature as before 1.10.0.
bool XmxSaveVramWanted() {
  static const bool wanted =
      GetPrivateProfileIntW(L"DLSSG", L"XmxSaveVram", 1,
                            (mfgunlock::backend::Directory() / L"MFGAdaUnlock.ini").c_str()) != 0;
  return wanted;
}
bool XmxSkipsNvidiaFeature() {
  // D3D12 only: on Vulkan the XMX provider owns the feature, and the D3D12 core hooks
  // retried every 250 ms in DOOM: The Dark Ages ("failed to install hooks in _nvngx.dll").
  if (g_render_api.load(std::memory_order_relaxed) == DetectedRenderApi::kVulkan) return false;
  return XmxSaveVramWanted() && mfgunlock::xmx::generator::Enabled() && mfgunlock::xmx::generator::Preflight();
}
// NVSDK_NGX_Handle is opaque in these headers; the runtime's own is { unsigned Id; }.
alignas(8) unsigned int g_xmx_handle_storage[2]{0x584D5801u, 0};  // "XMX" 1
NVSDK_NGX_Handle* XmxHandle() { return reinterpret_cast<NVSDK_NGX_Handle*>(&g_xmx_handle_storage); }
std::atomic<int> g_xmx_handles_live{0}, g_xmx_handles_created{0};
using NgxEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                    const NVSDK_NGX_Parameter*, void*);
using NgxReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
// _nvngx.dll exports no EvaluateFeature_C (driver 6xx), so only these two.
NgxEvaluateFn g_real_core_evaluate = nullptr;
NgxReleaseFn g_real_core_release = nullptr;
NVSDK_NGX_Result NVSDK_CONV HookedCoreEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                               const NVSDK_NGX_Parameter* params, void* callback) {
  if (handle == XmxHandle()) return mfgunlock::xmx::EvaluateWithoutNvidia(list, params);
  if (g_real_core_evaluate == nullptr) return NVSDK_NGX_Result_Fail;
  const NVSDK_NGX_Result result = g_real_core_evaluate(list, handle, params, callback);
  // 1.10.0 (sl_host.hpp): the game's DLSS SR inputs and output, tagged for its hosted Streamline.
  if (!NgxFailed(result)) mfgunlock::slhost::AfterEvaluate(list, handle, params);
  return result;
}
NVSDK_NGX_Result NVSDK_CONV HookedCoreRelease(NVSDK_NGX_Handle* handle) {
  if (handle == XmxHandle()) {
    const int left = g_xmx_handles_live.fetch_sub(1) - 1;
    reshade::log::message(reshade::log::level::info,
                          ("mfgunlock: xmx released its NGX handle (" + std::to_string(left) + " left)").c_str());
    return NVSDK_NGX_Result_Success;
  }
  mfgunlock::slhost::ObserveRelease(handle);
  if (g_real_core_release == nullptr) return NVSDK_NGX_Result_Fail;
  return g_real_core_release(handle);
}
const std::vector<mfgunlock::hook::HookItem> kXmxCoreHooks = {
    {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&g_real_core_evaluate),
     reinterpret_cast<void*>(&HookedCoreEvaluate)},
    {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void**>(&g_real_core_release),
     reinterpret_cast<void*>(&HookedCoreRelease)},
};
std::atomic_bool g_xmx_core_hooks_installed{false};

NVSDK_NGX_Result NVSDK_CONV HookedCreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                                NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  if (feature == NVSDK_NGX_Feature_FrameGeneration && handle != nullptr &&
      g_xmx_core_hooks_installed.load(std::memory_order_acquire) && XmxSkipsNvidiaFeature()) {
    mfgunlock::ampere::provider::g_create_seen.store(true, std::memory_order_release);
    *handle = XmxHandle();
    g_xmx_handles_created.fetch_add(1);
    const int live = g_xmx_handles_live.fetch_add(1) + 1;
    if (mfgunlock::xmx::preada::Admitted()) mfgunlock::xmx::preada::ObserveCreate(NVSDK_NGX_Result_Success);
    reshade::log::message(reshade::log::level::info,
                          ("mfgunlock: xmx created frame generation without NVIDIA's feature (" + std::to_string(live) +
                           " handle(s))").c_str());
    return NVSDK_NGX_Result_Success;
  }
  // 1.8.3, RTX 20/30 (1.9.0: with XMX or NVIDIA's generator): whether
  // NVIDIA's runtime created the feature there, for the panel. Nothing in the
  // call changes.
  if (feature == NVSDK_NGX_Feature_FrameGeneration && g_real_create_feature != nullptr &&
      mfgunlock::xmx::preada::Admitted()) {
    // 1.8.7: CUDA may keep pointers into the prepared containers from here on,
    // so nefh's preparation no longer rewrites a provider after this.
    mfgunlock::ampere::provider::g_create_seen.store(true, std::memory_order_release);
    const NVSDK_NGX_Result result = g_real_create_feature(list, feature, parameters, handle);
    mfgunlock::xmx::preada::ObserveCreate(result);
    return result;
  }
  if (feature == NVSDK_NGX_Feature_SuperSampling && parameters != nullptr &&
      !mfgunlock::dlsspreset::g_wrapped.load(std::memory_order_relaxed)) {
    const uint32_t selected = mfgunlock::dlsspreset::g_selected.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_game_preset_hints_lock);
    auto saved = std::find_if(g_game_preset_hints.begin(), g_game_preset_hints.end(),
                              [&](const auto& s) { return s.parameters == parameters; });
    if (selected != 0) {
      if (saved == g_game_preset_hints.end() && g_game_preset_hints.size() < 8) {
        GamePresetHints game{parameters, {}};
        for (size_t i = 0; i < std::size(kNgxPresetHints); ++i)
          if (NgxFailed(parameters->Get(kNgxPresetHints[i], &game.values[i]))) game.values[i] = 0;
        g_game_preset_hints.push_back(game);
      }
      for (const auto* hint : kNgxPresetHints) parameters->Set(hint, static_cast<unsigned int>(selected));
    } else if (saved != g_game_preset_hints.end()) {
      for (size_t i = 0; i < std::size(kNgxPresetHints); ++i) parameters->Set(kNgxPresetHints[i], saved->values[i]);
      g_game_preset_hints.erase(saved);
    }
    ReleaseSRWLockExclusive(&g_game_preset_hints_lock);
    mfgunlock::dlsspreset::g_ngx_seen.store(true, std::memory_order_relaxed);
    if (mfgunlock::dlsspreset::g_ngx_preset.exchange(selected, std::memory_order_relaxed) != selected) {
      std::stringstream s;
      s << "mfgunlock: DLSS Super Resolution created through NGX with "
        << (selected ? "preset " + std::to_string(selected) : std::string("the game's presets")) << '.';
      reshade::log::message(reshade::log::level::info, s.str().c_str());
    }
  }
  // Reached through a hook this copy did not install (see hook::JumpsIntoSelf).
  if (g_real_create_feature == nullptr) return NVSDK_NGX_Result_Fail;
  const NVSDK_NGX_Result result = g_real_create_feature(list, feature, parameters, handle);
  if (feature == NVSDK_NGX_Feature_SuperSampling && !NgxFailed(result) && handle != nullptr)
    mfgunlock::slhost::ObserveCreate(parameters, *handle);
  return result;
}

// Installed on its own, only while a preset is selected (or, since 1.8.3, XMX
// runs on an RTX 20/30), so a session that never uses the selector gains no
// NGX hook.
const std::vector<mfgunlock::hook::HookItem> kNgxPresetHooks = {
    {"NVSDK_NGX_D3D12_CreateFeature",
     reinterpret_cast<void**>(&g_real_create_feature),
     reinterpret_cast<void*>(&HookedCreateFeature)},
};
std::atomic_bool g_preset_hook_installed{false};

bool EnsureResident();

bool PresetHookWanted() {
  return mfgunlock::dlsspreset::g_selected.load(std::memory_order_relaxed) != 0 ||
         mfgunlock::xmx::preada::Admitted() || XmxSkipsNvidiaFeature() || mfgunlock::slhost::Wanted();
}

void TryInstallPresetHook() {
  if (!PresetHookWanted() || g_preset_hook_installed.load(std::memory_order_acquire)) return;
  // 1.8.3: not from ReShade's temporary startup copy of the addon. Its hook
  // outlived the unload and the next copy recursed through it on the first
  // DLSS creation (TLOU Part II with a saved preset could not start). The
  // Present retry below installs it once the addon is resident.
  if (!EnsureResident()) return;
  static SRWLOCK lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&lock);
  for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    if (g_preset_hook_installed.load(std::memory_order_relaxed)) break;
    HMODULE mod = GetModuleHandleW(name);
    if (mod == nullptr || GetProcAddress(mod, "NVSDK_NGX_D3D12_CreateFeature") == nullptr) continue;
    char narrow[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    // The XMX handle's Evaluate and Release first, so CreateFeature never hands it out unrouted.
    if ((XmxSkipsNvidiaFeature() || mfgunlock::slhost::Wanted()) && !g_xmx_core_hooks_installed.load(std::memory_order_relaxed) &&
        mfgunlock::hook::Install(mod, kXmxCoreHooks, narrow))
      g_xmx_core_hooks_installed.store(true, std::memory_order_release);
    if (mfgunlock::hook::Install(mod, kNgxPresetHooks, narrow))
      g_preset_hook_installed.store(true, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&lock);
}

// Present-time retry, at most every 250 ms, until the hook is installed. Some
// games map _nvngx.dll through an import that bypasses the LoadLibrary hook
// (KCD2 field test); they present their menu long before creating DLSS.
void RetryPresetHookFromPresent() {
  if (g_preset_hook_installed.load(std::memory_order_acquire) || !PresetHookWanted()) return;
  static std::atomic<uint64_t> last{0};
  const uint64_t now = GetTickCount64();
  uint64_t previous = last.load(std::memory_order_relaxed);
  if (now - previous < 250 || !last.compare_exchange_strong(previous, now, std::memory_order_relaxed)) return;
  TryInstallPresetHook();
}

// Explicit unload only. Normally unreachable: the hook goes in after the pin.
void UninstallPresetHook() {
  if (g_preset_hook_installed.load(std::memory_order_acquire) && mfgunlock::hook::Uninstall(kNgxPresetHooks))
    g_preset_hook_installed.store(false, std::memory_order_release);
}

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
  if (g_vtable_patch.patched.load(std::memory_order_acquire)) return;
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
// rewritten. 0x1b0 -> 0x190 lets AD10x take the Blackwell path.

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
  return mfgunlock::modulescan::ImageContains(mod, needle, needle_len);
}

// Modules searched for a marker that do not contain it. A module that does
// contain it is retried, since its session may fail only transiently. Updated
// under g_streamline_maintenance_lock.
mfgunlock::modulescan::SearchedImages g_ceiling_searched, g_flip_searched;

// True when the module has not been searched before and contains the marker.
bool UnsearchedModuleContains(mfgunlock::modulescan::SearchedImages& searched, HMODULE mod,
                              const char* needle, size_t needle_len) {
  mfgunlock::modulescan::ImageIdentity identity;
  if (!mfgunlock::modulescan::ReadIdentity(mod, identity) || searched.Contains(identity)) return false;
  if (ModuleContains(mod, needle, needle_len)) return true;
  searched.Add(identity);
  return false;
}

void RememberDlssgModule(HMODULE mod) {
  if (!mfgunlock::modulesession::Acquire(mod)) return;
  if (mod == nullptr || mod == g_self_module) return;
  if (std::find(g_dlssg_modules.begin(), g_dlssg_modules.end(), mod) == g_dlssg_modules.end()) {
    g_dlssg_modules.push_back(mod);
    // RTX 20/30 only: before the NGX core reads the provider's minimum.
    mfgunlock::xmx::preada::LowerProviderMinimum(mod);
    // 1.8.7: NVIDIA's kernels for this GPU, before the NGX core creates anything;
    // 1.9.0: then the temporal correction on the retargeted kernel.
    mfgunlock::xmx::preada::RetargetProvider(mod);
    mfgunlock::xmx::preada::ApplyTemporalCorrection(mod);
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
struct CodeTransaction { HMODULE module; uint64_t generation; std::vector<mfgunlock::patching::Write> writes; };
std::vector<CodeTransaction> g_gate_transactions, g_flip_transactions;
std::vector<mfgunlock::patching::Write> g_ceiling_writes;
std::vector<HMODULE> g_gate_modules;
std::vector<HMODULE> g_gate_rejected_modules;
std::atomic<int> g_gate_attempts{0};

// 1.7 (plan C): on a machine whose NVIDIA adapters are all RTX 50, DLSS-G
// already takes its Blackwell path and executes sm_120 cubins. The arch-gate
// rewrite is pointless there, and the quality kernels, which replace the
// provider's sm_89 (Ada) cubins, never run. Leave the provider alone and say so
// once, instead of logging "applied" for code the GPU does not execute.
bool NativeBlackwellLeavesProviderAlone() {
  if (!mfgunlock::gpuarch::AllNvidiaAdaptersBlackwell()) return false;
  static std::atomic_bool logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    reshade::log::message(reshade::log::level::info,
        g_rtx50_quality_fixes.load(std::memory_order_relaxed)
            ? "mfgunlock: RTX 50 (Blackwell) GPU: DLSS-G runs its native path; "
              "architecture gates and the Ada (sm_89) kernels are left alone. "
              "Rtx50QualityFixes=1: the validated warp blend is applied to the "
              "provider's sm_120 PTX (experimental)."
            : "mfgunlock: RTX 50 (Blackwell) GPU: DLSS-G runs its native path; "
              "architecture gates are left alone, and the quality fixes, built "
              "for Ada (sm_89) kernels, are not applied because this GPU does "
              "not run those kernels. Rtx50QualityFixes=1 applies the "
              "experimental sm_120 warp blend.");
  }
  return true;
}

// Split out so the load-time trigger can patch a module it already holds a
// handle to. That path runs under the loader lock, where CreateToolhelp32Snapshot
// (which FindDlssgModule uses) would deadlock -- so it must never scan.
void PatchArchGatesInModule(HMODULE mod) {
  if (NativeBlackwellLeavesProviderAlone()) return;
  if (!mod || std::find(g_gate_modules.begin(), g_gate_modules.end(), mod) != g_gate_modules.end() ||
      std::find(g_gate_rejected_modules.begin(), g_gate_rejected_modules.end(), mod) != g_gate_rejected_modules.end()) return;
  const auto* profile = mfgunlock::binarypatch::ProfileFor(mod, 0);
  // 1.8.4: no exact profile; the reviewed two-compare gate, if the provider has
  // exactly that ([DLSSG] GenericGate=0 turns this off).
  mfgunlock::binarypatch::Generic generic_storage;
  static const bool generic_allowed =
      GetPrivateProfileIntW(L"DLSSG", L"GenericGate", 1, (mfgunlock::backend::Directory() / L"MFGAdaUnlock.ini").c_str()) != 0;
  bool generic = false;
  if (!profile && generic_allowed && mfgunlock::binarypatch::GenericArchProfileFor(mod, generic_storage)) {
    profile = &generic_storage.profile;
    generic = true;
  }
  const auto* session = mfgunlock::modulesession::Acquire(mod);
  CodeTransaction transaction{mod, session ? session->generation : 0, {}};
  const bool planned = profile && session && mfgunlock::binarypatch::Plan(mod, *profile, transaction.writes);
  // RTX 20/30 with XMX only; RTX 40/50 keep the Ada threshold written above.
  if (planned) mfgunlock::xmx::preada::LowerGateWrites(transaction.writes);
  const bool applied = planned && mfgunlock::binarypatch::Apply(mod, transaction.writes);
  if (!applied) {
    if (mfgunlock::pointerredirect::Pending(transaction.writes)) g_gate_transactions.push_back(std::move(transaction));
    g_gate_rejected_modules.push_back(mod);
    // 1.9.0: which step refused, and in which file (an RTX 3080 report showed
    // only the generic line for a provider whose gate plans offline).
    const char* step = session == nullptr                           ? "no module session"
                       : !mfgunlock::modulesession::Validate(*session) ? "executable identity changed"
                       : profile == nullptr                          ? "no exact or generic profile for this file"
                       : !planned                                    ? "instruction preflight failed"
                                                                     : "write transaction failed";
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const auto v = mfgunlock::runtimeversion::FromModule(mod);
    std::ostringstream refusal;
    refusal << "mfgunlock: architecture gate refused for " << path;
    if (v.valid) refusal << " (" << v.major << '.' << v.minor << '.' << v.patch << '.' << v.revision << ")";
    refusal << ": " << step << ".";
    reshade::log::message(reshade::log::level::warning, refusal.str().c_str());
    return;
  }
  for (const auto& write : transaction.writes) g_gate_sites.push_back({mod, write.address, write.original[0]});
  g_gate_transactions.push_back(std::move(transaction));
  g_gate_modules.push_back(mod);
  g_gate_patched.store(true, std::memory_order_release);
  std::ostringstream identity_log;
  identity_log << "mfgunlock: architecture gates committed; provider candidate session=" << session->generation
      << "; original executable FNV1a64=0x" << std::hex << session->identity.code_hash
      << "; PE timestamp=0x" << session->identity.timestamp << std::dec
      << "; image bytes=" << session->identity.image_size
      << (generic ? "; generic gate (no exact profile for this provider)" : "")
      << "; candidate identity does not prove active feature selection.";
  reshade::log::message(reshade::log::level::info, identity_log.str().c_str());
}

void TryPatchDlssgArchGate() {
  ++g_gate_attempts;
  for (HMODULE mod : g_dlssg_modules) PatchArchGatesInModule(mod);
}

void RestoreDlssgArchGate() {
  for (auto& transaction : g_gate_transactions) mfgunlock::patching::Restore(transaction.writes, true);
  std::erase_if(g_gate_transactions, [](const auto& t) { return !mfgunlock::pointerredirect::Pending(t.writes); });
  if (!g_gate_transactions.empty()) return;
  g_gate_sites.clear(); g_gate_modules.clear(); g_gate_rejected_modules.clear();
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
  if (mfgunlock::xmx::preada::SkipQualityPatches()) return false;
  if (!mfgunlock::modulesession::Acquire(mod)) return false;
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
    if (!patches.empty() || !allocations.empty())
      g_blackwell_modules.push_back({mod, std::move(patches), std::move(allocations), {}});
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
  if (mfgunlock::xmx::preada::SkipQualityPatches()) return;
  const bool native_blackwell = NativeBlackwellLeavesProviderAlone();
  if (native_blackwell && !g_rtx50_quality_fixes.load(std::memory_order_relaxed))
    return;
  if (!mfgunlock::modulesession::Acquire(mod)) return;
  if (mod == nullptr || !ThinGeometryRequested() ||
      ModuleHasThinGeometryResult(mod)) {
    return;
  }
  ++g_thin_geometry_attempts;

  ThinGeometryModulePatch module_result;
  module_result.module = mod;
  const bool adaptive_quality =
      g_adaptive_quality.load(std::memory_order_relaxed);
  // On RTX 50 only the sm_120 warp blend applies: previous scatter rewrites
  // an sm_89 entry that GPU never selects.
  const mfgunlock::thingeometry::Options options{
      adaptive_quality ||
          g_thin_geometry_validated_warp_blend.load(std::memory_order_relaxed),
      !native_blackwell && !adaptive_quality &&
          g_thin_geometry_previous_scatter.load(std::memory_order_relaxed),
      native_blackwell ? 120u : 89u};
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
  if (mfgunlock::xmx::preada::SkipQualityPatches()) return;
  if (!mfgunlock::modulesession::Acquire(mod)) return;
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
    if (!patches.empty() || allocation) g_midpoint_modules.push_back({mod, std::move(patches), allocation});
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
  if (mfgunlock::xmx::preada::SkipQualityPatches()) return;
  if (mod == nullptr || ModuleHasTemporalPatch(mod)) return;
  if (NativeBlackwellLeavesProviderAlone()) return;
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
  // XMX's hook changes the provider's code, so its identity is taken first.
  struct AttachAfterGates { HMODULE mod; ~AttachAfterGates() { mfgunlock::xmx::Attach(mod); } } attach{mod};
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

struct DriverOnlyProvider {
  bool active = false;
  std::string version;
};
DriverOnlyProvider CheckDriverOnlyProvider();

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
  const std::vector<HMODULE> providers = g_dlssg_modules;
  // 1.8.7: RTX 50 leaves every provider unpatched, so g_gate_modules stays
  // empty and Dynamic MFG never saw a provider version ("NVIDIA's frame
  // generation file has not loaded yet"). Report every mapped provider there.
  if (mfgunlock::gpuarch::AllNvidiaAdaptersBlackwell()) version_candidates = providers;
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
  // After the gates, outside the lock. Before the addon is resident this only
  // remembers the providers; a preloaded one arrives only here (DL2's .bin).
  for (HMODULE module : providers) mfgunlock::xmx::Attach(module);
  TryInstallPresetHook();
  mfgunlock::xmx::preada::TryInstallStreamline();
  mfgunlock::xmx::preada::TryInstallRequirements();
  // File-version APIs are deliberately kept out of the loader callback. Only
  // candidates whose architecture gates were validated are reported here.
  for (HMODULE module : version_candidates) {
    mfgunlock::framecount::ObserveDlssgProviderVersion(module);
  }
  CheckDriverOnlyProvider();
}

// 1.8.5: NVIDIA used only the driver's own nvngx_dlssg.dll (DriverStore, an
// older 310.x build without the multi-frame gate) and Streamline reports 2x as
// its maximum. That happens when this game's driver profile turns the NVIDIA
// app's frame generation override off (it can write that when the game's
// settings are opened), or when no override exists and the game ships no
// provider. The fix is the same: the override set to Latest for this game.
// Judged by the outcome, so it never reads or writes the driver profile.
DriverOnlyProvider CheckDriverOnlyProvider() {
  // Called from provider maintenance and from the panel: never blocks either.
  static SRWLOCK lock = SRWLOCK_INIT;
  static DriverOnlyProvider cached;
  static size_t checked_count = SIZE_MAX;
  static bool logged = false;
  if (!mfgunlock::framecount::g_state_seen.load(std::memory_order_acquire) ||
      mfgunlock::framecount::g_runtime_max_generated.load(std::memory_order_relaxed) > 1)
    return {};
  if (!TryAcquireSRWLockExclusive(&lock)) return {};
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&lock); } } unlock;
  std::vector<HMODULE> providers;
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) return cached;
  providers = g_dlssg_modules;
  ReleaseSRWLockShared(&g_provider_maintenance_lock);
  if (providers.size() == checked_count) return cached;
  checked_count = providers.size();
  DriverOnlyProvider result;
  bool driver_store = false, other = false;
  for (HMODULE module : providers) {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) continue;
    for (wchar_t* c = path; *c; ++c) *c = static_cast<wchar_t>(towlower(*c));
    if (std::wcsstr(path, L"\\driverstore\\") != nullptr) {
      driver_store = true;
      const auto v = mfgunlock::runtimeversion::FromModule(module);
      if (v.valid)
        result.version = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    } else {
      other = true;
    }
  }
  result.active = driver_store && !other;
  cached = result;
  if (result.active && !logged) {
    logged = true;
    reshade::log::message(
        reshade::log::level::warning,
        ("mfgunlock: NVIDIA is using only the driver's built-in frame generation file" +
         (result.version.empty() ? std::string() : " (" + result.version + ")") +
         ", and Streamline offers 2x at most. Set \"DLSS Override - Frame Generation\" to Latest for this game in the NVIDIA app, then restart.")
            .c_str());
  }
  return result;
}

// 1.9.0: the frame generation chain, for the panel and the support report.
// Never blocks: a busy maintenance lock returns the last answer.
// Each link also carries a health, for the card's colour.
enum class ChainHealth { kUnknown, kOk, kWarn, kFail };
struct FrameGenChain {
  std::string streamline;  // sl.dlss_g.dll source and version
  std::string provider;    // nvngx_dlssg source(s) and version
  std::string gate;
  std::string slinit;
  std::string dynamic;
  std::string ngx_feature;  // 1.10.0: RTX 20/30 only; empty elsewhere
  ChainHealth streamline_health = ChainHealth::kUnknown, provider_health = ChainHealth::kUnknown,
              gate_health = ChainHealth::kUnknown, slinit_health = ChainHealth::kUnknown,
              dynamic_health = ChainHealth::kUnknown, ngx_feature_health = ChainHealth::kUnknown;
};
std::string ChainVersion(HMODULE module) {
  const auto v = mfgunlock::runtimeversion::FromModule(module);
  if (!v.valid) return std::string();
  return " " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch) + "." +
         std::to_string(v.revision);
}
FrameGenChain DescribeFrameGenChain() {
  static SRWLOCK lock = SRWLOCK_INIT;
  static FrameGenChain cached;
  static uint64_t last_ms = 0;
  if (!TryAcquireSRWLockExclusive(&lock)) return {};
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&lock); } } unlock;
  const uint64_t now = GetTickCount64();
  if (last_ms != 0 && now - last_ms < 1000) return cached;  // file versions: once a second at most
  last_ms = now;
  FrameGenChain chain;
  wchar_t path[MAX_PATH]{};
  HMODULE plugin = mfgunlock::framecount::g_active_streamline_module.load(std::memory_order_acquire);
  if (plugin == nullptr) plugin = GetModuleHandleW(L"sl.dlss_g.dll");
  if (plugin != nullptr && GetModuleFileNameW(plugin, path, MAX_PATH) != 0) {
    chain.streamline = std::string(mfgunlock::fgchain::Name(mfgunlock::fgchain::ClassifyStreamline(path))) +
                       ChainVersion(plugin);
    chain.streamline_health = ChainHealth::kOk;
  } else if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) {
    chain.streamline = "Streamline loaded; DLSS-G plugin not loaded";
    chain.streamline_health = ChainHealth::kWarn;
  } else {
    chain.streamline = "unknown (Streamline not loaded yet)";
  }
  std::vector<HMODULE> providers;
  bool have_providers = false;
  if (TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    providers = g_dlssg_modules;
    ReleaseSRWLockShared(&g_provider_maintenance_lock);
    have_providers = true;
  }
  if (!have_providers) return cached;
  bool driver_only = !providers.empty();
  for (HMODULE module : providers) {
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) continue;
    const auto source = mfgunlock::fgchain::ClassifyProvider(path);
    if (source != mfgunlock::fgchain::Source::kDriver) driver_only = false;
    if (!chain.provider.empty()) chain.provider += "; ";
    chain.provider += std::string(mfgunlock::fgchain::Name(source)) + ChainVersion(module);
  }
  if (chain.provider.empty()) chain.provider = "unknown (not loaded yet)";
  else chain.provider_health = driver_only ? ChainHealth::kWarn : ChainHealth::kOk;
  if (mfgunlock::gpuarch::AllNvidiaAdaptersBlackwell()) {
    chain.gate = "not needed (RTX 50)";
    chain.gate_health = ChainHealth::kOk;
  } else if (g_gate_patched.load(std::memory_order_acquire)) {
    chain.gate = "open in " + std::to_string(g_gate_modules.size()) + " file(s)";
    chain.gate_health = ChainHealth::kOk;
  } else if (!providers.empty()) {
    chain.gate = "not open (2x at most)";
    chain.gate_health = ChainHealth::kFail;
    // 1.10.0 (dashdogy A4): say why when every provider predates multi-frame.
    bool all_old = true;
    for (HMODULE module : providers)
      all_old = all_old && mfgunlock::runtimeversion::PredatesMultiFrameDlssg(
                               mfgunlock::runtimeversion::Pack(mfgunlock::runtimeversion::FromModule(module)));
    if (all_old)
      chain.gate += "; this DLSS-G version predates multi-frame generation (310.7 or newer is needed: "
                    "the NVIDIA app's DLSS override, or a newer nvngx_dlssg.dll)";
  } else {
    chain.gate = "unknown";
  }
  // 1.10.1 (plan item 3): Streamline 1.x has no slGetFeatureFunction; nothing in this addon works with it.
  if (const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
      interposer != nullptr && GetProcAddress(interposer, "slGetFeatureFunction") == nullptr) {
    chain.streamline = "Streamline 1.x" + ChainVersion(interposer) +
                       " (not supported: DLSS-G and Add frame generation need Streamline 2)";
    chain.streamline_health = ChainHealth::kFail;
  }
  if (mfgunlock::framecount::internal::g_init_seen.load(std::memory_order_acquire)) {
    chain.slinit = "seen by the addon";
    chain.slinit_health = ChainHealth::kOk;
  } else if (mfgunlock::framecount::internal::g_host_init_seen.load(std::memory_order_acquire)) {
    // 1.10.1 (plan item 5): only Add frame generation's own slInit ran.
    chain.slinit = "MFG Unlock's own Streamline (Add frame generation); the game's slInit not seen";
    chain.slinit_health = ChainHealth::kOk;
  } else if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) {
    const bool needed = mfgunlock::xmx::preada::Admitted() ||
                        mfgunlock::drsoverride::g_max_multiplier.load(std::memory_order_relaxed) != 0;
    // 1.10.0 (plan 1.5): the hook is in and never ran, yet the DLSS-G plugin is
    // mapped, which only slInit does: slInit was already running when the
    // addon loaded (Rift Apart: the interposer loads ReShade's dxgi.dll).
    const bool hooked = mfgunlock::framecount::internal::g_real_init != nullptr;
    const bool in_flight = hooked && GetModuleHandleW(L"sl.dlss_g.dll") != nullptr;
    chain.slinit = std::string(in_flight ? "started before the addon could hook it (the game loads ReShade from inside Streamline)"
                               : hooked  ? "not called since the addon loaded"
                                         : "ran before the addon loaded (or not hooked)") +
                   (needed ? "" : "; not needed here");
    // Only RTX 20/30 and the Dynamic MFG ceiling depend on it.
    chain.slinit_health = needed ? ChainHealth::kWarn : ChainHealth::kUnknown;
  } else {
    chain.slinit = "unknown (Streamline not loaded yet)";
  }
  if (!mfgunlock::framecount::g_dynamic_support_seen.load(std::memory_order_acquire)) {
    chain.dynamic = "unknown";
  } else if (mfgunlock::framecount::g_dynamic_supported.load(std::memory_order_relaxed)) {
    chain.dynamic = "supported";
    chain.dynamic_health = ChainHealth::kOk;
  } else {
    chain.dynamic = "not supported";
    chain.dynamic_health = ChainHealth::kWarn;
  }
  // 1.10.0 (plan 1.3): whether NVIDIA's runtime created the feature, RTX 20/30 only.
  if (mfgunlock::xmx::preada::Admitted()) {
    const int64_t created = mfgunlock::xmx::preada::g_create_result.load();
    const uint64_t on = mfgunlock::xmx::preada::g_first_on_tick.load(std::memory_order_acquire);
    if (created >= 0 && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(created))) {
      std::ostringstream text;
      text << "could not be created (0x" << std::hex << static_cast<uint32_t>(created) << ")";
      // 1.10.0 (dashdogy A3): the SetOptions hook now sends Off.
      if (mfgunlock::xmx::preada::HoldOff())
        text << std::dec << "; held off after " << mfgunlock::xmx::preada::kHoldOffAfter
             << " refusals so Streamline stops retrying";
      chain.ngx_feature = text.str();
      chain.ngx_feature_health = ChainHealth::kFail;
    } else if (created >= 0) {
      chain.ngx_feature = "created";
      chain.ngx_feature_health = ChainHealth::kOk;
    } else if (on == 0) {
      chain.ngx_feature = "not requested yet (frame generation off in the game)";
    } else if (mfgunlock::xmx::preada::CreateMissing(now)) {
      chain.ngx_feature = "not created " + std::to_string((now - on) / 1000) +
                          " s after the game turned frame generation on; nothing is generated (see the Streamline log)";
      chain.ngx_feature_health = ChainHealth::kFail;
    } else {
      chain.ngx_feature = "waiting for NVIDIA's runtime";
    }
  }
  cached = chain;
  return chain;
}

void RestoreMidpoint() {
  // These modules have retained leases. Restore ownership is checked by each writer.
  for (auto& module : g_midpoint_modules) mfgunlock::midpoint::Restore(module.patches, module.allocation);
  for (auto& module : g_blackwell_modules) mfgunlock::blackwell::Restore(module.patches, module.allocations);
  std::erase_if(g_midpoint_modules, [](const auto& m) { return m.patches.empty() && !m.allocation; });
  std::erase_if(g_blackwell_modules, [](const auto& m) { return m.patches.empty() && m.allocations.empty(); });
  if (g_midpoint_modules.empty()) g_midpoint_patched.store(false, std::memory_order_release);
  if (g_blackwell_modules.empty()) { g_blackwell_patched.store(false, std::memory_order_release); g_blackwell_applied_detail.clear(); }
  g_midpoint_rejected_modules.clear();
}

void RestoreThinGeometry() {
  for (auto& module : g_thin_geometry_modules) mfgunlock::thingeometry::Restore(module.redirects);
  std::erase_if(g_thin_geometry_modules, [](const auto& m) { return m.redirects.empty(); });
  if (g_thin_geometry_modules.empty()) g_thin_geometry_patched.store(false, std::memory_order_release);
}
