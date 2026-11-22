// SPDX-License-Identifier: MIT
// 1.12.0 plan (DLSS 5, N0.1): which NGX feature id NVIDIA's neural rendering (nvngx_dlssnr.dll) answers to.
//
// Asks the driver's NGX core (the _nvngx.dll that HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath
// names) for the requirements of feature ids 1 to 30 on the high-performance adapter, with an optional
// folder to look for snippets in (a game folder with nvngx_dlssnr.dll). Prints, per id, NGX's result, the
// support bits (0 supported, 1 check not present, 2 driver, 4 adapter, 8 OS, 16 not implemented) and the
// minimum GPU architecture (0x190 Ada, 0x1b0 Blackwell). Nothing is created or evaluated.
//
//   ngx_feature_probe.exe [snippet folder]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include <nvsdk_ngx_defs.h>

using Microsoft::WRL::ComPtr;
using RequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                     NVSDK_NGX_FeatureRequirement*);

static std::wstring CorePath() {
  wchar_t folder[MAX_PATH]{};
  DWORD size = sizeof(folder);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                   nullptr, folder, &size) != ERROR_SUCCESS)
    return {};
  return std::wstring(folder) + L"\\_nvngx.dll";
}

int wmain(int argc, wchar_t** argv) {
  const std::wstring core = CorePath();
  const HMODULE ngx = core.empty() ? nullptr : LoadLibraryW(core.c_str());
  if (ngx == nullptr) {
    std::printf("ngx_feature_probe: FAIL no NGX core (%ls)\n", core.c_str());
    return 1;
  }
  const auto requirements =
      reinterpret_cast<RequirementsFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
  if (requirements == nullptr) {
    std::printf("ngx_feature_probe: FAIL NVSDK_NGX_D3D12_GetFeatureRequirements missing\n");
    return 1;
  }
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
    std::printf("ngx_feature_probe: FAIL no adapter\n");
    return 1;
  }
  DXGI_ADAPTER_DESC1 desc{};
  adapter->GetDesc1(&desc);
  std::printf("adapter: %ls\ncore: %ls\n", desc.Description, core.c_str());

  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const wchar_t* folder = argc > 1 ? argv[1] : nullptr;
  const wchar_t* paths[1] = {folder};
  NVSDK_NGX_FeatureCommonInfo info{};
  info.PathListInfo.Path = folder ? paths : nullptr;
  info.PathListInfo.Length = folder ? 1u : 0u;
  if (folder) std::printf("snippet folder: %ls\n", folder);

  std::printf("%-4s %-12s %-10s %-8s\n", "id", "result", "support", "min arch");
  for (int id = 1; id <= 30; ++id) {
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = static_cast<NVSDK_NGX_Feature>(id);
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc.ProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";  // a probe's own id
    discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    discovery.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    discovery.ApplicationDataPath = temp;
    discovery.FeatureInfo = &info;
    NVSDK_NGX_FeatureRequirement out{};
    const NVSDK_NGX_Result result = requirements(adapter.Get(), &discovery, &out);
    std::printf("%-4d 0x%08x   0x%-8x 0x%x\n", id, static_cast<unsigned>(result),
                static_cast<unsigned>(out.FeatureSupported), out.MinHWArchitecture);
  }
  return 0;
}
