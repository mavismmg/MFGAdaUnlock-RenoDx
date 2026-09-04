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

#include <atomic>
#include <cstring>
#include <sstream>

#include <sl.h>
#include <sl_dlss_g.h>

#include <include/reshade.hpp>

#include "./ngx_hook.hpp"

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
inline std::atomic<unsigned int> g_actual_frames_presented{0};
inline std::atomic<unsigned int> g_max_actual_frames_presented{0};
inline std::atomic<unsigned long long> g_state_samples{0};
inline std::atomic<unsigned int> g_seen_present_counts{0};

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

inline SetOptionsFn g_real_set_options = nullptr;
inline GetStateFn g_real_get_state = nullptr;
inline GetFeatureFunctionFn g_real_get_feature_function = nullptr;
inline InitFn g_real_init = nullptr;

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
    const sl::Result result = g_real_set_options(viewport, options);
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
  if (requested >= desired) return g_real_set_options(viewport, options);

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
      return g_real_set_options(viewport, options);
    }
  }

  // Raise it for the duration of the call, then restore. The caller owns this
  // struct and may well reuse it; leaving it modified would be a side effect
  // nobody asked for.
  auto& mutable_options = const_cast<sl::DLSSGOptions&>(options);
  mutable_options.numFramesToGenerate = desired;
  sl::Result result = g_real_set_options(viewport, options);
  mutable_options.numFramesToGenerate = requested;

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
    mutable_options.numFramesToGenerate = desired;
    const sl::Result retry = g_real_set_options(viewport, options);
    mutable_options.numFramesToGenerate = requested;

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
    return g_real_set_options(viewport, options);
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
    const unsigned int previous_status =
        g_dlssg_status.exchange(status, std::memory_order_relaxed);
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
    if (first || previous_status != status) {
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

inline const std::vector<hook::HookItem> kInterposerHooks = {
    {"slGetFeatureFunction", reinterpret_cast<void**>(&g_real_get_feature_function),
     reinterpret_cast<void*>(&HookedGetFeatureFunction)},
};

// slInit is only detoured when the OTA override is actually wanted -- an unused
// hook on a game's init path is risk for nothing.
inline const std::vector<hook::HookItem> kInterposerHooksWithOta = {
    {"slGetFeatureFunction", reinterpret_cast<void**>(&g_real_get_feature_function),
     reinterpret_cast<void*>(&HookedGetFeatureFunction)},
    {"slInit", reinterpret_cast<void**>(&g_real_init), reinterpret_cast<void*>(&HookedInit)},
};

inline const std::vector<hook::HookItem>* g_installed_hooks = nullptr;

}  // namespace internal

// Must land before the game asks for the function pointer, which it does once
// during DLSS-G setup -- hence installing from the earliest event we get rather
// than waiting for a present.
inline void TryInstall() {
  if (g_hooked.load(std::memory_order_acquire)) return;
  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (interposer == nullptr) return;
  if (GetProcAddress(interposer, "slGetFeatureFunction") == nullptr) return;
  const auto* hooks = g_force_ota.load(std::memory_order_relaxed)
                          ? &internal::kInterposerHooksWithOta
                          : &internal::kInterposerHooks;
  if (!hook::Install(interposer, *hooks, "sl.interposer.dll")) return;
  internal::g_installed_hooks = hooks;
  g_hooked.store(true, std::memory_order_release);
}

inline void Uninstall() {
  if (!g_hooked.load(std::memory_order_acquire)) return;
  if (internal::g_installed_hooks != nullptr) hook::Uninstall(*internal::g_installed_hooks);
  internal::g_installed_hooks = nullptr;
  g_hooked.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::framecount
