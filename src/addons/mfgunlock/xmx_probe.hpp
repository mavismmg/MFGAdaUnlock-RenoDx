// SPDX-License-Identifier: MIT
#pragma once
// 1.8 (plan D.2): groundwork for the XMX mode, off unless MFGAdaUnlock.ini has
// [DLSSG] XmxProbe=1. Under the DLSS-G backend it observes what NGX evaluates in
// nvngx_dlssg and creates one raw XeFG context, to check our reading of
// libxess_fg.dll. It only hooks Evaluate: hooking CreateFeature makes the NVIDIA
// module fail with PlatformError. The context is neither initialized nor used.
// The offsets come from our own analysis (docs/research/xemfg-modo-xmx-2026-11-01.md).
#include <d3d12.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "backend_selection.hpp"
#include "ngx_hook.hpp"
#include "xmx_generator.hpp"

namespace mfgunlock::xmx {
constexpr DWORD kKnownTimestamp = 0x69CB0F4D;  // libxess_fg.dll 1.3.1.78
constexpr uintptr_t kFactoryRva = 0x1853B0;
constexpr uintptr_t kContextVtableRva = 0xF45D48;

inline bool Enabled() {
  static const bool value =
      GetPrivateProfileIntW(L"DLSSG", L"XmxProbe", 0, (backend::Directory() / L"MFGAdaUnlock.ini").c_str()) == 1;
  return value;
}
inline void Log(const std::string& text) {
  reshade::log::message(reshade::log::level::info, ("mfgunlock: xmx_probe " + text).c_str());
}

using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*, void*);
inline std::atomic<int> g_evaluations_logged{0};
inline std::atomic_bool g_context_probed{false};
constexpr int kEvaluationsToLog = 4;

// A separate function: SEH cannot share a frame with objects that unwind.
inline bool CallFactory(void* factory, ID3D12Device* device, void** context) noexcept {
  using Factory = int(__cdecl*)(ID3D12Device*, void**);
  __try {
    return reinterpret_cast<Factory>(factory)(device, context) == 0 && *context != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
inline void ProbeContext(ID3D12GraphicsCommandList* list) {
  if (list == nullptr || g_context_probed.exchange(true)) return;
  auto fg = GetModuleHandleW(L"libxess_fg.dll");
  if (fg == nullptr) {
    fg = LoadLibraryExW((backend::Directory() / L"libxess_fg.dll").c_str(), nullptr,
                        LOAD_WITH_ALTERED_SEARCH_PATH);
  }
  if (fg == nullptr) {
    Log("context skipped: libxess_fg.dll not found");
    return;
  }
  const auto* base = reinterpret_cast<const unsigned char*>(fg);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kKnownTimestamp) {
    Log("context skipped: unknown libxess_fg.dll build " + std::to_string(nt->FileHeader.TimeDateStamp));
    return;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) {
    Log("context skipped: no device");
    return;
  }
  // Left alive on purpose: its destructor has not been analyzed yet.
  void* context = nullptr;
  const bool created = CallFactory(const_cast<unsigned char*>(base) + kFactoryRva, device, &context);
  device->Release();
  if (!created) {
    Log("context creation failed");
    return;
  }
  // What the factory returns, as RVAs when they point into the DLL.
  std::string text = "context created:";
  const auto image = reinterpret_cast<uintptr_t>(base);
  const auto size = nt->OptionalHeader.SizeOfImage;
  const auto describe = [&](uintptr_t value) {
    return value >= image && value < image + size ? "rva " + std::to_string(value - image) : std::to_string(value);
  };
  auto* words = static_cast<uintptr_t*>(context);
  for (int i = 0; i < 4; ++i) {
    text += " [" + std::to_string(i) + "]=" + describe(words[i]);
    uintptr_t inner = 0;
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(words[i]), &inner, sizeof(inner), &read) &&
        read == sizeof(inner))
      text += "->" + describe(inner) + (inner - image == kContextVtableRva ? "(XefgContext)" : "");
  }
  Log(text);
}
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
template <int N>
struct Slot {
  static inline EvaluateFeatureFn evaluate = nullptr;
  static NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                              const NVSDK_NGX_Parameter* params, void* callback) {
    if (g_evaluations_logged.fetch_add(1, std::memory_order_relaxed) < kEvaluationsToLog) LogParameters(params);
    if (generator::Enabled()) {
      if (generator::Evaluate(list, params)) return NVSDK_NGX_Result_Success;
    } else {
      ProbeContext(list);
    }
    return evaluate(list, handle, params, callback);
  }
  static bool Install(HMODULE module) {
    evaluate = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (evaluate == nullptr) return false;
    if (DetourTransactionBegin() != NO_ERROR) return false;
    auto threads = hook::internal::OpenOtherThreads();
    for (HANDLE thread : threads.handles) DetourUpdateThread(thread);
    DetourAttach(reinterpret_cast<void**>(&evaluate), reinterpret_cast<void*>(&Evaluate));
    const auto result = DetourTransactionCommit();
    threads.Close();
    return result == NO_ERROR;
  }
};
inline bool InstallSlot(int slot, HMODULE module) {
  // Detours has one transaction at a time, and modules can load together.
  static std::mutex lock;
  std::lock_guard guard(lock);
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
  if (!Enabled() && !generator::Enabled()) return;
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
        Log("slot " + std::to_string(work->slot) + (InstallSlot(work->slot, work->module) ? " hooked " : " failed ") +
            std::filesystem::path(path).parent_path().filename().string());
        return 0;
      },
      new Work{pinned, slot}, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);
}
}  // namespace mfgunlock::xmx
