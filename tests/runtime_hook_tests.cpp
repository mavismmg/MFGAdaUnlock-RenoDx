// SPDX-License-Identifier: MIT
// Exercise production wrappers with deterministic providers, without a GPU.
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/addons/mfgunlock/framecount.hpp"
#include "../src/addons/mfgunlock/midpoint.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "FAILED: " #condition " at " << __LINE__ << '\n';          \
      std::abort();                                                             \
    }                                                                           \
  } while (false)

namespace fc = mfgunlock::framecount;
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int,
                                                         const char*) {}
std::vector<unsigned int> requests;
std::vector<sl::DLSSGMode> modes;
unsigned int accepted_count = 1;
bool transient = false;
unsigned int get_state_calls = 0;
size_t last_state_version = 0;
bool vram_request_seen = false;
sl::Result Provider(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
  requests.push_back(options.numFramesToGenerate);
  modes.push_back(options.mode);
  if (options.mode == sl::DLSSGMode::eOff) return sl::Result::eOk;
  if (transient && requests.size() == 1) return sl::Result::eErrorInvalidState;
  return options.numFramesToGenerate == accepted_count
      ? sl::Result::eOk : sl::Result::eErrorFeatureNotSupported;
}

sl::Result ReentrantGetState(const sl::ViewportHandle& viewport,
                             sl::DLSSGState& state,
                             const sl::DLSSGOptions* options) {
  ++get_state_calls;
  return fc::internal::HookedGetState(viewport, state, options);
}

sl::Result WorkingGetState(const sl::ViewportHandle&, sl::DLSSGState&,
                           const sl::DLSSGOptions*) {
  ++get_state_calls;
  return sl::Result::eOk;
}

sl::Result NativeState(const sl::ViewportHandle&, sl::DLSSGState& state,
                       const sl::DLSSGOptions* options) {
  ++get_state_calls;
  last_state_version = state.structVersion;
  if (options != nullptr &&
      (static_cast<uint32_t>(options->flags) &
       static_cast<uint32_t>(sl::DLSSGFlags::eRequestVRAMEstimate)) != 0) {
    vram_request_seen = true;
    state.estimatedVRAMUsageInBytes = 512ull << 20;
  }
  state.numFramesToGenerateMax = 1;
  state.numFramesActuallyPresented = 3;
  state.bIsVsyncSupportAvailable = sl::Boolean::eFalse;
  if (state.structVersion >= sl::kStructVersion4) {
    state.bIsDynamicMFGSupported = sl::Boolean::eTrue;
  }
  return sl::Result::eOk;
}

sl::Result FeatureProvider(sl::Feature, const char*, void*& function) {
  function = reinterpret_cast<void*>(&NativeState);
  return sl::Result::eOk;
}

int main() {
  fc::internal::g_real_set_options.store(&Provider);
  // Standalone tests have no ReShade logger. Mark one-shot runtime messages as
  // already emitted while still exercising every state and fallback branch.
  fc::g_intercepted.store(true);
  fc::g_force_failed_for.store(3);
  fc::g_force_multiplier.store(4);
  sl::DLSSGOptions options{};
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  const sl::ViewportHandle viewport(0);
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  if (requests != std::vector<unsigned int>{3, 3, 1}) {
    std::cerr << "first sequence:";
    for (const auto value : requests) std::cerr << ' ' << value;
    std::cerr << '\n';
  }
  CHECK((requests == std::vector<unsigned int>{3, 3, 1}));
  CHECK(fc::g_last_effective_generated.load() == 1);
  CHECK(fc::g_fixed_override_status.load() == static_cast<unsigned int>(
      mfgunlock::forcepolicy::FixedOverrideStatus::kRejected));
  CHECK(options.numFramesToGenerate == 1);

  // 1.5.6: the refusal is remembered. The next frame's call forwards the
  // game's request once instead of repeating the refused count and its retry.
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{1}));
  CHECK(fc::g_last_effective_generated.load() == 1);
  CHECK(fc::g_fixed_override_status.load() == static_cast<unsigned int>(
      mfgunlock::forcepolicy::FixedOverrideStatus::kRejected));
  // Each reset boundary allows one new attempt at the forced count.
  const auto retried_after = [&](auto&& boundary) {
    boundary();
    requests.clear();
    CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
    return requests == std::vector<unsigned int>{3, 3, 1};
  };
  CHECK(retried_after([&] {
    sl::DLSSGOptions off = options;
    off.mode = sl::DLSSGMode::eOff;
    CHECK(fc::internal::HookedSetOptions(viewport, off) == sl::Result::eOk);
  }));
  CHECK(retried_after([] { fc::NotifyFixedMultiplierChanged(4); }));
  CHECK(retried_after([] { fc::NotifySwapchainTransition(); }));
  fc::NotifySwapchainTransition();  // no refusal left for the cases below

  accepted_count = 3;
  transient = true;
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{3, 3}));
  CHECK(fc::g_last_effective_generated.load() == 3);
  CHECK(fc::g_fixed_override_status.load() == static_cast<unsigned int>(
      mfgunlock::forcepolicy::FixedOverrideStatus::kApplied));

  // A forced lower multiplier must replace a game's higher multiplier too.
  transient = false;
  accepted_count = 1;
  options.numFramesToGenerate = 3;
  fc::g_force_multiplier.store(2);
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{1}));
  CHECK(fc::g_last_effective_generated.load() == 1);
  CHECK(options.numFramesToGenerate == 3);

  options.mode = sl::DLSSGMode::eOff;
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{3}));
  CHECK(!fc::g_effective_request_seen.load());

  // A foreign wrapper can route its saved "original" back to our GetState
  // hook. This must fail closed instead of exhausting the game's stack, and a
  // later valid call on the same thread must still work.
  fc::internal::g_get_state_reentry_logged.store(true);
  fc::internal::g_real_get_state.store(&ReentrantGetState);
  sl::DLSSGState state{};
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) ==
        sl::Result::eErrorInvalidState);
  CHECK(get_state_calls == 1);
  CHECK(fc::internal::g_get_state_reentry_seen.load());
  CHECK(!fc::internal::g_get_state_call_active);
  fc::internal::g_real_get_state.store(&WorkingGetState);
  fc::g_addon_enabled.store(false);
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) == sl::Result::eOk);
  CHECK(get_state_calls == 2);
  CHECK(!fc::internal::g_get_state_call_active);
  fc::g_addon_enabled.store(true);

  // Outlaws keeps the game's ABI and max-count fields unmodified at startup,
  // but still observes successful presentation telemetry. Other games retain
  // the established advertised-ceiling behavior.
  fc::internal::g_real_get_state.store(&NativeState);
  fc::internal::g_get_state_reentry_seen.store(false);
  fc::g_status_ok_logged.store(true);
  fc::g_state_seen.store(true);
  fc::g_seen_present_counts.store(1u << 3);
  fc::g_capacity_advertised.store(true);
  fc::g_actual_frames_presented.store(3);
  fc::g_vsync_support_seen.store(true);
  fc::g_vsync_supported.store(false);
  fc::g_state_samples.store(0);
  fc::g_advertised_max_generated.store(5);
  fc::g_outlaws_get_state_compat.store(true);
  fc::g_dynamic_d3d12.store(true);
  fc::g_streamline_dynamic_version_active.store(true);
  fc::g_dynamic_mfg_enabled.store(false);
  fc::g_native_request_seen.store(false);
  state.structVersion = sl::kStructVersion2;
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) == sl::Result::eOk);
  CHECK(get_state_calls == 3);
  CHECK(last_state_version == sl::kStructVersion2);
  CHECK(state.numFramesToGenerateMax == 1);
  CHECK(fc::g_state_samples.load() == 1);
  CHECK(fc::g_actual_frames_presented.load() == 3);
  CHECK(fc::g_runtime_max_generated.load() == 1);

  fc::g_outlaws_get_state_compat.store(false);
  fc::g_dynamic_d3d12.store(false);
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) == sl::Result::eOk);
  CHECK(state.numFramesToGenerateMax == 5);

  // Outlaws keeps Dynamic blocked even when the saved cross-game preference is
  // enabled. Its fixed/native request and caller-owned GetState ABI stay intact.
  fc::g_outlaws_get_state_compat.store(true);
  fc::g_dynamic_d3d12.store(true);
  fc::g_dynamic_mfg_enabled.store(true);
  fc::g_dynamic_game_compat_blocked.store(true);
  fc::g_native_request_seen.store(true);
  fc::g_native_requested.store(1);
  fc::g_native_result.store(static_cast<unsigned int>(sl::Result::eOk));
  fc::g_dynamic_support_seen.store(true);
  fc::g_dynamic_supported.store(true);
  fc::g_dlssg_dynamic_version_seen.store(true);
  fc::g_force_multiplier.store(0);
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  accepted_count = 1;
  fc::g_state_seen.store(true);
  fc::g_max_actual_frames_presented.store(3);
  requests.clear();
  modes.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{1}));
  CHECK((modes == std::vector<sl::DLSSGMode>{sl::DLSSGMode::eOn}));
  CHECK(!fc::g_dynamic_applied.load());
  CHECK(!fc::g_state_seen.load());
  CHECK(fc::g_max_actual_frames_presented.load() == 0);
  state.structVersion = sl::kStructVersion2;
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) == sl::Result::eOk);
  CHECK(last_state_version == sl::kStructVersion2);
  CHECK(!fc::internal::g_get_state_reentry_seen.load());
  CHECK(fc::g_state_seen.load());
  CHECK(fc::g_max_actual_frames_presented.load() == 3);

  // Other games retain the established Dynamic v4 capability probe.
  fc::g_dynamic_game_compat_blocked.store(false);
  state.structVersion = sl::kStructVersion2;
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) == sl::Result::eOk);
  CHECK(last_state_version == sl::kStructVersion4);

  // Automatic multiplier trials are runtime-only: they alter the next
  // forwarded fixed request without overwriting the saved selection.
  fc::g_dynamic_mfg_enabled.store(false);
  fc::g_force_multiplier.store(4);
  fc::g_latency_guard_multiplier_override.store(3);
  accepted_count = 2;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{2}));
  CHECK(fc::g_force_multiplier.load() == 4);
  fc::g_latency_guard_multiplier_override.store(0);

  // The diagnostics-only VRAM request is injected into one natural GetState
  // call and its result is retained without changing normal callers.
  mfgunlock::memorypolicy::EstimateInputs memory_input{};
  memory_input.options_valid = true;
  memory_input.generated_frames = 3;
  memory_input.back_buffers = 3;
  memory_input.color_width = 2560;
  memory_input.color_height = 1440;
  memory_input.color_format = 24;
  memory_input.mvec_depth_width = 1708;
  memory_input.mvec_depth_height = 960;
  memory_input.mvec_format = 34;
  memory_input.depth_format = 19;
  const auto memory_plan =
      mfgunlock::memorypolicy::BuildEstimatePlan(memory_input);
  CHECK(memory_plan.readiness ==
        mfgunlock::memorypolicy::EstimateReadiness::kReady);
  fc::ResetVramEstimate();
  CHECK(fc::QueueVramEstimate(0, memory_plan));
  vram_request_seen = false;
  state = {};
  CHECK(fc::internal::HookedGetState(viewport, state, nullptr) ==
        sl::Result::eOk);
  CHECK(vram_request_seen);
  CHECK(fc::g_vram_estimate_status.load() == static_cast<unsigned int>(
      fc::VramEstimateStatus::kReady));
  CHECK(fc::g_vram_estimate_bytes.load() == (512ull << 20));

  // The release Outlaws path must leave the actual function pointer native;
  // wrapping it can recurse through the game's Streamline chain and gray out
  // the Frame Generation menu. Other games retain the established wrapper.
  fc::internal::g_real_get_feature_function = &FeatureProvider;
  fc::internal::g_get_state_wrapped_logged.store(true);
  fc::internal::g_real_get_state.store(nullptr);
  fc::g_outlaws_get_state_compat.store(true);
  void* state_function = nullptr;
  CHECK(fc::internal::HookedGetFeatureFunction(
            sl::kFeatureDLSS_G, "slDLSSGGetState", state_function) ==
        sl::Result::eOk);
  CHECK(state_function == reinterpret_cast<void*>(&NativeState));
  CHECK(fc::internal::g_real_get_state.load() == nullptr);

  fc::g_outlaws_get_state_compat.store(false);
  state_function = nullptr;
  CHECK(fc::internal::HookedGetFeatureFunction(
            sl::kFeatureDLSS_G, "slDLSSGGetState", state_function) ==
        sl::Result::eOk);
  CHECK(state_function == reinterpret_cast<void*>(&fc::internal::HookedGetState));
  CHECK(fc::internal::g_real_get_state.load() == &NativeState);

  // Preflight failure must not poison storage used by future hook retries.
  void* first = nullptr;
  void* second = reinterpret_cast<void*>(1);
  const std::vector<mfgunlock::hook::HookItem> hooks = {
      {"GetTickCount", &first, reinterpret_cast<void*>(&Provider)},
      {"GetCurrentProcessId", &second, reinterpret_cast<void*>(&Provider)}};
  CHECK(!mfgunlock::hook::Install(GetModuleHandleW(L"kernel32.dll"), hooks, "test"));
  CHECK(first == nullptr && second == reinterpret_cast<void*>(1));

  // The XMX hook: a thread walk that cannot finish patches nothing, the transaction
  // is closed for the next attempt, and that attempt goes through.
  {
    namespace hi = mfgunlock::hook::internal;
    static int aborted, attached, committed;
    aborted = attached = committed = 0;
    const auto real_detours = hi::detours;
    const auto real_threads = hi::thread_operations;
    hi::detours.begin = []() -> LONG { return NO_ERROR; };
    hi::detours.abort = []() -> LONG { ++aborted; return NO_ERROR; };
    hi::detours.commit = []() -> LONG { ++committed; return NO_ERROR; };
    hi::detours.update = [](HANDLE) -> LONG { return NO_ERROR; };
    hi::detours.attach = [](PVOID*, PVOID) -> LONG { ++attached; return NO_ERROR; };
    hi::thread_operations.snapshot = [](DWORD, DWORD) -> HANDLE {
      SetLastError(ERROR_ACCESS_DENIED);
      return INVALID_HANDLE_VALUE;
    };
    void* trampoline = reinterpret_cast<void*>(&Provider);
    CHECK(!mfgunlock::hook::InstallOne(&trampoline, reinterpret_cast<void*>(&Provider)));
    CHECK(aborted == 1 && attached == 0 && committed == 0);
    hi::thread_operations = real_threads;
    CHECK(mfgunlock::hook::InstallOne(&trampoline, reinterpret_cast<void*>(&Provider)));
    CHECK(aborted == 1 && attached == 1 && committed == 1);
    // Held by a thread killed at process exit: Uninstall gives up instead of hanging.
    {
      std::lock_guard held(hi::transaction_lock);
      std::atomic<bool> result{true};
      std::thread([&] { result = mfgunlock::hook::Uninstall({}); }).join();
      CHECK(!result.load());
    }
    hi::detours = real_detours;
  }

  // A failed descriptor restore must retain its backing allocation rather than
  // leave a live descriptor pointing at freed memory.
  void* temporal_allocation =
      VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  CHECK(temporal_allocation != nullptr);
  std::vector<mfgunlock::midpoint::Patch> failed_restore = {
      {reinterpret_cast<uint64_t*>(1), 0}};
  mfgunlock::midpoint::Restore(failed_restore, temporal_allocation);
  CHECK(temporal_allocation != nullptr);
  CHECK(VirtualFree(temporal_allocation, 0, MEM_RELEASE) != 0);

  std::cout << "runtime hook tests passed\n";
}
