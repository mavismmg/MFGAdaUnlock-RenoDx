// SPDX-License-Identifier: MIT
#include "native_dejitter.hpp"
#include "native_gpu.hpp"
#include "native_dejitter_cs.h" // fxc /Fh output: g_native_dejitter_cs
#include <iterator>

using Microsoft::WRL::ComPtr;
namespace mfgunlock::xemfg::native {
namespace {
DXGI_FORMAT ReadFormat(DXGI_FORMAT format) {
  switch(format) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
  }
}
// Half precision inputs stay half precision; anything else converts to 32-bit.
DXGI_FORMAT OutputFormat(DXGI_FORMAT read) {
  return read==DXGI_FORMAT_R16G16_FLOAT || read==DXGI_FORMAT_R16G16B16A16_FLOAT?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT;
}
}
MotionDejitter::~MotionDejitter() { Drain(1000); if(event) CloseHandle(event); }
bool MotionDejitter::Initialize(ID3D12Device* target) {
  device=target;
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
  ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants={0,0,6};
  parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable={2,ranges};
  D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob,errors;
  HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
  if(SUCCEEDED(hr)) hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));
  if(FAILED(hr)) return Fail(hr);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature=root.Get(); pso.CS={g_native_dejitter_cs,sizeof(g_native_dejitter_cs)};
  if(FAILED(hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)))) return Fail(hr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kSlots*2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  if(FAILED(hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))) return Fail(hr);
  descriptorSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for(auto& allocator:allocators)
    if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))) return Fail(hr);
  if(FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[0].Get(),nullptr,IID_PPV_ARGS(&list)))) return Fail(hr);
  list->Close();
  if(FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return Fail(hr);
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  return event?true:Fail(HRESULT_FROM_WIN32(GetLastError()));
}
bool MotionDejitter::EnsureTexture(ComPtr<ID3D12Resource>& texture,DXGI_FORMAT format,uint32_t width,uint32_t height,
    D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial) {
  if(texture) {
    const auto d=texture->GetDesc();
    if(d.Format==format && d.Width==width && d.Height==height) return true;
    texture.Reset();
  }
  D3D12_HEAP_PROPERTIES heapProps{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1;
  d.Format=format; d.SampleDesc.Count=1; d.Flags=flags;
  const HRESULT hr=device->CreateCommittedResource(&heapProps,D3D12_HEAP_FLAG_NONE,&d,initial,nullptr,IID_PPV_ARGS(&texture));
  return SUCCEEDED(hr)?true:Fail(hr);
}
bool MotionDejitter::Capture(ID3D12GraphicsCommandList* producer,ID3D12Resource* source,D3D12_RESOURCE_STATES state,
    const DejitterRegion& region,uint32_t index,ComPtr<ID3D12Resource>& staged) {
  auto& texture=staging[index%kSlots];
  // Staging rests in COMMON: the copy promotes it implicitly to COPY_DEST and
  // an explicit barrier returns it, so every capture starts the same way.
  if(!EnsureTexture(texture,source->GetDesc().Format,region.width,region.height,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COMMON))
    return false;
  Transition(producer,source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
  D3D12_TEXTURE_COPY_LOCATION src{source,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
  const D3D12_BOX box{region.x,region.y,0,region.x+region.width,region.y+region.height,1};
  producer->CopyTextureRegion(&dst,0,0,0,&src,&box);
  Transition(producer,source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
  Transition(producer,texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
  staged=texture;
  return true;
}
ID3D12GraphicsCommandList* MotionDejitter::Convert(ID3D12Resource* source,D3D12_RESOURCE_STATES state,
    const DejitterRegion& region,DejitterVector delta,ID3D12Resource** output) {
  if(open || !region.width || !region.height) { Fail(E_INVALIDARG); return nullptr; }
  slot=(slot+1)%kSlots;
  if(!WaitFence(fence.Get(),fenceValues[slot],event,250)) { Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT)); return nullptr; }
  const auto read=ReadFormat(source->GetDesc().Format);
  auto& target=outputs[slot];
  if(!EnsureTexture(target,OutputFormat(read),region.width,region.height,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) return nullptr;
  HRESULT hr=allocators[slot]->Reset();
  if(SUCCEEDED(hr)) hr=list->Reset(allocators[slot].Get(),pipeline.Get());
  if(FAILED(hr)) { Fail(hr); return nullptr; }
  open=true;
  auto cpu=heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=SIZE_T(slot)*2*descriptorSize;
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=UINT64(slot)*2*descriptorSize;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=read; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
  device->CreateShaderResourceView(source,&srv,cpu);
  cpu.ptr+=descriptorSize;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=target->GetDesc().Format; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(target.Get(),nullptr,&uav,cpu);
  const auto readable=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const bool transition=!(state&readable) || (state&~(readable|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
  if(transition) Transition(list.Get(),source,state,readable);
  Transition(list.Get(),target.Get(),readable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[]={heap.Get()};
  list->SetDescriptorHeaps(1,heaps);
  list->SetComputeRootSignature(root.Get());
  const uint32_t constants[6]={region.x,region.y,region.width,region.height,
      *reinterpret_cast<const uint32_t*>(&delta.x),*reinterpret_cast<const uint32_t*>(&delta.y)};
  list->SetComputeRoot32BitConstants(0,6,constants,0);
  list->SetComputeRootDescriptorTable(1,gpu);
  list->Dispatch((region.width+7)/8,(region.height+7)/8,1);
  Transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,readable);
  if(transition) Transition(list.Get(),source,readable,state);
  *output=target.Get();
  return list.Get();
}
bool MotionDejitter::Submit(ID3D12CommandQueue* queue) {
  if(!open) return Fail(E_UNEXPECTED);
  open=false;
  HRESULT hr=list->Close();
  if(FAILED(hr)) return Fail(hr);
  ID3D12CommandList* lists[]={list.Get()};
  queue->ExecuteCommandLists(1,lists);
  fenceValues[slot]=nextFence++;
  hr=queue->Signal(fence.Get(),fenceValues[slot]);
  return SUCCEEDED(hr)?true:Fail(hr);
}
bool MotionDejitter::Drain(uint32_t timeoutMs) {
  if(!fence || nextFence==1) return true;
  return WaitFence(fence.Get(),nextFence-1,event,timeoutMs);
}
}
