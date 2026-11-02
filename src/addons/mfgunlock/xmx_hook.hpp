// SPDX-License-Identifier: MIT
#pragma once
// 1.8 (plan D.2): the XMX mode's hook on NGX's Evaluate in nvngx_dlssg. Hooking a
// module stops its own generation: a pass-through call leaves a black image, and
// hooking CreateFeature makes it fail with PlatformError. So the hook goes in only
// after generator::Preflight, and from then on the generator owns the output.
#include <d3d12.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ngx_hook.hpp"
#include "xmx_generator.hpp"

namespace mfgunlock::xmx {
inline void Log(const std::string& text) {
  reshade::log::message(reshade::log::level::info, ("mfgunlock: xmx " + text).c_str());
}

using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*, void*);
inline std::atomic<int> g_evaluations_logged{0};
constexpr int kEvaluationsToLog = 4;

inline void LogParameters(const NVSDK_NGX_Parameter* params) {
  if (params == nullptr) return;
  std::string text = "evaluate";
  for (const char* name : {"DLSSG.Backbuffer", "DLSSG.Depth", "DLSSG.MVecs", "DLSSG.HUDLess", "DLSSG.OutputInterpolated",
                           "DLSSG.OutputReal"}) {
    ID3D12Resource* resource = nullptr;
    if (params->Get(name, &resource) != NVSDK_NGX_Result_Success || resource == nullptr) {
      text += std::string(" ") + name + "=none";
      continue;
    }
    const auto desc = resource->GetDesc();
    text += std::string(" ") + name + "=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + "/" +
            std::to_string(static_cast<int>(desc.Format));
  }
  for (const char* name : {"DLSSG.MultiFrameIndex", "DLSSG.MultiFrameCount", "DLSSG.Reset", "DLSSG.EnableInterp"}) {
    unsigned int value = 0;
    if (params->Get(name, &value) == NVSDK_NGX_Result_Success)
      text += std::string(" ") + name + "=" + std::to_string(value);
  }
  for (const char* name : {"DLSSG.MvecScaleX", "DLSSG.MvecScaleY", "DLSSG.JitterOffsetX", "DLSSG.JitterOffsetY"}) {
    float value = 0.f;
    if (params->Get(name, &value) == NVSDK_NGX_Result_Success)
      text += std::string(" ") + name + "=" + std::to_string(value);
  }
  Log(text);
}
// A process maps one nvngx_dlssg per location (game folder, DriverStore), and
// NGX may use any of them, so each module gets its own slot.
constexpr int kSlots = 4;
constexpr int kInstallAttempts = 20;
template <int N>
struct Slot {
  static inline EvaluateFeatureFn evaluate = nullptr;
  static NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                              const NVSDK_NGX_Parameter* params, void* callback) {
    if (g_evaluations_logged.fetch_add(1, std::memory_order_relaxed) < kEvaluationsToLog) LogParameters(params);
    generator::g_last_evaluate_tick.store(GetTickCount64(), std::memory_order_relaxed);
    if (generator::Evaluate(list, params)) return NVSDK_NGX_Result_Success;
    return evaluate(list, handle, params, callback);
  }
  static bool Install(HMODULE module) {
    evaluate = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (evaluate == nullptr) return false;
    return hook::InstallOne(reinterpret_cast<void**>(&evaluate), reinterpret_cast<void*>(&Evaluate));
  }
};
inline bool InstallSlot(int slot, HMODULE module) {
  switch (slot) {
    case 0: return Slot<0>::Install(module);
    case 1: return Slot<1>::Install(module);
    case 2: return Slot<2>::Install(module);
    case 3: return Slot<3>::Install(module);
    default: return false;
  }
}
// LoadLibrary notifications can hold the loader lock, and Detours walks the
// threads; a short-lived thread installs once that lock is gone.
inline void Attach(HMODULE module) {
  static SRWLOCK lock = SRWLOCK_INIT;
  static std::vector<HMODULE> seen;
  if (!generator::Enabled() || !generator::Preflight()) return;
  AcquireSRWLockExclusive(&lock);
  const bool known = std::find(seen.begin(), seen.end(), module) != seen.end();
  const int slot = static_cast<int>(seen.size());
  if (!known && slot < kSlots) seen.push_back(module);
  ReleaseSRWLockExclusive(&lock);
  if (known || slot >= kSlots) return;
  HMODULE pinned = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(module), &pinned))
    return;
  struct Work { HMODULE module; int slot; };
  const auto thread = CreateThread(
      nullptr, 0,
      [](void* value) -> DWORD {
        const std::unique_ptr<Work> work(static_cast<Work*>(value));
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(work->module, path, MAX_PATH);
        // A thread that starts or exits mid-walk fails the attempt; the next one sees it settled.
        bool hooked = false;
        int attempt = 1;
        for (; attempt <= kInstallAttempts; ++attempt) {
          hooked = InstallSlot(work->slot, work->module);
          if (hooked) break;
          Sleep(250);
        }
        (hooked ? generator::g_hooks_installed : generator::g_hooks_failed).fetch_add(1);
        Log("slot " + std::to_string(work->slot) + (hooked ? " hooked " : " failed ") +
            std::filesystem::path(path).parent_path().filename().string() + " attempts " +
            std::to_string((std::min)(attempt, kInstallAttempts)));
        return 0;
      },
      new Work{pinned, slot}, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);
}
}  // namespace mfgunlock::xmx
