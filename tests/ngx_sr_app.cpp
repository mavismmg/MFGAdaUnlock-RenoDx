// SPDX-License-Identifier: MIT
// 1.10.0 (plan "DLSS-G em jogos só com DLSS", step 0): a game that calls DLSS
// Super Resolution straight through NGX, without Streamline -- what RDR2, KCD2 and
// Metro Exodus EE do. It draws a moving pattern at half resolution, runs NVIDIA's
// real DLSS SR (nvngx_dlss.dll beside the exe) through the driver's _nvngx.dll and
// presents the upscaled image. ReShade and the addon load as in a game when
// ReShade sits beside it as d3d12.dll or dxgi.dll.
//
//   ngx_sr_app.exe [seconds=10] [--width 1280] [--height 720] [--windowed]
//
// Prints "ngx_sr_app: frames N evaluated N presented N" and exits 0 when DLSS SR
// ran on every frame; exit 77 when this machine has no NGX or DLSS SR.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#include <sl.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
using InitProjectFn = NVSDK_NGX_Result(NVSDK_CONV*)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*,
                                                    ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using ParamsFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                 const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

[[noreturn]] void Fail(const char* what, long code = 0, int exit_code = 1) {
  std::printf("ngx_sr_app: FAIL %s (0x%08lx)\n", what, static_cast<unsigned long>(code));
  std::fflush(stdout);
  ExitProcess(exit_code);
}
bool g_verbose = false;
void Step(const char* what) {
  if (!g_verbose) return;
  std::printf("ngx_sr_app: %s\n", what);
  std::fflush(stdout);
}
void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  if (g_verbose) std::fputs(message, stdout);
}
// DLSS-G turns itself off while the window is not focused ("window not
// focused" in sl.log), and Windows refuses the focus to a background process
// unless its input is attached to the foreground thread.
void TakeFocus(HWND window) {
  const HWND foreground = GetForegroundWindow();
  if (foreground == window) return;
  const DWORD other = GetWindowThreadProcessId(foreground, nullptr), self = GetCurrentThreadId();
  if (other && other != self) AttachThreadInput(self, other, TRUE);
  keybd_event(VK_MENU, 0, 0, 0);
  ShowWindow(window, SW_MINIMIZE);
  ShowWindow(window, SW_RESTORE);
  BringWindowToTop(window);
  SetForegroundWindow(window);
  SetFocus(window);
  keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
  if (other && other != self) AttachThreadInput(self, other, FALSE);
}
LONG WINAPI Crash(EXCEPTION_POINTERS* e) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(e->ExceptionRecord->ExceptionAddress), &m);
  wchar_t name[MAX_PATH]{};
  if (m) GetModuleFileNameW(m, name, MAX_PATH);
  std::printf("ngx_sr_app: CRASH 0x%08lx in %ls+0x%llx\n", e->ExceptionRecord->ExceptionCode,
              m ? std::filesystem::path(name).filename().c_str() : L"?",
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress) -
                                              reinterpret_cast<uintptr_t>(m)));
  std::fflush(stdout);
  return EXCEPTION_CONTINUE_SEARCH;
}
void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) Fail(what, hr);
}

// Where a game's NGX static library finds the driver's NGX core.
std::filesystem::path FindNgxCore() {
  wchar_t path[MAX_PATH]{};
  DWORD size = sizeof(path);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                   nullptr, path, &size) == ERROR_SUCCESS) {
    const auto candidate = std::filesystem::path(path) / L"_nvngx.dll";
    if (std::filesystem::exists(candidate)) return candidate;
  }
  std::filesystem::path newest;
  std::filesystem::file_time_type newest_time{};
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(L"C:\\Windows\\System32\\DriverStore\\FileRepository", ec)) {
    if (entry.path().filename().wstring().rfind(L"nv_dispi.inf_amd64_", 0) != 0) continue;
    const auto candidate = entry.path() / L"_nvngx.dll";
    if (!std::filesystem::exists(candidate)) continue;
    const auto time = std::filesystem::last_write_time(candidate, ec);
    if (newest.empty() || time > newest_time) newest = candidate, newest_time = time;
  }
  return newest;
}

constexpr char kShader[] = R"(
RWTexture2D<float4> color : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<float2> motion : register(u2);
cbuffer Frame : register(b0) { float shift; float speed; uint width; uint height; };
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  // Vertical bars and a band that move right by `speed` render pixels a frame.
  float x = id.x - shift;
  float bars = frac(x / 64.0) < 0.5 ? 1.0 : 0.25;
  float band = abs(float(id.y) - height * 0.5) < height * 0.1 ? 0.6 : 1.0;
  color[id] = float4(bars * band, 0.4 + 0.4 * sin(x / 40.0), float(id.y) / height, 1.0);
  depth[id] = 0.5;
  motion[id] = float2(-speed, 0.0);
}
)";

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
                               D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> resource;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
        "texture");
  return resource;
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}
}  // namespace

int main(int argc, char** argv) {
  double seconds = 10;
  UINT width = 1280, height = 720;
  bool windowed = false;
  bool allocate = false;
  bool debug = false;
  int sl_mode = 0;
  UINT chain_flags = 0;  // --waitable 0x40, --tearing 0x800 (games' usual flags)
  bool hdr10 = false;  // 1: the app starts Streamline (manual hooking) before NGX; 2: after DLSS SR exists  // a parameter block of its own, as many games do
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--width" && i + 1 < argc) width = static_cast<UINT>(std::stoul(argv[++i]));
    else if (a == "--height" && i + 1 < argc) height = static_cast<UINT>(std::stoul(argv[++i]));
    else if (a == "--windowed") windowed = true;
    else if (a == "--verbose") g_verbose = true;
    else if (a == "--allocate-params") allocate = true;
    else if (a == "--d3d-debug") debug = true;
    else if (a == "--sl") sl_mode = 1;
    else if (a == "--waitable") chain_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    else if (a == "--tearing") chain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    else if (a == "--hdr10") hdr10 = true;
    else if (a == "--sl-late") sl_mode = 2;
    else seconds = std::stod(a);
  }
  SetUnhandledExceptionFilter(Crash);
  const UINT render_width = width / 2, render_height = height / 2;
  (void)windowed;

  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"NgxSrApp";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowW(wc.lpszClassName, L"NGX DLSS SR app", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
  TakeFocus(window);

  if (debug) {
    ComPtr<ID3D12Debug> layer;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) layer->EnableDebugLayer();
  }
  ComPtr<IDXGIFactory4> factory;
  Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIFactory6> factory6;
  if (SUCCEEDED(factory.As(&factory6)))
    factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  ComPtr<ID3D12Device> device;
  Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
  static ComPtr<ID3D12InfoQueue> info_queue;
  if (debug) device.As(&info_queue);
  static const auto dump = [] {
    if (!info_queue) return;
    for (UINT64 i = 0, n = info_queue->GetNumStoredMessages(); i < n && i < 20; ++i) {
      SIZE_T size = 0;
      info_queue->GetMessage(i, nullptr, &size);
      std::vector<char> bytes(size);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      if (SUCCEEDED(info_queue->GetMessage(i, m, &size)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        std::printf("ngx_sr_app: D3D12 %s\n", m->pDescription);
    }
  };

  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = width;
  sc.Height = height;
  sc.Format = hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  sc.Flags = chain_flags;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = 3;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapchain1;
  Check(factory->CreateSwapChainForHwnd(queue.Get(), window, &sc, nullptr, nullptr, &swapchain1), "swap chain");
  ComPtr<IDXGISwapChain3> swapchain;
  Check(swapchain1.As(&swapchain), "swap chain 3");
  HANDLE waitable = nullptr;
  if (chain_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
    ComPtr<IDXGISwapChain2> chain2;
    if (SUCCEEDED(swapchain.As(&chain2))) {
      chain2->SetMaximumFrameLatency(2);
      waitable = chain2->GetFrameLatencyWaitableObject();
    }
  }

  Step("device and swap chain");
  const auto start_streamline = [&] {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto dir = std::filesystem::path(self).parent_path() / L"MFGUnlock-Streamline";
    HMODULE sl = LoadLibraryW((dir / L"sl.interposer.dll").c_str());
    if (!sl) Fail("sl.interposer.dll", GetLastError());
    auto sl_init = reinterpret_cast<PFun_slInit*>(GetProcAddress(sl, "slInit"));
    auto sl_device = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(sl, "slSetD3DDevice"));
    const std::wstring d = dir.wstring();
    const wchar_t* p[] = {d.c_str()};
    const sl::Feature f[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.pathsToPlugins = p;
    pref.numPathsToPlugins = 1;
    pref.featuresToLoad = f;
    pref.numFeaturesToLoad = 3;
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1";
    pref.projectId = "3c5b0a51-6f2e-4b8d-9d17-4f0b6e2a8c19";
    if (sl_init(pref, sl::kSDKVersion) != sl::Result::eOk) Fail("slInit");
    if (sl_device(device.Get()) != sl::Result::eOk) Fail("slSetD3DDevice");
    Step("Streamline started");
  };
  if (sl_mode == 1) start_streamline();
  // NGX, the way a game's static library reaches it.
  const auto core = FindNgxCore();
  if (core.empty()) Fail("no _nvngx.dll (not an NVIDIA driver)", 0, 77);
  HMODULE ngx = LoadLibraryW(core.c_str());
  if (!ngx) Fail("LoadLibrary _nvngx.dll", GetLastError(), 77);
  const auto init = reinterpret_cast<InitProjectFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));
  const auto capabilities = reinterpret_cast<ParamsFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
  const auto create = reinterpret_cast<CreateFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
  const auto evaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
  const auto release = reinterpret_cast<ReleaseFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
  const auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown1"));
  if (!init || !capabilities || !create || !evaluate || !release) Fail("NGX exports", 0, 77);

  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::wstring folder = std::filesystem::path(exe).parent_path().wstring();
  const wchar_t* paths[] = {folder.c_str()};
  NVSDK_NGX_FeatureCommonInfo info{};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  if (g_verbose) {
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
  }
  Step("NGX exports");
  NVSDK_NGX_Result result = init("6f1b7a3e-1d2c-4e5f-9a8b-0c1d2e3f4a5b", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                 folder.c_str(), device.Get(), NVSDK_NGX_Version_API, &info);
  if (NVSDK_NGX_FAILED(result)) Fail("NGX init", result, 77);
  Step("NGX init");
  NVSDK_NGX_Parameter* params = nullptr;
  result = capabilities(&params);
  if (NVSDK_NGX_FAILED(result) || params == nullptr) Fail("NGX capability parameters", result, 77);
  int available = 0;
  params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (!available) Fail("DLSS SR not available (nvngx_dlss.dll beside the exe?)", 0, 77);
  if (allocate) {
    const auto allocate_params = reinterpret_cast<ParamsFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
    params = nullptr;
    if (!allocate_params || NVSDK_NGX_FAILED(allocate_params(&params)) || params == nullptr)
      Fail("NGX AllocateParameters", 0, 77);
  }

  Step("capabilities");
  // Resources: render-resolution inputs, display-resolution output.
  const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto color = Texture(device.Get(), render_width, render_height, DXGI_FORMAT_R8G8B8A8_UNORM, uav,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto depth = Texture(device.Get(), render_width, render_height, DXGI_FORMAT_R32_FLOAT, uav,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto motion = Texture(device.Get(), render_width, render_height, DXGI_FORMAT_R16G16_FLOAT, uav,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto output = Texture(device.Get(), width, height, sc.Format, uav,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> heap;
  Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "descriptor heap");
  const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ID3D12Resource* targets[] = {color.Get(), depth.Get(), motion.Get()};
  for (UINT i = 0; i < 3; ++i) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(i) * step;
    device->CreateUnorderedAccessView(targets[i], nullptr, nullptr, handle);
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER root[2]{};
  root[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root[0].DescriptorTable = {1, &range};
  root[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root[1].Constants = {0, 0, 4};
  D3D12_ROOT_SIGNATURE_DESC root_desc{2, root};
  ComPtr<ID3DBlob> blob, errors;
  Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "root signature");
  ComPtr<ID3D12RootSignature> signature;
  Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)),
        "root signature object");
  ComPtr<ID3DBlob> code;
  Check(D3DCompile(kShader, sizeof(kShader) - 1, "scene", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors),
        "shader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = signature.Get();
  pso_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  Check(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "pipeline");

  ComPtr<ID3D12CommandAllocator> allocator;
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
        "command list");
  ComPtr<ID3D12Fence> fence;
  Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  UINT64 fence_value = 0;
  const auto wait_gpu = [&] {
    Check(queue->Signal(fence.Get(), ++fence_value), "signal");
    if (fence->GetCompletedValue() < fence_value) {
      fence->SetEventOnCompletion(fence_value, event);
      WaitForSingleObject(event, 5000);
    }
  };

  Step("pipeline");
  // DLSS SR creation, on the first command list, as games do.
  params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
  params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
  params->Set(NVSDK_NGX_Parameter_Width, render_width);
  params->Set(NVSDK_NGX_Parameter_Height, render_height);
  params->Set(NVSDK_NGX_Parameter_OutWidth, width);
  params->Set(NVSDK_NGX_Parameter_OutHeight, height);
  params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxPerf));
  params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
  NVSDK_NGX_Handle* dlss = nullptr;
  result = create(list.Get(), NVSDK_NGX_Feature_SuperSampling, params, &dlss);
  if (NVSDK_NGX_FAILED(result) || dlss == nullptr) Fail("DLSS SR CreateFeature", result, 77);
  if (const HRESULT closed = list->Close(); FAILED(closed)) { dump(); Fail("close create list", closed); }
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  wait_gpu();

  Step("DLSS SR created");
  if (sl_mode == 2) start_streamline();
  uint64_t frames = 0, evaluated = 0, presented = 0;
  const float speed = 4.f;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (frames % 120 == 0) TakeFocus(window);
    if (waitable) WaitForSingleObjectEx(waitable, 1000, TRUE);
    Check(allocator->Reset(), "allocator reset");
    Check(list->Reset(allocator.Get(), pso.Get()), "list reset");
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(signature.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    struct { float shift, speed; UINT w, h; } constants{speed * static_cast<float>(frames), speed, render_width, render_height};
    list->SetComputeRoot32BitConstants(1, 4, &constants, 0);
    list->Dispatch((render_width + 7) / 8, (render_height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER to_read[] = {
        Transition(color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(3, to_read);

    params->Set(NVSDK_NGX_Parameter_Color, color.Get());
    params->Set(NVSDK_NGX_Parameter_Output, output.Get());
    params->Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    params->Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    params->Set(NVSDK_NGX_Parameter_Reset, frames == 0 ? 1 : 0);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render_width);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render_height);
    result = evaluate(list.Get(), dlss, params, nullptr);
    if (NVSDK_NGX_SUCCEED(result)) ++evaluated;

    D3D12_RESOURCE_BARRIER back_to_write[] = {
        Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(3, back_to_write);
    ComPtr<ID3D12Resource> backbuffer;
    Check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "back buffer");
    D3D12_RESOURCE_BARRIER to_copy[] = {
        Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, to_copy);
    list->CopyResource(backbuffer.Get(), output.Get());
    D3D12_RESOURCE_BARRIER to_present[] = {
        Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)};
    list->ResourceBarrier(2, to_present);
    Check(list->Close(), "close frame list");
    queue->ExecuteCommandLists(1, lists);
    const UINT present_flags = (chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT shown = swapchain->Present(0, present_flags);
    if (SUCCEEDED(shown)) ++presented;
    else if (presented == 0 && frames < 3) std::printf("ngx_sr_app: Present failed 0x%08lx (removed 0x%08lx)\n",
                                                       static_cast<unsigned long>(shown),
                                                       static_cast<unsigned long>(device->GetDeviceRemovedReason()));
    wait_gpu();
    ++frames;
  }
  std::printf("ngx_sr_app: focused at the end %d\n", GetForegroundWindow() == window ? 1 : 0);
  std::printf("ngx_sr_app: frames %llu evaluated %llu presented %llu (%ux%u from %ux%u)\n",
              static_cast<unsigned long long>(frames), static_cast<unsigned long long>(evaluated),
              static_cast<unsigned long long>(presented), width, height, render_width, render_height);
  std::fflush(stdout);
  // The driver's Shutdown1 takes another signature than the SDK's; a game that
  // exits without it is common, and the process ends here anyway.
  release(dlss);
  (void)shutdown;
  return frames > 0 && evaluated == frames && presented == frames ? 0 : 1;
}
