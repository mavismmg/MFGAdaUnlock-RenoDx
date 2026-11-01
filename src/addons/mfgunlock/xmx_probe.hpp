// SPDX-License-Identifier: MIT
#pragma once
// 1.8 (plan D.2): groundwork for the XMX mode, off unless MFGAdaUnlock.ini has
// [DLSSG] XmxProbe=1. Under the DLSS-G backend it observes the calls NGX makes to
// nvngx_dlssg and creates one raw XeFG context, whose vtable shows whether our
// reading of libxess_fg.dll holds. Generation stays NVIDIA's: every call goes on
// unchanged, and the context is neither initialized nor used. The offsets come
// from our own analysis (docs/research/xemfg-modo-xmx-2026-11-01.md).
#include <d3d12.h>
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>
#include "backend_selection.hpp"
#include "ngx_hook.hpp"

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

using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                      NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*, void*);
inline CreateFeatureFn g_create_feature = nullptr;
inline EvaluateFeatureFn g_evaluate_feature = nullptr;
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
  const auto vtable = *static_cast<uintptr_t*>(context) - reinterpret_cast<uintptr_t>(base);
  Log("context created; vtable rva " + std::to_string(vtable) +
      (vtable == kContextVtableRva ? " (expected)" : " (UNEXPECTED)"));
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
inline NVSDK_NGX_Result NVSDK_CONV HookedCreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                                       NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle) {
  const auto result = g_create_feature(list, feature, params, handle);
  Log("create_feature feature=" + std::to_string(static_cast<int>(feature)) +
      " result=" + std::to_string(static_cast<unsigned int>(result)));
  ProbeContext(list);
  return result;
}
inline NVSDK_NGX_Result NVSDK_CONV HookedEvaluateFeature(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                                         const NVSDK_NGX_Parameter* params, void* callback) {
  if (g_evaluations_logged.fetch_add(1, std::memory_order_relaxed) < kEvaluationsToLog) LogParameters(params);
  return g_evaluate_feature(list, handle, params, callback);
}
// LoadLibrary notifications can hold the loader lock, and Detours walks the
// threads; a short-lived thread installs once that lock is gone.
inline void Attach(HMODULE module) {
  static std::atomic_bool attached{false};
  if (!Enabled() || attached.exchange(true)) return;
  HMODULE pinned = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(module), &pinned))
    return;
  const auto thread = CreateThread(
      nullptr, 0,
      [](void* value) -> DWORD {
        const bool ok = hook::Install(
            static_cast<HMODULE>(value),
            {{"NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void**>(&g_create_feature),
              reinterpret_cast<void*>(&HookedCreateFeature)},
             {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&g_evaluate_feature),
              reinterpret_cast<void*>(&HookedEvaluateFeature)}},
            "nvngx_dlssg (xmx_probe)");
        Log(ok ? "hooks installed" : "hooks failed");
        return 0;
      },
      pinned, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);
}
}  // namespace mfgunlock::xmx
