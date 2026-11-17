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
#include <utility>

#include "xmx_policy.hpp"

namespace mfgunlock::xmx::hudcomposite {
using Microsoft::WRL::ComPtr;

inline constexpr char kShader[] = R"(
Texture2D<float4> Real : register(t0);
Texture2D<float4> Hudless : register(t1);
RWTexture2D<float4> Output : register(u0);
RWByteAddressBuffer Count : register(u1);
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
  // 1.10.1: one pixel in 16 is counted, for the guard against a HUD-less color that
  // does not match the back buffer (RenoDX).
  if (hud && (id.x & 3) == 0 && (id.y & 3) == 0) Count.InterlockedAdd(0, 1);
  if (debug != 0) { if (hud) Output[id.xy] = float4(1, 0, 1, 1); }
  else if (hud) Output[id.xy] = Real[id.xy];
}
)";

inline constexpr uint32_t kSlots = 64;  // descriptor sets in flight (kPerSlot per slot)
inline constexpr uint32_t kPerSlot = 4;  // real, HUD-less, output, counter
inline constexpr uint32_t kReadbacks = 8;  // counter copies in flight
inline constexpr uint32_t kReadLag = 6;    // a copy this many passes old is done on the GPU
inline constexpr float kTolerance = 3.0f / 255.0f;

struct Pass {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pipeline;
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT increment{};
  uint32_t next{};
  // 1.10.1: the HUD-pixel counter. Never cleared: the CPU takes the difference
  // between two copies (wrap-around is harmless in unsigned arithmetic).
  ComPtr<ID3D12Resource> counter;
  ComPtr<ID3D12Resource> readback;
  const volatile uint32_t* mapped{};
  uint64_t passes{};
  uint64_t samples[kReadbacks]{};
  uint32_t lastTotal{};
  bool haveLast{};
  policy::HudGuard guard;
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
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2};
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
  D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kPerSlot,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pass.heap)))) return Fail(pass, "descriptor heap");
  pass.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const auto buffer = [&](D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                          ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES props{type};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)));
  };
  if (!buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pass.counter) ||
      !buffer(D3D12_HEAP_TYPE_READBACK, kReadbacks * sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE,
              D3D12_RESOURCE_STATE_COPY_DEST, pass.readback))
    return Fail(pass, "HUD counter buffers");
  void* mapped = nullptr;
  const D3D12_RANGE everything{0, kReadbacks * sizeof(uint32_t)};
  if (FAILED(pass.readback->Map(0, &everything, &mapped))) return Fail(pass, "HUD counter readback");
  pass.mapped = static_cast<const volatile uint32_t*>(mapped);
  return true;
}

// 1.10.1: the guard's reading of a pass kReadLag passes back (done on the GPU by then).
// Returns true when this reading turned the composition off.
inline bool Measure(Pass& pass) {
  if (pass.mapped == nullptr || pass.passes <= kReadLag) return false;
  const uint64_t index = pass.passes - kReadLag;  // the pass whose copy is read
  const uint32_t total = pass.mapped[index % kReadbacks];
  const uint64_t samples = pass.samples[index % kReadbacks];
  const bool had = pass.haveLast;
  const uint32_t previous = pass.lastTotal;
  pass.lastTotal = total;
  pass.haveLast = true;
  if (!had || samples == 0) return false;
  const uint32_t hud = total - previous;
  if (hud > samples) { pass.haveLast = false; return false; }  // a copy not finished yet; start over
  return pass.guard.Feed(static_cast<double>(hud) / static_cast<double>(samples));
}
inline bool Tripped() { return Get().guard.tripped; }

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
  cpu.ptr += static_cast<SIZE_T>(slot) * kPerSlot * pass.increment;
  gpu.ptr += static_cast<UINT64>(slot) * kPerSlot * pass.increment;
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
  cpu.ptr += pass.increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
  raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  raw.Format = DXGI_FORMAT_R32_TYPELESS;
  raw.Buffer.NumElements = 1;
  raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(pass.counter.Get(), nullptr, &raw, cpu);
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
  // The counter's running total goes to this pass's readback slot.
  D3D12_RESOURCE_BARRIER to_copy{};
  to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_copy.Transition.pResource = pass.counter.Get();
  to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &to_copy);
  const uint64_t index = ++pass.passes;
  list->CopyBufferRegion(pass.readback.Get(), (index % kReadbacks) * sizeof(uint32_t), pass.counter.Get(), 0,
                         sizeof(uint32_t));
  std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
  list->ResourceBarrier(1, &to_copy);
  pass.samples[index % kReadbacks] = uint64_t{(width + 3) / 4} * ((height + 3) / 4);
  return true;
}
}  // namespace mfgunlock::xmx::hudcomposite
