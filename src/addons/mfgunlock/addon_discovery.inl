// SPDX-License-Identifier: MIT
// DLSS-G discovery (plan 6.4): the bounded worker that retries the provider
// and Streamline discovery off the Present path for a few seconds after the
// first Present, then exits.
//
// A fragment of addon.cpp, included inside its anonymous namespace: it uses
// the configuration and state declared there and is not a standalone header.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "addon_discovery.inl is included only by addon.cpp"
#endif

// Keep compatibility retries away from Present. The load-time hook remains the
// primary path; this short-lived worker only covers unusual loaders, hook
// conflicts, and modules that appear during startup through an unobserved API.
// Once the required pieces have been verified it exits and performs no further
// work for the rest of the session.
constexpr DWORD kDiscoveryRetryIntervalMs = 250;
constexpr unsigned int kDiscoveryRetryLimit = 40;
std::atomic_bool g_discovery_worker_started{false};
std::atomic_bool g_discovery_worker_running{false};
std::atomic_bool g_discovery_worker_finished{false};
std::atomic<unsigned int> g_discovery_worker_passes{0};

bool DiscoveryRequirementsMet() {
  const bool provider_ready =
      (!g_enabled.load(std::memory_order_relaxed) ||
       g_gate_patched.load(std::memory_order_acquire)) &&
      (!g_enabled.load(std::memory_order_relaxed) ||
       !g_temporal_fix.load(std::memory_order_relaxed) ||
       g_midpoint_patched.load(std::memory_order_acquire) ||
       g_blackwell_patched.load(std::memory_order_acquire));
  const bool pacing_ready =
      !g_force_flip_meter_off.load(std::memory_order_relaxed) ||
      g_flip_meter_patched.load(std::memory_order_acquire) ||
      g_flip_meter_attempts.load(std::memory_order_relaxed) >=
          kMaxFlipMeterAttempts;
  const bool wrapper_ready =
      !g_enabled.load(std::memory_order_relaxed) ||
      g_ceiling_patched.load(std::memory_order_acquire) ||
      g_ceiling_plugin_seen.load(std::memory_order_acquire);
  return provider_ready && pacing_ready && wrapper_ready &&
         mfgunlock::loadhook::g_hooked.load(std::memory_order_acquire) &&
         mfgunlock::framecount::g_hooked.load(std::memory_order_acquire);
}

void RunBoundedDiscoveryWorker() {
  g_discovery_worker_running.store(true, std::memory_order_release);
  // This worker starts after the first real Present, past ReShade's temporary
  // addon probes (onenowy, 1.8.2 regression). The present callback has made the
  // addon resident by now, and provider maintenance hands every provider to
  // XMX after its gates are patched.
  unsigned int quiet_ready_passes = 0;
  for (unsigned int pass = 1; pass <= kDiscoveryRetryLimit; ++pass) {
    Sleep(kDiscoveryRetryIntervalMs);
    g_provider_rescan_requested.store(false, std::memory_order_release);
    RunProviderMaintenance();
    TryPatchFrameCountCeiling();
    if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
    mfgunlock::framecount::TryInstall();
    mfgunlock::loadhook::TryInstall();
    g_discovery_worker_passes.store(pass, std::memory_order_relaxed);

    if (DiscoveryRequirementsMet() &&
        !g_provider_rescan_requested.load(std::memory_order_acquire)) {
      // A short quiet period closes the race where another module is loaded as
      // the first successful pass finishes.
      if (++quiet_ready_passes >= 4) break;
    } else {
      quiet_ready_passes = 0;
    }
  }

  const bool ready = DiscoveryRequirementsMet();
  g_discovery_worker_running.store(false, std::memory_order_release);
  g_discovery_worker_finished.store(true, std::memory_order_release);
  reshade::log::message(
      ready ? reshade::log::level::info : reshade::log::level::warning,
      ready ? "mfgunlock: bounded startup discovery completed; no Present polling is active."
            : "mfgunlock: bounded startup discovery ended without verifying every component; "
              "the load-time trigger remains armed for late modules.");
}

DWORD WINAPI DiscoveryWorkerEntry(LPVOID pinned_module) {
  RunBoundedDiscoveryWorker();
  FreeLibraryAndExitThread(static_cast<HMODULE>(pinned_module), 0);
}

void StartDiscoveryWorker() {
  if (g_discovery_worker_started.load(std::memory_order_acquire)) return;
  bool expected = false;
  if (!g_discovery_worker_started.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }

  HMODULE pinned_module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
          reinterpret_cast<LPCWSTR>(&DiscoveryWorkerEntry), &pinned_module)) {
    g_discovery_worker_started.store(false, std::memory_order_release);
    return;
  }
  HANDLE thread = CreateThread(nullptr, 0, DiscoveryWorkerEntry, pinned_module, 0, nullptr);
  if (thread == nullptr) {
    FreeLibrary(pinned_module);
    g_discovery_worker_started.store(false, std::memory_order_release);
    return;
  }
  CloseHandle(thread);
}
