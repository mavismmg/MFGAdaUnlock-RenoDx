// SPDX-License-Identifier: MIT
// 1.12.0 (DLSS 5): the panel's DLSS 5 page and where neural rendering meets the game's frames
// (nr_runtime.hpp does the work). Included by addon.cpp inside its anonymous namespace, before the panel.
//
// Three places, all before frame generation sees the frame:
// - a game with its own Streamline (and so its own frame generation, NVIDIA's or XeMFG DLSS-G): the game's
//   Present on Streamline's proxy swap chain, hooked at load when [DLSS5] Enabled=1 (framecount.hpp);
// - Add frame generation: the hosted Streamline's Present (sl_host.hpp), once per game frame;
// - any other game: ReShade's present event (before ReShade's own effects and overlay).
// HDR10 and scRGB go through an invertible curve around NR (nr_runtime.hpp); the encoding comes from the
// color space ReShade reports for the swap chain.

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
enum Index : size_t { kEnabled, kNetwork, kStyle, kIntensity, kTone, kStructure, kSkin, kProtectHud, kScale, kStability, kAllowUnsigned };
inline int Setting(Index i) { return _wtoi(adv::Current(adv::kDlss5[i]).c_str()); }
inline bool Enabled() { return Setting(kEnabled) != 0; }
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
inline bool GameFrameGeneration() {
  return mfgunlock::framecount::g_actual_frames_presented.load(std::memory_order_relaxed) > 1 ||
         (mfgunlock::xmx::generator::Enabled() && mfgunlock::xmx::generator::Snapshot().generating);
}
inline void Run(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, mfgunlock::nr::Encoding encoding) {
  Locate();
  g_runtime.Stability(Setting(kStability));
  g_runtime.Apply(queue, backbuffer, D3D12_RESOURCE_STATE_PRESENT, CurrentLook(), std::clamp(Setting(kScale), 25, 100),
                  encoding);
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
// The game's Present on its Streamline's proxy, before Streamline's frame generation (framecount.hpp).
inline std::atomic_bool g_proxy_frames{false};
inline void ProxyFrame(IUnknown* chain, IUnknown* queue_object) {
  if (!Enabled()) {
    g_runtime.Off();
    return;
  }
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
  RunOn(chain, queue);
}
}  // namespace internal

// ReShade's present event (DLSS-G backend): games without frame generation.
inline void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect*,
                      const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  using namespace internal;
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
  // The host's Present and the game's Present on Streamline's proxy run it for those games.
  if (mfgunlock::slhost::Hosting() || g_proxy_frames.load(std::memory_order_relaxed)) return;
  if (queue->get_device()->get_api() != reshade::api::device_api::d3d12) return;
  if (GameFrameGeneration()) {
    // Here ReShade also sees the generated frames: NR needs the hook on the game's Present, set at load.
    g_runtime.Pause(mfgunlock::nr::State::kWithGameFg);
    return;
  }
  Run(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()),
      reinterpret_cast<ID3D12Resource*>(swapchain->get_current_back_buffer().handle), CurrentEncoding());
}
inline void Register() {
  mfgunlock::slhost::internal::on_game_frame_nr.store(&internal::HostFrame, std::memory_order_release);
  mfgunlock::framecount::internal::g_on_proxy_present_nr.store(&internal::ProxyFrame, std::memory_order_release);
  reshade::register_event<reshade::addon_event::present>(&OnPresent);
}
inline void Unregister() {
  mfgunlock::slhost::internal::on_game_frame_nr.store(nullptr, std::memory_order_release);
  mfgunlock::framecount::internal::g_on_proxy_present_nr.store(nullptr, std::memory_order_release);
  reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
}

// For the support report.
inline std::string Describe() {
  using mfgunlock::nr::State;
  const auto& r = internal::g_runtime;
  static const char* kNames[] = {"off", "no runtime", "unsigned runtime refused", "shim missing", "runtime refused the GPU",
                                 "HDR or unsupported format", "game frame generation", "failed", "running"};
  std::string text = kNames[static_cast<int>(r.state())];
  if (!r.runtime_path().empty()) {
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%ls", r.runtime_path().c_str());
    text += std::string("; runtime ") + path + (r.runtime_signed() ? " (signed by NVIDIA)" : " (unsigned)");
  }
  if (r.state() == State::kRunning) {
    char t[96];
    std::snprintf(t, sizeof(t), "; %ux%u, %.1f ms GPU", r.width(), r.height(), r.gpu_ms());
    text += t;
  }
  return text;
}

// The panel's DLSS 5 page.
inline void DrawPage() {
  using mfgunlock::nr::State;
  const auto& r = internal::g_runtime;
  ui::Card card("##dlss5_card");
  const State s = internal::Enabled() ? r.state() : State::kOff;
  char running[160]{};
  if (s == State::kRunning) {
    char what[96];
    std::snprintf(what, sizeof(what), "%ux%u, %.1f ms", r.width(), r.height(), r.gpu_ms());
    std::snprintf(running, sizeof(running), T(Str::kDlss5StateRunning_n28), what);
  }
  const char* text = s == State::kRunning       ? running
                     : s == State::kNoRuntime   ? T(Str::kDlss5StateNoRuntime_n29)
                     : s == State::kUnsigned    ? T(Str::kDlss5StateUnsigned_n30)
                     : s == State::kNoShim      ? T(Str::kDlss5StateNoShim_n31)
                     : s == State::kInitFailed  ? T(Str::kDlss5StateInitFailed_n32)
                     : s == State::kFormat      ? T(Str::kDlss5StateFormat_n33)
                     : s == State::kWithGameFg  ? T(Str::kDlss5StateGameFg_n34)
                     : s == State::kFailed      ? T(Str::kDlss5StateFailed_n35)
                                                : T(Str::kDlss5StateOff_n27);
  ui::Readout("##dlss5_state", T(Str::kDlss5Status_n26), text, T(Str::kDlss5EnabledDesc_n2),
              s == State::kRunning ? ui::Accent : (s == State::kOff ? ui::Muted : ui::Warning));
  if (!r.runtime_path().empty()) {
    char path[MAX_PATH + 32]{};
    std::snprintf(path, sizeof(path), "%ls%s", r.runtime_path().c_str(), r.runtime_signed() ? "" : " (unsigned)");
    ui::Readout("##dlss5_runtime", T(Str::kDlss5Runtime_n36), path, T(Str::kDlss5AllowUnsignedDesc_n4));
  }
  // Live settings, except AllowUnsigned: keep the restart notice for that one only.
  const bool changed = adv::Changed();
  for (size_t i = 0; i + 1 < std::size(adv::kDlss5); ++i) adv::DrawOption(adv::kDlss5[i]);
  adv::Changed() = changed;
  adv::DrawOption(adv::kDlss5[internal::kAllowUnsigned]);
  adv::Result();
}
}  // namespace dlss5
