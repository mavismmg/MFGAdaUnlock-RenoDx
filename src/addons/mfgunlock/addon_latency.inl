// SPDX-License-Identifier: MIT
// DLSS-G latency (plan 6.5): the display refresh, the DXGI latency policy,
// the Reflex waitable swapchain, the Reflex pacing lab, the VRAM diagnostics
// and the Latency Guard update, run from the Present path. The Latency Guard
// results are published for the UI as framecount::LatencyGuardView.
//
// A fragment of addon.cpp, included inside its anonymous namespace: it uses
// the configuration and state declared there and is not a standalone header.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "addon_latency.inl is included only by addon.cpp"
#endif

uint32_t DetectDisplayRefreshFps(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr || swapchain->get_hwnd() == nullptr) return 0;
  const HMONITOR monitor = MonitorFromWindow(
      static_cast<HWND>(swapchain->get_hwnd()), MONITOR_DEFAULTTONEAREST);
  if (monitor == nullptr) return 0;
  static HMONITOR cached_monitor = nullptr;
  static uint64_t cached_epoch = 0, checked_ms = 0;
  static uint32_t cached_refresh = 0;
  const auto epoch = mfgunlock::framecount::g_latency_guard_epoch.load(std::memory_order_acquire);
  const auto now = GetTickCount64();
  if (monitor == cached_monitor && epoch == cached_epoch && now - checked_ms < 5000)
    return cached_refresh;
  cached_monitor = monitor; cached_epoch = epoch; checked_ms = now; cached_refresh = 0;
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    return 0;
  cached_refresh = mode.dmDisplayFrequency > 1 && mode.dmDisplayFrequency <= 1000
             ? mode.dmDisplayFrequency
             : 0;
  return cached_refresh;
}

void ObserveDxgiLatencyPolicy(reshade::api::swapchain* swapchain) {
  auto* device = swapchain == nullptr ? nullptr : swapchain->get_device();
  if (device == nullptr ||
      (device->get_api() != reshade::api::device_api::d3d11 &&
       device->get_api() != reshade::api::device_api::d3d12) ||
      swapchain->get_native() == 0) {
    g_latency_guard_dxgi_observed.store(false, std::memory_order_release);
    g_latency_guard_waitable_swapchain.store(false,
                                              std::memory_order_relaxed);
    g_latency_guard_dxgi_max_latency.store(0, std::memory_order_relaxed);
    return;
  }
  auto* native = reinterpret_cast<IUnknown*>(
      static_cast<uintptr_t>(swapchain->get_native()));
  IDXGISwapChain2* swapchain2 = nullptr;
  if (FAILED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) ||
      swapchain2 == nullptr)
    return;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  const bool desc_ok = SUCCEEDED(swapchain2->GetDesc1(&desc));
  const bool waitable =
      desc_ok &&
      (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
  UINT maximum_latency = 0;
  if (!waitable || FAILED(swapchain2->GetMaximumFrameLatency(&maximum_latency)))
    maximum_latency = 0;
  swapchain2->Release();

  g_latency_guard_waitable_swapchain.store(waitable,
                                            std::memory_order_relaxed);
  g_latency_guard_dxgi_max_latency.store(maximum_latency,
                                         std::memory_order_relaxed);
  g_latency_guard_dxgi_observed.store(desc_ok, std::memory_order_release);
}

uint32_t DetectDisplayRefreshMilliHz(reshade::api::swapchain* swapchain) {
  const uint32_t fallback_fps = DetectDisplayRefreshFps(swapchain);
  const uint32_t fallback_millihz =
      fallback_fps >= 10 && fallback_fps <= 1000 ? fallback_fps * 1000u : 0u;
  // 1.10.0: only a D3D swap chain is a COM object. A Vulkan one (VkSwapchainKHR) read as IUnknown crashed DOOM: The Dark
  // Ages when the output-FPS cap was turned on.
  auto* device = swapchain == nullptr ? nullptr : swapchain->get_device();
  const bool dxgi = device != nullptr && (device->get_api() == reshade::api::device_api::d3d10 ||
                                          device->get_api() == reshade::api::device_api::d3d11 ||
                                          device->get_api() == reshade::api::device_api::d3d12);
  if (dxgi && swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain* dxgi_swapchain = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&dxgi_swapchain))) &&
        dxgi_swapchain != nullptr) {
      DXGI_SWAP_CHAIN_DESC desc{};
      const bool valid = SUCCEEDED(dxgi_swapchain->GetDesc(&desc)) &&
          desc.BufferDesc.RefreshRate.Numerator != 0 &&
          desc.BufferDesc.RefreshRate.Denominator != 0;
      const uint64_t millihz = valid
          ? (static_cast<uint64_t>(
                 desc.BufferDesc.RefreshRate.Numerator) * 1000ull +
             desc.BufferDesc.RefreshRate.Denominator / 2u) /
                desc.BufferDesc.RefreshRate.Denominator
          : 0;
      dxgi_swapchain->Release();
      if (millihz >= 10000 && millihz <= 1000000) {
        // Flip-model borderless swapchains often retain a stale nominal
        // BufferDesc refresh (commonly 60 Hz) on a 120-480 Hz desktop. Use the
        // rational value only when it agrees with the active monitor mode.
        const uint32_t rational = static_cast<uint32_t>(millihz);
        const uint32_t difference = rational > fallback_millihz
            ? rational - fallback_millihz
            : fallback_millihz - rational;
        if (fallback_millihz == 0 || difference <= 1500)
          return rational;
      }
    }
  }
  return fallback_millihz;
}

const char* ReflexWaitableFallbackText(
    mfgunlock::reflexpacing::FallbackReason reason) {
  using Reason = mfgunlock::reflexpacing::FallbackReason;
  switch (reason) {
    case Reason::kNotRequested: return "Native sleep requested";
    case Reason::kUnsupportedApi: return "Renderer is not D3D11/D3D12";
    case Reason::kMfgInactive: return "MFG is not active at 2x-6x";
    case Reason::kNoWaitableFlag: return "Swapchain has no waitable-object flag";
    case Reason::kNoWaitableHandle: return "DXGI returned no waitable handle";
    case Reason::kHandleDuplicationFailed: return "Waitable handle duplication failed";
    case Reason::kMaximumLatencyQueryFailed: return "MaximumFrameLatency query failed";
    case Reason::kMaximumLatencySetFailed: return "Could not set MaximumFrameLatency to 1";
    case Reason::kSwapchainChanged: return "Primary swapchain changed";
    case Reason::kTokenDuplicate: return "Duplicate Reflex frame token";
    case Reason::kTokenRegressed: return "Regressing Reflex frame token";
    case Reason::kConcurrentSleep: return "Concurrent slReflexSleep calls";
    case Reason::kNativeSleepFailed: return "Native slReflexSleep failed during probe";
    case Reason::kWaitTimeout: return "DXGI waitable object timed out";
    case Reason::kWaitFailed: return "DXGI waitable-object wait failed";
    default: return "None";
  }
}

void RestoreReflexWaitableLocked(
    reshade::api::swapchain* swapchain,
    mfgunlock::reflexpacing::FallbackReason reason) {
  const HANDLE duplicate = mfgunlock::reflexpacing::RetireWaitable(reason);
  if (duplicate != nullptr) CloseHandle(duplicate);
  if (g_reflex_waitable.maximum_latency_forced && swapchain != nullptr &&
      swapchain == g_reflex_waitable.owner && swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain2* swapchain2 = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) &&
        swapchain2 != nullptr) {
      UINT current = 0;
      if (SUCCEEDED(swapchain2->GetMaximumFrameLatency(&current)) &&
          current == 1 && g_reflex_waitable.native_maximum_latency != 0) {
        swapchain2->SetMaximumFrameLatency(
            g_reflex_waitable.native_maximum_latency);
      }
      swapchain2->Release();
    }
  }
  g_reflex_waitable.owner = nullptr;
  g_reflex_waitable.native_maximum_latency = 0;
  g_reflex_waitable.maximum_latency_forced = false;
}

bool PublishReflexWaitableLocked(reshade::api::swapchain* swapchain,
                                 uint32_t refresh_millihz) {
  using namespace mfgunlock::reflexpacing;
  if (swapchain == nullptr || swapchain->get_native() == 0) {
    SetFallback(FallbackReason::kNoWaitableHandle);
    return false;
  }
  auto* device = swapchain->get_device();
  if (device == nullptr ||
      (device->get_api() != reshade::api::device_api::d3d11 &&
       device->get_api() != reshade::api::device_api::d3d12)) {
    SetFallback(FallbackReason::kUnsupportedApi);
    return false;
  }
  auto* native = reinterpret_cast<IUnknown*>(
      static_cast<uintptr_t>(swapchain->get_native()));
  IDXGISwapChain2* swapchain2 = nullptr;
  if (FAILED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) ||
      swapchain2 == nullptr) {
    SetFallback(FallbackReason::kNoWaitableHandle);
    return false;
  }
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (FAILED(swapchain2->GetDesc1(&desc)) ||
      (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0) {
    swapchain2->Release();
    SetFallback(FallbackReason::kNoWaitableFlag);
    return false;
  }
  const HANDLE borrowed = swapchain2->GetFrameLatencyWaitableObject();
  if (borrowed == nullptr) {
    swapchain2->Release();
    SetFallback(FallbackReason::kNoWaitableHandle);
    return false;
  }
  UINT maximum_latency = 0;
  if (FAILED(swapchain2->GetMaximumFrameLatency(&maximum_latency)) ||
      maximum_latency == 0) {
    swapchain2->Release();
    SetFallback(FallbackReason::kMaximumLatencyQueryFailed);
    return false;
  }
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    swapchain2->Release();
    SetFallback(FallbackReason::kHandleDuplicationFailed);
    return false;
  }
  swapchain2->Release();
  const uint32_t two_periods_ms = refresh_millihz != 0
      ? static_cast<uint32_t>((2000000ull + refresh_millihz - 1ull) /
                              refresh_millihz)
      : 48u;
  const uint32_t timeout_ms =
      (std::clamp)(two_periods_ms + 2u, 8u, 50u);
  const uint64_t generation = ++g_reflex_waitable.generation;
  g_reflex_waitable.owner = swapchain;
  g_reflex_waitable.native_maximum_latency = maximum_latency;
  g_reflex_waitable.maximum_latency_forced = false;
  PublishWaitable(duplicate, generation, timeout_ms, maximum_latency);
  std::stringstream log;
  log << "mfgunlock: DXGI Waitable probe started (generation " << generation
      << ", native MaximumFrameLatency=" << maximum_latency
      << ", timeout=" << timeout_ms << " ms).";
  reshade::log::message(reshade::log::level::info, log.str().c_str());
  return true;
}

#if defined(MFGUNLOCK_NATIVE_XEMFG) && defined(MFGUNLOCK_XEMFG_VULKAN)
// 1.10.0 (plan XMX Vulkan pacing): the real-frame interval XMX's Vulkan provider can hold, as an output frame limit for
// the game's Reflex (framecount.hpp composes it). None when the GPU holds the display's refresh anyway. Changes under
// 3% are not replayed, so Reflex is not re-set every frame.
void UpdateXmxVulkanPacing(reshade::api::swapchain* swapchain) {
  namespace vk = mfgunlock::xemfg::native::vulkan;
  uint32_t limit_us = 0;
  const uint32_t base_us = vk::XmxPacingEnabled() ? vk::XmxPacingIntervalUs() : 0;
  if (base_us != 0 && mfgunlock::framecount::g_effective_request_seen.load(std::memory_order_acquire)) {
    const uint32_t multiplier = (std::max)(1u, mfgunlock::framecount::g_last_effective_generated.load(std::memory_order_relaxed) + 1u);
    const uint32_t output_us = base_us / multiplier;
    const uint32_t refresh_millihz = DetectDisplayRefreshMilliHz(swapchain);
    const uint32_t refresh_us = refresh_millihz ? uint32_t(1000000000ull / refresh_millihz) : 0;
    limit_us = refresh_us != 0 && output_us <= refresh_us + refresh_us / 50 ? 0 : output_us;
  }
  const uint32_t previous = mfgunlock::reflexpacing::g_xmx_vulkan_limit_us.load(std::memory_order_relaxed);
  const bool changed = (previous == 0) != (limit_us == 0) ||
      (previous != 0 && (limit_us > previous + previous / 33 || limit_us + previous / 33 < previous));
  if (!changed) return;
  mfgunlock::reflexpacing::g_xmx_vulkan_limit_us.store(limit_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_refresh_pending.store(true, std::memory_order_release);
  static uint32_t logged = 0;
  if (logged < 40) {
    ++logged;
    std::stringstream s;
    s << "mfgunlock: XMX Vulkan pacing: output limit " << (limit_us ? 1000000.0 / limit_us : 0.0)
      << " fps (usual real-frame interval " << vk::XmxPacingCostUs() / 1000.0 << " ms).";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
}
#endif

void UpdateReflexPacingLab(reshade::api::swapchain* swapchain) {
  using namespace mfgunlock;
  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire))
    return;
  const bool cap_poll_requested = framecount::g_addon_enabled.load(std::memory_order_relaxed) &&
      pacing::IsValidReflexOutputFpsCap(framecount::g_reflex_source_fps_cap.load());
  if (!cap_poll_requested && !reflexpacing::WaitableRequested() &&
      !reflexpacing::LabMeasurementEnabled() &&
      reflexpacing::g_sleep_dispatch.load(std::memory_order_acquire) ==
          reflexpacing::SleepDispatch::kNative &&
      g_reflex_waitable_idle.load(std::memory_order_acquire) &&
      reflexpacing::g_headroom_limit_us.load(std::memory_order_relaxed) == 0)
    return;
  static SRWLOCK update_lock = SRWLOCK_INIT;
  if (!TryAcquireSRWLockExclusive(&update_lock)) return;
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockExclusive(lock); }
  } unlock{&update_lock};

  const uint32_t estimated_multiplier =
      framecount::internal::ActiveMultiplierForEstimate();
  reflexpacing::g_mfg_multiplier.store(estimated_multiplier,
                                       std::memory_order_relaxed);

  static ULONGLONG next_status_sample = 0;
  const ULONGLONG now = GetTickCount64();
  const bool addon_enabled =
      framecount::g_addon_enabled.load(std::memory_order_relaxed);
  const uint64_t ui_heartbeat = reflexpacing::g_ui_heartbeat_ms.load(
      std::memory_order_acquire);
  const bool measure_native = addon_enabled &&
      (reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed) ||
       pacing::NormalizeReflexModeOverride(
           reflexpacing::g_mode_override.load(std::memory_order_relaxed)) !=
           pacing::ReflexModeOverride::kGame ||
       (ui_heartbeat != 0 && now - ui_heartbeat <= 1500));
  const bool fg_safe_off = addon_enabled &&
      pacing::NormalizeReflexModeOverride(
          reflexpacing::g_mode_override.load(std::memory_order_relaxed)) ==
          pacing::ReflexModeOverride::kOff;
  const auto next_dispatch =
      addon_enabled && reflexpacing::WaitableRequested()
          ? reflexpacing::SleepDispatch::kWaitable
          : (fg_safe_off
                 ? reflexpacing::SleepDispatch::kBypass
                 : (measure_native
                        ? reflexpacing::SleepDispatch::kMeasureNative
                        : reflexpacing::SleepDispatch::kNative));
  const auto previous_dispatch = reflexpacing::g_sleep_dispatch.exchange(
      next_dispatch, std::memory_order_acq_rel);
  if (previous_dispatch != next_dispatch &&
      (previous_dispatch == reflexpacing::SleepDispatch::kBypass ||
       next_dispatch == reflexpacing::SleepDispatch::kBypass)) {
    reshade::log::message(
        reshade::log::level::info,
        next_dispatch == reflexpacing::SleepDispatch::kBypass
            ? "mfgunlock: FG-safe Reflex Off active; Streamline mode remains On and slReflexSleep is bypassed."
            : "mfgunlock: FG-safe Reflex sleep bypass released.");
  }
  const bool lab_requested = addon_enabled &&
      (cap_poll_requested || reflexpacing::WaitableRequested() ||
       reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed));
  if (lab_requested && now >= next_status_sample) {
    next_status_sample = now + 500;
    bool vrr = false;
    uint32_t driver_multiplier = 0;
    if (auto* device = swapchain->get_device();
        device != nullptr && device->get_native() != 0 &&
        (device->get_api() == reshade::api::device_api::d3d11 ||
         device->get_api() == reshade::api::device_api::d3d12)) {
      nvapistatus::SleepStatus sleep{};
      sleep.version = nvapistatus::StructVersion<nvapistatus::SleepStatus, 1>();
      const auto get_sleep = nvapistatus::GetSleepStatus();
      if (get_sleep != nullptr && nvapistatus::Initialize() ==
                                      nvapistatus::kOk &&
          get_sleep(reinterpret_cast<IUnknown*>(
                        static_cast<uintptr_t>(device->get_native())),
                    &sleep) == nvapistatus::kOk) {
        vrr = sleep.fullscreen_vrr != 0;
        driver_multiplier = sleep.frame_generation_multiplier;
      }
    }
    reflexpacing::g_vrr_active.store(vrr, std::memory_order_relaxed);
    const uint32_t previous_cap_multiplier = framecount::g_cap_driver_multiplier.exchange(
        driver_multiplier, std::memory_order_relaxed);
    framecount::g_cap_driver_observed_ms.store(now, std::memory_order_release);
    if (previous_cap_multiplier != driver_multiplier) {
      framecount::g_latency_guard_epoch.fetch_add(1, std::memory_order_acq_rel);
      framecount::g_latency_guard_refresh_pending.store(true, std::memory_order_release);
    }
    if (driver_multiplier >= 2 && driver_multiplier <= 6)
      reflexpacing::g_mfg_multiplier.store(driver_multiplier,
                                           std::memory_order_relaxed);
    const uint32_t refresh_millihz = DetectDisplayRefreshMilliHz(swapchain);
    reflexpacing::g_refresh_millihz.store(refresh_millihz,
                                          std::memory_order_relaxed);
    const uint32_t headroom_limit =
        addon_enabled &&
                reflexpacing::g_headroom_enabled.load(
                    std::memory_order_relaxed) &&
                vrr
            ? pacing::VrrHeadroomFrameLimitUs(
                  refresh_millihz,
                  pacing::NormalizeHeadroomBasisPoints(
                      reflexpacing::g_headroom_basis_points.load(
                          std::memory_order_relaxed)))
            : 0;
    const uint32_t previous = reflexpacing::g_headroom_limit_us.exchange(
        headroom_limit, std::memory_order_acq_rel);
    if (previous != headroom_limit) {
      framecount::g_latency_guard_epoch.fetch_add(1,
                                                   std::memory_order_acq_rel);
      framecount::g_latency_guard_refresh_pending.store(
          true, std::memory_order_release);
    }
  } else if (!lab_requested &&
             reflexpacing::g_headroom_limit_us.exchange(
                 0, std::memory_order_acq_rel) != 0) {
    framecount::g_latency_guard_epoch.fetch_add(1, std::memory_order_acq_rel);
    framecount::g_latency_guard_refresh_pending.store(
        true, std::memory_order_release);
  }

  const bool requested =
      framecount::g_addon_enabled.load(std::memory_order_relaxed) &&
      reflexpacing::WaitableRequested();
  if (!requested && g_reflex_waitable_idle.load(std::memory_order_acquire))
    return;
  g_reflex_waitable_idle.store(false, std::memory_order_release);
  AcquireSRWLockExclusive(&g_reflex_waitable.lock);
  if (requested != g_reflex_waitable.last_requested) {
    g_reflex_waitable.last_requested = requested;
    g_reflex_waitable.fallback_latched = false;
    if (requested) {
      framecount::g_latency_guard_auto_cap_ready.store(
          false, std::memory_order_release);
      framecount::g_latency_guard_active_source_cap_fps.store(
          0, std::memory_order_relaxed);
      framecount::g_latency_guard_multiplier_override.store(
          0, std::memory_order_release);
      framecount::g_latency_guard_multiplier_trial_accepted.store(
          false, std::memory_order_relaxed);
      framecount::g_latency_guard_refresh_pending.store(
          true, std::memory_order_release);
    }
    if (!requested && g_reflex_waitable.owner != nullptr)
      RestoreReflexWaitableLocked(g_reflex_waitable.owner,
                                  reflexpacing::FallbackReason::kNotRequested);
  }
  if (!requested) {
    reflexpacing::g_waitable_state.store(
        reflexpacing::WaitableState::kNative, std::memory_order_release);
    g_reflex_waitable_idle.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  const uint32_t live_multiplier = reflexpacing::g_mfg_multiplier.load(
      std::memory_order_relaxed);
  if (live_multiplier < 2 || live_multiplier > 6) {
    if (g_reflex_waitable.owner != nullptr) {
      RestoreReflexWaitableLocked(
          g_reflex_waitable.owner,
          reflexpacing::FallbackReason::kNotRequested);
    }
    reflexpacing::g_fallback_reason.store(
        reflexpacing::FallbackReason::kMfgInactive,
        std::memory_order_relaxed);
    reflexpacing::g_waitable_state.store(
        reflexpacing::WaitableState::kWaitingForSwapchain,
        std::memory_order_release);
    g_reflex_waitable.fallback_latched = false;
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  const auto state = reflexpacing::g_waitable_state.load(
      std::memory_order_acquire);
  if (state == reflexpacing::WaitableState::kFallback) {
    const auto reason = reflexpacing::g_fallback_reason.load(
        std::memory_order_relaxed);
    if (g_reflex_waitable.owner != nullptr)
      RestoreReflexWaitableLocked(g_reflex_waitable.owner, reason);
    if (!g_reflex_waitable.fallback_latched) {
      std::stringstream log;
      log << "mfgunlock: DXGI Waitable fell back to native Reflex sleep: "
          << ReflexWaitableFallbackText(reason) << ".";
      reshade::log::message(reshade::log::level::warning,
                            log.str().c_str());
      g_reflex_waitable.fallback_latched = true;
    }
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  if (g_reflex_waitable.owner != nullptr &&
      g_reflex_waitable.owner != swapchain) {
    RestoreReflexWaitableLocked(g_reflex_waitable.owner,
                                reflexpacing::FallbackReason::kSwapchainChanged);
    g_reflex_waitable.fallback_latched = true;
    ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
    return;
  }
  if (g_reflex_waitable.owner == nullptr &&
      !g_reflex_waitable.fallback_latched) {
    PublishReflexWaitableLocked(
        swapchain,
        reflexpacing::g_refresh_millihz.load(std::memory_order_relaxed));
  } else if (state == reflexpacing::WaitableState::kActivationPending &&
             g_reflex_waitable.owner == swapchain &&
             swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain2* swapchain2 = nullptr;
    bool success = false;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain2))) &&
        swapchain2 != nullptr) {
      success = g_reflex_waitable.native_maximum_latency == 1 ||
                SUCCEEDED(swapchain2->SetMaximumFrameLatency(1));
      swapchain2->Release();
    }
    g_reflex_waitable.maximum_latency_forced =
        success && g_reflex_waitable.native_maximum_latency != 1;
    reflexpacing::MarkActivationResult(g_reflex_waitable.generation, success);
    if (success) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: DXGI Waitable probe passed; alternate pacing is active with MaximumFrameLatency=1.");
    }
  }
  ReleaseSRWLockExclusive(&g_reflex_waitable.lock);
}

// 1.10.0 (plan 1.7): the adapter is kept per device. A factory per sample
// went through ReShade's CreateDXGIFactory1 hook once a second while the
// panel was open, and filled every field log with that line.
IDXGIAdapter3* ResolveVideoMemoryAdapter(reshade::api::device* device) {
  IDXGIAdapter3* adapter3 = nullptr;
  if (device->get_api() == reshade::api::device_api::d3d12) {
    auto* d3d12 = reinterpret_cast<ID3D12Device*>(
        static_cast<uintptr_t>(device->get_native()));
    IDXGIFactory4* factory = nullptr;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    const auto create_factory = dxgi == nullptr
        ? nullptr
        : reinterpret_cast<CreateFactoryFn>(
              GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (create_factory != nullptr &&
        SUCCEEDED(create_factory(IID_PPV_ARGS(&factory))) &&
        factory != nullptr) {
      factory->EnumAdapterByLuid(d3d12->GetAdapterLuid(),
                                 IID_PPV_ARGS(&adapter3));
      factory->Release();
    }
  } else if (device->get_api() == reshade::api::device_api::d3d11) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(device->get_native()));
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
        dxgi_device != nullptr) {
      if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr) {
        adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        adapter->Release();
      }
      dxgi_device->Release();
    }
  }
  return adapter3;
}

// Called from the present callback only (UpdateVramDiagnostics).
bool QueryVideoMemory(reshade::api::device* device,
                      DXGI_MEMORY_SEGMENT_GROUP group,
                      DXGI_QUERY_VIDEO_MEMORY_INFO& info) {
  if (device == nullptr || device->get_native() == 0) return false;
  static IDXGIAdapter3* cached = nullptr;
  static uint64_t cached_device = 0;
  if (cached != nullptr && cached_device != device->get_native()) {
    cached->Release();
    cached = nullptr;
  }
  if (cached == nullptr) {
    cached = ResolveVideoMemoryAdapter(device);
    cached_device = device->get_native();
  }
  if (cached == nullptr) return false;
  return SUCCEEDED(cached->QueryVideoMemoryInfo(0, group, &info));
}

void UpdateVramDiagnostics(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire))
    return;
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG heartbeat =
      g_vram_ui_heartbeat_ms.load(std::memory_order_acquire);
  if (heartbeat == 0 || now - heartbeat > 1500) {
    if (g_vram_capture_owned.exchange(false, std::memory_order_acq_rel))
      mfgunlock::inputdiag::g_enabled.store(false,
                                            std::memory_order_release);
    return;
  }

  static ULONGLONG next_sample = 0;
  if (now < next_sample) return;
  next_sample = now + 1000;

  auto* device = swapchain->get_device();
  const auto api = device == nullptr
      ? mfgunlock::memorypolicy::GraphicsApi::kUnknown
      : (device->get_api() == reshade::api::device_api::d3d11
             ? mfgunlock::memorypolicy::GraphicsApi::kD3D11
             : (device->get_api() == reshade::api::device_api::d3d12
                    ? mfgunlock::memorypolicy::GraphicsApi::kD3D12
                    : (device->get_api() == reshade::api::device_api::vulkan
                           ? mfgunlock::memorypolicy::GraphicsApi::kVulkan
                           : mfgunlock::memorypolicy::GraphicsApi::kUnknown)));
  const bool supports_dxgi =
      mfgunlock::memorypolicy::SupportsDxgiBudget(api);
  if (!supports_dxgi) {
    // Vulkan native handles are not COM objects. Clear any values retained
    // from an earlier D3D swapchain, but leave provider/resource telemetry on.
    ClearDxgiDiagnostics();
  } else if (swapchain->get_native() != 0) {
    auto* native = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(swapchain->get_native()));
    IDXGISwapChain1* swapchain1 = nullptr;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain1))) &&
        swapchain1 != nullptr) {
      DXGI_SWAP_CHAIN_DESC1 desc{};
      if (SUCCEEDED(swapchain1->GetDesc1(&desc)))
        g_vram_swapchain_buffers.store(desc.BufferCount,
                                       std::memory_order_relaxed);
      swapchain1->Release();
    }
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
  const bool local_ok = supports_dxgi && QueryVideoMemory(
      device, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, local);
  const bool nonlocal_ok = supports_dxgi && QueryVideoMemory(
      device, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, nonlocal);
  if (local_ok) {
    g_vram_local_usage.store(local.CurrentUsage, std::memory_order_relaxed);
    g_vram_local_budget.store(local.Budget, std::memory_order_relaxed);
    g_vram_local_available.store(local.AvailableForReservation,
                                 std::memory_order_relaxed);
    g_vram_dxgi_seen.store(true, std::memory_order_release);
  }
  if (nonlocal_ok) {
    g_vram_nonlocal_usage.store(nonlocal.CurrentUsage,
                                std::memory_order_relaxed);
    g_vram_nonlocal_budget.store(nonlocal.Budget,
                                 std::memory_order_relaxed);
  }
}

// 1.10.0 (plan XMX/VRAM, item 1): the cost of turning frame generation on,
// sampled with or without the panel open, until the measurement window ends.
SRWLOCK g_vram_timeline_lock = SRWLOCK_INIT;
mfgunlock::vramtimeline::Timeline g_vram_timeline;
std::string FrameGenerationVramReport();
void SampleFrameGenerationVram(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr || swapchain != g_primary_swapchain.load(std::memory_order_acquire)) return;
  const ULONGLONG now = GetTickCount64();
  static ULONGLONG next_sample = 0;
  if (now < next_sample) return;
  next_sample = now + 1000;
  const uint64_t first_on = mfgunlock::xmx::preada::g_first_on_tick.load(std::memory_order_acquire);
  AcquireSRWLockShared(&g_vram_timeline_lock);
  const bool done = g_vram_timeline.Done(now, first_on);
  ReleaseSRWLockShared(&g_vram_timeline_lock);
  if (done) {
    // Once, so the log carries it without the panel (tools/measure_fg_vram.py reads it).
    static std::atomic_bool logged{false};
    if (!logged.exchange(true)) {
      const std::string line = "frame generation VRAM: " + FrameGenerationVramReport();
      reshade::log::message(reshade::log::level::info, ("mfgunlock: " + line).c_str());
      // The measurement tool only (MFG_VRAM_LOG=1): ReShade drops an early-loaded
      // add-on's lines in NVIDIA's Streamline Sample, so it also goes to a file.
      wchar_t wanted[4]{};
      if (GetEnvironmentVariableW(L"MFG_VRAM_LOG", wanted, 4) != 0 && wanted[0] == L'1')
        if (FILE* f = _wfopen((mfgunlock::backend::Directory() / L"MFGAdaUnlock-vram.txt").c_str(), L"w")) {
          fputs((line + "\n").c_str(), f);
          fclose(f);
        }
    }
    return;
  }
  auto* device = swapchain->get_device();
  if (device == nullptr || device->get_api() == reshade::api::device_api::vulkan) return;
  DXGI_QUERY_VIDEO_MEMORY_INFO local{};
  if (!QueryVideoMemory(device, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, local)) return;
  AcquireSRWLockExclusive(&g_vram_timeline_lock);
  g_vram_timeline.Sample(now, first_on, local.CurrentUsage);
  ReleaseSRWLockExclusive(&g_vram_timeline_lock);
}
std::string FrameGenerationVramReport() {
  AcquireSRWLockShared(&g_vram_timeline_lock);
  const auto timeline = g_vram_timeline;
  ReleaseSRWLockShared(&g_vram_timeline_lock);
  return mfgunlock::vramtimeline::Describe(
      timeline, mfgunlock::xmx::generator::g_copy_bytes.load(std::memory_order_relaxed),
      mfgunlock::xmx::generator::g_copy_resources.load(std::memory_order_relaxed));
}

mfgunlock::pacing::MarkerHealth AssessReflexMarkers(
    const mfgunlock::nvapistatus::GuardObservation& observation) {
  using mfgunlock::pacing::MarkerHealth;
  if (observation.latency_status != mfgunlock::nvapistatus::kOk)
    return MarkerHealth::kUnavailable;
  if (observation.valid_latency_frames < 48)
    return MarkerHealth::kWaiting;
  const auto& frame = observation.latest;
  const bool complete = frame.simulation_start_time != 0 &&
                        frame.simulation_end_time != 0 &&
                        frame.render_submit_start_time != 0 &&
                        frame.render_submit_end_time != 0 &&
                        frame.present_start_time != 0 &&
                        frame.present_end_time != 0 &&
                        frame.gpu_render_start_time != 0 &&
                        frame.gpu_render_end_time != 0;
  if (!complete) return MarkerHealth::kIncomplete;
  const bool ordered =
      frame.simulation_start_time <= frame.simulation_end_time &&
      frame.render_submit_start_time <= frame.render_submit_end_time &&
      frame.present_start_time <= frame.present_end_time &&
      frame.gpu_render_start_time <= frame.gpu_render_end_time;
  if (!ordered) return MarkerHealth::kInvalidOrder;
  if (observation.sleep_status == mfgunlock::nvapistatus::kOk &&
      observation.sleep.game_sleep == 0)
    return MarkerHealth::kMissingSleep;
  if (!observation.source_timing_confident)
    return MarkerHealth::kUnstableTiming;
  return MarkerHealth::kHealthy;
}

void UpdateLatencyGuard(reshade::api::swapchain* swapchain) {
  using mfgunlock::pacing::LatencyGuardMode;
  const auto configured_guard_mode = static_cast<LatencyGuardMode>(
      mfgunlock::framecount::g_latency_guard_mode.load(
          std::memory_order_relaxed));
  const auto waitable_state =
      mfgunlock::reflexpacing::g_waitable_state.load(
          std::memory_order_acquire);
  const bool waitable_controls_pacing =
      waitable_state == mfgunlock::reflexpacing::WaitableState::kProbing ||
      waitable_state ==
          mfgunlock::reflexpacing::WaitableState::kActivationPending ||
      waitable_state == mfgunlock::reflexpacing::WaitableState::kActive;
  const auto guard_mode =
      configured_guard_mode == LatencyGuardMode::kAutomatic &&
              waitable_controls_pacing
          ? LatencyGuardMode::kMonitor
          : configured_guard_mode;

  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire)) return;
  if (!mfgunlock::framecount::g_latency_guard_refresh_pending.load(std::memory_order_acquire) &&
      !mfgunlock::framecount::g_latency_guard_auto_cap_ready.load(std::memory_order_acquire)) {
    const uint64_t heartbeat = mfgunlock::framecount::g_latency_guard_ui_heartbeat_ms.load(
        std::memory_order_acquire);
    if (guard_mode == LatencyGuardMode::kOff ||
        (guard_mode == LatencyGuardMode::kMonitor &&
         (heartbeat == 0 || GetTickCount64() - heartbeat > 1500)))
      return;
  }
  // Prevent two presenting threads from racing the sampler/controller.
  static SRWLOCK sample_lock = SRWLOCK_INIT;
  if (!TryAcquireSRWLockExclusive(&sample_lock)) return;
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&sample_lock); } } unlock;
  auto* device = swapchain->get_device();
  if (device == nullptr ||
      (device->get_api() != reshade::api::device_api::d3d11 &&
       device->get_api() != reshade::api::device_api::d3d12) || device->get_native() == 0) {
    if (mfgunlock::framecount::g_latency_guard_auto_cap_ready.exchange(false)) {
      mfgunlock::framecount::g_latency_guard_active_source_cap_fps.store(0);
      mfgunlock::framecount::g_latency_guard_epoch.fetch_add(1);
      mfgunlock::framecount::g_latency_guard_refresh_pending.store(true);
    }
    return; // Never replay Reflex options through an unavailable device/session.
  }
  // Restoring the native Reflex options is also deferred to Present so the UI
  // never invokes Streamline directly. This executes only on a mode/cap state
  // transition, not once per frame.
  const bool refresh_pending =
      mfgunlock::framecount::g_latency_guard_refresh_pending.exchange(
          false, std::memory_order_acq_rel);
  if (refresh_pending) {
    mfgunlock::framecount::internal::RefreshReflexTarget();
  }
  if (guard_mode == LatencyGuardMode::kOff || swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire))
    return;

  static ULONGLONG next_sample = 0;
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG ui_heartbeat =
      mfgunlock::framecount::g_latency_guard_ui_heartbeat_ms.load(
          std::memory_order_acquire);
  if (guard_mode == LatencyGuardMode::kMonitor &&
      (ui_heartbeat == 0 || now - ui_heartbeat > 1500))
    return;
  if (now < next_sample) return;
  next_sample = now + 500;

  LARGE_INTEGER sample_begin{}, sample_end{};
  const uint64_t sample_frequency =
      mfgunlock::nvapistatus::QpcFrequency();
  QueryPerformanceCounter(&sample_begin);
  const auto observation = mfgunlock::nvapistatus::ObserveGuard(
      reinterpret_cast<IUnknown*>(
          static_cast<uintptr_t>(device->get_native())),
      mfgunlock::framecount::g_latency_guard_epoch.load(std::memory_order_acquire));
  QueryPerformanceCounter(&sample_end);
  const uint32_t sample_cost_us = sample_frequency > 0 &&
          sample_end.QuadPart >= sample_begin.QuadPart
      ? static_cast<uint32_t>((std::min)(
            uint64_t{UINT32_MAX},
            static_cast<uint64_t>(sample_end.QuadPart - sample_begin.QuadPart) *
                 1000000ull / sample_frequency))
      : 0;
  ObserveDxgiLatencyPolicy(swapchain);
  const uint32_t refresh_fps = DetectDisplayRefreshFps(swapchain);
  const bool sleep_available =
      observation.sleep_status == mfgunlock::nvapistatus::kOk;
  const uint32_t live_multiplier =
      sleep_available ? observation.sleep.frame_generation_multiplier : 0;
  const bool vsync_active =
      sleep_available && observation.sleep.control_panel_vsync != 0;
  const auto marker_health = AssessReflexMarkers(observation);
  const uint32_t queue_wait_us = observation.median_queue_wait_us;
  const uint32_t gpu_frame_time_us = observation.median_gpu_frame_time_us;
  const uint32_t pipeline_latency_us =
      observation.median_pipeline_latency_us;
  const uint32_t observed_source_interval_us =
      observation.source_interval_us;
  const bool queue_action_timing_confident =
      observation.source_timing_confident &&
      observation.queue_timing_confident;

  const auto recommendation =
      mfgunlock::pacing::BuildLatencyGuardRecommendation(
          vsync_active, refresh_fps,
          mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed)
              ? mfgunlock::framecount::g_dynamic_target_fps.load(std::memory_order_relaxed) : 0,
          sleep_available ? observation.sleep.dynamic_frame_time_target_us : 0,
          live_multiplier, observed_source_interval_us, queue_wait_us,
          queue_action_timing_confident);

  mfgunlock::framecount::g_latency_guard_units.store(
      static_cast<unsigned int>(observation.timestamp_units), std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_new_frames.store(observation.new_frames, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_display_refresh_fps.store(
      refresh_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_live_multiplier.store(
      live_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_output_target_fps.store(
      recommendation.output_target_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_recommended_source_cap_fps.store(
      recommendation.source_cap_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_estimated_source_fps.store(
      recommendation.estimated_source_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_simulation_fps.store(
      mfgunlock::pacing::FrameLimitUsToFps(
          observation.simulation_interval_us),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_projected_output_fps.store(
      recommendation.projected_output_fps, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_suggested_multiplier.store(
      recommendation.suggested_total_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_high.store(
      recommendation.multiplier_may_be_higher_than_needed,
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_marker_health.store(
      static_cast<unsigned int>(marker_health), std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_queue_wait_us.store(
      queue_wait_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_queue_p95_us.store(
      observation.p95_queue_wait_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_gpu_frame_time_us.store(
      gpu_frame_time_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_gpu_frame_p95_us.store(
      observation.p95_gpu_frame_time_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_gpu_active_us.store(
      observation.median_gpu_active_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_pipeline_latency_us.store(
      pipeline_latency_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_pipeline_p95_us.store(
      observation.p95_pipeline_latency_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_input_to_gpu_end_us.store(
      observation.median_input_to_gpu_end_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_input_to_simulation_us.store(
      observation.median_input_to_simulation_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_simulation_cpu_us.store(
      observation.median_simulation_cpu_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_submit_cpu_us.store(
      observation.median_submit_cpu_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_ai_frame_time_us.store(
      observation.median_ai_frame_time_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_sample_cost_us.store(
      sample_cost_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_iflip_known.store(
      sleep_available, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_iflip_active.store(
      sleep_available && observation.sleep.fullscreen_independent_flip != 0,
      std::memory_order_relaxed);
  const auto latency_bottleneck =
      mfgunlock::pacing::ClassifyLatencyBottleneck(
          observation.source_timing_confident,
          recommendation.source_oversubscribed,
          observation.queue_timing_confident
              ? observation.p95_queue_wait_us : 0,
          observed_source_interval_us,
          observation.median_gpu_active_us,
          observation.median_ai_frame_time_us);
  mfgunlock::framecount::g_latency_guard_bottleneck.store(
      static_cast<unsigned int>(latency_bottleneck),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_timing_samples.store(
      observation.consecutive_timing_samples, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_timing_confident.store(
      observation.source_timing_confident, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_queue_timing_confident.store(
      observation.queue_timing_confident, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_timing_issue_mask.store(
      observation.source_timing_issue_mask, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_oversubscribed.store(
      recommendation.source_oversubscribed, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_sample_seen.store(
      true, std::memory_order_release);

  const bool pressure_candidate =
      mfgunlock::pacing::ShouldApplyAutomaticLatencyCap(
          guard_mode, marker_health,
          mfgunlock::framecount::g_reflex_hooked.load(
              std::memory_order_acquire),
          mfgunlock::framecount::g_reflex_options_seen.load(
              std::memory_order_acquire),
          recommendation);
  static mfgunlock::latency::QueueTrial trial;
  static mfgunlock::latency::MultiplierTrial multiplier_trial;
  const uint32_t configured_multiplier =
      mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed);
  const unsigned int current_limit_source =
      mfgunlock::framecount::g_reflex_limit_source.load(
          std::memory_order_relaxed);
  const auto user_source_cap_state =
      mfgunlock::framecount::internal::ResolveUserSourceCapState();
  const bool user_source_cap_requested = user_source_cap_state.requested;
  const bool user_source_cap_ready =
      mfgunlock::framecount::internal::UserSourceCapReady();
  const bool user_source_cap_rejected =
      user_source_cap_state.status ==
      mfgunlock::framecount::internal::UserSourceCapStatus::kRejected;
  if (user_source_cap_requested && !user_source_cap_ready &&
      !user_source_cap_rejected) {
    // Apply the user-requested final/output cap once at the established safe
    // Reflex boundary. It is invariant across multiplier trials, so later
    // 6x->5x->4x->3x transitions never need another Reflex replay.
    mfgunlock::framecount::g_latency_guard_refresh_pending.store(
        true, std::memory_order_release);
    mfgunlock::framecount::internal::RefreshReflexTarget();
  }
  const bool base_safe = guard_mode == LatencyGuardMode::kAutomatic &&
      marker_health == mfgunlock::pacing::MarkerHealth::kHealthy &&
      observation.source_timing_confident && sleep_available &&
      observation.sleep.game_sleep &&
      !observation.sleep.dynamic_frame_generation_control &&
      !mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed) &&
      mfgunlock::framecount::g_reflex_options_seen.load(std::memory_order_acquire) &&
      mfgunlock::forcepolicy::IsFixedMultiplier(configured_multiplier);
  const uint32_t old_multiplier_override =
      mfgunlock::framecount::g_latency_guard_multiplier_override.load(
          std::memory_order_relaxed);
  const bool multiplier_safe = base_safe &&
      mfgunlock::framecount::g_latency_guard_active_source_cap_fps.load(
          std::memory_order_relaxed) == 0;
  const auto responsive_reason = multiplier_safe
      ? mfgunlock::latency::ClassifyResponsiveTrialReason(
            recommendation.source_oversubscribed,
            observed_source_interval_us,
            observation.queue_timing_confident
                ? observation.p95_queue_wait_us : 0,
            observation.median_input_to_gpu_end_us,
            pipeline_latency_us,
            observation.median_gpu_active_us,
            observation.median_ai_frame_time_us)
      : mfgunlock::latency::ResponsiveTrialReason::kNone;

  using mfgunlock::latency::ResponsiveTrialBlocker;
  ResponsiveTrialBlocker trial_blocker = ResponsiveTrialBlocker::kNone;
  if (guard_mode != LatencyGuardMode::kAutomatic) {
    trial_blocker = ResponsiveTrialBlocker::kMonitorOnly;
  } else if (marker_health ==
                 mfgunlock::pacing::MarkerHealth::kUnstableTiming ||
             (marker_health == mfgunlock::pacing::MarkerHealth::kHealthy &&
              !observation.source_timing_confident)) {
    trial_blocker = ResponsiveTrialBlocker::kSourceTimingUnverified;
  } else if (marker_health != mfgunlock::pacing::MarkerHealth::kHealthy) {
    trial_blocker = ResponsiveTrialBlocker::kReflexUnhealthy;
  } else if (!sleep_available ||
             observation.sleep.dynamic_frame_generation_control ||
             mfgunlock::framecount::g_dynamic_applied.load(
                 std::memory_order_relaxed)) {
    trial_blocker = ResponsiveTrialBlocker::kDynamicActive;
  } else if (configured_multiplier < 4 || configured_multiplier > 6) {
    trial_blocker = ResponsiveTrialBlocker::kFixedMultiplierRequired;
  } else if (multiplier_trial.phase ==
                 mfgunlock::latency::MultiplierTrialPhase::kIdle &&
             live_multiplier != configured_multiplier) {
    trial_blocker = ResponsiveTrialBlocker::kLiveMultiplierUnconfirmed;
  } else if (!mfgunlock::framecount::g_reflex_options_seen.load(
                 std::memory_order_acquire)) {
    trial_blocker = ResponsiveTrialBlocker::kReflexOptionsUnavailable;
  } else if (user_source_cap_rejected) {
    trial_blocker = ResponsiveTrialBlocker::kUserCapRejected;
  } else if (user_source_cap_requested && !user_source_cap_ready) {
    trial_blocker = ResponsiveTrialBlocker::kUserCapPending;
  } else if (responsive_reason ==
             mfgunlock::latency::ResponsiveTrialReason::kNone) {
    trial_blocker = ResponsiveTrialBlocker::kNoTrigger;
  }
  mfgunlock::framecount::g_latency_guard_trial_blocker.store(
      static_cast<unsigned int>(trial_blocker), std::memory_order_relaxed);
  const mfgunlock::latency::MultiplierTrialSample multiplier_sample{
      observed_source_interval_us,
      queue_wait_us,
      observation.p95_queue_wait_us,
      pipeline_latency_us,
      observation.p95_pipeline_latency_us,
      observation.median_ai_frame_time_us};
  const uint32_t multiplier_override = multiplier_trial.Update(
      now,
      mfgunlock::framecount::g_latency_guard_epoch.load(
          std::memory_order_acquire),
      configured_multiplier, live_multiplier, multiplier_safe,
      responsive_reason, multiplier_sample, user_source_cap_ready,
      observation.window_first_frame, observation.window_last_frame);
  mfgunlock::framecount::g_latency_guard_multiplier_override.store(
      multiplier_override, std::memory_order_release);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_accepted.store(
      multiplier_trial.accepted, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_phase.store(
      static_cast<unsigned int>(multiplier_trial.phase),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_reason.store(
      static_cast<unsigned int>(multiplier_trial.reason),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_approved.store(
      multiplier_trial.approved_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_candidate.store(
      multiplier_trial.candidate_multiplier, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_baseline_samples.store(
      static_cast<unsigned int>(multiplier_trial.baseline_window.count),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_samples.store(
      static_cast<unsigned int>(multiplier_trial.candidate_window.count),
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_baseline_pipeline_us.store(
      multiplier_trial.last_compared_baseline.samples != 0
          ? multiplier_trial.last_compared_baseline.pipeline_us
          : multiplier_trial.baseline.pipeline_us,
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_baseline_p95_us.store(
      multiplier_trial.last_compared_baseline.samples != 0
          ? multiplier_trial.last_compared_baseline.pipeline_p95_us
          : multiplier_trial.baseline.pipeline_p95_us,
      std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_pipeline_us.store(
      multiplier_trial.latest_trial.pipeline_us, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_multiplier_trial_p95_us.store(
      multiplier_trial.latest_trial.pipeline_p95_us,
      std::memory_order_relaxed);
  if (old_multiplier_override != multiplier_override) {
    // The next normal game SetOptions submission picks up (or releases) the
    // bounded runtime override. The user output-FPS cap is unchanged, so do
    // not replay Reflex or add a second reconfiguration at this transition.
    mfgunlock::framecount::g_force_failed_for.store(0,
                                                    std::memory_order_relaxed);
  }
  const bool multiplier_fallback_allowed = multiplier_override == 0 &&
      multiplier_trial.attempted &&
      multiplier_trial.phase ==
          mfgunlock::latency::MultiplierTrialPhase::kCooldown;
  const bool multiplier_sequence_active =
      multiplier_trial.phase !=
          mfgunlock::latency::MultiplierTrialPhase::kIdle &&
      multiplier_trial.phase !=
          mfgunlock::latency::MultiplierTrialPhase::kCooldown;
  const bool queue_trim_safe = base_safe &&
      observation.queue_timing_confident && !user_source_cap_requested &&
      mfgunlock::framecount::g_reflex_native_limit_us.load(
          std::memory_order_relaxed) == 0 &&
      (observation.sleep.sleep_interval_us == 0 ||
       current_limit_source == static_cast<unsigned int>(
           mfgunlock::framecount::internal::ReflexTargetSource::kLatencyGuard));
  const auto old_cap = mfgunlock::framecount::g_latency_guard_active_source_cap_fps.load(std::memory_order_relaxed);
  const auto cap = trial.Update(now,
      mfgunlock::framecount::g_latency_guard_epoch.load(std::memory_order_acquire), live_multiplier,
      queue_trim_safe && multiplier_override == 0 && !multiplier_sequence_active &&
          (responsive_reason ==
               mfgunlock::latency::ResponsiveTrialReason::kNone ||
           multiplier_fallback_allowed),
      pressure_candidate && multiplier_override == 0 &&
              !multiplier_sequence_active &&
              (responsive_reason ==
                   mfgunlock::latency::ResponsiveTrialReason::kNone ||
               multiplier_fallback_allowed)
          ? recommendation.source_cap_fps : 0,
      observed_source_interval_us, queue_wait_us, pipeline_latency_us,
      mfgunlock::framecount::internal::ReadQueueCapAck(),
      observation.window_first_frame, observation.window_last_frame);
  mfgunlock::framecount::g_latency_guard_active_source_cap_fps.store(cap, std::memory_order_relaxed);
  mfgunlock::framecount::g_latency_guard_auto_cap_ready.store(cap != 0, std::memory_order_release);
  if (cap != old_cap) {
    mfgunlock::framecount::g_latency_guard_refresh_pending.store(true, std::memory_order_release);
    mfgunlock::framecount::internal::RefreshReflexTarget();
  }
}
