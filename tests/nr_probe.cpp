// SPDX-License-Identifier: MIT
// 1.12.0 (DLSS 5, N0.2/N2.3): NVIDIA's neural rendering (nvngx_dlssnr.dll, NGX feature 18) on an image,
// outside any game, to tell whether a runtime works on this GPU and what each setting does.
//
//   nr_probe.exe --snippet <nvngx_dlssnr.dll> --image <in.png> --out <out.png>
//                [--allow-unsigned] [--frames N] [--preset N] [--style N] [--intensity F]
//                [--tone F] [--structure F] [--skin F] [--automask 0|1] [--app-id N] [--init-params] [--diff <png>]
//
// The runtime is the user's own file; nothing is downloaded. A file without NVIDIA's Authenticode signature is
// refused unless --allow-unsigned. Calls into the snippet go through mfgunlock_nvngx.dll (nr_shim.cpp), since
// the snippet answers only callers whose module path contains "nvngx.dll". Exit codes: 0 the image changed
// (NR ran), 3 it ran but the image did not change, 2 refused, 1 failed.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <softpub.h>
#include <wincodec.h>
#include <wintrust.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include "../src/addons/mfgunlock/nr_parameters.hpp"

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;

namespace {
constexpr int kFeatureNr = 18;
constexpr int kSnippetSdk = 0x15;
constexpr const char* kProjectId = "3c5b0a51-6f2e-4b8d-9d17-4f0b6e2a8c19";  // MFG Unlock's (force_fg.hpp)

void Fail(const char* what, unsigned code = 0) {
  std::printf("nr_probe: FAIL %s (0x%08x)\n", what, code);
  std::exit(1);
}
void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) Fail(what, static_cast<unsigned>(hr));
}
bool NgxOk(int r) { return r == 1; }

// Authenticode: valid and signed by NVIDIA Corporation.
bool SignedByNvidia(const std::wstring& path, std::wstring& signer) {
  WINTRUST_FILE_INFO file{sizeof(file), path.c_str(), nullptr, nullptr};
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  bool nvidia = false;
  if (status == ERROR_SUCCESS) {
    if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
      if (CRYPT_PROVIDER_SGNR* sgnr = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
        if (sgnr->csCertChain > 0 && sgnr->pasCertChain[0].pCert) {
          wchar_t name[256]{};
          CertGetNameStringW(sgnr->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
          signer = name;
          nvidia = signer == L"NVIDIA Corporation";
        }
  } else {
    signer = status == TRUST_E_NOSIGNATURE ? L"(no signature)" : L"(signature not valid)";
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  return nvidia;
}

struct Image {
  UINT width = 0, height = 0;
  std::vector<uint8_t> rgba;
};
Image LoadImage(const std::wstring& path, UINT want_w = 0, UINT want_h = 0) {
  ComPtr<IWICImagingFactory> wic;
  Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "WIC");
  ComPtr<IWICBitmapDecoder> decoder;
  Check(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
        "open image");
  ComPtr<IWICBitmapFrameDecode> frame;
  Check(decoder->GetFrame(0, &frame), "image frame");
  // --size: the image resized first (the cost of a resolution).
  ComPtr<IWICBitmapSource> source = frame;
  ComPtr<IWICBitmapScaler> scaler;
  if (want_w && want_h) {
    Check(wic->CreateBitmapScaler(&scaler), "scaler");
    Check(scaler->Initialize(frame.Get(), want_w, want_h, WICBitmapInterpolationModeFant), "scale");
    source = scaler;
  }
  ComPtr<IWICFormatConverter> converter;
  Check(wic->CreateFormatConverter(&converter), "converter");
  Check(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom),
        "convert image");
  Image image;
  converter->GetSize(&image.width, &image.height);
  image.rgba.resize(size_t(image.width) * image.height * 4);
  Check(converter->CopyPixels(nullptr, image.width * 4, UINT(image.rgba.size()), image.rgba.data()), "pixels");
  return image;
}
void SaveImage(const std::wstring& path, const Image& image) {
  ComPtr<IWICImagingFactory> wic;
  Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "WIC");
  ComPtr<IWICStream> stream;
  Check(wic->CreateStream(&stream), "stream");
  Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "out file");
  ComPtr<IWICBitmapEncoder> encoder;
  Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "encoder");
  Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder init");
  ComPtr<IWICBitmapFrameEncode> frame;
  Check(encoder->CreateNewFrame(&frame, nullptr), "frame");
  Check(frame->Initialize(nullptr), "frame init");
  Check(frame->SetSize(image.width, image.height), "size");
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
  Check(frame->SetPixelFormat(&format), "format");
  // The PNG encoder may answer with its own BGRA instead of the RGBA asked for: swap to match.
  std::vector<uint8_t> pixels = image.rgba;
  if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
  else if (!IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA))
    Fail("the PNG encoder offers neither RGBA nor BGRA");
  Check(frame->WritePixels(image.height, image.width * 4, UINT(pixels.size()), pixels.data()), "write");
  Check(frame->Commit(), "commit");
  Check(encoder->Commit(), "encoder commit");
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 0;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  void Submit() {
    Check(list->Close(), "close list");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), ++value), "signal");
    if (fence->GetCompletedValue() < value) {
      fence->SetEventOnCompletion(value, event);
      if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) Fail("GPU wait timed out");
    }
    Check(device->GetDeviceRemovedReason(), "device removed");
    Check(allocator->Reset(), "allocator reset");
    Check(list->Reset(allocator.Get(), nullptr), "list reset");
  }
  void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
  }
};

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> r;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "texture");
  return r;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{type};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
  return r;
}

std::wstring CoreFolder() {
  wchar_t folder[MAX_PATH]{};
  DWORD size = sizeof(folder);
  RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
               nullptr, folder, &size);
  return folder;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring snippet_path, image_path, out_path, diff_path;
  bool allow_unsigned = false, init_params = false, own_params = false, keys = false;
  float scale = 1.f;
  UINT size_w = 0, size_h = 0;
  int frames = 10;
  unsigned preset = 0, style = 0, automask = 1;
  float intensity = 1.f, tone = 1.f, structure = 1.f, skin = 1.f;
  unsigned long long app_id = 0x1000000ull;
  // 1.12.0 (DLSS 5, phase A): a time test. NR on a static image (pan 0) or one moving left by `pan` pixels a
  // frame, with motion vectors and depth given or not: how much its output changes between frames where
  // the content is the same (flicker, or the vectors' convention when they are wrong).
  int temporal = 0, pan = 0;
  std::wstring guides = L"none";
  float guide_ratio = 1.f;  // the guides' size against the color's (a game renders below its output)
  // 1.12.1: a static "HUD" box over the panning image (--hud), its vectors zeroed or not (--hud-mv 0|1).
  bool hud = false, hud_mv = true;
  // 1.12.1: DLSSNR.ControlMask (left half 1, right half 0) with DLSSNR.MaskBlendMode N (--control-mask N; -1 off),
  // and --mask-value V for the left half (0..1).
  int control_mask = -1;
  float mask_value = 1.f;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--snippet") snippet_path = next();
    else if (a == L"--image") image_path = next();
    else if (a == L"--out") out_path = next();
    else if (a == L"--allow-unsigned") allow_unsigned = true;
    else if (a == L"--init-params") init_params = true;
    else if (a == L"--frames") frames = _wtoi(next().c_str());
    else if (a == L"--preset") preset = _wtoi(next().c_str());
    else if (a == L"--style") style = _wtoi(next().c_str());
    else if (a == L"--automask") automask = _wtoi(next().c_str());
    else if (a == L"--intensity") intensity = float(_wtof(next().c_str()));
    else if (a == L"--tone") tone = float(_wtof(next().c_str()));
    else if (a == L"--structure") structure = float(_wtof(next().c_str()));
    else if (a == L"--skin") skin = float(_wtof(next().c_str()));
    else if (a == L"--app-id") app_id = _wcstoui64(next().c_str(), nullptr, 0);
    else if (a == L"--diff") diff_path = next();
    else if (a == L"--own-params") own_params = true;
    else if (a == L"--keys") keys = true;
    else if (a == L"--size") { const std::wstring v = next(); swscanf_s(v.c_str(), L"%ux%u", &size_w, &size_h); }
    else if (a == L"--scale") scale = float(_wtof(next().c_str()));
    else if (a == L"--temporal") temporal = _wtoi(next().c_str());
    else if (a == L"--pan") pan = _wtoi(next().c_str());
    else if (a == L"--guides") guides = next();
    else if (a == L"--guide-ratio") guide_ratio = float(_wtof(next().c_str()));
    else if (a == L"--hud") hud = true;
    else if (a == L"--control-mask") control_mask = _wtoi(next().c_str());
    else if (a == L"--mask-value") mask_value = float(_wtof(next().c_str()));
    else if (a == L"--hud-mv") hud_mv = _wtoi(next().c_str()) != 0;
  }
  if (snippet_path.empty() || image_path.empty() || out_path.empty()) {
    std::printf("usage: nr_probe --snippet <nvngx_dlssnr.dll> --image <in.png> --out <out.png> [options]\n");
    return 2;
  }

  // 1. The runtime file: whose it is.
  std::wstring signer;
  const bool nvidia = SignedByNvidia(snippet_path, signer);
  DWORD handle = 0, size = GetFileVersionInfoSizeW(snippet_path.c_str(), &handle);
  std::vector<uint8_t> version(size);
  VS_FIXEDFILEINFO* info = nullptr;
  UINT info_size = 0;
  if (size && GetFileVersionInfoW(snippet_path.c_str(), 0, size, version.data()))
    VerQueryValueW(version.data(), L"\\", reinterpret_cast<void**>(&info), &info_size);
  std::printf("snippet: %ls\n  version %u.%u.%u.%u, signer %ls\n", snippet_path.c_str(),
              info ? HIWORD(info->dwFileVersionMS) : 0, info ? LOWORD(info->dwFileVersionMS) : 0,
              info ? HIWORD(info->dwFileVersionLS) : 0, info ? LOWORD(info->dwFileVersionLS) : 0, signer.c_str());
  if (!nvidia && !allow_unsigned) {
    std::printf("nr_probe: REFUSED the file is not signed by NVIDIA; --allow-unsigned runs it anyway\n");
    return 2;
  }

  // 2. The device.
  Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
  const Image input = LoadImage(image_path, size_w, size_h);
  std::printf("image: %ux%u\n", input.width, input.height);
  Gpu gpu;
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "adapter");
  DXGI_ADAPTER_DESC1 adesc{};
  adapter->GetDesc1(&adesc);
  std::printf("adapter: %ls\n", adesc.Description);
  Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)), "device");
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  Check(gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.queue)), "queue");
  Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)), "allocator");
  Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&gpu.list)),
        "list");
  Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");

  // 3. NGX: the driver's core for the parameter block, the snippet (through the shim) for the feature.
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const HMODULE core = own_params ? nullptr : LoadLibraryW((CoreFolder() + L"\\_nvngx.dll").c_str());
  if (!core && !own_params) Fail("no NGX core");
  using InitProjectFn = int(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
  using AllocateFn = int(__cdecl*)(NVSDK_NGX_Parameter**);
  const auto init_project =
      core ? reinterpret_cast<InitProjectFn>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init_ProjectID")) : nullptr;
  const auto allocate =
      core ? reinterpret_cast<AllocateFn>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters")) : nullptr;
  if (!own_params && (!init_project || !allocate)) Fail("NGX core exports");
  int r = 1;
  if (!own_params) {
    r = init_project(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.12", temp, gpu.device.Get(), NVSDK_NGX_Version_API,
                     nullptr);
    std::printf("core Init_ProjectID: 0x%08x\n", unsigned(r));
  }
  NVSDK_NGX_Parameter* params = nullptr;
  mfgunlock::nr::Parameters own;
  if (own_params) {
    params = &own;  // MFG Unlock's own block: the driver's NGX core is not asked for one
    std::printf("parameters: MFG Unlock's own block\n");
  } else {
    r = allocate(&params);
    std::printf("core AllocateParameters: 0x%08x\n", unsigned(r));
    if (!NgxOk(r) || !params) Fail("parameters", unsigned(r));
  }

  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring shim_path = exe;
  shim_path = shim_path.substr(0, shim_path.find_last_of(L'\\')) + L"\\mfgunlock_nvngx.dll";
  const HMODULE shim = LoadLibraryW(shim_path.c_str());
  const HMODULE nr = LoadLibraryExW(snippet_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!shim) Fail("mfgunlock_nvngx.dll");
  if (!nr) Fail("load snippet", GetLastError());
  using ShimInit = int(__cdecl*)(void*, unsigned long long, const wchar_t*, void*, int, const void*);
  using ShimCreate = int(__cdecl*)(void*, void*, int, void*, void**);
  using ShimEvaluate = int(__cdecl*)(void*, void*, const void*, const void*, void*);
  using ShimRelease = int(__cdecl*)(void*, void*);
  const auto shim_init = reinterpret_cast<ShimInit>(GetProcAddress(shim, "MfgNr_Init"));
  const auto shim_create = reinterpret_cast<ShimCreate>(GetProcAddress(shim, "MfgNr_Create"));
  const auto shim_evaluate = reinterpret_cast<ShimEvaluate>(GetProcAddress(shim, "MfgNr_Evaluate"));
  const auto shim_release = reinterpret_cast<ShimRelease>(GetProcAddress(shim, "MfgNr_Release"));
  void* nr_init = GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
  void* nr_create = GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
  void* nr_evaluate = GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
  void* nr_release = GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");
  if (!shim_init || !shim_create || !shim_evaluate || !shim_release || !nr_init || !nr_create || !nr_evaluate ||
      !nr_release)
    Fail("snippet or shim exports");
  r = shim_init(nr_init, app_id, temp, gpu.device.Get(), kSnippetSdk, init_params ? params : nullptr);
  std::printf("snippet Init_Ext (app id 0x%llx, sdk 0x%x): 0x%08x\n", app_id, kSnippetSdk, unsigned(r));
  if (!NgxOk(r)) Fail("snippet Init_Ext", unsigned(r));

  // 4. The feature, with its look (read only at creation).
  const UINT w = input.width, h = input.height;
  params->Set("DLSSNR.Enabled", 1);
  params->Set("DLSSNR.Width", w);
  params->Set("DLSSNR.Height", h);
  params->Set("CreationNodeMask", 1u);
  params->Set("VisibilityNodeMask", 1u);
  params->Set("DLSSNR.Hint.Render.Preset", preset);
  params->Set("DLSSNR.Style", style);
  params->Set("DLSSNR.Intensity", intensity);
  params->Set("DLSSNR.LocalToneStrength", tone);
  params->Set("DLSSNR.LocalStructureStrength", structure);
  params->Set("DLSSNR.SkinStructureStrength", skin);
  params->Set("DLSSNR.UseAutoMask", automask);
  params->Set("DLSSNR.UICorrection", 0u);
  params->Set("DLSSNR.ScalingRatio", scale);
  params->Set("DLSSNR.DepthInverted", 0);
  if (control_mask >= 0) params->Set("DLSSNR.MaskBlendMode", unsigned(control_mask));
  // Round trip: the parameter block's methods are the ones this header declares.
  unsigned check_width = 0;
  float check_intensity = -1.f;
  params->Get("DLSSNR.Width", &check_width);
  params->Get("DLSSNR.Intensity", &check_intensity);
  std::printf("parameters read back: width %u, intensity %.2f\n", check_width, check_intensity);
  if (check_width != w || check_intensity != intensity) Fail("parameter round trip");

  NVSDK_NGX_Handle* feature = nullptr;
  r = shim_create(nr_create, gpu.list.Get(), kFeatureNr, params, reinterpret_cast<void**>(&feature));
  std::printf("CreateFeature(18): 0x%08x\n", unsigned(r));
  if (!NgxOk(r)) Fail("CreateFeature", unsigned(r));
  gpu.Submit();

  // 5. The image in, NR, the image out.
  auto color = Texture(gpu.device.Get(), w, h, D3D12_RESOURCE_STATE_COPY_DEST);
  auto output = Texture(gpu.device.Get(), w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  const D3D12_RESOURCE_DESC cdesc = color->GetDesc();
  gpu.device->GetCopyableFootprints(&cdesc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto upload = Buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = Buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  {
    uint8_t* mapped = nullptr;
    Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
    for (UINT y = 0; y < h; ++y) {
      auto* row = reinterpret_cast<HALF*>(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
      for (UINT x = 0; x < w * 4; ++x) row[x] = XMConvertFloatToHalf(input.rgba[size_t(y) * w * 4 + x] / 255.f);
    }
    upload->Unmap(0, nullptr);
  }
  D3D12_TEXTURE_COPY_LOCATION tex{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION buf{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  buf.PlacedFootprint = footprint;
  gpu.list->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
  gpu.Barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  gpu.Submit();
  // The control mask: R8, the left half `mask_value`, the right half 0.
  ComPtr<ID3D12Resource> mask, mask_upload;
  if (control_mask >= 0) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&mask)),
          "mask");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mfp{};
    UINT64 mbytes = 0;
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &mfp, nullptr, nullptr, &mbytes);
    mask_upload = Buffer(gpu.device.Get(), mbytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* m = nullptr;
    Check(mask_upload->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map mask");
    const uint8_t on = uint8_t(std::lround(mask_value * 255.f));
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x) m[mfp.Offset + size_t(y) * mfp.Footprint.RowPitch + x] = x < w / 2 ? on : 0;
    mask_upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION mdst{mask.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION msrc{mask_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    msrc.PlacedFootprint = mfp;
    gpu.list->CopyTextureRegion(&mdst, 0, 0, 0, &msrc, nullptr);
    gpu.Barrier(mask.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Submit();
  }

  // GPU time of each evaluate, from timestamps around it.
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
  Check(gpu.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "query heap");
  auto stamps = Buffer(gpu.device.Get(), 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT64 ticks_per_second = 1;
  gpu.queue->GetTimestampFrequency(&ticks_per_second);
  double gpu_total = 0.0, gpu_min = 1e9;
  double total_ms = 0.0;
  for (int f = 0; f < frames; ++f) {
    params->Set("DLSSNR.Color", color.Get());
    params->Set("DLSSNR.Output", output.Get());
    params->Set("DLSSNR.Width", w);
    params->Set("DLSSNR.Height", h);
    params->Set("DLSSNR.Enabled", 1);
    params->Set("DLSSNR.Reset", f == 0 ? 1 : 0);
    for (const char* name : {"DLSSNR.Color", "DLSSNR.Output"}) {
      char key[96];
      std::snprintf(key, sizeof(key), "%sSubrectBaseX", name);
      params->Set(key, 0u);
      std::snprintf(key, sizeof(key), "%sSubrectBaseY", name);
      params->Set(key, 0u);
      std::snprintf(key, sizeof(key), "%sSubrectWidth", name);
      params->Set(key, w);
      std::snprintf(key, sizeof(key), "%sSubrectHeight", name);
      params->Set(key, h);
    }
    params->Set("DLSSNR.MVecScaleX", 1.f);
    params->Set("DLSSNR.MVecScaleY", 1.f);
    if (control_mask >= 0) {
      params->Set("DLSSNR.ControlMask", mask.Get());
      params->Set("DLSSNR.MaskBlendMode", unsigned(control_mask));
      params->Set("DLSSNR.ControlMaskSubrectBaseX", 0u);
      params->Set("DLSSNR.ControlMaskSubrectBaseY", 0u);
      params->Set("DLSSNR.ControlMaskSubrectWidth", w);
      params->Set("DLSSNR.ControlMaskSubrectHeight", h);
    }
    const auto start = std::chrono::steady_clock::now();
    gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    r = shim_evaluate(nr_evaluate, gpu.list.Get(), feature, params, nullptr);
    if (!NgxOk(r)) Fail("EvaluateFeature", unsigned(r));
    gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
    gpu.Submit();
    {
      UINT64* t = nullptr;
      D3D12_RANGE range{0, 16};
      if (SUCCEEDED(stamps->Map(0, &range, reinterpret_cast<void**>(&t)))) {
        const double gpu_ms = double(t[1] - t[0]) * 1000.0 / double(ticks_per_second);
        stamps->Unmap(0, nullptr);
        if (f > 0) {
          gpu_total += gpu_ms;
          gpu_min = (std::min)(gpu_min, gpu_ms);
        }
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (f > 0) total_ms += ms;  // the first frame builds the network
    if (f == 0) std::printf("EvaluateFeature: 0x%08x, first frame %.1f ms\n", unsigned(r), ms);
  }
  if (frames > 1)
    std::printf("average after the first: %.2f ms per frame (CPU wall), GPU %.2f ms average, %.2f ms best\n",
                total_ms / (frames - 1), gpu_total / (frames - 1), gpu_min);
  if (keys && own_params)
    for (const auto& [name, present] : own.Asked()) std::printf("  asked %-48s %s\n", name.c_str(), present ? "set" : "-");

  gpu.Barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = footprint;
  gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  gpu.Submit();

  Image result{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
  double diff = 0.0, diff_left = 0.0, diff_right = 0.0;
  {
    uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, SIZE_T(total)};
    Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map readback");
    for (UINT y = 0; y < h; ++y) {
      const auto* row = reinterpret_cast<const HALF*>(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
      for (UINT x = 0; x < w * 4; ++x) {
        float v = XMConvertHalfToFloat(row[x]);
        v = v != v ? 0.f : (v < 0.f ? 0.f : (v > 1.f ? 1.f : v));
        const uint8_t b = uint8_t(std::lround(v * 255.f));
        const size_t i = size_t(y) * w * 4 + x;
        result.rgba[i] = (x % 4 == 3) ? 255 : b;
        if (x % 4 != 3) {
          const double d = std::fabs(double(b) - double(input.rgba[i]));
          diff += d;
          (x / 4 < w / 2 ? diff_left : diff_right) += d;
        }
      }
    }
    readback->Unmap(0, nullptr);
  }
  diff /= double(w) * h * 3;
  SaveImage(out_path, result);
  // Where NR changed the image: the difference, eight times brighter.
  if (!diff_path.empty()) {
    Image amplified{w, h, std::vector<uint8_t>(result.rgba.size())};
    for (size_t i = 0; i < result.rgba.size(); ++i)
      amplified.rgba[i] = (i % 4 == 3) ? 255 : uint8_t((std::min)(255, 8 * std::abs(int(result.rgba[i]) - int(input.rgba[i]))));
    SaveImage(diff_path, amplified);
  }
  std::printf("output: %ls\nmean absolute change: %.3f of 255\n", out_path.c_str(), diff);
  std::printf("mean absolute change: left half %.3f, right half %.3f of 255\n", diff_left / (double(w / 2) * h * 3),
              diff_right / (double(w - w / 2) * h * 3));
  shim_release(nr_release, feature);
  if (temporal > 0) {
    const UINT margin = 64, cw = w - margin;
    params->Set("DLSSNR.Width", cw);
    NVSDK_NGX_Handle* moving = nullptr;
    r = shim_create(nr_create, gpu.list.Get(), kFeatureNr, params, reinterpret_cast<void**>(&moving));
    if (!NgxOk(r)) Fail("CreateFeature (temporal)", unsigned(r));
    gpu.Submit();
    const UINT gw = UINT(cw * guide_ratio + 0.5f), gh = UINT(h * guide_ratio + 0.5f);
    const auto texture = [&](UINT tw, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, UINT th = 0) {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC d{};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      d.Width = tw;
      d.Height = th ? th : h;
      d.DepthOrArraySize = 1;
      d.MipLevels = 1;
      d.Format = format;
      d.SampleDesc.Count = 1;
      d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ComPtr<ID3D12Resource> t;
      Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&t)),
            "temporal texture");
      return t;
    };
    // A texture filled with one value per pixel, through an upload buffer.
    std::vector<ComPtr<ID3D12Resource>> keep;
    const auto filled = [&](DXGI_FORMAT format, UINT texel_bytes, const void* texel) {
      auto t = texture(gw, format, D3D12_RESOURCE_STATE_COPY_DEST, gh);
      const D3D12_RESOURCE_DESC d = t->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT64 bytes = 0;
      gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
      auto up = Buffer(gpu.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      uint8_t* m = nullptr;
      Check(up->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map fill");
      for (UINT y = 0; y < gh; ++y)
        for (UINT x = 0; x < gw; ++x) std::memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch + x * texel_bytes, texel, texel_bytes);
      up->Unmap(0, nullptr);
      D3D12_TEXTURE_COPY_LOCATION dst{t.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.PlacedFootprint = fp;
      gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      gpu.Barrier(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      keep.push_back(up);
      return t;
    };
    // Content moves left by `pan`: the pixel at x came from x + pan, so the vector to the previous
    // position is +pan pixels (DLSS's convention); the other modes test the alternatives.
    float mv = 0.f, mv_scale = 1.f;
    const bool with_guides = guides != L"none";
    if (guides == L"px") mv = float(pan);
    else if (guides == L"neg") mv = -float(pan);
    else if (guides == L"norm") mv = float(pan) / float(cw);
    else if (guides == L"normneg") mv = -float(pan) / float(cw);
    else if (guides == L"normscaled") { mv = float(pan) / float(cw); mv_scale = float(cw); }
    else if (guides == L"gpx") mv = float(pan) * guide_ratio;  // pixels of the guides' own size
    const HALF mv_texel[2] = {XMConvertFloatToHalf(mv), XMConvertFloatToHalf(0.f)};
    const float depth_texel = 0.5f;
    auto mvec = filled(DXGI_FORMAT_R16G16_FLOAT, 4, mv_texel);
    auto depth = filled(DXGI_FORMAT_R32_FLOAT, 4, &depth_texel);
    // The HUD box: a fixed part of the source image, drawn at the same place every frame (a HUD does not
    // move with the camera). Its vectors are zero with --hud-mv 0, the camera's otherwise.
    const UINT hud_x = cw / 4, hud_y = h * 3 / 4, hud_w = cw / 2, hud_h = h / 10;
    if (hud && !hud_mv && with_guides) {
      const HALF zero[2] = {XMConvertFloatToHalf(0.f), XMConvertFloatToHalf(0.f)};
      auto zeros = filled(DXGI_FORMAT_R16G16_FLOAT, 4, zero);
      gpu.Barrier(zeros.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.Barrier(mvec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      const UINT gx = UINT(hud_x * guide_ratio), gy = UINT(hud_y * guide_ratio);
      D3D12_BOX zb{0, 0, 0, UINT(hud_w * guide_ratio), UINT(hud_h * guide_ratio), 1};
      D3D12_TEXTURE_COPY_LOCATION zd{mvec.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION zs{zeros.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      gpu.list->CopyTextureRegion(&zd, gx, gy, 0, &zs, &zb);
      gpu.Barrier(mvec.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      keep.push_back(zeros);
    }
    auto in2 = texture(cw, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto out2 = texture(cw, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC od = out2->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp{};
    UINT64 obytes = 0;
    gpu.device->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, &obytes);
    auto back = Buffer(gpu.device.Get(), obytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.Barrier(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Submit();
    std::vector<float> prev, cur(size_t(cw) * h);
    double sum = 0.0, hud_sum = 0.0;
    int counted = 0;
    for (int f = 0; f < temporal; ++f) {
      const UINT x0 = pan > 0 ? UINT(pan * f) % (margin - UINT(pan)) : 0;
      if (pan > 0 && f > 0 && x0 < UINT(pan)) prev.clear();  // wrapped: not the same content
      D3D12_BOX box{x0, 0, 0, x0 + cw, h, 1};
      D3D12_TEXTURE_COPY_LOCATION dst{in2.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION src{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      if (hud) {  // the same source patch at the same place every frame
        D3D12_BOX hb{0, 0, 0, hud_w, hud_h, 1};
        gpu.list->CopyTextureRegion(&dst, hud_x, hud_y, 0, &src, &hb);
      }
      gpu.Barrier(in2.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      params->Set("DLSSNR.Color", in2.Get());
      params->Set("DLSSNR.Output", out2.Get());
      params->Set("DLSSNR.MVec", with_guides ? mvec.Get() : nullptr);
      params->Set("DLSSNR.Depth", with_guides ? depth.Get() : nullptr);
      params->Set("DLSSNR.Width", cw);
      params->Set("DLSSNR.Height", h);
      params->Set("DLSSNR.Reset", f == 0 ? 1 : 0);
      for (const char* name : {"DLSSNR.Color", "DLSSNR.Output", "DLSSNR.MVec", "DLSSNR.Depth"}) {
        const bool guide = name[7] == 'M' || name[7] == 'D';
        char key[96];
        std::snprintf(key, sizeof(key), "%sSubrectBaseX", name);
        params->Set(key, 0u);
        std::snprintf(key, sizeof(key), "%sSubrectBaseY", name);
        params->Set(key, 0u);
        std::snprintf(key, sizeof(key), "%sSubrectWidth", name);
        params->Set(key, guide ? gw : cw);
        std::snprintf(key, sizeof(key), "%sSubrectHeight", name);
        params->Set(key, guide ? gh : h);
      }
      params->Set("DLSSNR.MVecScaleX", mv_scale);
      params->Set("DLSSNR.MVecScaleY", mv_scale == 1.f ? 1.f : float(h));
      r = shim_evaluate(nr_evaluate, gpu.list.Get(), moving, params, nullptr);
      if (!NgxOk(r)) Fail("EvaluateFeature (temporal)", unsigned(r));
      gpu.Barrier(in2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.Barrier(out2.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION osrc{out2.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION odst{back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      odst.PlacedFootprint = ofp;
      gpu.list->CopyTextureRegion(&odst, 0, 0, 0, &osrc, nullptr);
      gpu.Barrier(out2.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.Submit();
      uint8_t* m = nullptr;
      D3D12_RANGE range{0, SIZE_T(obytes)};
      Check(back->Map(0, &range, reinterpret_cast<void**>(&m)), "map temporal");
      for (UINT y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const HALF*>(m + ofp.Offset + size_t(y) * ofp.Footprint.RowPitch);
        for (UINT x = 0; x < cw; ++x)
          cur[size_t(y) * cw + x] = 0.2126f * XMConvertHalfToFloat(row[x * 4]) + 0.7152f * XMConvertHalfToFloat(row[x * 4 + 1]) +
                                    0.0722f * XMConvertHalfToFloat(row[x * 4 + 2]);
      }
      back->Unmap(0, nullptr);
      // The HUD box: still, so compared at the same place.
      if (hud && !prev.empty() && f >= 3) {
        double d = 0.0;
        for (UINT y = hud_y; y < hud_y + hud_h; ++y)
          for (UINT x = hud_x; x < hud_x + hud_w; ++x) d += std::fabs(double(cur[size_t(y) * cw + x]) - prev[size_t(y) * cw + x]);
        hud_sum += d / double(hud_w * hud_h) * 255.0;
      }
      // The same content: this frame's x against the previous frame's x + pan.
      if (!prev.empty() && f >= 3) {
        double d = 0.0;
        size_t n = 0;
        for (UINT y = 0; y < h; ++y)
          for (UINT x = 0; x + UINT(pan) < cw; ++x, ++n) d += std::fabs(double(cur[size_t(y) * cw + x]) - prev[size_t(y) * cw + x + pan]);
        sum += d / double(n) * 255.0;
        ++counted;
      }
      prev = cur;
    }
    std::printf("temporal: pan %d px/frame, guides %ls (mv %.4f, scale %.0f): change between frames %.3f of 255 (%d pairs)\n",
                pan, guides.c_str(), mv, mv_scale, counted ? sum / counted : -1.0, counted);
    if (hud)
      std::printf("temporal: HUD box (vectors %s): change between frames %.3f of 255\n", hud_mv ? "the camera's" : "zero",
                  counted ? hud_sum / counted : -1.0);
    shim_release(nr_release, moving);
  }
  return diff > 0.5 ? 0 : 3;
}
