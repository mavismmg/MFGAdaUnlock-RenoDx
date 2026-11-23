// SPDX-License-Identifier: MIT
// 1.12.0 (DLSS 5): the panel's DLSS 5 page and where neural rendering meets the game's frames
// (nr_runtime.hpp does the work). Included by addon.cpp inside its anonymous namespace, before the panel.
//
// Where NR runs, all before frame generation sees the frame:
// - after the game's DLSS SR or Ray Reconstruction, on the game's command list, with its motion vectors
//   and depth (addon_provider.inl's NGX hooks; [DLSS5] HookPoint Auto or After upscaling);
// - a game with its own Streamline (and so its own frame generation): the game's Present on Streamline's
//   proxy swap chain, hooked at load when [DLSS5] Enabled=1 (framecount.hpp);
// - Add frame generation: the hosted Streamline's Present (sl_host.hpp), once per game frame;
// - any other game: ReShade's present event (before ReShade's own effects and overlay).
// Before Present, NR's motion is the game's own TAA motion once proved (the finder below), else motion
// estimated from the image (nr_runtime.hpp). HDR10 and scRGB go through an invertible curve around NR.

namespace dlss5 {
namespace adv = mfgunlock::ui::advanced;
namespace ui = mfgunlock::ui;

namespace internal {
inline mfgunlock::nr::Runtime g_runtime;
inline std::atomic_bool g_located{false};
// The swap chain's encoding as ReShade last reported it (its present event runs for every game).
inline std::atomic<int> g_encoding{static_cast<int>(mfgunlock::nr::Encoding::kSdr)};
inline mfgunlock::nr::Encoding CurrentEncoding() { return static_cast<mfgunlock::nr::Encoding>(g_encoding.load()); }
// The option's index in adv::kDlss5.
enum Index : size_t {
  kEnabled, kNetwork, kStyle, kIntensity, kTone, kStructure, kSkin, kProtectHud, kScale, kStability, kHookPoint,
  kPaperWhite, kMotion, kTaaFinder, kToggleKey, kCompareKey, kKeyModifier, kVramMargin, kCoexist, kAllowUnsigned
};
inline int Setting(Index i) { return _wtoi(adv::Current(adv::kDlss5[i]).c_str()); }
// Off by the on/off key: until pressed again, the session runs without NR (the setting stays).
inline std::atomic_bool g_bypassed{false};
// When ReShade last rendered effects (Vulkan: NR runs in begin_effects then, else in Present).
inline std::atomic<uint64_t> g_effects_tick{0};
inline bool Enabled() { return Setting(kEnabled) != 0 && !g_bypassed.load(std::memory_order_relaxed); }
inline mfgunlock::nr::Look CurrentLook() {
  mfgunlock::nr::Look look;
  look.preset = Setting(kNetwork) == 1 ? 1u : 0u;
  look.style = static_cast<unsigned>(std::clamp(Setting(kStyle), 0, 2));
  look.intensity = std::clamp(Setting(kIntensity), 0, 200);
  look.tone = std::clamp(Setting(kTone), 0, 200);
  look.structure = std::clamp(Setting(kStructure), 0, 200);
  look.skin = std::clamp(Setting(kSkin), 0, 200);
  look.automask = Setting(kProtectHud) != 0;
  return look;
}
// Once, on the first frame with DLSS 5 on: where the runtime is and whether it may run.
inline void Locate() {
  if (g_located.exchange(true)) return;
  wchar_t configured[MAX_PATH]{};
  GetPrivateProfileStringW(L"DLSS5", L"RuntimePath", L"", configured, MAX_PATH,
                           (mfgunlock::backend::Directory() / L"MFGAdaUnlock.ini").c_str());
  g_runtime.Locate(mfgunlock::backend::Directory(), configured, Setting(kAllowUnsigned) != 0);
}

// Windows' SDR content brightness of the display (its "SDR white level"), in nits; 0 when unknown.
inline float WindowsSdrWhite() {
  static float nits = 0.f;
  static uint64_t checked = 0;
  const uint64_t now = GetTickCount64();
  if (checked != 0 && now - checked < 5000) return nits;
  checked = now;
  UINT32 paths = 0, modes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || paths == 0) return nits;
  std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
  std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) != ERROR_SUCCESS)
    return nits;
  float best = 0.f;
  for (UINT32 i = 0; i < paths; ++i) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = path[i].targetInfo.adapterId;
    white.header.id = path[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
      best = (std::max)(best, white.SDRWhiteLevel / 1000.f * 80.f);
  }
  return nits = best;
}
inline float PaperWhite() {
  const int set = Setting(kPaperWhite);
  if (set > 0) return static_cast<float>(set);
  const float windows = WindowsSdrWhite();
  return windows > 0.f ? windows : 203.f;
}
// The live settings the runtime reads every frame.
inline void Configure() {
  Locate();
  g_runtime.Stability(Setting(kStability));
  g_runtime.Paper(PaperWhite());
  g_runtime.Motion(std::clamp(Setting(kMotion), 0, 2));
  g_runtime.VramMargin(Setting(kVramMargin));
  g_runtime.Coexist(Setting(kCoexist) == 0);
}

// ---- Phase F: the game's own TAA motion and depth (D3D12, before Present).
// Textures the game made as render targets in a two-channel 16-bit format are motion candidates, depth
// targets depth candidates. Each frame: the candidate written last and the depth bound most; their state
// from the game's barriers. The runtime uses them once they agree with the image's estimated motion.
namespace taa {
struct Texture {
  uint32_t width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool depth = false;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  bool state_known = false;
  uint64_t written = 0, binds_frame = 0;
  uint32_t binds = 0;
  int cleared = -1;  // the last depth clear value: 0 or 1 (reversed or not), -1 unknown
};
inline std::mutex g_lock;
inline std::unordered_map<uint64_t, Texture> g_textures;
inline std::atomic<uint64_t> g_frame{1};
inline bool g_registered = false;
inline bool MotionFormat(reshade::api::format f) {
  using F = reshade::api::format;
  return f == F::r16g16_float || f == F::r16g16_typeless || f == F::r16g16_snorm || f == F::r16g16_unorm;
}
inline bool DepthFormat(reshade::api::format f) {
  using F = reshade::api::format;
  return f == F::d32_float || f == F::r32_typeless || f == F::d24_unorm_s8_uint || f == F::r24_g8_typeless ||
         f == F::d32_float_s8_uint || f == F::r32_g8_typeless || f == F::d16_unorm || f == F::r16_typeless;
}
// The format a shader reads the texture with.
inline DXGI_FORMAT ReadFormat(reshade::api::format f) {
  using F = reshade::api::format;
  switch (f) {
    case F::r16g16_float: case F::r16g16_typeless: return DXGI_FORMAT_R16G16_FLOAT;
    case F::r16g16_snorm: return DXGI_FORMAT_R16G16_SNORM;
    case F::r16g16_unorm: return DXGI_FORMAT_R16G16_UNORM;
    case F::d32_float: case F::r32_typeless: return DXGI_FORMAT_R32_FLOAT;
    case F::d24_unorm_s8_uint: case F::r24_g8_typeless: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case F::d32_float_s8_uint: case F::r32_g8_typeless: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case F::d16_unorm: case F::r16_typeless: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
inline void OnInitResource(reshade::api::device* device, const reshade::api::resource_desc& desc,
                           const reshade::api::subresource_data*, reshade::api::resource_usage, reshade::api::resource r) {
  using reshade::api::resource_usage;
  if (device->get_api() != reshade::api::device_api::d3d12 || desc.type != reshade::api::resource_type::texture_2d ||
      desc.texture.samples > 1 || desc.texture.width < 320)
    return;
  const bool motion = MotionFormat(desc.texture.format) && (desc.usage & resource_usage::render_target) != 0;
  const bool depth = DepthFormat(desc.texture.format) && (desc.usage & resource_usage::depth_stencil) != 0 &&
                     (desc.usage & resource_usage::shader_resource) != 0;
  if (!motion && !depth) return;
  Texture t;
  t.width = desc.texture.width;
  t.height = desc.texture.height;
  t.format = ReadFormat(desc.texture.format);
  t.depth = depth;
  std::lock_guard guard(g_lock);
  g_textures[r.handle] = t;
}
inline void OnDestroyResource(reshade::api::device*, reshade::api::resource r) {
  std::lock_guard guard(g_lock);
  g_textures.erase(r.handle);
}
inline void OnBarrier(reshade::api::command_list*, uint32_t count, const reshade::api::resource* resources,
                      const reshade::api::resource_usage*, const reshade::api::resource_usage* states) {
  std::lock_guard guard(g_lock);
  if (g_textures.empty()) return;
  for (uint32_t i = 0; i < count; ++i) {
    const auto it = g_textures.find(resources[i].handle);
    if (it == g_textures.end()) continue;
    // ReShade's usage bits are D3D12's states; "general" and "present" are COMMON.
    const uint32_t bits = static_cast<uint32_t>(states[i]);
    it->second.state = (bits & 0x80000000u) ? D3D12_RESOURCE_STATE_COMMON : static_cast<D3D12_RESOURCE_STATES>(bits);
    it->second.state_known = true;
  }
}
inline void OnBindTargets(reshade::api::command_list* cmd_list, uint32_t count, const reshade::api::resource_view* rtvs,
                          reshade::api::resource_view dsv) {
  auto* device = cmd_list->get_device();
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  std::lock_guard guard(g_lock);
  if (g_textures.empty()) return;
  for (uint32_t i = 0; i < count; ++i) {
    if (rtvs[i].handle == 0) continue;
    const auto it = g_textures.find(device->get_resource_from_view(rtvs[i]).handle);
    if (it != g_textures.end() && !it->second.depth) it->second.written = frame;
  }
  if (dsv.handle != 0) {
    const auto it = g_textures.find(device->get_resource_from_view(dsv).handle);
    if (it != g_textures.end()) {
      if (it->second.binds_frame != frame) {
        it->second.binds_frame = frame;
        it->second.binds = 0;
      }
      ++it->second.binds;
    }
  }
}
inline bool OnClearDepth(reshade::api::command_list* cmd_list, reshade::api::resource_view dsv, const float* depth,
                         const uint8_t*, uint32_t, const reshade::api::rect*) {
  if (depth == nullptr || dsv.handle == 0) return false;
  const auto handle = cmd_list->get_device()->get_resource_from_view(dsv).handle;
  std::lock_guard guard(g_lock);
  const auto it = g_textures.find(handle);
  if (it != g_textures.end()) it->second.cleared = *depth < 0.5f ? 0 : 1;
  return false;
}
// This frame's candidates, sized like the image (any render scale), with a known state.
inline bool Pick(uint32_t width, uint32_t height, mfgunlock::nr::TaaInputs& out) {
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  const double aspect = double(width) / double(height);
  std::lock_guard guard(g_lock);
  uint64_t best_motion = 0, best_depth = 0;
  uint32_t best_area = 0, best_binds = 0;
  for (const auto& [handle, t] : g_textures) {
    if (!t.state_known || t.format == DXGI_FORMAT_UNKNOWN) continue;
    if (std::fabs(double(t.width) / double(t.height) - aspect) > 0.05 || t.width * 2 < width) continue;
    if (!t.depth && t.written + 1 >= frame && t.width * t.height > best_area) {
      best_area = t.width * t.height;
      best_motion = handle;
    }
    if (t.depth && t.binds_frame + 1 >= frame && t.binds > best_binds) {
      best_binds = t.binds;
      best_depth = handle;
    }
  }
  if (best_motion == 0) return false;
  const Texture& m = g_textures[best_motion];
  out.motion = reinterpret_cast<ID3D12Resource*>(best_motion);
  out.motion_format = m.format;
  out.motion_state = m.state;
  if (best_depth != 0) {
    const Texture& d = g_textures[best_depth];
    out.depth = reinterpret_cast<ID3D12Resource*>(best_depth);
    out.depth_format = d.format;
    out.depth_state = d.state;
    out.depth_inverted = d.cleared == 0;
  }
  return true;
}
inline void Register() {
  if (g_registered) return;
  g_registered = true;
  reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
  reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
  reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindTargets);
  reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(&OnClearDepth);
}
inline void Unregister() {
  if (!g_registered) return;
  g_registered = false;
  reshade::unregister_event<reshade::addon_event::init_resource>(&OnInitResource);
  reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
  reshade::unregister_event<reshade::addon_event::barrier>(&OnBarrier);
  reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindTargets);
  reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(&OnClearDepth);
}
}  // namespace taa

// For the Vulkan runtime: where the files are (false when NR may not run).
inline bool LocateFiles(std::wstring& runtime, std::wstring& shim) {
  Locate();
  using mfgunlock::nr::State;
  const State s = g_runtime.state();
  if (s == State::kNoRuntime || s == State::kUnsigned || s == State::kNoShim) return false;
  runtime = g_runtime.runtime_path();
  shim = g_runtime.shim_path();
  return !runtime.empty() && !shim.empty();
}
inline bool GameFrameGeneration() {
  return mfgunlock::framecount::g_actual_frames_presented.load(std::memory_order_relaxed) > 1 ||
         (mfgunlock::xmx::generator::Enabled() && mfgunlock::xmx::generator::Snapshot().generating);
}
inline void Run(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, mfgunlock::nr::Encoding encoding) {
  Configure();
  mfgunlock::nr::TaaInputs found;
  const auto desc = backbuffer->GetDesc();
  const bool have = taa::g_registered && Setting(kMotion) != 2 &&
                    taa::Pick(static_cast<uint32_t>(desc.Width), desc.Height, found);
  g_runtime.Apply(queue, backbuffer, D3D12_RESOURCE_STATE_PRESENT, CurrentLook(), std::clamp(Setting(kScale), 25, 100),
                  encoding, have ? &found : nullptr);
}
// The current back buffer of a swap chain (Streamline's proxy hands out the one the game rendered).
inline void RunOn(IUnknown* chain, ID3D12CommandQueue* queue) {
  static std::atomic_int reported{0};
  const auto report = [](int step, HRESULT hr) {
    if (reported.exchange(step) == step) return;
    char text[160];
    std::snprintf(text, sizeof(text), "mfgunlock: DLSS 5: the swap chain did not give %s (0x%08x).",
                  step == 1 ? "IDXGISwapChain3" : step == 2 ? "its command queue" : "its back buffer",
                  static_cast<unsigned>(hr));
    reshade::log::message(reshade::log::level::warning, text);
  };
  IDXGISwapChain3* chain3 = nullptr;
  HRESULT hr = chain ? chain->QueryInterface(IID_PPV_ARGS(&chain3)) : E_POINTER;
  if (FAILED(hr)) return report(1, hr);
  ID3D12CommandQueue* own_queue = nullptr;
  if (queue == nullptr) {  // D3D12: GetDevice on a swap chain gives its queue
    hr = chain3->GetDevice(IID_PPV_ARGS(&own_queue));
    if (FAILED(hr)) report(2, hr);
    queue = own_queue;
  }
  ID3D12Resource* backbuffer = nullptr;
  if (queue != nullptr) {
    hr = chain3->GetBuffer(chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr)) report(3, hr);
  }
  if (backbuffer != nullptr) {
    Run(queue, backbuffer, CurrentEncoding());
    backbuffer->Release();
  }
  if (own_queue) own_queue->Release();
  chain3->Release();
}
// After the game's DLSS SR or Ray Reconstruction (phase A): NR on its output, on its command list, with its
// motion vectors and depth. [DLSS5] HookPoint: 0 Auto (here when the game has DLSS, else at Present),
// 1 here only, 2 Present only. Auto stays here for the session once it ran here.
enum HookPoint { kHookAuto = 0, kHookUpscaled = 1, kHookPresent = 2 };
inline std::atomic<uint64_t> g_upscaled_tick{0};
inline std::atomic<uint64_t> g_prefg_seen{0};
// REFramework (Capcom's RE Engine games, which need it with ReShade) resizes its own back-buffer views on
// the game's resize thread while Present runs: with NR's work in Present the game's first resize leaves
// REFramework drawing to a freed back buffer (RE Requiem: a GPU page fault in its overlay a few seconds in).
// Auto then runs NR only after the game's upscaler.
inline bool ReframeworkLoaded() {
  static std::atomic<int> known{-1};
  static std::atomic<uint64_t> checked{0};
  if (known.load(std::memory_order_relaxed) == 1) return true;
  const uint64_t now = GetTickCount64();
  if (now - checked.load(std::memory_order_relaxed) < 5000) return false;
  checked.store(now, std::memory_order_relaxed);
  const HMODULE m = GetModuleHandleW(L"dinput8.dll");
  wchar_t path[MAX_PATH]{};
  if (m == nullptr || !GetModuleFileNameW(m, path, MAX_PATH)) return false;
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
  if (size == 0) return false;
  std::vector<uint8_t> data(size);
  if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
  struct Lang { WORD language, codepage; }* langs = nullptr;
  UINT bytes = 0;
  if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &bytes) || bytes < sizeof(Lang))
    return false;
  wchar_t key[64];
  std::swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductName", langs[0].language, langs[0].codepage);
  wchar_t* name = nullptr;
  if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&name), &bytes) || name == nullptr) return false;
  if (std::wcsstr(name, L"REFramework") == nullptr) return false;
  known.store(1, std::memory_order_relaxed);
  reshade::log::message(reshade::log::level::info,
                        "mfgunlock: DLSS 5: REFramework is loaded: with Where NR runs = Auto, NR runs only after the "
                        "game's upscaler (before Present it breaks REFramework's overlay on a resize).");
  return true;
}
inline bool PresentRoutes() {
  const int point = Setting(kHookPoint);
  if (point == kHookAuto && ReframeworkLoaded()) return false;
  const bool prefg = GetTickCount64() - g_prefg_seen.load(std::memory_order_relaxed) <= 1000;
  if (point == kHookPresent) return !prefg;  // the real frame before frame generation stands for Present there
  return point == kHookAuto && g_upscaled_tick.load(std::memory_order_relaxed) == 0 && !prefg;
}
// Why the route after the upscaler did not run, once per reason, for a tester's log.
inline void Skipped(int reason, const char* why) {
  static std::atomic<int> seen{0};
  if (seen.fetch_or(1 << reason) & (1 << reason)) return;
  reshade::log::message(reshade::log::level::info, (std::string("mfgunlock: DLSS 5: after the upscaler, skipped: ") + why).c_str());
}
inline void AfterUpscaler(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* p, int flags) {
  if (!Enabled()) return Skipped(0, "DLSS 5 off");
  if (Setting(kHookPoint) == kHookPresent) return Skipped(1, "Where NR runs is Present");
  const auto resource = [&](const char* name) {
    ID3D12Resource* r = nullptr;
    p->Get(name, &r);
    return r;
  };
  const auto number = [&](const char* name, unsigned fallback) {
    unsigned v = fallback;
    p->Get(name, &v);
    return v;
  };
  const auto real = [&](const char* name, float fallback) {
    float v = fallback;
    p->Get(name, &v);
    return v;
  };
  ID3D12Resource* output = resource(NVSDK_NGX_Parameter_Output);
  ID3D12Resource* mvec = resource(NVSDK_NGX_Parameter_MotionVectors);
  ID3D12Resource* depth = resource(NVSDK_NGX_Parameter_Depth);
  if (!output || !mvec || !depth) return Skipped(2, "the call has no output, motion vectors or depth");
  // One upscale a frame is the picture: a smaller second one (a mirror, a scope) is left alone.
  static std::atomic<uint64_t> widest{0};
  const uint64_t width = output->GetDesc().Width;
  if (width * 4 < widest.load(std::memory_order_relaxed) * 3) return Skipped(3, "a smaller second upscale");
  if (width > widest.load(std::memory_order_relaxed)) widest.store(width, std::memory_order_relaxed);
  mfgunlock::nr::Guides g;
  const auto dd = depth->GetDesc();
  const auto od = output->GetDesc();
  if (Setting(kMotion) != 2) {
    g.mvec = mvec;
    g.depth = depth;
  }
  g.depth_w = number(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, static_cast<unsigned>(dd.Width));
  g.depth_h = number(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, dd.Height);
  g.depth_x = number(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
  g.depth_y = number(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
  const bool low_res = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
  g.mv_w = low_res ? g.depth_w : static_cast<uint32_t>(od.Width);
  g.mv_h = low_res ? g.depth_h : od.Height;
  g.mv_x = number(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
  g.mv_y = number(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
  g.mv_scale_x = real(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
  g.mv_scale_y = real(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
  g.depth_inverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  int reset = 0;
  p->Get(NVSDK_NGX_Parameter_Reset, &reset);
  g.reset = reset != 0;
  Configure();
  if (!g_runtime.ApplyAfterUpscale(list, output, g, CurrentLook(), std::clamp(Setting(kScale), 25, 100))) {
    char why[96];
    std::snprintf(why, sizeof(why), "the runtime declined (state %d, output format %d)", static_cast<int>(g_runtime.state()),
                  static_cast<int>(od.Format));
    return Skipped(4, why);
  }
  if (g_upscaled_tick.exchange(GetTickCount64(), std::memory_order_relaxed) == 0) {
    char text[200];
    std::snprintf(text, sizeof(text),
                  "mfgunlock: DLSS 5: after the game's upscaler: output %llux%u (format %d), guides %ux%u, "
                  "vector scale %.3g x %.3g, depth %s.",
                  static_cast<unsigned long long>(od.Width), od.Height, static_cast<int>(od.Format), g.depth_w,
                  g.depth_h, g.mv_scale_x, g.mv_scale_y, g.depth_inverted ? "inverted" : "normal");
    reshade::log::message(reshade::log::level::info, text);
  }
}
// Before the game's frame generation (NGX feature 11, NVIDIA's or XeMFG DLSS-G): its real frame, with the
// motion vectors and depth it was given, when no upscaler call reached NR in the last half second.
inline std::atomic<uint64_t> g_prefg_tick{0};
inline void Skipped(int reason, const char* why);
// Also for HookPoint Present: frame generation's real frame is the final image, HUD included, and the
// Present ReShade sees in such a game also carries the generated frames (TLOU2).
inline void BeforeFrameGeneration(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* p) {
  if (!Enabled()) return;
  if (Setting(kHookPoint) != kHookPresent && GetTickCount64() - g_upscaled_tick.load(std::memory_order_relaxed) < 500)
    return;  // done after the upscaler
  ID3D12Resource *backbuffer = nullptr, *mvec = nullptr, *depth = nullptr;
  p->Get("DLSSG.Backbuffer", &backbuffer);
  p->Get("DLSSG.MVecs", &mvec);
  p->Get("DLSSG.Depth", &depth);
  if (!backbuffer) return Skipped(5, "frame generation's call has no back buffer");
  g_prefg_seen.store(GetTickCount64(), std::memory_order_relaxed);
  // Multi-frame generation calls once per generated frame (three times a real frame at 4x), each with the
  // same real frame: NR once, on the first. Again on the same frame it would process its own output and
  // move its history three times a frame (TLOU2 at 4x: a dark, crushed image with sparkles).
  unsigned index = 0;
  p->Get("DLSSG.MultiFrameIndex", &index);
  if (index > 1) return;
  mfgunlock::nr::Guides g;
  if (Setting(kMotion) != 2 && mvec && depth) {
    const auto md = mvec->GetDesc(), dd = depth->GetDesc();
    g.mvec = mvec;
    g.depth = depth;
    g.mv_w = static_cast<uint32_t>(md.Width);
    g.mv_h = md.Height;
    g.depth_w = static_cast<uint32_t>(dd.Width);
    g.depth_h = dd.Height;
    // Streamline's scale takes the vectors to the screen's 0..1; NR wants pixels of their texture.
    const float sx = mfgunlock::framecount::internal::g_sl_mvec_scale_x.load(std::memory_order_relaxed);
    const float sy = mfgunlock::framecount::internal::g_sl_mvec_scale_y.load(std::memory_order_relaxed);
    g.mv_scale_x = sx != 0.f ? sx * g.mv_w : 1.f;
    g.mv_scale_y = sy != 0.f ? sy * g.mv_h : 1.f;
    g.depth_inverted = mfgunlock::framecount::internal::g_sl_depth_inverted.load(std::memory_order_relaxed) == 1;
  }
  // Frame generation's inputs are read by compute: the state the game's Streamline left them in.
  D3D12_COMMAND_LIST_TYPE type = list->GetType();
  const D3D12_RESOURCE_STATES state = type == D3D12_COMMAND_LIST_TYPE_DIRECT
                                          ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                          : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  Configure();
  if (!g_runtime.ApplyAfterUpscale(list, backbuffer, g, CurrentLook(), std::clamp(Setting(kScale), 25, 100), state,
                                   static_cast<int>(CurrentEncoding()), 4)) {
    char why[96];
    std::snprintf(why, sizeof(why), "before frame generation, the runtime declined (state %d)", static_cast<int>(g_runtime.state()));
    return Skipped(6, why);
  }
  if (g_prefg_tick.exchange(GetTickCount64(), std::memory_order_relaxed) == 0) {
    const auto bd = backbuffer->GetDesc();
    char text[220];
    std::snprintf(text, sizeof(text),
                  "mfgunlock: DLSS 5: before the game's frame generation: frame %llux%u (format %d), guides %ux%u, "
                  "vector scale %.3g x %.3g, depth %s.",
                  static_cast<unsigned long long>(bd.Width), bd.Height, static_cast<int>(bd.Format), g.mv_w, g.mv_h,
                  g.mv_scale_x, g.mv_scale_y, g.depth_inverted ? "inverted" : "normal");
    reshade::log::message(reshade::log::level::info, text);
  }
}
// The game's Present on its Streamline's proxy, before Streamline's frame generation (framecount.hpp).
inline std::atomic_bool g_proxy_frames{false};
inline void ProxyFrame(IUnknown* chain, IUnknown* queue_object) {
  if (!Enabled()) {
    g_runtime.Off();
    return;
  }
  if (!PresentRoutes()) return;
  if (!g_proxy_frames.exchange(true, std::memory_order_relaxed))
    reshade::log::message(reshade::log::level::info,
                          "mfgunlock: DLSS 5: on the game's Present to Streamline, before its frame generation.");
  ID3D12CommandQueue* queue = nullptr;
  if (queue_object != nullptr) queue_object->QueryInterface(IID_PPV_ARGS(&queue));
  RunOn(chain, queue);
  if (queue) queue->Release();
}
// The hosted Streamline's Present, before Streamline gets the frame (sl_host.hpp).
inline void HostFrame(IDXGISwapChain* chain, ID3D12CommandQueue* queue) {
  if (!Enabled()) {
    g_runtime.Off();
    return;
  }
  if (!PresentRoutes()) return;
  RunOn(chain, queue);
}

// ---- Hotkeys: on/off and compare, with the modifier, while the game's window has the focus.
inline std::string KeyName(int key) {
  if (key <= 0) return {};
  const int modifier = std::clamp(Setting(kKeyModifier), 0, 3);
  static const char* kModifiers[] = {"", "Shift+", "Ctrl+", "Alt+"};
  char name[64]{};
  UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
  if (key >= VK_F1 && key <= VK_F24) std::snprintf(name, sizeof(name), "F%d", key - VK_F1 + 1);
  else if (!scan || !GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name))) std::snprintf(name, sizeof(name), "#%d", key);
  return std::string(kModifiers[modifier]) + name;
}
inline bool Pressed(int key, bool& was_down) {
  if (key <= 0 || key > 254) return false;
  const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
  const bool pressed = down && !was_down;
  was_down = down;
  if (!pressed) return false;
  DWORD owner = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &owner);
  if (owner != GetCurrentProcessId()) return false;
  const int modifiers[] = {0, VK_SHIFT, VK_CONTROL, VK_MENU};
  const int modifier = modifiers[std::clamp(Setting(kKeyModifier), 0, 3)];
  return modifier == 0 || (GetAsyncKeyState(modifier) & 0x8000) != 0;
}
inline void PollKeys() {
  static bool toggle_down = false, compare_down = false;
  if (Pressed(Setting(kToggleKey), toggle_down)) {
    const bool off = !g_bypassed.load();
    g_bypassed.store(off);
    if (off) g_runtime.Off();
    reshade::log::message(reshade::log::level::info, off ? "mfgunlock: DLSS 5: off by the on/off key." :
                                                           "mfgunlock: DLSS 5: on again by the on/off key.");
  }
  if (Pressed(Setting(kCompareKey), compare_down)) g_runtime.Compare(!mfgunlock::nr::g_status.compare.load());
}

// ---- Looks: network, style and strengths under a name. Built-in ones, and the player's in
// [DLSS5.Looks] name=network,style,intensity,tone,structure,skin.
struct LookValues {
  int v[6];  // network, style, intensity, tone, structure, skin
  bool operator==(const LookValues& o) const { return std::equal(std::begin(v), std::end(v), std::begin(o.v)); }
};
inline constexpr Index kLookKeys[6] = {kNetwork, kStyle, kIntensity, kTone, kStructure, kSkin};
struct BuiltinLook { Str name; LookValues values; };
inline const BuiltinLook kBuiltinLooks[] = {
    {Str::kDlss5LookDefault_n85, {{0, 0, 100, 100, 100, 100}}},
    {Str::kDlss5LookNatural_n86, {{0, 1, 90, 90, 100, 100}}},
    {Str::kDlss5LookCinematic_n87, {{0, 2, 110, 120, 110, 100}}},
    {Str::kDlss5LookSubtle_n88, {{0, 1, 60, 70, 80, 100}}},
    {Str::kDlss5LookOriginal_n89, {{1, 0, 100, 100, 100, 100}}},
};
inline std::filesystem::path Ini() { return mfgunlock::backend::Directory() / L"MFGAdaUnlock.ini"; }
inline LookValues CurrentValues() {
  LookValues l{};
  for (int i = 0; i < 6; ++i) l.v[i] = Setting(kLookKeys[i]);
  return l;
}
inline void ApplyValues(const LookValues& l) {
  for (int i = 0; i < 6; ++i) adv::Store(adv::kDlss5[kLookKeys[i]], std::to_wstring(l.v[i]));
}
inline std::vector<std::pair<std::string, LookValues>> SavedLooks() {
  std::vector<std::pair<std::string, LookValues>> out;
  wchar_t keys[4096]{};
  GetPrivateProfileSectionW(L"DLSS5.Looks", keys, 4096, Ini().c_str());
  for (const wchar_t* k = keys; *k; k += wcslen(k) + 1) {
    const wchar_t* eq = wcschr(k, L'=');
    if (!eq) continue;
    LookValues l{};
    if (swscanf_s(eq + 1, L"%d,%d,%d,%d,%d,%d", &l.v[0], &l.v[1], &l.v[2], &l.v[3], &l.v[4], &l.v[5]) != 6) continue;
    char name[128]{};
    WideCharToMultiByte(CP_UTF8, 0, k, static_cast<int>(eq - k), name, sizeof(name) - 1, nullptr, nullptr);
    out.emplace_back(name, l);
  }
  return out;
}
inline void SaveLook(const std::string& name, const LookValues& l) {
  wchar_t wname[128]{}, value[96];
  MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 127);
  std::swprintf(value, 96, L"%d,%d,%d,%d,%d,%d", l.v[0], l.v[1], l.v[2], l.v[3], l.v[4], l.v[5]);
  WritePrivateProfileStringW(L"DLSS5.Looks", wname, value, Ini().c_str());
}
inline void DeleteLook(const std::string& name) {
  wchar_t wname[128]{};
  MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 127);
  WritePrivateProfileStringW(L"DLSS5.Looks", wname, nullptr, Ini().c_str());
}
}  // namespace internal

// ReShade's present event: every game; the keys, the encoding, and NR for games without another route.
inline void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect*,
                      const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  using namespace internal;
  taa::g_frame.fetch_add(1, std::memory_order_relaxed);
  if (Setting(kEnabled) != 0) PollKeys();
  if (!Enabled()) {
    g_runtime.Off();
    return;
  }
  if (queue == nullptr || swapchain == nullptr) return;
  // The encoding, for every route.
  using mfgunlock::nr::Encoding;
  const auto color_space = swapchain->get_color_space();
  g_encoding.store(static_cast<int>(color_space == reshade::api::color_space::hdr10_st2084      ? Encoding::kPq
                                    : color_space == reshade::api::color_space::extended_srgb_linear ? Encoding::kScRgb
                                                                                                     : Encoding::kSdr));
  if (queue->get_device()->get_api() == reshade::api::device_api::vulkan) {
    // ReShade's begin_effects runs it when ReShade renders effects; here only when it does not (no effects).
    if (GetTickCount64() - g_effects_tick.load(std::memory_order_relaxed) > 1000)
      mfgunlock::nr::vk::Present(queue, swapchain->get_current_back_buffer(), reshade::api::resource_usage::present,
                                 CurrentLook(), std::clamp(Setting(kScale), 25, 100), CurrentEncoding(), PaperWhite(),
                                 Setting(kStability));
    return;
  }
  if (queue->get_device()->get_api() == reshade::api::device_api::d3d12) {
    auto* native = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (native != nullptr && SUCCEEDED(native->GetDevice(IID_PPV_ARGS(&device))) &&
        FAILED(device->GetDeviceRemovedReason())) {
      mfgunlock::nr::internal::Runtime::Dred(device.Get());
      return;
    }
  }
  // The host's Present and the game's Present on Streamline's proxy run it for those games.
  if (mfgunlock::slhost::Hosting() || g_proxy_frames.load(std::memory_order_relaxed) || !PresentRoutes()) return;
  if (queue->get_device()->get_api() != reshade::api::device_api::d3d12) return;
  if (GameFrameGeneration()) {
    // Here ReShade also sees the generated frames: NR needs the hook on the game's Present, set at load.
    g_runtime.Pause(mfgunlock::nr::State::kWithGameFg);
    return;
  }
  Run(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()),
      reinterpret_cast<ID3D12Resource*>(swapchain->get_current_back_buffer().handle), CurrentEncoding());
}
// Vulkan: right before ReShade's effects, after every add-on's Present (nr_vulkan.hpp).
inline void OnBeginEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list*, reshade::api::resource_view,
                           reshade::api::resource_view) {
  using namespace internal;
  if (!Enabled() || runtime == nullptr || runtime->get_device()->get_api() != reshade::api::device_api::vulkan) return;
  g_effects_tick.store(GetTickCount64(), std::memory_order_relaxed);
  mfgunlock::nr::vk::Present(runtime->get_command_queue(), runtime->get_current_back_buffer(),
                             reshade::api::resource_usage::render_target, CurrentLook(), std::clamp(Setting(kScale), 25, 100),
                             CurrentEncoding(), PaperWhite(), Setting(kStability));
}
inline void Register() {
  // Opt-in crash diagnostics: DRED must be on before the game makes its D3D12 device.
  if (internal::Setting(internal::kEnabled) != 0 &&
      GetPrivateProfileIntW(L"Diagnostics", L"ForceDRED", 0, internal::Ini().c_str()) != 0) {
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
      dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    AddVectoredExceptionHandler(1, &mfgunlock::nr::internal::AnyFault);
  }
  mfgunlock::slhost::internal::on_game_frame_nr.store(&internal::HostFrame, std::memory_order_release);
  mfgunlock::framecount::internal::g_on_proxy_present_nr.store(&internal::ProxyFrame, std::memory_order_release);
  mfgunlock::nr::upscaler::g_after.store(&internal::AfterUpscaler, std::memory_order_release);
  mfgunlock::nr::upscaler::g_before_fg.store(&internal::BeforeFrameGeneration, std::memory_order_release);
  reshade::register_event<reshade::addon_event::present>(&OnPresent);
  reshade::register_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
  mfgunlock::nr::vk::g_locate.store(&internal::LocateFiles);
  // Vulkan: NR's device extensions go in when the game makes its device (wanted at load).
  if (internal::Setting(internal::kEnabled) != 0) mfgunlock::nr::vk::Hook();
  // The TAA finder watches the game's resources: only when wanted at load (DLSS 5 on, motion not off).
  if (internal::Setting(internal::kEnabled) != 0 && internal::Setting(internal::kTaaFinder) != 0 &&
      internal::Setting(internal::kMotion) != 2)
    internal::taa::Register();
}
inline void Unregister() {
  mfgunlock::slhost::internal::on_game_frame_nr.store(nullptr, std::memory_order_release);
  mfgunlock::framecount::internal::g_on_proxy_present_nr.store(nullptr, std::memory_order_release);
  mfgunlock::nr::upscaler::g_after.store(nullptr, std::memory_order_release);
  mfgunlock::nr::upscaler::g_before_fg.store(nullptr, std::memory_order_release);
  reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
  reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
  internal::taa::Unregister();
}

// For the support report.
inline std::string Describe() {
  using mfgunlock::nr::State;
  const auto& r = internal::g_runtime;
  static const char* kNames[] = {"off", "no runtime", "unsigned runtime refused", "shim missing", "runtime refused the GPU",
                                 "unsupported format", "game frame generation", "failed", "running", "paused for VRAM",
                                 "standing aside for another DLSS 5 mod"};
  std::string text = kNames[static_cast<int>(r.state())];
  if (internal::g_bypassed.load()) text += " (off by key)";
  if (!r.runtime_path().empty()) {
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%ls", r.runtime_path().c_str());
    text += std::string("; runtime ") + path + (r.runtime_signed() ? " (signed by NVIDIA)" : " (unsigned)");
  }
  if (r.state() == State::kRunning) {
    const auto& st = mfgunlock::nr::g_status;
    static const char* kRoutes[] = {"", "before Present", "after upscaling", "Vulkan before Present",
                                    "before frame generation"};
    static const char* kGuides[] = {"no motion", "game motion+depth", "estimated motion", "TAA motion"};
    char t[160];
    std::snprintf(t, sizeof(t), "; %ux%u, %.1f ms GPU, %s, %s", r.width(), r.height(), r.gpu_ms(),
                  kRoutes[std::clamp(st.route.load(), 0, 4)], kGuides[std::clamp(st.guides.load(), 0, 3)]);
    text += t;
  }
  return text;
}

// ---- The panel's DLSS 5 page.
namespace internal {
inline void DrawStatus() {
  using mfgunlock::nr::State;
  const auto& r = g_runtime;
  const auto& st = mfgunlock::nr::g_status;
  const State s = Setting(kEnabled) != 0 ? r.state() : State::kOff;
  std::string text;
  ImVec4 color = ui::Warning;
  if (Setting(kEnabled) != 0 && g_bypassed.load()) {
    text = T(Str::kDlss5StateBypassed_n66);
    color = ui::Muted;
  } else if (s == State::kRunning || (st.route.load() == 3 && !mfgunlock::nr::vk::NeedsRestart())) {
    const int route = std::clamp(st.route.load(), 0, 4), guides = std::clamp(st.guides.load(), 0, 3);
    const Str routes[] = {Str::kDlss5RoutePresent_n77, Str::kDlss5RoutePresent_n77, Str::kDlss5RouteUpscaled_n76,
                          Str::kDlss5RouteVulkan_n93, Str::kDlss5RoutePreFg_n96};
    const Str kinds[] = {Str::kDlss5GuidesNone_n81, Str::kDlss5GuidesGame_n78, Str::kDlss5GuidesEstimated_n79,
                         Str::kDlss5GuidesTaa_n80};
    char what[256];
    char cost[32] = "";
    if (st.ms.load() > 0.f) std::snprintf(cost, sizeof(cost), " · %.1f ms", st.ms.load());
    std::snprintf(what, sizeof(what), "%s, %s · %ux%u%s", T(routes[route]), T(kinds[guides]), st.width.load(), st.height.load(),
                  cost);
    char line[320];
    std::snprintf(line, sizeof(line), T(Str::kDlss5StateRunning_n28), what);
    text = line;
    color = ui::Accent;
  } else if (s == State::kForeign) {
    char name[MAX_PATH]{}, line[MAX_PATH + 200];
    std::snprintf(name, sizeof(name), "%ls", r.foreign().c_str());
    std::snprintf(line, sizeof(line), T(Str::kDlss5StateForeign_n65), name);
    text = line;
  } else {
    text = s == State::kNoRuntime    ? T(Str::kDlss5StateNoRuntime_n29)
           : s == State::kUnsigned   ? T(Str::kDlss5StateUnsigned_n30)
           : s == State::kNoShim     ? T(Str::kDlss5StateNoShim_n31)
           : s == State::kInitFailed ? T(Str::kDlss5StateInitFailed_n32)
           : s == State::kFormat     ? T(Str::kDlss5StateFormat_n33)
           : s == State::kWithGameFg ? T(Str::kDlss5StateGameFg_n34)
           : s == State::kFailed     ? T(Str::kDlss5StateFailed_n35)
           : s == State::kVram       ? T(Str::kDlss5StateVram_n64)
           : mfgunlock::nr::vk::NeedsRestart() ? T(Str::kDlss5StateVulkanExt_n91)
                                     : T(Str::kDlss5StateOff_n27);
    color = s == State::kOff ? ui::Muted : ui::Warning;
  }
  ui::StatusChip(text.c_str(), color);
  if (st.compare.load()) ui::Notice(T(Str::kDlss5Comparing_n82), ui::Accent);
  const std::string toggle = KeyName(Setting(kToggleKey)), compare = KeyName(Setting(kCompareKey));
  if (!toggle.empty() && !compare.empty()) {
    char hint[192];
    std::snprintf(hint, sizeof(hint), T(Str::kDlss5Hotkeys_n83), toggle.c_str(), compare.c_str());
    ui::Description(hint);
  }
}
inline void DrawLooks() {
  const LookValues now = CurrentValues();
  const auto saved = SavedLooks();
  std::string current = T(Str::kDlss5LookCustom_n84);
  for (const auto& b : kBuiltinLooks)
    if (b.values == now) current = T(b.name);
  for (const auto& [name, values] : saved)
    if (values == now) current = name;
  static char name_buffer[64] = "";
  ui::Setting("##dlss5_look", T(Str::kDlss5LookPreset_n71), T(Str::kDlss5LookPresetDesc_n72), [&] {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##dlss5_look_combo", current.c_str())) {
      for (const auto& b : kBuiltinLooks)
        if (ImGui::Selectable(T(b.name), b.values == now)) ApplyValues(b.values);
      if (!saved.empty()) ImGui::Separator();
      for (const auto& [name, values] : saved)
        if (ImGui::Selectable(name.c_str(), values == now)) ApplyValues(values);
      ImGui::EndCombo();
    }
  });
  ui::Setting("##dlss5_look_save", T(Str::kDlss5LookName_n75), T(Str::kDlss5LookPresetDesc_n72), [&] {
    const float button = ImGui::CalcTextSize(T(Str::kDlss5SaveLook_n73)).x + ImGui::CalcTextSize(T(Str::kDlss5DeleteLook_n74)).x +
                         ImGui::GetStyle().FramePadding.x * 4.f + ImGui::GetStyle().ItemSpacing.x * 2.f;
    ImGui::SetNextItemWidth((std::max)(80.f, ImGui::GetContentRegionAvail().x - button));
    ImGui::InputTextWithHint("##dlss5_look_name", current.c_str(), name_buffer, sizeof(name_buffer));
    ImGui::SameLine();
    const std::string typed = name_buffer;
    ImGui::BeginDisabled(typed.empty() || typed.find('=') != std::string::npos);
    if (ImGui::Button(T(Str::kDlss5SaveLook_n73))) {
      SaveLook(typed, now);
      name_buffer[0] = 0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool is_saved = false;
    for (const auto& [name, values] : saved) is_saved |= name == current;
    ImGui::BeginDisabled(!is_saved);
    if (ImGui::Button(T(Str::kDlss5DeleteLook_n74))) DeleteLook(current);
    ImGui::EndDisabled();
  });
}
}  // namespace internal

inline void DrawPage() {
  using namespace internal;
  const bool changed = adv::Changed();
  {
    ui::Card card("##dlss5_hero");
    adv::DrawOption(adv::kDlss5[kEnabled]);
    DrawStatus();
  }
  if (ui::SectionHeader(T(Str::kDlss5SectionLook_n67), true)) {
    ui::Card card("##dlss5_look_card");
    DrawLooks();
    for (Index i : {kNetwork, kStyle, kIntensity, kTone, kStructure, kSkin}) adv::DrawOption(adv::kDlss5[i]);
  }
  if (ui::SectionHeader(T(Str::kDlss5SectionQuality_n68), true)) {
    ui::Card card("##dlss5_quality_card");
    for (Index i : {kScale, kHookPoint, kMotion, kTaaFinder, kStability, kProtectHud}) adv::DrawOption(adv::kDlss5[i]);
  }
  const bool hdr = CurrentEncoding() != mfgunlock::nr::Encoding::kSdr;
  if (ui::SectionHeader(T(Str::kDlss5SectionHdr_n69), hdr)) {
    ui::Card card("##dlss5_hdr_card");
    adv::DrawOption(adv::kDlss5[kPaperWhite]);
    const float windows = WindowsSdrWhite();
    if (windows > 0.f) {
      char line[96];
      std::snprintf(line, sizeof(line), T(Str::kDlss5PaperAuto_n90), static_cast<int>(windows + 0.5f));
      ui::Description(line);
    }
  }
  if (ui::SectionHeader(T(Str::kDlss5SectionAdvanced_n70), false)) {
    ui::Card card("##dlss5_advanced_card");
    for (Index i : {kToggleKey, kCompareKey, kKeyModifier, kVramMargin, kCoexist}) adv::DrawOption(adv::kDlss5[i]);
    const auto& r = g_runtime;
    if (!r.runtime_path().empty()) {
      char path[MAX_PATH + 32]{};
      std::snprintf(path, sizeof(path), "%ls%s", r.runtime_path().c_str(), r.runtime_signed() ? "" : " (unsigned)");
      ui::Readout("##dlss5_runtime", T(Str::kDlss5Runtime_n36), path, T(Str::kDlss5AllowUnsignedDesc_n4));
    }
    // Live settings above; the restart notice only for these (read at load).
    adv::Changed() = changed;
    adv::DrawOption(adv::kDlss5[kAllowUnsigned]);
  } else {
    adv::Changed() = changed;
  }
  adv::Result();
}
}  // namespace dlss5
