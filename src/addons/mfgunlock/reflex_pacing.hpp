/*
 * Reflex/Pacing Lab runtime shared by the Streamline and DXGI hooks.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include <sl_reflex.h>

#include "pacing_policy.hpp"

namespace mfgunlock::reflexpacing {

enum class WaitableState : uint32_t {
  kNative = 0,
  kWaitingForSwapchain,
  kProbing,
  kActivationPending,
  kActive,
  kFallback,
};

enum class FallbackReason : uint32_t {
  kNone = 0,
  kNotRequested,
  kUnsupportedApi,
  kMfgInactive,
  kNoWaitableFlag,
  kNoWaitableHandle,
  kHandleDuplicationFailed,
  kMaximumLatencyQueryFailed,
  kMaximumLatencySetFailed,
  kSwapchainChanged,
  kTokenDuplicate,
  kTokenRegressed,
  kConcurrentSleep,
  kNativeSleepFailed,
  kWaitTimeout,
  kWaitFailed,
};

enum class SleepDispatch : uint32_t {
  kNative = 0,
  kMeasureNative,
  kWaitable,
  kBypass,
};

struct SampleStats {
  uint32_t count = 0;
  uint32_t median_us = 0;
  uint32_t p95_us = 0;
  uint32_t p99_us = 0;
  uint32_t mean_delta_us = 0;
};

template <size_t Capacity = 2048>
struct SampleRing {
  SRWLOCK lock = SRWLOCK_INIT;
  std::array<uint32_t, Capacity> values{};
  uint32_t write = 0;
  uint32_t count = 0;

  void Add(uint32_t value) {
    AcquireSRWLockExclusive(&lock);
    values[write] = value;
    write = (write + 1u) % static_cast<uint32_t>(Capacity);
    if (count < Capacity) ++count;
    ReleaseSRWLockExclusive(&lock);
  }

  void Reset() {
    AcquireSRWLockExclusive(&lock);
    write = 0;
    count = 0;
    ReleaseSRWLockExclusive(&lock);
  }

  SampleStats Stats() {
    std::array<uint32_t, Capacity> ordered{};
    uint32_t copied = 0;
    AcquireSRWLockShared(&lock);
    copied = count;
    const uint32_t start = count == Capacity ? write : 0;
    for (uint32_t i = 0; i < copied; ++i)
      ordered[i] = values[(start + i) % static_cast<uint32_t>(Capacity)];
    ReleaseSRWLockShared(&lock);
    SampleStats result{};
    result.count = copied;
    if (copied == 0) return result;
    uint64_t delta_total = 0;
    for (uint32_t i = 1; i < copied; ++i) {
      const uint32_t a = ordered[i - 1];
      const uint32_t b = ordered[i];
      delta_total += a > b ? a - b : b - a;
    }
    result.mean_delta_us = copied > 1
                               ? static_cast<uint32_t>(delta_total /
                                                       (copied - 1u))
                               : 0;
    std::sort(ordered.begin(), ordered.begin() + copied);
    result.median_us = ordered[(copied - 1u) / 2u];
    result.p95_us = ordered[((copied - 1u) * 95u) / 100u];
    result.p99_us = ordered[((copied - 1u) * 99u) / 100u];
    return result;
  }
};

inline std::atomic<uint32_t> g_mode_override{
    static_cast<uint32_t>(pacing::ReflexModeOverride::kGame)};
inline std::atomic<uint32_t> g_pacing_method{
    static_cast<uint32_t>(pacing::ReflexPacingMethod::kNativeSleep)};
inline std::atomic_bool g_headroom_enabled{false};
inline std::atomic<uint32_t> g_headroom_basis_points{100};
inline std::atomic_bool g_vrr_active{false};
inline std::atomic<uint32_t> g_refresh_millihz{0};
inline std::atomic<uint32_t> g_headroom_limit_us{0};
// 1.10.0 (plan XMX Vulkan pacing): the output frame limit XMX's Vulkan provider can hold (us, 0 none), composed like
// the VRR headroom limit; a user cap takes its place (framecount.hpp).
inline std::atomic<uint32_t> g_xmx_vulkan_limit_us{0};
inline std::atomic<uint32_t> g_native_mode{0};
inline std::atomic<uint32_t> g_forwarded_mode{0};
inline std::atomic_bool g_mode_applied{false};
inline std::atomic_bool g_mode_rejected{false};
inline std::atomic<uint32_t> g_mfg_multiplier{0};
inline std::atomic<uint64_t> g_ui_heartbeat_ms{0};
inline std::atomic<SleepDispatch> g_sleep_dispatch{SleepDispatch::kNative};

inline std::atomic<WaitableState> g_waitable_state{WaitableState::kNative};
inline std::atomic<FallbackReason> g_fallback_reason{FallbackReason::kNone};
inline std::atomic<HANDLE> g_waitable_handle{nullptr};
inline std::atomic<uint64_t> g_waitable_generation{0};
inline std::atomic<uint32_t> g_waitable_readers{0};
inline std::atomic_bool g_waitable_retiring{false};
inline std::atomic<HANDLE> g_retire_event{nullptr};
inline std::atomic<uint32_t> g_sleep_inflight{0};
inline std::atomic_bool g_token_seen{false};
inline std::atomic<uint32_t> g_last_token{0};
inline std::atomic<uint32_t> g_probe_samples{0};
inline std::atomic<uint32_t> g_timeout_ms{50};
inline std::atomic<uint32_t> g_timeout_count{0};
inline std::atomic<uint32_t> g_wait_failure_count{0};
inline std::atomic<uint32_t> g_native_maximum_latency{0};
inline std::atomic<uint32_t> g_effective_maximum_latency{0};
inline std::atomic_bool g_maximum_latency_forced{false};
inline SampleRing<> g_sleep_intervals;
inline SampleRing<> g_sleep_durations;
inline SampleRing<> g_present_intervals;
inline std::atomic<uint64_t> g_last_sleep_qpc{0};
inline std::atomic<uint64_t> g_last_present_qpc{0};

inline uint64_t QpcFrequency() {
  static const uint64_t frequency = [] {
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value)
               ? static_cast<uint64_t>(value.QuadPart)
               : 0ull;
  }();
  return frequency;
}

inline uint32_t TicksToUs(uint64_t ticks) {
  const uint64_t frequency = QpcFrequency();
  if (frequency == 0 || ticks == 0) return 0;
  const uint64_t microseconds =
      ticks > UINT64_MAX / 1000000ull
          ? UINT64_MAX
          : ticks * 1000000ull / frequency;
  return microseconds > UINT32_MAX ? UINT32_MAX
                                   : static_cast<uint32_t>(microseconds);
}

inline bool LabMeasurementEnabled() {
  if (pacing::NormalizeReflexPacingMethod(
          g_pacing_method.load(std::memory_order_relaxed)) !=
      pacing::ReflexPacingMethod::kNativeSleep)
    return true;
  if (g_headroom_enabled.load(std::memory_order_relaxed) ||
      pacing::NormalizeReflexModeOverride(
          g_mode_override.load(std::memory_order_relaxed)) !=
          pacing::ReflexModeOverride::kGame)
    return true;
  const uint64_t heartbeat = g_ui_heartbeat_ms.load(std::memory_order_acquire);
  return heartbeat != 0 && GetTickCount64() - heartbeat <= 1500;
}

inline bool WaitableRequested() {
  return pacing::NormalizeReflexPacingMethod(
             g_pacing_method.load(std::memory_order_relaxed)) ==
         pacing::ReflexPacingMethod::kDxgiWaitable;
}

inline bool FgSafeOffRequested() {
  return pacing::NormalizeReflexModeOverride(
             g_mode_override.load(std::memory_order_relaxed)) ==
         pacing::ReflexModeOverride::kOff;
}

inline sl::Result NativeOrFgSafeBypass(const sl::FrameToken& frame,
                                       PFun_slReflexSleep* real) {
  return FgSafeOffRequested() ? sl::Result::eOk : real(frame);
}

inline void SetFallback(FallbackReason reason) {
  g_fallback_reason.store(reason, std::memory_order_relaxed);
  g_waitable_state.store(WaitableState::kFallback,
                         std::memory_order_release);
}

inline void ResetCadence() {
  g_last_sleep_qpc.store(0, std::memory_order_relaxed);
  g_last_present_qpc.store(0, std::memory_order_relaxed);
  g_sleep_intervals.Reset();
  g_sleep_durations.Reset();
  g_present_intervals.Reset();
}

inline void ResetProbeState() {
  g_sleep_inflight.store(0, std::memory_order_relaxed);
  g_token_seen.store(false, std::memory_order_relaxed);
  g_last_token.store(0, std::memory_order_relaxed);
  g_probe_samples.store(0, std::memory_order_relaxed);
  g_timeout_count.store(0, std::memory_order_relaxed);
  g_wait_failure_count.store(0, std::memory_order_relaxed);
  ResetCadence();
}

inline void PublishWaitable(HANDLE handle, uint64_t generation,
                            uint32_t timeout_ms, uint32_t native_latency) {
  ResetProbeState();
  g_waitable_generation.store(generation, std::memory_order_relaxed);
  g_timeout_ms.store((std::clamp)(timeout_ms, 8u, 50u),
                     std::memory_order_relaxed);
  g_native_maximum_latency.store(native_latency, std::memory_order_relaxed);
  g_effective_maximum_latency.store(native_latency,
                                    std::memory_order_relaxed);
  g_maximum_latency_forced.store(false, std::memory_order_relaxed);
  g_fallback_reason.store(FallbackReason::kNone, std::memory_order_relaxed);
  g_waitable_handle.store(handle, std::memory_order_release);
  g_waitable_state.store(WaitableState::kProbing,
                         std::memory_order_release);
}

// The caller owns and closes the returned duplicated handle. New readers can
// no longer acquire it after the exchange; existing readers signal the rare
// retirement waiter when they leave the sleep hook.
inline HANDLE RetireWaitable(FallbackReason reason) {
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_retire_event.store(event, std::memory_order_release);
  g_waitable_retiring.store(true, std::memory_order_release);
  HANDLE handle = g_waitable_handle.exchange(nullptr,
                                               std::memory_order_acq_rel);
  if (g_waitable_readers.load(std::memory_order_acquire) != 0) {
    if (event != nullptr)
      WaitForSingleObject(event, INFINITE);
    else
      handle = nullptr;  // Fail safe: never close a handle still in use.
  }
  g_waitable_retiring.store(false, std::memory_order_release);
  g_retire_event.store(nullptr, std::memory_order_release);
  if (event != nullptr) CloseHandle(event);
  g_waitable_generation.store(0, std::memory_order_relaxed);
  g_maximum_latency_forced.store(false, std::memory_order_relaxed);
  g_effective_maximum_latency.store(0, std::memory_order_relaxed);
  g_native_maximum_latency.store(0, std::memory_order_relaxed);
  g_fallback_reason.store(reason, std::memory_order_relaxed);
  g_waitable_state.store(
      reason == FallbackReason::kNotRequested
          ? WaitableState::kNative
          : WaitableState::kFallback,
      std::memory_order_release);
  return handle;
}

inline void MarkActivationResult(uint64_t generation, bool success) {
  if (generation != g_waitable_generation.load(std::memory_order_acquire))
    return;
  if (!success) {
    SetFallback(FallbackReason::kMaximumLatencySetFailed);
    return;
  }
  g_effective_maximum_latency.store(1, std::memory_order_relaxed);
  g_maximum_latency_forced.store(
      g_native_maximum_latency.load(std::memory_order_relaxed) != 1,
      std::memory_order_relaxed);
  WaitableState expected = WaitableState::kActivationPending;
  g_waitable_state.compare_exchange_strong(
      expected, WaitableState::kActive, std::memory_order_acq_rel);
}

inline void RecordPresent() {
  if (!LabMeasurementEnabled()) return;
  LARGE_INTEGER counter{};
  if (!QueryPerformanceCounter(&counter)) return;
  const uint64_t now = static_cast<uint64_t>(counter.QuadPart);
  const uint64_t previous = g_last_present_qpc.exchange(
      now, std::memory_order_acq_rel);
  if (previous != 0 && now > previous)
    g_present_intervals.Add(TicksToUs(now - previous));
}

inline sl::Result MeasureNativeSleep(const sl::FrameToken& frame,
                                     PFun_slReflexSleep* real) {
  if (real == nullptr) return sl::Result::eErrorNotInitialized;
  LARGE_INTEGER begin{}, end{};
  QueryPerformanceCounter(&begin);
  const sl::Result result = real(frame);
  if (QueryPerformanceCounter(&end)) {
    const uint64_t now = static_cast<uint64_t>(end.QuadPart);
    const uint64_t previous = g_last_sleep_qpc.exchange(
        now, std::memory_order_acq_rel);
    if (previous != 0 && now > previous)
      g_sleep_intervals.Add(TicksToUs(now - previous));
    if (end.QuadPart >= begin.QuadPart)
      g_sleep_durations.Add(TicksToUs(
          static_cast<uint64_t>(end.QuadPart - begin.QuadPart)));
  }
  return result;
}

inline bool ValidateToken(uint32_t token) {
  if (!g_token_seen.exchange(true, std::memory_order_acq_rel)) {
    g_last_token.store(token, std::memory_order_relaxed);
    return true;
  }
  const uint32_t previous = g_last_token.exchange(token,
                                                   std::memory_order_acq_rel);
  if (token == previous) {
    SetFallback(FallbackReason::kTokenDuplicate);
    return false;
  }
  // Signed subtraction accepts the normal uint32_t wrap while rejecting an
  // old or regressing token.
  if (static_cast<int32_t>(token - previous) <= 0) {
    SetFallback(FallbackReason::kTokenRegressed);
    return false;
  }
  return true;
}

struct SleepScope {
  bool owner = false;
  SleepScope() {
    owner = g_sleep_inflight.fetch_add(1, std::memory_order_acq_rel) == 0;
    if (!owner) SetFallback(FallbackReason::kConcurrentSleep);
  }
  ~SleepScope() {
    g_sleep_inflight.fetch_sub(1, std::memory_order_acq_rel);
  }
};

struct ReaderScope {
  HANDLE handle = nullptr;
  ReaderScope() {
    g_waitable_readers.fetch_add(1, std::memory_order_seq_cst);
    handle = g_waitable_handle.load(std::memory_order_acquire);
  }
  ~ReaderScope() {
    if (g_waitable_readers.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        g_waitable_retiring.load(std::memory_order_acquire)) {
      if (HANDLE event = g_retire_event.load(std::memory_order_acquire);
          event != nullptr)
        SetEvent(event);
    }
  }
};

inline sl::Result HandleSleep(const sl::FrameToken& frame,
                              PFun_slReflexSleep* real) {
  if (real == nullptr) return sl::Result::eErrorNotInitialized;
  const WaitableState state =
      g_waitable_state.load(std::memory_order_acquire);
  if (!WaitableRequested() ||
      (state != WaitableState::kProbing &&
       state != WaitableState::kActivationPending &&
       state != WaitableState::kActive))
    return NativeOrFgSafeBypass(frame, real);

  const uint32_t multiplier = g_mfg_multiplier.load(std::memory_order_relaxed);
  if (multiplier < 2 || multiplier > 6) {
    // Games commonly start calling Reflex before FG is enabled in their menu.
    // Keep probing dormant in that state; only an already-active alternate
    // pacer treats loss of MFG as a transition back to native sleep.
    if (state == WaitableState::kActive)
      SetFallback(FallbackReason::kMfgInactive);
    return NativeOrFgSafeBypass(frame, real);
  }

  SleepScope scope;
  if (!scope.owner) return NativeOrFgSafeBypass(frame, real);
  const uint32_t token = static_cast<uint32_t>(frame);
  if (!ValidateToken(token)) return NativeOrFgSafeBypass(frame, real);

  LARGE_INTEGER begin{}, end{};
  const bool measure = LabMeasurementEnabled();
  if (measure) QueryPerformanceCounter(&begin);
  sl::Result result = sl::Result::eOk;
  if (state == WaitableState::kActive) {
    ReaderScope reader;
    if (reader.handle == nullptr) {
      SetFallback(FallbackReason::kNoWaitableHandle);
      result = NativeOrFgSafeBypass(frame, real);
    } else {
      const DWORD wait = WaitForSingleObjectEx(
          reader.handle, g_timeout_ms.load(std::memory_order_relaxed), FALSE);
      if (wait == WAIT_OBJECT_0) {
        result = sl::Result::eOk;
      } else {
        if (wait == WAIT_TIMEOUT) {
          g_timeout_count.fetch_add(1, std::memory_order_relaxed);
          SetFallback(FallbackReason::kWaitTimeout);
        } else {
          g_wait_failure_count.fetch_add(1, std::memory_order_relaxed);
          SetFallback(FallbackReason::kWaitFailed);
        }
        result = NativeOrFgSafeBypass(frame, real);
      }
    }
  } else {
    result = real(frame);
    if (result != sl::Result::eOk) {
      SetFallback(FallbackReason::kNativeSleepFailed);
    } else if (state == WaitableState::kProbing &&
               g_probe_samples.fetch_add(1, std::memory_order_acq_rel) + 1u >=
                   120u) {
      WaitableState expected = WaitableState::kProbing;
      g_waitable_state.compare_exchange_strong(
          expected, WaitableState::kActivationPending,
          std::memory_order_acq_rel);
    }
  }
  if (measure && QueryPerformanceCounter(&end)) {
    const uint64_t now = static_cast<uint64_t>(end.QuadPart);
    const uint64_t previous = g_last_sleep_qpc.exchange(
        now, std::memory_order_acq_rel);
    if (previous != 0 && now > previous)
      g_sleep_intervals.Add(TicksToUs(now - previous));
    if (end.QuadPart >= begin.QuadPart)
      g_sleep_durations.Add(TicksToUs(
          static_cast<uint64_t>(end.QuadPart - begin.QuadPart)));
  }
  return result;
}

}  // namespace mfgunlock::reflexpacing
