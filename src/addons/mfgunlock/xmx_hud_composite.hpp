#pragma once
// 1.10.0: XMX's HUD without XeFG's UI composition. XeFG's composition
// (ExecuteParams::uiComposition) left the generated frames uninterpolated when
// called below its swap chain (KCD2 with the HUD fix's HUD-less color, DOOM:
// The Dark Ages with the game's own HUD-less color and UI texture, every UI
// mode). Here XeFG interpolates the HUD-less color alone, and this pass puts the
// newest real frame's HUD over each generated frame: a pixel is HUD where the
// back buffer differs from the HUD-less color (with one pixel of dilation for
// glyph edges). Recorded on the evaluation's list, with its own root signature,
// pipeline and descriptor heap; NGX callers rebind their state after Evaluate.
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace mfgunlock::xmx::hudcomposite {
using Microsoft::WRL::ComPtr;

inline constexpr char kShader[] = R"(
Texture2D<float4> Real : register(t0);
Texture2D<float4> Hudless : register(t1);
RWTexture2D<float4> Output : register(u0);
cbuffer Params : register(b0) { uint2 size; float tolerance; uint debug; };
bool Hud(int2 p) {
  const float3 d = abs(Real[p].rgb - Hudless[p].rgb);
  return max(d.r, max(d.g, d.b)) > tolerance;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  bool hud = false;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      const int2 q = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1);
      hud = hud || Hud(q);
    }
  if (debug != 0) { if (hud) Output[id.xy] = float4(1, 0, 1, 1); }
  else if (hud) Output[id.xy] = Real[id.xy];
}
)";

inline constexpr uint32_t kSlots = 64;  // descriptor sets in flight (3 per slot)
inline constexpr float kTolerance = 3.0f / 255.0f;

struct Pass {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pipeline;
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT increment{};
  uint32_t next{};
  bool failed{};
  std::string fault;
};
inline Pass& Get() {
  static auto* pass = new Pass;
  return *pass;
}

inline DXGI_FORMAT UavFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
  }
}

inline bool Fail(Pass& pass, const std::string& why) {
  pass.failed = true;
  pass.fault = why;
  return false;
}

inline bool Initialize(Pass& pass, ID3D12Device* device) {
  if (pass.pipeline) return true;
  if (pass.failed) return false;
  pass.device = device;
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  if (compile == nullptr) return Fail(pass, "d3dcompiler_47.dll is missing");
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(compile(kShader, sizeof(kShader) - 1, "xmx_hud_composite", nullptr, nullptr, "main", "cs_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    return Fail(pass, "the shader did not compile");
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 4};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {2, ranges};
  D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob;
  // From d3d12.dll at run time: every target that builds addon.cpp links without d3d12.lib.
  auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (serialize == nullptr) return Fail(pass, "D3D12SerializeRootSignature");
  if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pass.root))))
    return Fail(pass, "root signature");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = pass.root.Get();
  pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pass.pipeline)))) return Fail(pass, "pipeline");
  D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 3,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pass.heap)))) return Fail(pass, "descriptor heap");
  pass.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

// `output` in UNORDERED_ACCESS, `real` and `hudless` in NON_PIXEL_SHADER_RESOURCE,
// all the same size; states are left as they were.
inline bool Apply(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* output,
                  ID3D12Resource* real, ID3D12Resource* hudless, bool debug = false) {
  auto& pass = Get();
  if (!Initialize(pass, device)) return false;
  const auto out = output->GetDesc(), from = real->GetDesc(), hud = hudless->GetDesc();
  if (out.Width != from.Width || out.Height != from.Height || hud.Width != from.Width || hud.Height != from.Height)
    return false;
  const uint32_t slot = pass.next++ % kSlots;
  auto cpu = pass.heap->GetCPUDescriptorHandleForHeapStart();
  auto gpu = pass.heap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(slot) * 3 * pass.increment;
  gpu.ptr += static_cast<UINT64>(slot) * 3 * pass.increment;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = UavFormat(from.Format);
  device->CreateShaderResourceView(real, &srv, cpu);
  cpu.ptr += pass.increment;
  srv.Format = UavFormat(hud.Format);
  device->CreateShaderResourceView(hudless, &srv, cpu);
  cpu.ptr += pass.increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = UavFormat(out.Format);
  device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
  ID3D12DescriptorHeap* heaps[] = {pass.heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(pass.root.Get());
  list->SetPipelineState(pass.pipeline.Get());
  const uint32_t width = static_cast<uint32_t>(out.Width), height = out.Height;
  uint32_t constants[4] = {width, height, 0, debug ? 1u : 0u};
  const float tolerance = kTolerance;
  std::memcpy(&constants[2], &tolerance, sizeof(tolerance));
  list->SetComputeRoot32BitConstants(0, 4, constants, 0);
  list->SetComputeRootDescriptorTable(1, gpu);
  // XeFG's writes to the output come first.
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = output;
  list->ResourceBarrier(1, &barrier);
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  list->ResourceBarrier(1, &barrier);
  return true;
}
}  // namespace mfgunlock::xmx::hudcomposite
