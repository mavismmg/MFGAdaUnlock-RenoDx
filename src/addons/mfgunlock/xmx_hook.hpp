// SPDX-License-Identifier: MIT
#pragma once
// 1.8 (plan D.2): the XMX mode's hook on NGX's Evaluate in nvngx_dlssg. Hooking a
// module stops its own generation: a pass-through call leaves a black image, and
// hooking CreateFeature makes it fail with PlatformError. So the hook goes in only
// after generator::Preflight, and from then on the generator owns the output.
#include <d3d12.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
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
    // Reached through a hook this copy did not install (see hook::JumpsIntoSelf).
    if (evaluate == nullptr) return NVSDK_NGX_Result_Fail;
    return evaluate(list, handle, params, callback);
  }
  static bool Install(HMODULE module) {
    evaluate = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (evaluate == nullptr) return false;
    if (hook::InstallOne(reinterpret_cast<void**>(&evaluate), reinterpret_cast<void*>(&Evaluate))) return true;
    evaluate = nullptr;
    return false;
  }
  static bool Uninstall() {
    if (evaluate == nullptr) return true;
    return hook::Uninstall({{"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&evaluate),
                             reinterpret_cast<void*>(&Evaluate)}});
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
inline bool UninstallSlot(int slot) {
  switch (slot) {
    case 0: return Slot<0>::Uninstall();
    case 1: return Slot<1>::Uninstall();
    case 2: return Slot<2>::Uninstall();
    case 3: return Slot<3>::Uninstall();
    default: return true;
  }
}

// 1.8.3: ReShade loads, unloads and reloads add-ons around its temporary
// startup devices, and a hook cannot outlive the copy whose code it calls
// (1.8.2: the next copy refused the provider's identity, and hooking over the
// leftover jump recursed). Until the addon is process-resident -- pinned at the
// first real Present -- Attach only remembers the module; Release installs.
namespace internal {
inline SRWLOCK lock = SRWLOCK_INIT;
inline std::vector<HMODULE> seen, pending;
inline bool resident = false;
}  // namespace internal

// The hook's bytes count as the addon's own code, so validating the provider's
// identity later in this session ignores them (see modulesession::Validate).
inline void RecordHookBytes(HMODULE module, uint8_t* address, const std::array<uint8_t, 16>& before) {
  const auto* session = modulesession::Acquire(module);
  if (session == nullptr || !patching::Readable(address, before.size())) return;
  patching::Write write;
  write.address = address;
  write.original.assign(before.begin(), before.end());
  write.replacement.assign(address, address + before.size());
  write.applied = write.original != write.replacement;
  if (write.applied) modulesession::RecordCode(*session, {write});
}

inline void InstallInThread(HMODULE module, int slot) {
  struct Work { HMODULE module; int slot; };
  const auto thread = CreateThread(
      nullptr, 0,
      [](void* value) -> DWORD {
        const std::unique_ptr<Work> work(static_cast<Work*>(value));
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(work->module, path, MAX_PATH);
        const std::filesystem::path file(path);
        const std::string where = file.parent_path().filename().string() + "/" + file.filename().string();
        auto* entry = reinterpret_cast<uint8_t*>(GetProcAddress(work->module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        if (hook::JumpsIntoSelf(entry)) {
          generator::g_hooks_failed.fetch_add(1);
          generator::Fail("an earlier copy of the add-on left its hook in NVIDIA's generator; restart the game");
          Log("slot " + std::to_string(work->slot) + " refused " + where + ": hook left by an unloaded copy");
          return 0;
        }
        // The provider's identity is hashed on its unhooked code, whichever
        // path reached here first; the lease is only contended briefly.
        for (int wait = 0; wait < 40 && modulesession::Acquire(work->module) == nullptr; ++wait) Sleep(5);
        std::array<uint8_t, 16> before{};
        std::memcpy(before.data(), entry, before.size());
        // A thread that starts or exits mid-walk fails the attempt; the next one sees it settled.
        bool hooked = false;
        int attempt = 1;
        for (; attempt <= kInstallAttempts; ++attempt) {
          hooked = InstallSlot(work->slot, work->module);
          if (hooked) break;
          Sleep(250);
        }
        if (hooked) RecordHookBytes(work->module, entry, before);
        (hooked ? generator::g_hooks_installed : generator::g_hooks_failed).fetch_add(1);
        Log("slot " + std::to_string(work->slot) + (hooked ? " hooked " : " failed ") + where + " attempts " +
            std::to_string((std::min)(attempt, kInstallAttempts)));
        return 0;
      },
      new Work{module, slot}, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);
}

// Under the loader lock only plain bookkeeping happens here; Detours walks the
// threads, so a short-lived thread installs once that lock is gone.
inline void Attach(HMODULE module) {
  if (module == nullptr || !generator::Enabled() || !generator::Preflight()) return;
  // A renamed Vulkan provider has no D3D12 entry and takes no slot.
  if (GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature") == nullptr) return;
  AcquireSRWLockExclusive(&internal::lock);
  const bool known = std::find(internal::seen.begin(), internal::seen.end(), module) != internal::seen.end();
  if (!internal::resident) {
    const bool queued =
        std::find(internal::pending.begin(), internal::pending.end(), module) != internal::pending.end();
    // The provider stays mapped until Release; NGX may unload a fallback copy.
    HMODULE pinned = nullptr;
    if (!known && !queued &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(module), &pinned))
      internal::pending.push_back(module);
    ReleaseSRWLockExclusive(&internal::lock);
    return;
  }
  const int slot = static_cast<int>(internal::seen.size());
  const auto action = policy::PlanSlot(known, slot, kSlots);
  HMODULE pinned = nullptr;
  const bool install = action == policy::SlotAction::Install &&
                       GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                          reinterpret_cast<LPCWSTR>(module), &pinned);
  if (install) internal::seen.push_back(module);
  ReleaseSRWLockExclusive(&internal::lock);
  if (action == policy::SlotAction::Full) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set()) {
      generator::g_hooks_failed.fetch_add(1);
      Log("no free slot for another copy of the generator");
    }
  }
  if (install) InstallInThread(pinned, slot);
}

// Called once the addon is pinned: from here on Attach installs, and every
// module remembered before installs now.
inline void Release() {
  AcquireSRWLockExclusive(&internal::lock);
  if (internal::resident) {
    ReleaseSRWLockExclusive(&internal::lock);
    return;
  }
  internal::resident = true;
  const auto pending = std::move(internal::pending);
  internal::pending.clear();
  ReleaseSRWLockExclusive(&internal::lock);
  if (!pending.empty()) Log("installing on " + std::to_string(pending.size()) + " module(s) mapped before the first Present");
  for (HMODULE module : pending) Attach(module);
}

// Explicit unload only. Normally unreachable: hooks go in after the pin.
inline void Uninstall() {
  AcquireSRWLockExclusive(&internal::lock);
  const int slots = static_cast<int>(internal::seen.size());
  ReleaseSRWLockExclusive(&internal::lock);
  for (int slot = 0; slot < slots; ++slot) UninstallSlot(slot);
}
}  // namespace mfgunlock::xmx
