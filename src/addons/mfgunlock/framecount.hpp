/*
 * Advertising and forcing the requested frame multiplier.
 * SPDX-License-Identifier: MIT
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * Two different kinds of game need two different things.
 *
 * Cyberpunk 2077 and GTA V Enhanced have their own 2x/3x/4x selector. Once the
 * runtime reports the added capacity, the game asks for 3x or 4x by itself.
 * Forcing anything would override the player's own choice, so this is OFF by
 * default. STALKER 2 also has a native selector, but builds it from
 * slDLSSGGetState::numFramesToGenerateMax. That return value is raised to the
 * wrapper ceiling verified by addon.cpp so 3x/4x actually appear in its menu.
 *
 * Deep Rock Galactic (and anything else where frame generation is just a
 * on/off toggle) will only ever ask for 1 generated frame, no matter how
 * capable the runtime claims to be. Nothing downstream can help: sl.dlss_g
 * loops numFramesToGenerate times, so a request of 1 produces exactly one
 * generated frame. This is the job the NVIDIA App does for 50-series users via
 * a driver registry setting -- overriding the request, not the capability.
 *
 * The lever is slDLSSGSetOptions. It is not exported: the app obtains it
 * through slGetFeatureFunction (which sl.interposer.dll does export), so we
 * hook that, hand back a wrapper, and raise numFramesToGenerate in transit.
 *
 * DLSSGOptions::numFramesToGenerate counts GENERATED frames, not total:
 * 2x -> 1, 3x -> 2, 4x -> 3 (sl_dlss_g.h). The struct is passed by const
 * reference; the wrapper raises the value, calls through, and puts the caller's
 * field back exactly as it found it rather than leaving a surprise behind.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <sl.h>
#include <sl_dlss_g.h>

#include <include/reshade.hpp>

#include "./hdr_compat.hpp"
#include "./ngx_hook.hpp"
#include "./quality_guard.hpp"

namespace mfgunlock::framecount {

// 0 = leave the game's request alone. 2..5 = force that multiplier.
inline std::atomic<unsigned int> g_force_multiplier{0};
inline std::atomic_bool g_hooked{false};
inline std::atomic_bool g_intercepted{false};
inline std::atomic<unsigned int> g_last_requested{0};
inline std::atomic<unsigned int> g_last_forced{0};
inline std::atomic_bool g_declined_no_pacing{false};
inline std::atomic<unsigned int> g_force_failed_for{0};
inline std::atomic<unsigned int> g_last_result{0};
inline std::atomic_bool g_native_request_seen{false};
inline std::atomic<unsigned int> g_native_requested{0};
inline std::atomic<unsigned int> g_native_result{0};
inline std::atomic_bool g_state_seen{false};
inline std::atomic<unsigned int> g_state_result{0};
inline std::atomic<unsigned int> g_dlssg_status{0};
inline std::atomic_bool g_status_ok_logged{false};
inline std::atomic_bool g_failure_status_logged{false};
inline std::atomic<unsigned int> g_actual_frames_presented{0};
inline std::atomic<unsigned int> g_max_actual_frames_presented{0};
inline std::atomic<unsigned long long> g_state_samples{0};
inline std::atomic<unsigned int> g_seen_present_counts{0};

// Some games submit HUD-less/UI resources in a color space which does not
// match their final HDR color buffer. The resulting invalid separation mask
// can create halos, ghosting and edge artifacts in every generated frame.
// Quality Guard is the conservative default. In HDR it uses final color rather
// than an untrusted optional HUD split. In SDR it suppresses the optional split
// only when dimensions, format or resource metadata prove it invalid. Required
// color, depth and motion-vector inputs and frame pacing are never rewritten.
enum class HdrCompatibilityMode : unsigned int {
  kNative = 0,
  kUiRecomposition = 1,
  kFinalColorFallback = 2,
};
inline std::atomic<unsigned int> g_hdr_compatibility_mode{
    static_cast<unsigned int>(HdrCompatibilityMode::kFinalColorFallback)};
inline std::atomic_bool g_hdr_active{false};
inline std::atomic_bool g_ui_recomposition_applied{false};
inline std::atomic_bool g_ui_recomposition_fell_back{false};
inline std::atomic<unsigned int> g_ui_recomposition_source_version{0};
inline std::atomic<unsigned int> g_ui_recomposition_result{0};
inline std::atomic_bool g_hud_inputs_suppressed{false};
inline std::atomic<unsigned long long> g_hud_suppression_calls{0};
inline std::atomic<uint32_t> g_quality_issue_mask{0};
inline std::atomic<unsigned long long> g_quality_resets_requested{0};
inline std::atomic<unsigned long long> g_quality_resets_injected{0};
inline std::atomic_bool g_hdr_state_seen{false};
inline std::atomic_bool g_quality_reset_logged{false};
// Optional DLSS-G depth-discontinuity tuning. NVIDIA's documented default is
// 40.0; smaller values can help when the game's linear depth is compressed.
// It is disabled by default because the best value is integration-specific.
inline std::atomic<unsigned int> g_depth_edge_guard_level{0};
inline std::atomic_bool g_depth_edge_override_applied{false};
inline std::atomic_bool g_depth_edge_override_logged{false};
inline std::atomic<float> g_last_native_depth_separation{0.0f};

// Published by addon.cpp only after the active Streamline wrapper's pacing and
// hard ceiling have both been verified. Games such as STALKER 2 build their
// native 2x/3x/4x selector from DLSSGState::numFramesToGenerateMax rather than
// from the NGX parameter block, so the SetOptions hook alone cannot expose the
// additional choices.
inline std::atomic<unsigned int> g_advertised_max_generated{0};
inline std::atomic_bool g_capacity_advertised{false};
inline std::atomic<unsigned int> g_runtime_max_generated{0};

// Whether to re-enable Streamline's OTA plugin loading at slInit.
// OFF by default. GTA V Enhanced drops the OTA flags, and forcing them only
// matters if a newer plugin would then load -- which crashes that game. Its
// 2.9.1.0 plugin clamps to 3 generated frames (4x) and that is its real limit.
inline std::atomic_bool g_force_ota{false};
inline std::atomic_bool g_ota_forced{false};
inline std::atomic<unsigned long long> g_ota_flags_before{0};

// Asking for more than one generated frame while hardware flip metering is
// still enabled is the frozen-presentation failure, and the ordering makes that
// easy to walk into: the DLSS-G plugin is not loaded until the feature starts,
// so the pacing patch cannot land until after the game has already been
// configured. By the time slDLSSGSetOptions is called the plugin IS loaded, so
// that is the moment to make sure -- and to refuse to force if we cannot.
inline void (*g_ensure_pacing)() = nullptr;
inline bool (*g_pacing_ready)() = nullptr;

namespace internal {

using SetOptionsFn = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
using GetStateFn =
    sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
using GetFeatureFunctionFn = sl::Result (*)(sl::Feature, const char*, void*&);
using InitFn = sl::Result (*)(const sl::Preferences&, uint64_t);
using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
                               sl::CommandBuffer*);
using SetTagForFrameFn = PFun_slSetTagForFrame*;
using SetConstantsFn = PFun_slSetConstants*;

inline SetOptionsFn g_real_set_options = nullptr;
inline GetStateFn g_real_get_state = nullptr;
inline GetFeatureFunctionFn g_real_get_feature_function = nullptr;
inline InitFn g_real_init = nullptr;
inline SetTagFn g_real_set_tag = nullptr;
inline SetTagForFrameFn g_real_set_tag_for_frame = nullptr;
inline SetConstantsFn g_real_set_constants = nullptr;

constexpr uint32_t kUnusedViewport = (std::numeric_limits<uint32_t>::max)();
struct QualityViewportState {
  std::atomic<uint32_t> key{kUnusedViewport};
  std::atomic_bool options_seen{false};
  std::atomic<uint32_t> mode{0};
  std::atomic<uint32_t> generated_frames{0};
  std::atomic<uint32_t> flags{0};
  std::atomic<uint32_t> color_width{0};
  std::atomic<uint32_t> color_height{0};
  std::atomic<uint32_t> color_format{0};
  std::atomic<uint32_t> mvec_width{0};
  std::atomic<uint32_t> mvec_height{0};
  std::atomic<uint32_t> backbuffer_width{0};
  std::atomic<uint32_t> backbuffer_height{0};
  std::atomic<uint32_t> backbuffer_format{0};
  std::atomic_bool hud_separation_seen{false};
  std::atomic_bool hud_separation_suppressed{false};
  std::atomic<uint64_t> reset_requested{0};
  std::atomic<uint64_t> reset_applied{0};
};
inline std::array<QualityViewportState, 8> g_quality_viewports{};

inline QualityViewportState* GetQualityState(const sl::ViewportHandle& viewport) {
  const uint32_t key = static_cast<uint32_t>(viewport);
  for (auto& state : g_quality_viewports) {
    if (state.key.load(std::memory_order_acquire) == key) return &state;
  }
  for (auto& state : g_quality_viewports) {
    uint32_t unused = kUnusedViewport;
    if (state.key.compare_exchange_strong(unused, key, std::memory_order_acq_rel))
      return &state;
    if (unused == key) return &state;
  }
  return nullptr;
}

inline void RequestReset(QualityViewportState* state) {
  if (state == nullptr) return;
  state->reset_requested.fetch_add(1, std::memory_order_release);
  g_quality_resets_requested.fetch_add(1, std::memory_order_relaxed);
}

inline void RequestAllResets() {
  for (auto& state : g_quality_viewports) {
    if (state.key.load(std::memory_order_acquire) != kUnusedViewport)
      RequestReset(&state);
  }
}

inline void ForgetOutputDescriptions() {
  for (auto& state : g_quality_viewports) {
    state.backbuffer_width.store(0, std::memory_order_relaxed);
    state.backbuffer_height.store(0, std::memory_order_relaxed);
    state.backbuffer_format.store(0, std::memory_order_relaxed);
    state.hud_separation_seen.store(false, std::memory_order_relaxed);
    state.hud_separation_suppressed.store(false, std::memory_order_relaxed);
  }
}

inline void ObserveOptionsTransition(const sl::ViewportHandle& viewport,
                                     const sl::DLSSGOptions& options,
                                     uint32_t generated_frames) {
  QualityViewportState* state = GetQualityState(viewport);
  if (state == nullptr) return;

  const uint32_t mode = static_cast<uint32_t>(options.mode);
  const uint32_t flags = static_cast<uint32_t>(options.flags);
  const bool changed = state->options_seen.load(std::memory_order_acquire) &&
      (state->mode.load(std::memory_order_relaxed) != mode ||
       state->generated_frames.load(std::memory_order_relaxed) != generated_frames ||
       state->flags.load(std::memory_order_relaxed) != flags ||
       state->color_width.load(std::memory_order_relaxed) != options.colorWidth ||
       state->color_height.load(std::memory_order_relaxed) != options.colorHeight ||
       state->color_format.load(std::memory_order_relaxed) != options.colorBufferFormat ||
       state->mvec_width.load(std::memory_order_relaxed) != options.mvecDepthWidth ||
       state->mvec_height.load(std::memory_order_relaxed) != options.mvecDepthHeight);

  state->mode.store(mode, std::memory_order_relaxed);
  state->generated_frames.store(generated_frames, std::memory_order_relaxed);
  state->flags.store(flags, std::memory_order_relaxed);
  state->color_width.store(options.colorWidth, std::memory_order_relaxed);
  state->color_height.store(options.colorHeight, std::memory_order_relaxed);
  state->color_format.store(options.colorBufferFormat, std::memory_order_relaxed);
  state->mvec_width.store(options.mvecDepthWidth, std::memory_order_relaxed);
  state->mvec_height.store(options.mvecDepthHeight, std::memory_order_relaxed);
  state->options_seen.store(true, std::memory_order_release);
  if (changed &&
      g_hdr_compatibility_mode.load(std::memory_order_relaxed) ==
          static_cast<unsigned int>(HdrCompatibilityMode::kFinalColorFallback)) {
    RequestReset(state);
  }
}

inline qualityguard::OutputDescription ExpectedOutput(const QualityViewportState* state) {
  if (state == nullptr) return {};
  qualityguard::OutputDescription result{
      state->backbuffer_width.load(std::memory_order_relaxed),
      state->backbuffer_height.load(std::memory_order_relaxed),
      state->backbuffer_format.load(std::memory_order_relaxed)};
  if (!result.HasDimensions()) {
    result.width = state->color_width.load(std::memory_order_relaxed);
    result.height = state->color_height.load(std::memory_order_relaxed);
  }
  if (!result.HasFormat())
    result.format = state->color_format.load(std::memory_order_relaxed);
  return result;
}

inline float DepthSeparationForLevel(unsigned int level) {
  switch (level) {
    case 1:
      return 20.0f;
    case 2:
      return 10.0f;
    case 3:
      return 4.0f;
    case 4:
      return 1.0f;
    default:
      return 0.0f;
  }
}

inline sl::Result CallSetOptions(const sl::ViewportHandle& viewport,
                                 const sl::DLSSGOptions& options,
                                 uint32_t generated_frames = 0,
                                 bool override_generated_frames = false) {
  ObserveOptionsTransition(viewport, options,
                           override_generated_frames ? generated_frames
                                                     : options.numFramesToGenerate);
  const bool recompose =
                         g_hdr_compatibility_mode.load(std::memory_order_relaxed) ==
                             static_cast<unsigned int>(HdrCompatibilityMode::kUiRecomposition) &&
                         g_hdr_active.load(std::memory_order_relaxed) &&
                         options.mode != sl::DLSSGMode::eOff;

  if (recompose) {
    sl::DLSSGOptions forwarded{};
    if (hdrcompat::BuildUiRecompositionOptions(
            options, forwarded, generated_frames, override_generated_frames)) {
      g_ui_recomposition_source_version.store(
          static_cast<unsigned int>(options.structVersion), std::memory_order_relaxed);
      const sl::Result result = g_real_set_options(viewport, forwarded);
      g_ui_recomposition_result.store(static_cast<unsigned int>(result),
                                      std::memory_order_relaxed);
      if (result == sl::Result::eOk) {
        if (!g_ui_recomposition_applied.exchange(true, std::memory_order_relaxed)) {
          std::stringstream s;
          s << "mfgunlock: HDR UI recomposition enabled; promoted DLSSGOptions v"
            << options.structVersion << " to v" << forwarded.structVersion
            << " while preserving the game's " << forwarded.numFramesToGenerate
            << " generated-frame request.";
          reshade::log::message(reshade::log::level::info, s.str().c_str());
        }
        return result;
      }

      if (!g_ui_recomposition_fell_back.exchange(true, std::memory_order_relaxed)) {
        std::stringstream s;
        s << "mfgunlock: HDR UI recomposition was rejected with sl::Result "
          << static_cast<unsigned int>(result)
          << "; retrying the original options so native frame generation remains available.";
        reshade::log::message(reshade::log::level::warning, s.str().c_str());
      }
    }
  }

  if (!override_generated_frames) return g_real_set_options(viewport, options);

  auto& mutable_options = const_cast<sl::DLSSGOptions&>(options);
  const uint32_t original = mutable_options.numFramesToGenerate;
  mutable_options.numFramesToGenerate = generated_frames;
  const sl::Result result = g_real_set_options(viewport, options);
  mutable_options.numFramesToGenerate = original;
  return result;
}

template <typename Forward>
inline sl::Result FilterHudSeparationTags(const sl::ViewportHandle& viewport,
                                          const sl::ResourceTag* tags, uint32_t count,
                                          Forward&& forward) {
  const bool filter =
      g_hdr_compatibility_mode.load(std::memory_order_relaxed) ==
          static_cast<unsigned int>(HdrCompatibilityMode::kFinalColorFallback);
  if (!filter || tags == nullptr || count == 0 || count > 64)
    return forward(tags);

  QualityViewportState* state = GetQualityState(viewport);
  const qualityguard::Assessment assessment = qualityguard::AssessTags(
      tags, count, g_hdr_active.load(std::memory_order_relaxed), ExpectedOutput(state));
  if (state != nullptr) {
    bool output_changed = false;
    if (assessment.observed_backbuffer.HasDimensions()) {
      const uint32_t previous_width =
          state->backbuffer_width.load(std::memory_order_relaxed);
      const uint32_t previous_height =
          state->backbuffer_height.load(std::memory_order_relaxed);
      output_changed = (previous_width != 0 && previous_height != 0) &&
                       (previous_width != assessment.observed_backbuffer.width ||
                        previous_height != assessment.observed_backbuffer.height);
      state->backbuffer_width.store(assessment.observed_backbuffer.width,
                                    std::memory_order_relaxed);
      state->backbuffer_height.store(assessment.observed_backbuffer.height,
                                     std::memory_order_relaxed);
    }
    if (assessment.observed_backbuffer.HasFormat()) {
      const uint32_t previous_format =
          state->backbuffer_format.load(std::memory_order_relaxed);
      output_changed = output_changed ||
                       (previous_format != 0 &&
                        previous_format != assessment.observed_backbuffer.format);
      state->backbuffer_format.store(assessment.observed_backbuffer.format,
                                     std::memory_order_relaxed);
    }
    if (output_changed) RequestReset(state);

    // Changing between an optional HUD split and final-color input invalidates
    // the temporal history just as a resolution change does. Only observe tag
    // calls that actually contain HUD/UI resources because games may submit
    // required resources in separate batches.
    if (assessment.has_hud_separation) {
      const bool was_seen =
          state->hud_separation_seen.exchange(true, std::memory_order_acq_rel);
      const bool was_suppressed = state->hud_separation_suppressed.exchange(
          assessment.suppress_hud_separation, std::memory_order_acq_rel);
      if ((!was_seen && assessment.suppress_hud_separation) ||
          (was_seen && was_suppressed != assessment.suppress_hud_separation)) {
        RequestReset(state);
      }
    }
  }
  if (!assessment.suppress_hud_separation) return forward(tags);

  static_assert(std::is_trivially_copyable_v<sl::ResourceTag>);
  alignas(sl::ResourceTag)
      std::array<std::byte, sizeof(sl::ResourceTag) * 64> storage;
  std::memcpy(storage.data(), tags, sizeof(sl::ResourceTag) * count);
  auto* forwarded = reinterpret_cast<sl::ResourceTag*>(storage.data());
  const uint32_t suppressed = hdrcompat::SuppressHudSeparationResources(forwarded, count);
  if (suppressed == 0) return forward(tags);

  g_hud_suppression_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t previous_issues =
      g_quality_issue_mask.fetch_or(assessment.issues, std::memory_order_relaxed);
  if (!g_hud_inputs_suppressed.exchange(true, std::memory_order_relaxed)) {
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: Quality Guard is active; incompatible optional HUD-less/UI tags are cleared before Streamline while color, depth, motion vectors, multiplier and pacing are preserved.");
  }
  if ((assessment.issues & ~previous_issues) != 0) {
    std::stringstream s;
    s << "mfgunlock: Quality Guard observed new optional-input issue mask 0x"
      << std::hex << assessment.issues << std::dec
      << "; using final color for this tag submission.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  return forward(forwarded);
}

inline sl::Result HookedSetTag(const sl::ViewportHandle& viewport,
                               const sl::ResourceTag* tags, uint32_t count,
                               sl::CommandBuffer* command_buffer) {
  if (g_real_set_tag == nullptr) return sl::Result::eErrorNotInitialized;
  return FilterHudSeparationTags(viewport, tags, count, [&](const sl::ResourceTag* forwarded) {
    return g_real_set_tag(viewport, forwarded, count, command_buffer);
  });
}

inline sl::Result HookedSetTagForFrame(const sl::FrameToken& frame,
                                       const sl::ViewportHandle& viewport,
                                       const sl::ResourceTag* tags, uint32_t count,
                                       sl::CommandBuffer* command_buffer) {
  if (g_real_set_tag_for_frame == nullptr) return sl::Result::eErrorNotInitialized;
  return FilterHudSeparationTags(viewport, tags, count, [&](const sl::ResourceTag* forwarded) {
    return g_real_set_tag_for_frame(frame, viewport, forwarded, count, command_buffer);
  });
}

inline sl::Result HookedSetConstants(const sl::Constants& values,
                                     const sl::FrameToken& frame,
                                     const sl::ViewportHandle& viewport) {
  if (g_real_set_constants == nullptr) return sl::Result::eErrorNotInitialized;

  QualityViewportState* state = GetQualityState(viewport);
  if (state == nullptr || !state->options_seen.load(std::memory_order_acquire) ||
      state->mode.load(std::memory_order_relaxed) ==
          static_cast<uint32_t>(sl::DLSSGMode::eOff)) {
    return g_real_set_constants(values, frame, viewport);
  }
  const uint64_t requested = state->reset_requested.load(std::memory_order_acquire);
  const bool reset_pending =
      requested != state->reset_applied.load(std::memory_order_relaxed);
  const float depth_override = DepthSeparationForLevel(
      g_depth_edge_guard_level.load(std::memory_order_relaxed));
  const bool override_depth = values.structVersion >= sl::kStructVersion2 &&
                              depth_override > 0.0f;
  if (!reset_pending && !override_depth)
    return g_real_set_constants(values, frame, viewport);

  alignas(sl::Constants) std::array<std::byte, sizeof(sl::Constants)> storage{};
  if (!qualityguard::CopyConstantsWithQualityOverrides(
          values, storage.data(), storage.size(),
          reset_pending && values.reset != sl::Boolean::eTrue, depth_override)) {
    return g_real_set_constants(values, frame, viewport);
  }
  const sl::Result result = g_real_set_constants(
      *reinterpret_cast<const sl::Constants*>(storage.data()), frame, viewport);
  if (result == sl::Result::eOk && override_depth) {
    g_last_native_depth_separation.store(
        values.minRelativeLinearDepthObjectSeparation, std::memory_order_relaxed);
    g_depth_edge_override_applied.store(true, std::memory_order_relaxed);
    if (!g_depth_edge_override_logged.exchange(true, std::memory_order_relaxed)) {
      std::stringstream s;
      s << "mfgunlock: optional DLSS-G depth-edge guard is active (game "
        << values.minRelativeLinearDepthObjectSeparation << ", forwarded "
        << depth_override << ").";
      reshade::log::message(reshade::log::level::info, s.str().c_str());
    }
  }
  if (result == sl::Result::eOk && reset_pending) {
    state->reset_applied.store(requested, std::memory_order_release);
    g_quality_resets_injected.fetch_add(1, std::memory_order_relaxed);
    if (!g_quality_reset_logged.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: Quality Guard synchronized Streamline temporal history after an HDR, swapchain, resolution or multiplier transition.");
    }
  }
  return result;
}

// Which sl.dlss_g the game ends up using is decided here, not on disk.
//
// Streamline can load plugins the driver has downloaded into
// C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\... but only if the
// app asks for it at slInit. Cyberpunk and Deep Rock Galactic do, which is why
// both silently run a 2.12.129.0 plugin regardless of the (much older) copies
// sitting in their folders. GTA V Enhanced does not, so it is stuck with
// whatever is on disk beside it -- and that copy was clamped to 3 generated
// frames.
//
// Both flags are in the SDK's own default; the app has to actively drop them.
// Putting them back is a far better answer than shipping DLLs by hand, because
// the driver then supplies a matched, signed, current plugin set.
inline sl::Result HookedInit(const sl::Preferences& pref, uint64_t sdk_version) {
  if (g_real_init == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_force_ota.load(std::memory_order_relaxed)) return g_real_init(pref, sdk_version);

  constexpr uint64_t kOta = static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA) |
                            static_cast<uint64_t>(sl::PreferenceFlags::eLoadDownloadedPlugins);

  auto& mutable_pref = const_cast<sl::Preferences&>(pref);
  const uint64_t before = static_cast<uint64_t>(mutable_pref.flags);
  if ((before & kOta) == kOta) {
    g_ota_flags_before.store(before, std::memory_order_relaxed);
    reshade::log::message(reshade::log::level::info,
                          "mfgunlock: slInit already requests OTA plugins; nothing to change.");
    return g_real_init(pref, sdk_version);
  }

  mutable_pref.flags = static_cast<sl::PreferenceFlags>(before | kOta);
  const sl::Result result = g_real_init(pref, sdk_version);
  mutable_pref.flags = static_cast<sl::PreferenceFlags>(before);

  g_ota_flags_before.store(before, std::memory_order_relaxed);
  g_ota_forced.store(result == sl::Result::eOk, std::memory_order_relaxed);

  std::stringstream s;
  s << "mfgunlock: slInit flags 0x" << std::hex << before << " -> 0x" << (before | kOta) << std::dec
    << " (eAllowOTA | eLoadDownloadedPlugins) so the driver's OTA plugin set is used"
    << (result == sl::Result::eOk ? "." : " -- but slInit returned an error, so this had no effect.");
  reshade::log::message(result == sl::Result::eOk ? reshade::log::level::info
                                                  : reshade::log::level::warning,
                        s.str().c_str());
  return result;
}

inline sl::Result HookedSetOptions(const sl::ViewportHandle& viewport,
                                   const sl::DLSSGOptions& options) {
  if (g_real_set_options == nullptr) return sl::Result::eErrorNotInitialized;

  const unsigned int multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  if (multiplier < 2 || options.mode == sl::DLSSGMode::eOff) {
    const sl::Result result = CallSetOptions(viewport, options);
    if (options.mode != sl::DLSSGMode::eOff) {
      const unsigned int requested = options.numFramesToGenerate;
      const unsigned int previous = g_native_requested.exchange(requested, std::memory_order_relaxed);
      const unsigned int raw_result = static_cast<unsigned int>(result);
      const unsigned int previous_result =
          g_native_result.exchange(raw_result, std::memory_order_relaxed);
      const bool first = !g_native_request_seen.exchange(true, std::memory_order_relaxed);
      if (first || previous != requested || previous_result != raw_result) {
        std::stringstream s;
        s << "mfgunlock: native slDLSSGSetOptions requested numFramesToGenerate=" << requested
          << " (" << (requested + 1) << "x) and returned sl::Result " << raw_result << ".";
        reshade::log::message(result == sl::Result::eOk ? reshade::log::level::info
                                                       : reshade::log::level::warning,
                              s.str().c_str());
      }
    }
    return result;
  }

  const uint32_t desired = multiplier - 1;  // generated frames, not total
  const uint32_t requested = options.numFramesToGenerate;
  g_last_requested.store(requested, std::memory_order_relaxed);
  if (requested >= desired) return CallSetOptions(viewport, options);

  // More than one generated frame needs software pacing. The plugin is loaded
  // by now, so this is our last and best chance to patch it.
  if (desired > 1) {
    if (g_pacing_ready != nullptr && !g_pacing_ready() && g_ensure_pacing != nullptr) {
      g_ensure_pacing();
    }
    if (g_pacing_ready != nullptr && !g_pacing_ready()) {
      if (!g_declined_no_pacing.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: NOT forcing the multiplier -- flip metering is still enabled, and "
            "asking for more than one generated frame without software pacing freezes "
            "presentation. Leaving the game's own request alone.");
      }
      return CallSetOptions(viewport, options);
    }
  }

  // Forward through a local upgraded structure when HDR recomposition is
  // active. The native fallback temporarily changes only the requested count
  // and restores the game-owned structure before returning.
  sl::Result result = CallSetOptions(viewport, options, desired, true);

  // A rejected call means frame generation just stays off, which looks exactly
  // like "the mod broke FG". Never leave the game worse than we found it: if
  // the runtime refuses the raised count, put the original request through so
  // the player still gets the frame generation they asked for.
  if (result != sl::Result::eOk) {
    // eErrorFeatureManagerInvalidState is not "your count is too high" -- it can
    // simply mean DLSS-G was not ready yet. Retry once with the SAME raised
    // count: if that succeeds the first failure was transient state, and
    // dropping straight back to the game's request would have silently given up
    // a working 4x. Only if the retry fails too is the count really refused.
    const sl::Result retry = CallSetOptions(viewport, options, desired, true);

    if (retry == sl::Result::eOk) {
      if (!g_intercepted.exchange(true, std::memory_order_relaxed)) {
        std::stringstream s;
        s << "mfgunlock: slDLSSGSetOptions returned " << static_cast<unsigned int>(result)
          << " on the first attempt but accepted numFramesToGenerate=" << desired
          << " on retry -- that first failure was feature-manager state, not the count.";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
      g_last_result.store(static_cast<unsigned int>(retry), std::memory_order_relaxed);
      g_last_forced.store(desired, std::memory_order_relaxed);
      return retry;
    }

    if (g_force_failed_for.exchange(desired, std::memory_order_relaxed) != desired) {
      std::stringstream s;
      s << "mfgunlock: slDLSSGSetOptions refused numFramesToGenerate=" << desired
        << " twice (sl::Result " << static_cast<unsigned int>(result) << " then "
        << static_cast<unsigned int>(retry) << "); falling back to the game's own request of "
        << requested << ". The count itself is being refused, not a transient state.";
      reshade::log::message(reshade::log::level::warning, s.str().c_str());
    }
    g_last_result.store(static_cast<unsigned int>(retry), std::memory_order_relaxed);
    return CallSetOptions(viewport, options);
  }

  if (!g_intercepted.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: raising DLSS-G numFramesToGenerate from " << requested << " to " << desired
      << " (" << multiplier << "x) -- the game only ever asks for "
      << (requested + 1) << "x. slDLSSGSetOptions accepted it.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  g_last_result.store(static_cast<unsigned int>(result), std::memory_order_relaxed);
  g_last_forced.store(desired, std::memory_order_relaxed);
  return result;
}

inline sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                 const sl::DLSSGOptions* options) {
  if (g_real_get_state == nullptr) return sl::Result::eErrorNotInitialized;

  const sl::Result result = g_real_get_state(viewport, state, options);
  const unsigned int raw_result = static_cast<unsigned int>(result);
  g_state_result.store(raw_result, std::memory_order_relaxed);
  if (result == sl::Result::eOk) {
    const unsigned int status = static_cast<unsigned int>(state.status);
    g_dlssg_status.store(status, std::memory_order_relaxed);
    const unsigned int presented = state.numFramesActuallyPresented;
    const unsigned int previous =
        g_actual_frames_presented.exchange(presented, std::memory_order_relaxed);
    unsigned int observed_max = g_max_actual_frames_presented.load(std::memory_order_relaxed);
    while (presented > observed_max &&
           !g_max_actual_frames_presented.compare_exchange_weak(
               observed_max, presented, std::memory_order_relaxed)) {
    }
    g_state_samples.fetch_add(1, std::memory_order_relaxed);
    const bool first = !g_state_seen.exchange(true, std::memory_order_relaxed);
    bool log_status = false;
    if (status == 0) {
      log_status = !g_status_ok_logged.exchange(true, std::memory_order_relaxed);
    } else {
      log_status = !g_failure_status_logged.exchange(true, std::memory_order_relaxed);
    }
    if (log_status) {
      std::stringstream s;
      s << "mfgunlock: slDLSSGGetState runtime status is 0x" << std::hex << status << std::dec
        << (status == 0 ? " (OK)." : " (DLSS-G reported one or more failure flags).");
      reshade::log::message(status == 0 ? reshade::log::level::info
                                        : reshade::log::level::warning,
                            s.str().c_str());
    }
    if (first || previous != presented) {
      const unsigned int bit = presented < 32 ? (1u << presented) : 0u;
      const unsigned int seen =
          bit == 0 ? ~0u : g_seen_present_counts.fetch_or(bit, std::memory_order_relaxed);
      if (first || (seen & bit) == 0) {
        std::stringstream s;
        s << "mfgunlock: slDLSSGGetState reports " << presented
          << " frame(s) actually presented since its previous call.";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
    }
  }
  if (result != sl::Result::eOk || state.structVersion < sl::kStructVersion2) return result;

  const unsigned int reported = state.numFramesToGenerateMax;
  g_runtime_max_generated.store(reported, std::memory_order_relaxed);

  const unsigned int wanted = g_advertised_max_generated.load(std::memory_order_relaxed);
  if (wanted < 2 || reported >= wanted) return result;

  state.numFramesToGenerateMax = wanted;
  if (!g_capacity_advertised.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: slDLSSGGetState reported a maximum of " << reported
      << " generated frame(s); advertising the verified Streamline ceiling of " << wanted
      << " so the game's native multiplier selector can expose up to " << (wanted + 1) << "x.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  return result;
}

inline sl::Result HookedGetFeatureFunction(sl::Feature feature, const char* function_name,
                                           void*& function) {
  const sl::Result result = g_real_get_feature_function(feature, function_name, function);
  if (result != sl::Result::eOk || function_name == nullptr || function == nullptr) return result;
  if (std::strcmp(function_name, "slDLSSGSetOptions") == 0) {
    g_real_set_options = reinterpret_cast<SetOptionsFn>(function);
    function = reinterpret_cast<void*>(&HookedSetOptions);
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: wrapped slDLSSGSetOptions; frame-multiplier override is live.");
  } else if (std::strcmp(function_name, "slDLSSGGetState") == 0) {
    g_real_get_state = reinterpret_cast<GetStateFn>(function);
    function = reinterpret_cast<void*>(&HookedGetState);
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: wrapped slDLSSGGetState; native multiplier-menu override is live.");
  }
  return result;
}

inline std::vector<hook::HookItem> g_installed_hooks;
inline std::atomic_bool g_installing{false};

}  // namespace internal

// A temporal reset is only requested after a state transition. It is consumed
// once by HookedSetConstants and never injected continuously, so normal frame
// pacing and the game's steady-state temporal history remain untouched.
inline void NotifyHdrState(bool hdr) {
  const bool previous = g_hdr_active.exchange(hdr, std::memory_order_relaxed);
  const bool seen = g_hdr_state_seen.exchange(true, std::memory_order_acq_rel);
  if (seen && previous != hdr &&
      g_hdr_compatibility_mode.load(std::memory_order_relaxed) ==
          static_cast<unsigned int>(HdrCompatibilityMode::kFinalColorFallback)) {
    internal::RequestAllResets();
  }
}

inline void NotifySwapchainTransition() {
  internal::ForgetOutputDescriptions();
  if (g_hdr_compatibility_mode.load(std::memory_order_relaxed) ==
      static_cast<unsigned int>(HdrCompatibilityMode::kFinalColorFallback)) {
    internal::RequestAllResets();
  }
}

inline void NotifyQualityModeChanged() {
  internal::ForgetOutputDescriptions();
  internal::RequestAllResets();
}

inline void NotifyDepthEdgeTuningChanged() {
  g_depth_edge_override_applied.store(false, std::memory_order_relaxed);
  g_depth_edge_override_logged.store(false, std::memory_order_relaxed);
  internal::RequestAllResets();
}

// Must land before the game asks for the function pointer, which it does once
// during DLSS-G setup -- hence installing from the earliest event we get rather
// than waiting for a present.
inline void TryInstall() {
  if (g_hooked.load(std::memory_order_acquire)) return;
  bool expected = false;
  if (!internal::g_installing.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }
  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (interposer == nullptr ||
      GetProcAddress(interposer, "slGetFeatureFunction") == nullptr) {
    internal::g_installing.store(false, std::memory_order_release);
    return;
  }

  std::vector<hook::HookItem> hooks = {
      {"slGetFeatureFunction", reinterpret_cast<void**>(&internal::g_real_get_feature_function),
       reinterpret_cast<void*>(&internal::HookedGetFeatureFunction)}};
  if (GetProcAddress(interposer, "slSetTag") != nullptr) {
    hooks.push_back(
        {"slSetTag", reinterpret_cast<void**>(&internal::g_real_set_tag),
         reinterpret_cast<void*>(&internal::HookedSetTag)});
  }
  if (GetProcAddress(interposer, "slSetTagForFrame") != nullptr) {
    hooks.push_back(
        {"slSetTagForFrame", reinterpret_cast<void**>(&internal::g_real_set_tag_for_frame),
         reinterpret_cast<void*>(&internal::HookedSetTagForFrame)});
  }
  if (GetProcAddress(interposer, "slSetConstants") != nullptr) {
    hooks.push_back(
        {"slSetConstants", reinterpret_cast<void**>(&internal::g_real_set_constants),
         reinterpret_cast<void*>(&internal::HookedSetConstants)});
  }
  // slInit is only detoured when the OTA override is actually wanted.
  if (g_force_ota.load(std::memory_order_relaxed)) {
    hooks.push_back(
        {"slInit", reinterpret_cast<void**>(&internal::g_real_init),
         reinterpret_cast<void*>(&internal::HookedInit)});
  }
  if (!hook::Install(interposer, hooks, "sl.interposer.dll")) {
    internal::g_installing.store(false, std::memory_order_release);
    return;
  }
  internal::g_installed_hooks = std::move(hooks);
  g_hooked.store(true, std::memory_order_release);
  internal::g_installing.store(false, std::memory_order_release);
}

inline void Uninstall() {
  if (!g_hooked.load(std::memory_order_acquire)) return;
  if (!internal::g_installed_hooks.empty()) hook::Uninstall(internal::g_installed_hooks);
  internal::g_installed_hooks.clear();
  g_hooked.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::framecount
