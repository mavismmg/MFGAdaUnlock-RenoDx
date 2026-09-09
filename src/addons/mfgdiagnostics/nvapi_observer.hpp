/*
 * Read-only NVIDIA latency and NGX-override observation.
 * SPDX-License-Identifier: MIT
 *
 * The declarations below are the minimal ABI subset needed from NVIDIA's
 * public NVAPI SDK. They intentionally expose no setters and perform no API
 * hooks. Structure sizes are checked against the 2026-09-08 public headers.
 */

#pragma once

#include <windows.h>
#include <unknwn.h>

#include <cstdint>

namespace mfgdiagnostics::nvapi {

using Status = int32_t;
constexpr Status kOk = 0;

template <typename T, uint32_t Version>
constexpr uint32_t StructVersion() {
  return static_cast<uint32_t>(sizeof(T)) | (Version << 16);
}

struct SleepStatus {
  uint32_t version;
  uint8_t low_latency_mode;
  uint8_t fullscreen_vrr;
  uint8_t control_panel_vsync;
  uint32_t sleep_interval_us;
  uint8_t game_sleep;
  uint8_t fullscreen_independent_flip;
  uint8_t frame_generation_multiplier;
  uint8_t dynamic_frame_generation_control;
  uint32_t dynamic_frame_time_target_us;
  uint8_t reserved[114];
};
static_assert(sizeof(SleepStatus) == 136);

struct LatencyFrame {
  uint64_t frame_id;
  uint64_t input_sample_time;
  uint64_t simulation_start_time;
  uint64_t simulation_end_time;
  uint64_t render_submit_start_time;
  uint64_t render_submit_end_time;
  uint64_t present_start_time;
  uint64_t present_end_time;
  uint64_t driver_start_time;
  uint64_t driver_end_time;
  uint64_t os_render_queue_start_time;
  uint64_t os_render_queue_end_time;
  uint64_t gpu_render_start_time;
  uint64_t gpu_render_end_time;
  uint32_t gpu_active_render_time_us;
  uint32_t gpu_frame_time_us;
  uint64_t camera_constructed_time;
  uint32_t cross_adapter_copy_time_us;
  uint32_t ai_frame_time_us;
  uint8_t reserved[104];
};
static_assert(sizeof(LatencyFrame) == 240);

struct LatencyResult {
  uint32_t version;
  uint32_t alignment_padding;
  LatencyFrame frames[64];
  uint8_t reserved[32];
};
static_assert(sizeof(LatencyResult) == 15400);

struct NgxOverrideState {
  uint32_t version;
  uint32_t process_id;
  uint64_t feedback_super_resolution;
  uint64_t feedback_ray_reconstruction;
  uint64_t feedback_frame_generation;
  float scaling_ratio;
  uint32_t performance_mode;
  uint32_t render_preset;
  uint32_t frame_generation_count;
  uint32_t frame_generation_preset;
  uint32_t frame_generation_mode;
  uint64_t reserved0;
  uint32_t reserved1;
  uint32_t reserved[7];
};
static_assert(sizeof(NgxOverrideState) == 96);

struct Snapshot {
  bool library_loaded = false;
  Status initialize_status = INT32_MIN;
  Status sleep_status = INT32_MIN;
  Status latency_status = INT32_MIN;
  Status ngx_override_status = INT32_MIN;
  SleepStatus sleep{};
  LatencyResult latency{};
  NgxOverrideState ngx{};
};

using QueryInterfaceFn = void*(__cdecl*)(uint32_t);
using InitializeFn = Status(__cdecl*)();
using GetSleepStatusFn = Status(__cdecl*)(IUnknown*, SleepStatus*);
using GetLatencyFn = Status(__cdecl*)(IUnknown*, LatencyResult*);
using GetNgxOverrideStateFn = Status(__cdecl*)(NgxOverrideState*);

// Public interface IDs from NVIDIA/nvapi nvapi_interface.h.
constexpr uint32_t kInitializeId = 0x0150E828;
constexpr uint32_t kGetSleepStatusId = 0xAEF96CA1;
constexpr uint32_t kGetLatencyId = 0x1A587F9C;
constexpr uint32_t kGetNgxOverrideStateId = 0x3FD96FBA;

inline HMODULE Module() {
  static const HMODULE module =
      LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module;
}

inline QueryInterfaceFn QueryInterface() {
  static const auto query = Module() == nullptr
                                ? nullptr
                                : reinterpret_cast<QueryInterfaceFn>(
                                      GetProcAddress(Module(), "nvapi_QueryInterface"));
  return query;
}

inline Status Initialize() {
  static const Status status = [] {
    const auto query = QueryInterface();
    if (query == nullptr) return INT32_MIN;
    const auto initialize = reinterpret_cast<InitializeFn>(query(kInitializeId));
    return initialize == nullptr ? INT32_MIN : initialize();
  }();
  return status;
}

inline Snapshot Observe(IUnknown* device, uint32_t process_id) {
  Snapshot result{};
  const HMODULE module = Module();
  if (module == nullptr) return result;
  result.library_loaded = true;

  const auto query = QueryInterface();
  if (query == nullptr) return result;
  result.initialize_status = Initialize();

  if (device != nullptr) {
    result.sleep.version = StructVersion<SleepStatus, 1>();
    if (const auto get_sleep =
            reinterpret_cast<GetSleepStatusFn>(query(kGetSleepStatusId));
        get_sleep != nullptr) {
      result.sleep_status = get_sleep(device, &result.sleep);
    }

    result.latency.version = StructVersion<LatencyResult, 1>();
    if (const auto get_latency = reinterpret_cast<GetLatencyFn>(query(kGetLatencyId));
        get_latency != nullptr) {
      result.latency_status = get_latency(device, &result.latency);
    }
  }

  result.ngx.version = StructVersion<NgxOverrideState, 2>();
  result.ngx.process_id = process_id;
  if (const auto get_ngx =
          reinterpret_cast<GetNgxOverrideStateFn>(query(kGetNgxOverrideStateId));
      get_ngx != nullptr) {
    result.ngx_override_status = get_ngx(&result.ngx);
  }
  return result;
}

}  // namespace mfgdiagnostics::nvapi
