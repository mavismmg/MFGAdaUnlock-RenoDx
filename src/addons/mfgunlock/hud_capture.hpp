// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (HUD fix for the Streamline host, sl_host.hpp): the image before the
// HUD, for games whose DLSS SR output is not it (KCD2 runs DLSS SR on linear
// HDR and tonemaps afterwards). DLSS-G and XMX need it to keep the HUD (the
// crosshair) still; without it the HUD is moved with the scene behind it.
//
// After the game's DLSS SR of a frame (sl_host arms it), the last full-screen
// pass (3 or 4 vertices, one instance) into a target with the back buffer's
// size and format family is the finished scene; the first draw into that same
// target that is not full-screen starts the HUD. Right before that draw the
// target is copied to a texture of the addon's and tagged as Streamline's
// HUD-less color for the frame. KCD2 (HudTrace, 2026-11-15): DLSS SR -> HDR
// passes -> "draw v3 -> 2560x1440 RGBA8", then dozens of small HUD draws into
// it, then the copy to the back buffer. A game without that pattern (RDR2
// composes its HUD with compute) gets no capture and runs as before.
#include <d3d12.h>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <include/reshade.hpp>
#include "sl_host.hpp"

namespace mfgunlock::hudcapture {
namespace api = reshade::api;

namespace internal {
inline std::mutex lock;
inline std::map<api::command_list*, api::resource_view> targets;  // render target 0 per command list
inline uint64_t candidate = 0;                                    // the full-screen pass's target this frame
inline uint32_t candidate_frame = 0;
inline api::resource copy{0};
inline api::resource_desc copy_desc{};

inline int Family(api::format f) {
  switch (f) {
    case api::format::r8g8b8a8_typeless: case api::format::r8g8b8a8_unorm: case api::format::r8g8b8a8_unorm_srgb:
    case api::format::b8g8r8a8_typeless: case api::format::b8g8r8a8_unorm: case api::format::b8g8r8a8_unorm_srgb:
      return 1;
    case api::format::r10g10b10a2_typeless: case api::format::r10g10b10a2_unorm: return 2;
    case api::format::r16g16b16a16_typeless: case api::format::r16g16b16a16_float: return 3;
    default: return 100 + static_cast<int>(f);
  }
}
inline int DxgiFamily(int f) {
  switch (f) {
    case 27: case 28: case 29: case 87: case 90: case 91: return 1;  // RGBA8 / BGRA8
    case 23: case 24: return 2;                                      // R10G10B10A2
    case 9: case 10: return 3;                                       // RGBA16
    default: return 100 + f;
  }
}

// Every bind, armed or not: a target bound before DLSS SR may receive the passes after it.
inline void OnBindTargets(api::command_list* cmd, uint32_t count, const api::resource_view* rtvs, api::resource_view) {
  std::lock_guard guard(lock);
  targets[cmd] = count > 0 && rtvs != nullptr ? rtvs[0] : api::resource_view{0};
}

// Copies `target` (bound as render target) and tags the copy for `frame`.
inline void Capture(api::command_list* cmd, api::resource target, const api::resource_desc& desc, uint32_t frame) {
  api::device* device = cmd->get_device();
  if (copy.handle == 0 || copy_desc.texture.width != desc.texture.width || copy_desc.texture.height != desc.texture.height ||
      copy_desc.texture.format != desc.texture.format) {
    xmx::generator::g_own_hudless.store(nullptr, std::memory_order_release);
    if (copy.handle != 0) device->destroy_resource(copy);  // the previous size's: its frame was presented long ago
    copy = {0};
    api::resource_desc d = desc;
    d.heap = api::memory_heap::gpu_only;
    d.usage = api::resource_usage::copy_dest | api::resource_usage::shader_resource;
    d.flags = api::resource_flags::none;
    if (!device->create_resource(d, nullptr, api::resource_usage::copy_dest, &copy)) {
      copy = {0};
      return;
    }
    copy_desc = desc;
  }
  cmd->barrier(target, api::resource_usage::render_target, api::resource_usage::copy_source);
  cmd->copy_resource(target, copy);
  cmd->barrier(target, api::resource_usage::copy_source, api::resource_usage::render_target);
  xmx::generator::g_own_hudless.store(reinterpret_cast<ID3D12Resource*>(copy.handle), std::memory_order_release);
  xmx::generator::g_own_hudless_seq.fetch_add(1, std::memory_order_release);
  slhost::TagHudless(reinterpret_cast<ID3D12Resource*>(copy.handle),
                     reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native()), frame);
}

// 1.10.1 (plan item 4): [DLSSG] HudTrace=N logs, for N frames after DLSS SR, every draw,
// dispatch and copy with its render target, so a game the fix misses (Metro Exodus in
// HDR: "HUD fix captures 0") shows where its HUD is drawn. Off by default.
inline std::atomic<int> trace_frames{0};
inline std::atomic<int> trace_lines{0};
inline uint32_t trace_frame = 0;
inline constexpr int kTraceLineLimit = 1500;
inline void Trace(const std::string& text) {
  if (trace_lines.fetch_add(1, std::memory_order_relaxed) >= kTraceLineLimit) return;
  reshade::log::message(reshade::log::level::info, ("mfgunlock: HudTrace " + text).c_str());
}
// The frame being traced, or 0. A new armed frame uses up one of the N.
inline uint32_t TraceFrame() {
  if (trace_frames.load(std::memory_order_relaxed) <= 0) return 0;
  const uint32_t frame = slhost::HudArmIndex();
  if (frame == 0) return 0;
  std::lock_guard guard(lock);
  if (frame != trace_frame) {
    if (trace_frames.fetch_sub(1, std::memory_order_relaxed) <= 0) return 0;
    trace_frame = frame;
    Trace("frame " + std::to_string(frame) + ": back buffer " + std::to_string(slhost::BackbufferWidth()) + "x" +
          std::to_string(slhost::BackbufferHeight()) + " DXGI format " + std::to_string(slhost::BackbufferFormat()));
  }
  return frame;
}
inline std::string Describe(api::device* device, api::resource resource) {
  if (resource.handle == 0) return "none";
  const api::resource_desc d = device->get_resource_desc(resource);
  if (d.type != api::resource_type::texture_2d) return "buffer";
  return std::to_string(d.texture.width) + "x" + std::to_string(d.texture.height) + " fmt " +
         std::to_string(static_cast<int>(d.texture.format)) + " #" + std::to_string(resource.handle & 0xFFFFFF);
}
inline void TraceDraw(api::command_list* cmd, const char* kind, uint32_t count, uint32_t instances) {
  if (TraceFrame() == 0) return;
  api::resource_view view{0};
  {
    std::lock_guard guard(lock);
    const auto it = targets.find(cmd);
    if (it != targets.end()) view = it->second;
  }
  api::device* device = cmd->get_device();
  const api::resource target = view.handle != 0 ? device->get_resource_from_view(view) : api::resource{0};
  Trace(std::string(kind) + " " + std::to_string(count) + "x" + std::to_string(instances) + " -> " + Describe(device, target));
}
inline bool OnTraceDispatch(api::command_list* cmd, uint32_t x, uint32_t y, uint32_t z) {
  if (TraceFrame() != 0)
    Trace("dispatch " + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z));
  return false;
}
inline bool OnTraceCopy(api::command_list* cmd, api::resource source, api::resource dest) {
  if (TraceFrame() != 0)
    Trace("copy " + Describe(cmd->get_device(), source) + " -> " + Describe(cmd->get_device(), dest));
  return false;
}
inline bool OnTraceCopyRegion(api::command_list* cmd, api::resource source, uint32_t, const api::subresource_box*,
                              api::resource dest, uint32_t, const api::subresource_box*, api::filter_mode) {
  return OnTraceCopy(cmd, source, dest);
}

inline void OnDrawCommon(api::command_list* cmd, bool full_screen) {
  const uint32_t frame = slhost::HudArmIndex();
  if (frame == 0) return;
  api::resource_view view{0};
  {
    std::lock_guard guard(lock);
    const auto it = targets.find(cmd);
    if (it == targets.end()) return;
    view = it->second;
  }
  if (view.handle == 0) return;
  api::device* device = cmd->get_device();
  const api::resource target = device->get_resource_from_view(view);
  if (target.handle == 0) return;
  const api::resource_desc desc = device->get_resource_desc(target);
  // The back buffer's size (the DLSS SR output's is the same) and format family.
  const int bb = slhost::BackbufferFormat();
  if (desc.type != api::resource_type::texture_2d || Family(desc.texture.format) != DxgiFamily(bb)) return;
  const uint32_t bw = slhost::BackbufferWidth(), bh = slhost::BackbufferHeight();
  if (bw == 0 || desc.texture.width != bw || desc.texture.height != bh) return;
  std::lock_guard guard(lock);
  if (candidate_frame != frame) {
    candidate = 0;
    candidate_frame = frame;
  }
  if (full_screen) {
    candidate = target.handle;
    return;
  }
  if (candidate == 0 || candidate != target.handle || !slhost::DisarmHud(frame)) return;
  Capture(cmd, target, desc, frame);
}
inline bool OnDraw(api::command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
  TraceDraw(cmd, "draw", vertices, instances);
  OnDrawCommon(cmd, (vertices == 3 || vertices == 4) && instances == 1);
  return false;
}
inline bool OnDrawIndexed(api::command_list* cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
  TraceDraw(cmd, "draw_indexed", indices, instances);
  OnDrawCommon(cmd, false);
  return false;
}
}  // namespace internal

// While the Streamline host is wanted ([DLSSG] AddFrameGeneration); [DLSSG] HudFix=0 turns it off.
inline std::atomic_bool g_registered{false};
inline void Register(const std::filesystem::path& ini) {
  if (GetPrivateProfileIntW(L"DLSSG", L"HudFix", 1, ini.c_str()) == 0) return;
  g_registered.store(true);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&internal::OnBindTargets);
  reshade::register_event<reshade::addon_event::draw>(&internal::OnDraw);
  reshade::register_event<reshade::addon_event::draw_indexed>(&internal::OnDrawIndexed);
  if (const int frames = static_cast<int>(GetPrivateProfileIntW(L"DLSSG", L"HudTrace", 0, ini.c_str())); frames > 0) {
    internal::trace_frames.store(frames > 20 ? 20 : frames);
    reshade::register_event<reshade::addon_event::dispatch>(&internal::OnTraceDispatch);
    reshade::register_event<reshade::addon_event::copy_resource>(&internal::OnTraceCopy);
    reshade::register_event<reshade::addon_event::copy_texture_region>(&internal::OnTraceCopyRegion);
    reshade::log::message(reshade::log::level::info,
                          ("mfgunlock: HudTrace on for " + std::to_string(internal::trace_frames.load()) +
                           " frame(s) after DLSS SR.").c_str());
  }
}

}  // namespace mfgunlock::hudcapture
