// SPDX-License-Identifier: MIT
// 1.11.0 (O1-O4): the performance overlay (OSD), drawn over the game with the ReShade menu closed.
//
// ReShade builds an ImGui frame every frame only while some addon listens to reshade_overlay
// (runtime_gui.cpp: "Early-out to avoid costly ImGui calls"). So the overlay registers that event
// the first time it is turned on, from ReShade's present event on the present thread, where
// ReShade also invokes it; a session that never turns it on pays nothing. Its shortcut is read in
// the present event with GetAsyncKeyState while the game's window has the focus.
//
// Frames: the "output" ring takes every present ReShade sees (with frame generation, generated
// frames included where Streamline or XeMFG presents through ReShade); the "game" ring takes one
// timestamp per frame the game rendered: the hosted Streamline's Present (sl_host.hpp), else the
// game's own Reflex sleep (framecount.hpp). XeMFG (CUDA) reports its own counters.
// Included by addon.cpp after addon_overlay.inl.

namespace perfosd {  // inside addon.cpp's anonymous namespace
namespace adv = mfgunlock::ui::advanced;
namespace ui = mfgunlock::ui;
namespace perfstats = mfgunlock::perfstats;
namespace slhost = mfgunlock::slhost;
namespace framecount = mfgunlock::framecount;
namespace xmx = mfgunlock::xmx;
namespace backend = mfgunlock::backend;
namespace config = mfgunlock::config;

namespace internal {
inline perfstats::Ring g_output, g_game;
inline uint64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}
inline uint64_t Frequency() {
  static const uint64_t f = [] {
    LARGE_INTEGER t;
    QueryPerformanceFrequency(&t);
    return static_cast<uint64_t>(t.QuadPart);
  }();
  return f;
}
inline void GameFrame() { g_game.Push(Qpc()); }
inline void ReflexSleep() {
  // The host paces its own frames (and calls this sleep too): its Present counts instead.
  if (!slhost::Hosting()) g_game.Push(Qpc());
}
inline int Setting(size_t index) { return _wtoi(adv::Current(adv::kOverlay[index]).c_str()); }
inline bool Enabled() { return Setting(0) != 0; }
// The panel's cache and the file, without the "restart to apply" notice: the overlay is live.
inline void SetEnabled(bool on) {
  const auto& option = adv::kOverlay[0];
  const wchar_t* value = on ? L"1" : L"0";
  if (config::Save(backend::Directory() / L"MFGAdaUnlock.ini", {{option.section, option.key, value}}))
    adv::Current(option) = value;
}
inline std::atomic_bool g_registered{false};
void OnOverlay(reshade::api::effect_runtime* runtime);

// Statistics, refreshed 4 times a second.
struct View {
  perfstats::Summary game, output;
  bool game_live = false, output_live = false;
  std::vector<float> graph_ms;  // newest last
  float graph_top = 33.3f;
  uint64_t vram_used = 0, vram_budget = 0;
};
inline View g_view;
inline uint64_t g_next_refresh = 0;

// The device's video memory (D3D12 and D3D11), queried once a second.
inline IDXGIAdapter3* g_adapter = nullptr;
inline bool g_adapter_tried = false;
inline void QueryVram(reshade::api::effect_runtime* runtime, View& view) {
  if (!g_adapter_tried) {
    g_adapter_tried = true;
    LUID luid{};
    bool known = false;
    auto* device = runtime->get_device();
    if (device->get_api() == reshade::api::device_api::d3d12) {
      luid = reinterpret_cast<ID3D12Device*>(device->get_native())->GetAdapterLuid();
      known = true;
    } else if (device->get_api() == reshade::api::device_api::d3d11) {
      IDXGIDevice* dxgi = nullptr;
      if (SUCCEEDED(reinterpret_cast<IUnknown*>(device->get_native())->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
          DXGI_ADAPTER_DESC desc{};
          if (SUCCEEDED(adapter->GetDesc(&desc))) {
            luid = desc.AdapterLuid;
            known = true;
          }
          adapter->Release();
        }
        dxgi->Release();
      }
    }
    // Through GetProcAddress: not every build of the addon links dxgi.lib.
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    const auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    if (known && create != nullptr && SUCCEEDED(create(IID_PPV_ARGS(&factory)))) {
      factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g_adapter));
      factory->Release();
    }
  }
  if (g_adapter == nullptr) return;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
    view.vram_used = info.CurrentUsage;
    view.vram_budget = info.Budget;
  }
}

inline void Refresh(reshade::api::effect_runtime* runtime) {
  const uint64_t now = GetTickCount64();
  if (now < g_next_refresh) return;
  const bool vram_due = now / 1000 != g_next_refresh / 1000;
  g_next_refresh = now + 250;
  const uint64_t f = Frequency(), qpc = Qpc();
  const auto live = [&](const perfstats::Ring& r) { return r.Last() != 0 && qpc - r.Last() < f; };
  View& v = g_view;
  v.game_live = live(g_game);
  v.output_live = live(g_output);
  // A 2 s window for the rates, at most the ring.
  const auto window = [&](const perfstats::Ring& r) {
    auto intervals = r.Intervals(perfstats::kCapacity);
    uint64_t span = 0;
    size_t keep = 0;
    for (size_t i = intervals.size(); i > 0 && span < 2 * f; --i, ++keep) span += intervals[i - 1];
    intervals.erase(intervals.begin(), intervals.end() - static_cast<ptrdiff_t>(keep));
    return intervals;
  };
  v.game = v.game_live ? perfstats::Summarize(window(g_game), f) : perfstats::Summary{};
  v.output = v.output_live ? perfstats::Summarize(window(g_output), f) : perfstats::Summary{};
  const auto& graph_source = v.game_live ? g_game : g_output;
  const auto recent = graph_source.Intervals(240);
  v.graph_ms.assign(recent.size(), 0.f);
  float top = 0.f;
  for (size_t i = 0; i < recent.size(); ++i) {
    v.graph_ms[i] = static_cast<float>(double(recent[i]) * 1000.0 / double(f));
    top = (std::max)(top, v.graph_ms[i]);
  }
  v.graph_top = (std::max)(16.7f, top * 1.1f);
  if (vram_due && Setting(1) >= 2) QueryVram(runtime, v);
}

// DLSS-G's status flags (sl::DLSSGStatus), short.
inline std::string StatusText(unsigned int status) {
  if (status == 0) return "OK";
  std::string text;
  const auto add = [&](const char* s) { text += (text.empty() ? "" : ", ") + std::string(s); };
  if (status & 0x1) add("resolution too low");
  if (status & 0x2) add("Reflex not detected");
  if (status & 0x4) add("HDR format not supported");
  if (status & 0x8) add("constants invalid");
  if (status & 0x10) add("GetCurrentBackBufferIndex not called");
  if (status & ~0x1Fu) add("other");
  return text;
}

// The method in use and what it reports, one line.
// `measured`: output frames per game frame as measured here (0 when unknown). Cyberpunk with XeMFG
// DLSS-G (test build 5): the runtime's numFramesActuallyPresented stayed 0 while XMX generated 4x,
// and this line said "not generating"; XMX's own state and the measured ratio decide now, as the panel.
inline std::string MethodLine(unsigned int& multiplier, double measured) {
  multiplier = 0;
#if defined(MFGUNLOCK_NATIVE_XEMFG)
  if (backend::active == backend::Kind::XeMfg) {
    const auto s = mfgunlock::xemfg::native::Snapshot();
    multiplier = s.enabled ? s.multiplier : 1;
    return std::string("XeMFG ") + (s.enabled ? std::to_string(s.multiplier) + "x" : "off") +
           (s.detail[0] ? std::string(" - ") + s.detail : std::string());
  }
#endif
  const unsigned int presented = framecount::g_actual_frames_presented.load(std::memory_order_relaxed);
  const unsigned int status = framecount::g_dlssg_status.load(std::memory_order_relaxed);
  const unsigned int measured_x = measured >= 1.5 ? static_cast<unsigned int>(measured + 0.5) : 0;
  multiplier = presented > 1 ? presented : measured_x;
  const bool xmx_on = xmx::generator::Enabled();
  std::string method = xmx_on ? "XeMFG DLSS-G" : "DLSS-G";
  if (slhost::Hosting()) method += " (Add frame generation)";
  if (xmx_on) {
    const auto r = xmx::generator::Snapshot();
    if (r.generating) return method + (multiplier > 1 ? " " + std::to_string(multiplier) + "x" : std::string()) + " - generating";
    if (r.evaluating && r.idle != xmx::policy::Idle::None)
      return method + " - waiting: " + xmx::policy::IdleText(r.idle);
    if (!r.failure.empty()) return method + " - failed: " + r.failure;
    return method + " - not generating";
  }
  if (multiplier <= 1) return method + " - not generating";
  return method + " " + std::to_string(multiplier) + "x - " + StatusText(status);
}

inline ImVec4 FpsColor(double fps) {
  if (fps <= 0.0) return ImVec4(0.7f, 0.7f, 0.7f, 1.f);
  if (fps < 30.0) return ImVec4(1.f, 0.45f, 0.4f, 1.f);
  if (fps < 55.0) return ImVec4(1.f, 0.8f, 0.35f, 1.f);
  return ImVec4(0.55f, 0.95f, 0.55f, 1.f);
}

inline void OnOverlay(reshade::api::effect_runtime* runtime) {
  if (runtime == nullptr || !Enabled()) return;
  Refresh(runtime);
  const View& v = g_view;
  const int level = std::clamp(Setting(1), 0, 2);
  const int corner = std::clamp(Setting(2), 0, 3);
  const float scale = static_cast<float>(std::clamp(Setting(3), 50, 200)) / 100.f;
  const float opacity = static_cast<float>(std::clamp(Setting(4), 0, 100)) / 100.f;

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  constexpr float kMargin = 10.f;
  const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
  ImGui::SetNextWindowPos(ImVec2(right ? display.x - kMargin : kMargin, bottom ? display.y - kMargin : kMargin),
                          ImGuiCond_Always, ImVec2(right ? 1.f : 0.f, bottom ? 1.f : 0.f));
  ImGui::SetNextWindowBgAlpha(opacity);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!ImGui::Begin("##mfgunlock_perf_overlay", nullptr, kFlags)) {
    ImGui::End();
    return;
  }
  // ImGui 1.92 (ReShade's table has no SetWindowFontScale): the size comes with the font.
  ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * scale);
  // The runtime's own count, for the game rate when only output frames are timed.
  const unsigned int multiplier = framecount::g_actual_frames_presented.load(std::memory_order_relaxed);
  // The game's frames when known; otherwise what ReShade presents.
  const perfstats::Summary& frames = v.game_live ? v.game : v.output;
  double output_fps = v.output_live ? v.output.fps : 0.0;
#if defined(MFGUNLOCK_NATIVE_XEMFG)
  // XeMFG (CUDA) presents its own frames: its counters, once a second.
  static uint64_t native_tick = 0, native_frames = 0, native_presents = 0;
  static double native_game_fps = 0.0, native_output_fps = 0.0;
  if (backend::active == backend::Kind::XeMfg) {
    const uint64_t now = GetTickCount64();
    if (now - native_tick >= 1000) {
      const auto s = mfgunlock::xemfg::native::Snapshot();
      if (native_tick != 0) {
        const double seconds = double(now - native_tick) / 1000.0;
        native_game_fps = double(s.frames - native_frames) / seconds;
        native_output_fps = double(s.outputPresents - native_presents) / seconds;
      }
      native_tick = now;
      native_frames = s.frames;
      native_presents = s.outputPresents;
    }
    if (native_output_fps > 0.0) output_fps = native_output_fps;
  }
#endif
  double game_fps = frames.fps;
#if defined(MFGUNLOCK_NATIVE_XEMFG)
  if (backend::active == backend::Kind::XeMfg && native_game_fps > 0.0) game_fps = native_game_fps;
#endif
  if (!v.game_live && multiplier > 1 && output_fps > 0.0) game_fps = output_fps / multiplier;

  unsigned int shown_multiplier = 0;
  const std::string method =
      MethodLine(shown_multiplier, game_fps > 0.0 && output_fps > 0.0 ? output_fps / game_fps : 0.0);
  // Minimal: the output frame rate, and the game's when frames are generated.
  char line[192];
  const double shown = output_fps > 0.0 ? output_fps : game_fps;
  ImGui::TextColored(FpsColor(game_fps > 0.0 ? game_fps : shown), "%.0f FPS", shown);
  if (game_fps > 0.0 && output_fps > game_fps * 1.2) {
    ImGui::SameLine();
    ImGui::TextDisabled("(game %.0f)", game_fps);
  }
  if (level >= 1) {
    if (frames.frames > 0) {
      std::snprintf(line, sizeof(line), "Frame time %.1f ms  p99 %.1f ms  max %.1f ms", frames.average_ms,
                    frames.p99_ms, frames.max_ms);
      ImGui::TextUnformatted(line);
      std::snprintf(line, sizeof(line), "1%% low %.0f  0.1%% low %.0f  hitches %u", frames.low1_fps, frames.low01_fps,
                    frames.hitches);
      ImGui::TextUnformatted(line);
    }
    ImGui::TextDisabled("%s", method.c_str());
    // 1.12.0 (DLSS 5): neural rendering's route, motion and cost while it runs.
    const auto& nr = mfgunlock::nr::g_status;
    if (nr.route.load(std::memory_order_relaxed) != 0) {
      static const char* kRoutes[] = {"", "before Present", "after upscaling", "Vulkan", "before frame generation"};
      static const char* kGuides[] = {"no motion", "game motion", "estimated motion", "TAA motion"};
      char cost[24] = "";
      if (nr.ms.load() > 0.f) std::snprintf(cost, sizeof(cost), " %.1f ms", nr.ms.load());
      std::snprintf(line, sizeof(line), "DLSS 5 NR%s  %s, %s%s", cost, kRoutes[std::clamp(nr.route.load(), 0, 4)],
                    kGuides[std::clamp(nr.guides.load(), 0, 3)], nr.compare.load() ? "  (compare)" : "");
      ImGui::TextUnformatted(line);
    }
  }
  if (level >= 2) {
    if (!v.graph_ms.empty())
      ImGui::PlotLines("##frame_times", v.graph_ms.data(), static_cast<int>(v.graph_ms.size()), 0, nullptr, 0.f,
                       v.graph_top, ImVec2(240.f * scale, 48.f * scale));
    const auto latency = framecount::LatencyGuardSnapshot();
    if (latency.sample_seen && latency.pipeline_latency_us != 0) {
      std::snprintf(line, sizeof(line), "Latency %.1f ms  p95 %.1f ms", latency.pipeline_latency_us / 1000.0,
                    latency.pipeline_p95_us / 1000.0);
      ImGui::TextUnformatted(line);
    }
    if (latency.gpu_active_us != 0) {
      std::snprintf(line, sizeof(line), "GPU %.1f ms", latency.gpu_active_us / 1000.0);
      ImGui::TextUnformatted(line);
      if (latency.display_refresh_fps != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("display %u Hz", latency.display_refresh_fps);
      }
    }
    if (v.vram_budget != 0) {
      std::snprintf(line, sizeof(line), "VRAM %.1f / %.1f GB", double(v.vram_used) / 1073741824.0,
                    double(v.vram_budget) / 1073741824.0);
      ImGui::TextUnformatted(line);
    }
  }
  ImGui::PopFont();
  ImGui::End();
}

// Shortcut: the modifier held (0 none, 1 Shift, 2 Ctrl, 3 Alt) and the key's press.
inline bool ShortcutPressed() {
  const int key = Setting(5);
  if (key <= 0 || key > 254) return false;
  static bool was_down = false;
  const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
  const bool pressed = down && !was_down;
  was_down = down;
  if (!pressed) return false;
  DWORD owner = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &owner);
  if (owner != GetCurrentProcessId()) return false;
  const int modifiers[] = {0, VK_SHIFT, VK_CONTROL, VK_MENU};
  const int modifier = modifiers[std::clamp(Setting(6), 0, 3)];
  return modifier == 0 || (GetAsyncKeyState(modifier) & 0x8000) != 0;
}
}  // namespace internal

// ReShade's present event (both backends): the output ring, the shortcut, and the overlay's event.
inline void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*,
                      const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  using namespace internal;
  g_output.Push(Qpc());
  if (ShortcutPressed()) SetEnabled(!Enabled());
  if (Enabled() && !g_registered.exchange(true)) {
    reshade::register_event<reshade::addon_event::reshade_overlay>(&OnOverlay);
    reshade::log::message(reshade::log::level::info, "mfgunlock: performance overlay on.");
  }
}
inline void Register() {
  slhost::internal::on_game_frame.store(&internal::GameFrame, std::memory_order_release);
  framecount::internal::g_on_reflex_sleep.store(&internal::ReflexSleep, std::memory_order_release);
  reshade::register_event<reshade::addon_event::present>(&OnPresent);
}
inline void Unregister() {
  slhost::internal::on_game_frame.store(nullptr, std::memory_order_release);
  framecount::internal::g_on_reflex_sleep.store(nullptr, std::memory_order_release);
  reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
  if (internal::g_registered.exchange(false))
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(&internal::OnOverlay);
}
// The panel's section (both backends).
inline void DrawSettings() {
  if (!ui::SectionHeader(T(Str::kPerfOverlaySection_o0), internal::Enabled())) return;
  ui::Card card("##perf_overlay_card");
  // Live settings: keep the "restart to apply" notice for the settings that need it.
  const bool changed = adv::Changed();
  adv::Draw(adv::kOverlay);
  adv::Changed() = changed;
}
}  // namespace perfosd
