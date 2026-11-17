// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (plan "DLSS-G em jogos só com DLSS", route B): NVIDIA's DLSS frame
// generation, and XMX inside it, for games that call DLSS Super Resolution
// straight through NGX and have no Streamline (RDR2, KCD2, Metro Exodus EE).
//
// The addon becomes the game's Streamline integration, with NVIDIA's own
// "manual hooking" mode (ProgrammingGuideManualHooking.md):
// - when the game creates its D3D12 device, Streamline 2.14 (MIT, NVIDIA's
//   signed binaries from MFGUnlock-Streamline\ beside the addon) is initialized
//   with DLSS-G, Reflex and PCL, without hooking anything by itself;
// - the real DXGI factory's swap chain creation is detoured so that the swap
//   chain ReShade receives is Streamline's proxy: game -> ReShade -> Streamline
//   -> DXGI. ReShade still sees one Present per game frame;
// - each DLSS SR evaluation (the NGX core hooks of addon_provider.inl) tags its
//   depth and motion vectors, and its output as the HUD-less color, with
//   constants built like the XeMFG DLSS-SR input's (no camera in NGX: a 60
//   degree projection and an identity view, camera motion in the vectors);
// - Reflex markers come from ReShade's present events.
// From there the rest of the addon sees a Streamline game: the panel's
// multiplier, XMX (with its video memory fix) and the support report.
//
// [DLSSG] HostStreamline=1, off by default. Inert when the game loads its own
// Streamline, when the backend is not DLSS-G, or when the files are missing.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <psapi.h>
#include <DirectXMath.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include "backend_selection.hpp"
#include "force_fg.hpp"
#include "sl_module.hpp"
#include "swapchain_format_guard.hpp"
#include "ngx_hook.hpp"

namespace mfgunlock::slhost {

enum class State : int {
  kOff = 0,          // not wanted
  kWaiting,          // wanted, no D3D12 device yet
  kGameStreamline,   // the game has its own Streamline: nothing to do
  kMissingFiles,     // MFGUnlock-Streamline\sl.interposer.dll not found
  kFailed,           // Streamline refused (see Report)
  kReady,            // Streamline initialized, waiting for the swap chain
  kSwapChain,        // the swap chain is Streamline's
  kGenerating,       // DLSS SR seen and DLSS-G turned on
};

inline const wchar_t* kFolder = L"MFGUnlock-Streamline";

namespace internal {
inline std::atomic<State> state{State::kOff};
inline std::mutex report_lock;
inline std::string report;
inline HMODULE module = nullptr;
inline PFun_slInit* init = nullptr;
inline PFun_slSetD3DDevice* set_device = nullptr;
inline PFun_slUpgradeInterface* upgrade = nullptr;
inline PFun_slGetNewFrameToken* new_token = nullptr;
inline PFun_slSetTagForFrame* set_tags = nullptr;
inline PFun_slSetConstants* set_constants = nullptr;
inline PFun_slGetFeatureFunction* feature_function = nullptr;
inline PFun_slIsFeatureLoaded* feature_loaded = nullptr;
inline PFun_slDLSSGSetOptions* dlssg_options = nullptr;
inline PFun_slDLSSGGetState* dlssg_state = nullptr;
inline PFun_slReflexSetOptions* reflex_options = nullptr;
inline PFun_slPCLSetMarker* marker = nullptr;
inline PFun_slReflexSleep* reflex_sleep = nullptr;
inline std::atomic<int> backbuffer_format{0};  // DXGI_FORMAT of the game's swap chain
// 1.10.1: the swap chain is RGBA16F (scRGB HDR) now. DLSS-G does not generate
// there (DLSS-G programming guide 2.14.16, section 11: "DLSS-G currently does NOT
// support FP16 pixel format and scRGB color space"); it reports nothing either
// (status 0, one frame per game frame: Kingdom Come: Deliverance II in HDR).
inline std::atomic_bool fp16_hdr{false};
inline std::atomic<uint32_t> hud_arm_index{0};  // the frame whose image before the HUD is still to be found
inline std::atomic<uint64_t> hud_captures{0};
inline std::atomic<uint64_t> evaluations{0}, tagged{0}, frames{0};
inline std::atomic_bool options_on{false};
inline IDXGIFactory2* proxy_factory = nullptr;  // kept alive with the swap chain
inline ID3D12Device* game_device = nullptr;
inline bool above_reshade = false;
inline std::atomic_bool presents_hooked{false};
inline void HookProxyPresent(void* chain);  // Streamline wraps ReShade's factory (HookFactory)
inline std::atomic<uint32_t> backbuffer_width{0}, backbuffer_height{0};
inline std::atomic<void*> game_chain{nullptr};  // Streamline's proxy swap chain, for the report
inline thread_local bool inside_creation = false;

// DLSS SR features: NGX handle -> create flags.
struct Feature {
  const void* handle;
  int flags;
};
inline std::mutex features_lock;
inline Feature features[8]{};

inline void SetReport(const std::string& text) {
  std::lock_guard guard(report_lock);
  report = text;
  reshade::log::message(reshade::log::level::info, ("mfgunlock: Streamline host: " + text).c_str());
}
}  // namespace internal

inline bool Wanted() {
  static const bool wanted = [] {
    const auto ini = backend::Directory() / L"MFGAdaUnlock.ini";
    return backend::Read() == backend::Kind::DlssG &&
           (GetPrivateProfileIntW(L"DLSSG", L"HostStreamline", 0, ini.c_str()) != 0 ||
            GetPrivateProfileIntW(L"DLSSG", L"AddFrameGeneration", 0, ini.c_str()) != 0);
  }();
  return wanted;
}
inline State Current() { return internal::state.load(std::memory_order_acquire); }
inline std::string Report() {
  std::lock_guard guard(internal::report_lock);
  return internal::report;
}
// Our own Streamline is up: the parts of the addon that look for sl.interposer.dll see it.
inline bool Hosting() { return Current() >= State::kReady; }

// ------------------------------------------------------------------ swap chain

namespace internal {
using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                 IDXGISwapChain1**);
using CreateLegacyFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
inline CreateHwndFn real_hwnd = nullptr;
inline CreateLegacyFn real_legacy = nullptr;

inline bool Engage(IUnknown* queue) {
  if (inside_creation || Current() != State::kReady || queue == nullptr) return false;
  ID3D12CommandQueue* d3d12 = nullptr;
  if (FAILED(queue->QueryInterface(IID_PPV_ARGS(&d3d12)))) return false;
  d3d12->Release();
  return true;
}
// The device the game holds: ReShade's wrapper, which ReShade stores in the
// native device's private data (d3d12_device.cpp, D3D12Device's constructor).
// Streamline must get this one. With the native device, NGX creates the
// game's DLSS SR under Streamline's identity from a ReShade-wrapped command
// list, and that list then fails to close (E_INVALIDARG, NGX-only test app).
inline ID3D12Device* GameDevice(ID3D12Device* native) {
  constexpr GUID kReShadeDevice = {0x2523aff4, 0x978b, 0x4939, {0xba, 0x16, 0x8e, 0xe8, 0x76, 0xa4, 0xcb, 0x2a}};
  ID3D12Device* proxy = nullptr;
  UINT size = sizeof(proxy);
  if (SUCCEEDED(native->GetPrivateData(kReShadeDevice, &size, &proxy)) && size == sizeof(proxy) && proxy != nullptr)
    return proxy;
  return native;
}
// slSetD3DDevice, on the game's first swap chain creation. Not from ReShade's
// init_device: Streamline calls into ReShade's device there, while ReShade
// still holds the lock of that event, and the game hung (NGX-only test app).
inline bool AttachDevice(IUnknown* queue) {
  static bool attached = false;
  if (attached) return true;
  attached = true;
  // The device of the queue the swap chain presents with, not the first one
  // the game created: RDR2 creates and destroys a device before its real one,
  // and Streamline then read a released device (null access in sl.interposer).
  ID3D12CommandQueue* d3d12 = nullptr;
  ID3D12Device* native = nullptr;
  if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&d3d12)))) {
    d3d12->GetDevice(IID_PPV_ARGS(&native));
    d3d12->Release();
  }
  if (native == nullptr) {
    state = State::kFailed;
    SetReport("the swap chain's queue has no D3D12 device");
    return false;
  }
  game_device = GameDevice(native);
  native->Release();  // the game keeps it alive
  sl::Result result = set_device(game_device);
  if (result != sl::Result::eOk) {
    state = State::kFailed;
    SetReport("slSetD3DDevice failed (" + std::to_string(static_cast<int>(result)) + ")");
    return false;
  }
  bool loaded = false;
  if (feature_loaded) feature_loaded(sl::kFeatureDLSS_G, loaded);
  void* fn = nullptr;
  if (feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn) == sl::Result::eOk)
    dlssg_options = reinterpret_cast<PFun_slDLSSGSetOptions*>(fn);
  fn = nullptr;
  if (feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", fn) == sl::Result::eOk)
    dlssg_state = reinterpret_cast<PFun_slDLSSGGetState*>(fn);
  fn = nullptr;
  if (feature_function(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk)
    reflex_options = reinterpret_cast<PFun_slReflexSetOptions*>(fn);
  fn = nullptr;
  if (feature_function(sl::kFeatureReflex, "slReflexSleep", fn) == sl::Result::eOk)
    reflex_sleep = reinterpret_cast<PFun_slReflexSleep*>(fn);
  fn = nullptr;
  if (feature_function(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk)
    marker = reinterpret_cast<PFun_slPCLSetMarker*>(fn);
  if (!loaded || dlssg_options == nullptr) {
    state = State::kFailed;
    SetReport("Streamline started, but DLSS-G did not load on this GPU or driver");
    return false;
  }
  if (reflex_options) {
    sl::ReflexOptions reflex{};
    reflex.mode = sl::ReflexMode::eLowLatency;
    reflex_options(reflex);
  }
  return true;
}
// Streamline's proxy for this factory, created once.
inline IDXGIFactory2* Proxy(IDXGIFactory* native) {
  if (proxy_factory != nullptr) return proxy_factory;
  IDXGIFactory2* base = nullptr;
  if (FAILED(native->QueryInterface(IID_PPV_ARGS(&base)))) return nullptr;
  void* upgraded = base;
  if (upgrade(&upgraded) != sl::Result::eOk || upgraded == nullptr) {
    base->Release();
    return nullptr;
  }
  proxy_factory = static_cast<IDXGIFactory2*>(upgraded);
  return proxy_factory;
}
// 1.10.1 (plan item 7): another addon (RenoDX's HDR upgrade) changed the real swap chain's format
// under Streamline, whose buffers keep the game's. Streamline's swap chain is released and the
// game gets a plain one; Add frame generation stays off for the session.
inline bool StepAside(const scguard::Watch& watch, IUnknown** chain) {
  if (!watch.Changed()) return false;
  if (chain != nullptr && *chain != nullptr) {
    (*chain)->Release();
    *chain = nullptr;
  }
  state = State::kFailed;
  SetReport("not used: another addon changed the swap chain's format under Streamline (" + watch.Text() +
            "; RenoDX's HDR upgrade?). Use the game's own HDR, or turn the other addon's swap chain upgrade off");
  return true;
}
inline HRESULT STDMETHODCALLTYPE CreateForHwnd(IDXGIFactory2* self, IUnknown* queue, HWND window,
                                               const DXGI_SWAP_CHAIN_DESC1* desc,
                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
                                               IDXGISwapChain1** chain) {
  if (!Engage(queue)) return real_hwnd(self, queue, window, desc, fullscreen, output, chain);
  if (!AttachDevice(queue)) return real_hwnd(self, queue, window, desc, fullscreen, output, chain);
  IDXGIFactory2* proxy = Proxy(self);
  if (proxy == nullptr) {
    state = State::kFailed;
    SetReport("Streamline did not wrap the DXGI factory; the game runs without it");
    return real_hwnd(self, queue, window, desc, fullscreen, output, chain);
  }
  inside_creation = true;
  const scguard::Watch watch;
  const HRESULT hr = proxy->CreateSwapChainForHwnd(queue, window, desc, fullscreen, output, chain);
  inside_creation = false;
  if (SUCCEEDED(hr) && StepAside(watch, reinterpret_cast<IUnknown**>(chain)))
    return real_hwnd(self, queue, window, desc, fullscreen, output, chain);
  if (SUCCEEDED(hr)) {
    game_chain = *chain;
    backbuffer_format = desc ? static_cast<int>(desc->Format) : 0;
    if (above_reshade) HookProxyPresent(*chain);
    state = State::kSwapChain;
    SetReport("the game's swap chain is Streamline's (" + std::to_string(desc ? desc->Width : 0) + "x" +
              std::to_string(desc ? desc->Height : 0) + (above_reshade ? ", above ReShade)" : ", under ReShade)"));
  } else {
    SetReport("Streamline's swap chain failed (" + std::to_string(static_cast<long>(hr)) + "); retrying without it");
    return real_hwnd(self, queue, window, desc, fullscreen, output, chain);
  }
  return hr;
}
inline HRESULT STDMETHODCALLTYPE CreateLegacy(IDXGIFactory* self, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** chain) {
  if (!Engage(queue)) return real_legacy(self, queue, desc, chain);
  if (!AttachDevice(queue)) return real_legacy(self, queue, desc, chain);
  IDXGIFactory2* proxy = Proxy(self);
  if (proxy == nullptr) return real_legacy(self, queue, desc, chain);
  inside_creation = true;
  const scguard::Watch watch;
  const HRESULT hr = proxy->CreateSwapChain(queue, desc, chain);
  inside_creation = false;
  if (SUCCEEDED(hr) && StepAside(watch, reinterpret_cast<IUnknown**>(chain))) return real_legacy(self, queue, desc, chain);
  if (SUCCEEDED(hr)) {
    game_chain = *chain;
    backbuffer_format = desc ? static_cast<int>(desc->BufferDesc.Format) : 0;
    if (above_reshade) HookProxyPresent(*chain);
    state = State::kSwapChain;
    SetReport("the game's swap chain is Streamline's (legacy creation)");
    return hr;
  }
  return real_legacy(self, queue, desc, chain);
}
// The real DXGI's factory code, shared by every factory object of the process
// (ReShade's wrappers call it underneath).
// ReShade's module: it exports its add-on entry and CreateDXGIFactory2 under
// either name (dxgi.dll or d3d12.dll).
inline HMODULE ReShadeAsDxgi() {
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
  for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i)
    if (GetProcAddress(modules[i], "ReShadeRegisterAddon") && GetProcAddress(modules[i], "CreateDXGIFactory2"))
      return modules[i];
  return nullptr;
}
// Where the swap chain creation is taken over: ReShade's own factory wrapper,
// so Streamline sits above it (game -> Streamline -> ReShade -> DXGI), the
// order of games that ship Streamline; ReShade then presents every frame,
// generated ones included. Under ReShade (the real DXGI's factory code, the
// fallback when no ReShade module is found) Streamline's first Present
// crashed in the driver with ReShade as dxgi.dll (RDR2, KCD2, the NGX-only
// test app); it ran with ReShade as d3d12.dll (RDR2).
inline bool HookFactory() {
  HMODULE dxgi = ReShadeAsDxgi();
  above_reshade = dxgi != nullptr;
  if (dxgi == nullptr) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    dxgi = LoadLibraryW((std::wstring(system) + L"\\dxgi.dll").c_str());
  }
  if (dxgi == nullptr) return false;
  using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
  const auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
  IDXGIFactory2* factory = nullptr;
  if (create == nullptr || FAILED(create(0, IID_PPV_ARGS(&factory)))) return false;
  void** table = *reinterpret_cast<void***>(factory);
  real_hwnd = reinterpret_cast<CreateHwndFn>(table[15]);
  real_legacy = reinterpret_cast<CreateLegacyFn>(table[10]);
  factory->Release();
  const bool hwnd = hook::InstallOne(reinterpret_cast<void**>(&real_hwnd), reinterpret_cast<void*>(&CreateForHwnd));
  const bool legacy = hook::InstallOne(reinterpret_cast<void**>(&real_legacy), reinterpret_cast<void*>(&CreateLegacy));
  return hwnd && legacy;
}
}  // namespace internal

// ReShade's init_device, before the game creates its swap chain.
inline void OnDevice(ID3D12Device* device) {
  using namespace internal;
  if (!Wanted() || device == nullptr) return;
  static std::atomic_bool once{false};
  if (once.exchange(true)) return;
  // 1.10.1 (plan item 1): only a Streamline that was started (slInit maps sl.common.dll) is the
  // game's. Shadow of the Tomb Raider had an interposer mapped by RenoDX's DLSS addon, never started.
  if (slmodule::StartedByAnother()) {
    state = State::kGameStreamline;
    SetReport("the game has its own Streamline; not hosting");
    return;
  }
  const auto folder = backend::Directory() / kFolder;
  if (!std::filesystem::exists(folder / L"sl.interposer.dll") || !std::filesystem::exists(folder / L"sl.dlss_g.dll")) {
    state = State::kMissingFiles;
    SetReport("MFGUnlock-Streamline\\sl.interposer.dll or sl.dlss_g.dll is missing beside the addon");
    return;
  }
  // The addon must outlive the detours and Streamline's callbacks.
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&OnDevice), &self);
  module = LoadLibraryExW((folder / L"sl.interposer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    state = State::kFailed;
    SetReport("sl.interposer.dll did not load (" + std::to_string(GetLastError()) + ")");
    return;
  }
  slmodule::HostLoaded(module);
  const auto get = [&](const char* name) { return GetProcAddress(module, name); };
  init = reinterpret_cast<PFun_slInit*>(get("slInit"));
  set_device = reinterpret_cast<PFun_slSetD3DDevice*>(get("slSetD3DDevice"));
  upgrade = reinterpret_cast<PFun_slUpgradeInterface*>(get("slUpgradeInterface"));
  new_token = reinterpret_cast<PFun_slGetNewFrameToken*>(get("slGetNewFrameToken"));
  set_tags = reinterpret_cast<PFun_slSetTagForFrame*>(get("slSetTagForFrame"));
  set_constants = reinterpret_cast<PFun_slSetConstants*>(get("slSetConstants"));
  feature_function = reinterpret_cast<PFun_slGetFeatureFunction*>(get("slGetFeatureFunction"));
  feature_loaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(get("slIsFeatureLoaded"));
  if (!init || !set_device || !upgrade || !new_token || !set_tags || !set_constants || !feature_function) {
    state = State::kFailed;
    SetReport("sl.interposer.dll lacks the Streamline 2 entry points");
    return;
  }
  const std::wstring folder_text = folder.wstring();
  const std::wstring logs = backend::Directory().wstring();
  const wchar_t* paths[] = {folder_text.c_str()};
  const sl::Feature wanted[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  sl::Preferences preferences{};
  preferences.pathsToPlugins = paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = logs.c_str();
  preferences.featuresToLoad = wanted;
  preferences.numFeaturesToLoad = 3;
  preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = "1.10.0";
  // MFG Unlock's own project identity, not the game's.
  preferences.projectId = forcefg::kHostProjectId;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  sl::Result result = init(preferences, sl::kSDKVersion);
  if (result != sl::Result::eOk) {
    state = State::kFailed;
    SetReport("slInit failed (" + std::to_string(static_cast<int>(result)) + ")");
    return;
  }
  if (!HookFactory()) {
    state = State::kFailed;
    SetReport("the DXGI swap chain creation could not be hooked");
    return;
  }
  state = State::kReady;
  // XeFG's UI composition (uiComposition=1) left XMX's frames uninterpolated in KCD2; it stays opt-in
  // ([DLSSG] XmxUiComposition=1), and the HUD-less color is tagged for NVIDIA's generator alone.
  SetReport("Streamline 2 started; the game's device goes to it when the game creates its swap chain");
}

// ------------------------------------------------------------------ frames

namespace internal {
// One Streamline frame per game frame, numbered by the game's DLSS SR
// evaluations. A game that records frame N+1 while another thread presents
// frame N (RDR2) must present each frame with the token its inputs were tagged
// on: with one "current" token, every other frame had no inputs and DLSS-G
// turned itself off and on at every frame. Evaluations queue their index; each
// Present takes the oldest one (a frame without DLSS SR gets a fresh index).
inline std::mutex frames_lock;
inline uint32_t next_index = 1;
inline uint32_t pending[8]{};
inline uint32_t pending_count = 0;
inline uint32_t presenting = 0;  // the index between Present and finish_present
inline uint32_t slept = 0;       // the next frame's index, after Reflex's sleep marked its simulation start
inline double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
  static LARGE_INTEGER frequency{};
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  return double(b.QuadPart - a.QuadPart) * 1000.0 / double(frequency.QuadPart);
}
inline LARGE_INTEGER last_present_end{};
inline double last_sleep_ms = 0.0;

inline sl::FrameToken* TokenFor(uint32_t index) {
  sl::FrameToken* t = nullptr;
  if (new_token == nullptr || new_token(t, &index) != sl::Result::eOk) return nullptr;
  return t;
}
inline void Mark(sl::PCLMarker which, uint32_t index) {
  if (marker == nullptr || index == 0) return;
  if (sl::FrameToken* t = TokenFor(index)) marker(which, *t);
}
// From the DLSS SR evaluation: the index its inputs are tagged on.
inline uint32_t BeginFrame() {
  uint32_t index;
  {
    std::lock_guard guard(frames_lock);
    index = next_index++;
    if (pending_count == std::size(pending)) {  // no Present for a while: drop the oldest
      for (uint32_t i = 1; i < pending_count; ++i) pending[i - 1] = pending[i];
      --pending_count;
    }
    pending[pending_count++] = index;
  }
  {
    std::lock_guard guard(frames_lock);
    if (index != slept) Mark(sl::PCLMarker::eSimulationStart, index);
  }
  Mark(sl::PCLMarker::eSimulationEnd, index);
  Mark(sl::PCLMarker::eRenderSubmitStart, index);
  return index;
}
// From the Present: the oldest evaluated frame not presented yet.
// None pending: one of ReShade's extra present events (DLSS-G presenting its
// generated frames reaches ReShade's present event too, 1.7 events per game
// frame on the NGX-only test app, through the same swap chain interface), or a
// frame without DLSS SR; a fresh index there left Streamline a frame without
// inputs ("tags for frame N not set yet") and DLSS-G off for it.
inline uint32_t PresentFrame() {
  std::lock_guard guard(frames_lock);
  if (pending_count == 0) return presenting = 0;
  // KCD2 evaluates DLSS SR a little more often than it presents (928 against
  // 900): oldest-first then fell further behind every second, and DLSS-G got
  // stale inputs ("Unable to find 'common' constants for frame 319 - using last
  // set for frame 322"). At most one frame in flight is kept (RDR2 records
  // frame N+1 while presenting N); older evaluations are dropped.
  while (pending_count > 2) {
    for (uint32_t i = 1; i < pending_count; ++i) pending[i - 1] = pending[i];
    --pending_count;
  }
  presenting = pending[0];
  for (uint32_t i = 1; i < pending_count; ++i) pending[i - 1] = pending[i];
  --pending_count;
  return presenting;
}
}  // namespace internal

namespace internal {
// Before the game's Present reaches Streamline.
inline void BeforePresent() {
  const State s = Current();
  if (s != State::kSwapChain && s != State::kGenerating) return;
  // The HUD fix compares its target with the back buffer's size (Metro Exodus in HDR took a smaller one).
  if (auto* chain = static_cast<IDXGISwapChain*>(game_chain.load())) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(chain->GetDesc(&d))) {
      backbuffer_width = d.BufferDesc.Width;
      backbuffer_height = d.BufferDesc.Height;
      const bool fp16 = d.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
      if (fp16 != fp16_hdr.exchange(fp16))
        SetReport(fp16 ? "the game's HDR uses an FP16 (scRGB) swap chain, which DLSS-G does not support: no frames are "
                         "generated. Turn the game's HDR off, or use the XeMFG method"
                       : "the game's swap chain left FP16 HDR; DLSS-G can generate again");
    }
  }
  const uint32_t index = PresentFrame();
  // 1.10.1 (plan item 6): a loading screen renders without DLSS SR, so these frames get no
  // Reflex markers; DLSS-G stayed on and logged "Reflex not detected" ~300 times (Metro Exodus).
  // After a few such frames in a row DLSS-G is paused; the next DLSS SR frame turns it back on.
  static uint32_t without_sr = 0;
  static bool paused = false;
  if (index == 0) {  // not a frame the game rendered with DLSS SR
    if (options_on.load(std::memory_order_acquire) && !paused && ++without_sr >= 3) {
      sl::DLSSGOptions options{};
      options.mode = sl::DLSSGMode::eOff;
      dlssg_options(sl::ViewportHandle{0}, options);
      paused = true;
      static std::atomic_int logged{0};
      if (logged.fetch_add(1) < 5)
        reshade::log::message(reshade::log::level::info,
                              "mfgunlock: Streamline host: frames without DLSS Super Resolution (a loading screen?); "
                              "DLSS-G paused until the game uses DLSS SR again.");
    }
    return;
  }
  without_sr = 0;
  if (paused) {
    paused = false;  // the eOn call below every frame turns it back on
    static std::atomic_int logged{0};
    if (logged.fetch_add(1) < 5)
      reshade::log::message(reshade::log::level::info, "mfgunlock: Streamline host: DLSS SR is back; DLSS-G resumed.");
  }
  Mark(sl::PCLMarker::eRenderSubmitEnd, index);
  Mark(sl::PCLMarker::ePresentStart, index);
  const uint64_t presents = frames.fetch_add(1, std::memory_order_relaxed) + 1;
  // Hitches: the longest gap between two game frames in each 2 s window, and
  // how many passed 50 ms (traversal stutter shows as isolated long gaps).
  static LARGE_INTEGER frequency{}, previous{};
  static double worst_ms = 0.0;
  static uint32_t hitches = 0;
  {
    LARGE_INTEGER now_qpc;
    QueryPerformanceCounter(&now_qpc);
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    if (previous.QuadPart != 0) {
      const double gap = double(now_qpc.QuadPart - previous.QuadPart) * 1000.0 / double(frequency.QuadPart);
      if (gap > worst_ms) worst_ms = gap;
      if (gap > 50.0) ++hitches;
    }
    previous = now_qpc;
  }
  if (presents % 300 == 0 && presents <= 900)
    reshade::log::message(reshade::log::level::info,
                          ("mfgunlock: Streamline host: presents " + std::to_string(presents) + " evaluations " +
                           std::to_string(evaluations.load()) + " thread " + std::to_string(GetCurrentThreadId()))
                              .c_str());
  // Every 2 s while generating: what DLSS-G itself reports (status flags such
  // as Reflex not detected or invalid constants) and the game's frame rate.
  if (s == State::kGenerating && dlssg_state != nullptr) {
    static uint64_t last_tick = 0, last_presents = 0;
    const uint64_t now = GetTickCount64();
    if (now - last_tick >= 2000) {
      sl::DLSSGState st{};
      if (dlssg_state(sl::ViewportHandle{0}, st, nullptr) == sl::Result::eOk) {
        const double fps = last_tick ? double(presents - last_presents) * 1000.0 / double(now - last_tick) : 0.0;
        char text[240];
        snprintf(text, sizeof(text),
                 "mfgunlock: Streamline host: DLSS-G status 0x%x, frames presented per game frame %u, game %.1f fps, "
                 "longest frame %.0f ms, hitches over 50 ms %u",
                 static_cast<unsigned>(st.status), st.numFramesActuallyPresented, fps, worst_ms, hitches);
        worst_ms = 0.0;
        hitches = 0;
        reshade::log::message(reshade::log::level::info, text);
      }
      last_tick = now;
      last_presents = presents;
    }
  }
  // Every game frame on the present thread, as games do: the panel's
  // multiplier replaces the count in the addon's slDLSSGSetOptions wrapper,
  // so a change applies at the next frame (3x/4x did not take with one call).
  if (options_on.load(std::memory_order_acquire)) {
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    dlssg_options(sl::ViewportHandle{0}, options);
  } else if (tagged.load(std::memory_order_acquire) > 2) {
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    const sl::Result result = dlssg_options(sl::ViewportHandle{0}, options);
    options_on = true;
    if (result == sl::Result::eOk) {
      state = State::kGenerating;
      SetReport("DLSS-G turned on from the game's DLSS Super Resolution");
    } else {
      SetReport("slDLSSGSetOptions failed (" + std::to_string(static_cast<int>(result)) + ")");
    }
  }
}
// After the game's Present returned from Streamline.
inline void AfterPresent() {
  const State s = Current();
  if (s != State::kSwapChain && s != State::kGenerating) return;
  uint32_t index;
  {
    std::lock_guard guard(frames_lock);
    index = presenting;
  }
  Mark(sl::PCLMarker::ePresentEnd, index);
  // Reflex paces DLSS-G from its sleep at the start of the game's next frame.
  // The game calls none, so it sleeps here, after the Present of the frame
  // before (the frame pacing was uneven in KCD2 without it).
  if (reflex_sleep == nullptr || index == 0) return;
  uint32_t next;
  {
    std::lock_guard guard(frames_lock);
    next = next_index;
  }
  if (sl::FrameToken* t = TokenFor(next)) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    reflex_sleep(*t);
    QueryPerformanceCounter(&b);
    last_sleep_ms = Ms(a, b);
    Mark(sl::PCLMarker::eSimulationStart, next);
    std::lock_guard guard(frames_lock);
    slept = next;
  }
}

// With Streamline above ReShade, ReShade's present events come after
// Streamline handled the frame (and once per presented frame, generated ones
// included): the Reflex markers landed a frame off ("Reflex not detected ...
// 4945 != 4944" in KCD2) and the pacing was uneven. The game's own Present on
// Streamline's proxy is hooked instead: once per game frame, before Streamline.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
inline PresentFn real_present = nullptr;
inline Present1Fn real_present1 = nullptr;
// Hitch breakdown: for a game frame over 100 ms, how long the Present into
// Streamline (DLSS-G, XMX, driver), Reflex's sleep and everything outside
// both (the game) took. Logged, at most 40 times a session.
template <class Call>
inline HRESULT TimedPresent(Call&& call) {
  LARGE_INTEGER start, presented, finished;
  QueryPerformanceCounter(&start);
  BeforePresent();
  const HRESULT hr = call();
  QueryPerformanceCounter(&presented);
  last_sleep_ms = 0.0;
  AfterPresent();
  QueryPerformanceCounter(&finished);
  if (last_present_end.QuadPart != 0) {
    const double frame = Ms(last_present_end, finished), in_present = Ms(start, presented);
    static int logged = 0;
    if (frame > 100.0 && logged < 40) {
      ++logged;
      char text[200];
      snprintf(text, sizeof(text),
               "mfgunlock: Streamline host: hitch %.0f ms: game %.0f ms, Present %.0f ms, Reflex sleep %.0f ms",
               frame, frame - in_present - last_sleep_ms, in_present, last_sleep_ms);
      reshade::log::message(reshade::log::level::info, text);
    }
  }
  last_present_end = finished;
  return hr;
}
inline HRESULT STDMETHODCALLTYPE ProxyPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
  return TimedPresent([&] { return real_present(self, sync, flags); });
}
inline HRESULT STDMETHODCALLTYPE ProxyPresent1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS* params) {
  return TimedPresent([&] { return real_present1(self, sync, flags, params); });
}
inline void HookProxyPresent(void* chain) {
  static std::atomic_bool done{false};
  if (chain == nullptr || done.exchange(true)) return;
  void** table = *reinterpret_cast<void***>(chain);
  real_present = reinterpret_cast<PresentFn>(table[8]);
  real_present1 = reinterpret_cast<Present1Fn>(table[22]);
  const bool a = hook::InstallOne(reinterpret_cast<void**>(&real_present), reinterpret_cast<void*>(&ProxyPresent));
  const bool b = hook::InstallOne(reinterpret_cast<void**>(&real_present1), reinterpret_cast<void*>(&ProxyPresent1));
  presents_hooked = a && b;
  SetReport(std::string("Streamline's Present ") + (presents_hooked ? "hooked: frames are paced from the game's Present"
                                                                   : "could not be hooked; ReShade's present events pace it"));
}
}  // namespace internal

// ReShade's present events: they pace the frames only when Streamline's own
// Present is not hooked (Streamline under ReShade, or the hook failed).
inline void OnPresent(void* native_chain) {
  (void)native_chain;
  if (!internal::presents_hooked) internal::BeforePresent();
}
inline void OnFinishPresent(void* native_chain) {
  (void)native_chain;
  if (!internal::presents_hooked) internal::AfterPresent();
}

// NGX core hooks (addon_provider.inl).
inline void ObserveCreate(const NVSDK_NGX_Parameter* params, const NVSDK_NGX_Handle* handle) {
  using namespace internal;
  if (!Hosting() || handle == nullptr) return;
  int flags = 0;
  if (params) params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
  std::lock_guard guard(features_lock);
  for (auto& f : features)
    if (f.handle == nullptr || f.handle == handle) {
      f = {handle, flags};
      return;
    }
}
inline void ObserveRelease(const NVSDK_NGX_Handle* handle) {
  using namespace internal;
  std::lock_guard guard(features_lock);
  for (auto& f : features)
    if (f.handle == handle) f = {};
}
// After the game's DLSS SR ran on `list`: its inputs and output are still valid there.
inline void AfterEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                          const NVSDK_NGX_Parameter* p) {
  using namespace internal;
  const State s = Current();
  if ((s != State::kSwapChain && s != State::kGenerating) || list == nullptr || p == nullptr) return;
  int flags = -1;
  {
    std::lock_guard guard(features_lock);
    for (const auto& f : features)
      if (f.handle == handle) flags = f.flags;
  }
  if (flags < 0) {
    // Created before the NGX hooks were in: the vectors' size tells their
    // resolution; depth direction and jitter are assumed (normal, unjittered).
    static std::atomic_bool logged{false};
    ID3D12Resource* mv = nullptr;
    ID3D12Resource* out = nullptr;
    p->Get(NVSDK_NGX_Parameter_MotionVectors, &mv);
    p->Get(NVSDK_NGX_Parameter_Output, &out);
    if (!mv || !out) return;
    flags = mv->GetDesc().Width < out->GetDesc().Width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
    if (!logged.exchange(true))
      SetReport("DLSS SR was created before the host's hooks; assuming normal depth and unjittered vectors");
  }
  const uint32_t frame = BeginFrame();
  sl::FrameToken* current = TokenFor(frame);
  if (current == nullptr) return;
  evaluations.fetch_add(1, std::memory_order_relaxed);
  const auto get_resource = [&](const char* name) {
    ID3D12Resource* r = nullptr;
    p->Get(name, &r);
    return r;
  };
  const auto get_uint = [&](const char* name, unsigned fallback) {
    unsigned v = fallback;
    p->Get(name, &v);
    return v;
  };
  const auto get_float = [&](const char* name, float fallback) {
    float v = fallback;
    p->Get(name, &v);
    return v;
  };
  ID3D12Resource* depth = get_resource(NVSDK_NGX_Parameter_Depth);
  ID3D12Resource* motion = get_resource(NVSDK_NGX_Parameter_MotionVectors);
  ID3D12Resource* output = get_resource(NVSDK_NGX_Parameter_Output);
  if (!depth || !motion || !output) return;
  unsigned width = get_uint(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0);
  unsigned height = get_uint(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0);
  if (!width || !height) {
    const auto d = depth->GetDesc();
    width = static_cast<unsigned>(d.Width);
    height = d.Height;
  }
  const bool low_res = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
  const bool inverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  const bool jittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
  const auto out_desc = output->GetDesc();
  sl::Extent depth_extent{get_uint(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0),
                          get_uint(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0), width, height};
  sl::Extent motion_extent{get_uint(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0),
                           get_uint(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0),
                           low_res ? width : static_cast<unsigned>(out_desc.Width),
                           low_res ? height : out_desc.Height};
  sl::Extent output_extent{0, 0, static_cast<unsigned>(out_desc.Width), out_desc.Height};
  const uint32_t readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  sl::Resource depth_resource(sl::ResourceType::eTex2d, depth, readable);
  sl::Resource motion_resource(sl::ResourceType::eTex2d, motion, readable);
  sl::Resource output_resource(sl::ResourceType::eTex2d, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // The DLSS SR output is the image before the HUD only if nothing changes its
  // colors before the HUD: KCD2 runs DLSS SR on linear HDR (RGBA16F) and
  // tonemaps afterwards into RGBA8, and tagging it made DLSS-G treat the whole
  // screen, HUD included, as UI. Tagged only when its format family is the
  // back buffer's; without it the HUD is interpolated with the scene.
  const auto family = [](int f) {
    switch (f) {
      case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 1;
      case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 2;
      case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 3;
      default: return 100 + f;
    }
  };
  const bool hudless = family(static_cast<int>(out_desc.Format)) == family(backbuffer_format.load());
  // Otherwise the HUD fix (hud_capture.hpp) looks for the image before the HUD
  // in this frame's passes after DLSS SR.
  hud_arm_index.store(hudless ? 0 : frame, std::memory_order_release);
  {
    static std::atomic_bool logged{false};
    if (!logged.exchange(true))
      SetReport(std::string("DLSS SR output format ") + std::to_string(static_cast<int>(out_desc.Format)) +
                ", back buffer " + std::to_string(backbuffer_format.load()) +
                (hudless ? ": tagged as the image before the HUD" : ": not the image before the HUD, the HUD is interpolated"));
  }
  const sl::ResourceTag tags[] = {
      sl::ResourceTag(&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &depth_extent),
      sl::ResourceTag(&motion_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
                      &motion_extent),
      sl::ResourceTag(&output_resource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow,
                      &output_extent)};
  if (set_tags(*current, sl::ViewportHandle{0}, tags, hudless ? 3 : 2, reinterpret_cast<sl::CommandBuffer*>(list)) !=
      sl::Result::eOk)
    return;

  // No camera in NGX: as the XeMFG DLSS-SR input (native_ngx.cpp).
  constexpr float kFov = 1.0471976f, kNear = 0.1f, kFar = 10000.f;
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  DirectX::XMFLOAT4X4 projection, inverse, identity;
  const auto proj = DirectX::XMMatrixPerspectiveFovLH(kFov, aspect, inverted ? kFar : kNear, inverted ? kNear : kFar);
  DirectX::XMStoreFloat4x4(&projection, proj);
  DirectX::XMStoreFloat4x4(&inverse, DirectX::XMMatrixInverse(nullptr, proj));
  DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
  sl::Constants c{};
  std::memcpy(&c.cameraViewToClip, &projection, sizeof(projection));
  std::memcpy(&c.clipToCameraView, &inverse, sizeof(inverse));
  std::memcpy(&c.clipToLensClip, &identity, sizeof(identity));
  std::memcpy(&c.clipToPrevClip, &identity, sizeof(identity));
  std::memcpy(&c.prevClipToClip, &identity, sizeof(identity));
  c.jitterOffset = {get_float(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f), get_float(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f)};
  c.mvecScale = {get_float(NVSDK_NGX_Parameter_MV_Scale_X, 1.f) / static_cast<float>(motion_extent.width),
                 get_float(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f) / static_cast<float>(motion_extent.height)};
  c.cameraPinholeOffset = {0.f, 0.f};
  c.cameraPos = {0.f, 0.f, 0.f};
  // No camera orientation in NGX. DLSS-G's automatic scene change detection
  // reads these vectors and runs only when they are orthonormal (DLSS-G
  // programming guide, section 20); with a fixed orientation it kept
  // overriding the reset flag in KCD2 (161 times in 45 s, frame pacing and
  // stutter). Zero vectors turn it off; the game's own DLSS reset still applies.
  c.cameraUp = {0.f, 0.f, 0.f};
  c.cameraRight = {0.f, 0.f, 0.f};
  c.cameraFwd = {0.f, 0.f, 0.f};
  c.cameraNear = kNear;
  c.cameraFar = kFar;
  c.cameraFOV = kFov;
  c.cameraAspectRatio = aspect;
  c.depthInverted = inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  int reset = 0;
  p->Get(NVSDK_NGX_Parameter_Reset, &reset);
  c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.motionVectorsJittered = jittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  if (set_constants(c, *current, sl::ViewportHandle{0}) == sl::Result::eOk) tagged.fetch_add(1, std::memory_order_relaxed);
}

// The support report and the panel.
// The HUD fix's copy of the image before the HUD, for the frame `index`,
// recorded on the game's command list right after the copy. Tagged as valid
// only now, like the other inputs: Streamline clones it on that list. As valid
// until Present, sl.dlss_g read it in its Present and faulted on the first
// frame (KCD2 with XMX, 2026-11-15: 190_E658700.dll+0x4c184, presentation stopped).
inline void TagHudless(ID3D12Resource* copy, ID3D12GraphicsCommandList* list, uint32_t index) {
  using namespace internal;
  if (copy == nullptr || list == nullptr || set_tags == nullptr) return;
  sl::FrameToken* token = TokenFor(index);
  if (token == nullptr) return;
  const auto d = copy->GetDesc();
  sl::Extent extent{0, 0, static_cast<uint32_t>(d.Width), d.Height};
  sl::Resource resource(sl::ResourceType::eTex2d, copy, D3D12_RESOURCE_STATE_COPY_DEST);
  const sl::ResourceTag tag(&resource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &extent);
  if (set_tags(*token, sl::ViewportHandle{0}, &tag, 1, reinterpret_cast<sl::CommandBuffer*>(list)) == sl::Result::eOk &&
      hud_captures.fetch_add(1) == 0)
    SetReport("HUD fix: the image before the HUD was found and is tagged every frame");
}
inline int BackbufferFormat() { return internal::backbuffer_format.load(); }
inline bool Fp16Hdr() { return internal::fp16_hdr.load(); }
inline uint32_t BackbufferWidth() { return internal::backbuffer_width.load(); }
inline uint32_t BackbufferHeight() { return internal::backbuffer_height.load(); }
inline uint32_t HudArmIndex() { return internal::hud_arm_index.load(std::memory_order_acquire); }
inline bool DisarmHud(uint32_t index) {
  uint32_t expected = index;
  return internal::hud_arm_index.compare_exchange_strong(expected, 0);
}

inline uint64_t HudCaptures() { return internal::hud_captures.load(); }
inline std::string Describe() {
  const State s = Current();
  if (s == State::kOff) return "off";
  std::string text = Report();
  text += "; HUD fix captures " + std::to_string(internal::hud_captures.load());
  text += "; DLSS SR evaluations " + std::to_string(internal::evaluations.load()) + ", tagged " +
          std::to_string(internal::tagged.load()) + ", presents " + std::to_string(internal::frames.load());
  return text;
}

}  // namespace mfgunlock::slhost
