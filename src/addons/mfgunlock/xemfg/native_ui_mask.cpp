// SPDX-License-Identifier: MIT
#include "native_ui_mask.hpp"
#include "native_gpu.hpp"
#include "native_ui_mask_cs.h" // fxc /Fh output: g_native_ui_mask_cs
#include <iterator>

using Microsoft::WRL::ComPtr;
namespace mfgunlock::xemfg::native {
namespace {
constexpr auto kReadable=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
bool UiMask::SupportedFormat(DXGI_FORMAT value) {
  return value==DXGI_FORMAT_R8G8B8A8_UNORM || value==DXGI_FORMAT_B8G8R8A8_UNORM
      || value==DXGI_FORMAT_R10G10B10A2_UNORM;
}
UiMask::~UiMask() { Drain(1000); if(event) CloseHandle(event); }
bool UiMask::Initialize(ID3D12Device* target,DXGI_FORMAT output) {
  device=target; format=output;
  if(!SupportedFormat(format)) return Fail(DXGI_ERROR_UNSUPPORTED);
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
  if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))
      || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return Fail(DXGI_ERROR_UNSUPPORTED);
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0};
  ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,4};
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
  pso.pRootSignature=root.Get(); pso.CS={g_native_ui_mask_cs,sizeof(g_native_ui_mask_cs)};
  if(FAILED(hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)))) return Fail(hr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kSlots*6,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
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
bool UiMask::Ensure(ComPtr<ID3D12Resource>& texture,DXGI_FORMAT textureFormat,uint32_t width,uint32_t height,
    D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial) {
  if(texture) {
    const auto d=texture->GetDesc();
    if(d.Format==textureFormat && d.Width==width && d.Height==height) return true;
    texture.Reset();
  }
  D3D12_HEAP_PROPERTIES heapProps{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1;
  d.Format=textureFormat; d.SampleDesc.Count=1; d.Flags=flags;
  const HRESULT hr=device->CreateCommittedResource(&heapProps,D3D12_HEAP_FLAG_NONE,&d,initial,nullptr,IID_PPV_ARGS(&texture));
  return SUCCEEDED(hr)?true:Fail(hr);
}
ID3D12GraphicsCommandList* UiMask::Record(ID3D12Resource* backbuffer,D3D12_RESOURCE_STATES state,
    ID3D12Resource* reference,D3D12_RESOURCE_STATES referenceState,const DejitterRegion& area,
    ID3D12Resource** hudlessOut,ID3D12Resource** uiOut) {
  const auto bd=backbuffer?backbuffer->GetDesc():D3D12_RESOURCE_DESC{};
  if(open || !backbuffer || bd.Format!=format || !bd.Width || !bd.Height) { Fail(E_INVALIDARG); return nullptr; }
  const uint32_t previous=slot;
  slot=(slot+1)%kSlots;
  if(!WaitFence(fence.Get(),fenceValues[slot],event,250)) { Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT)); return nullptr; }
  const auto width=uint32_t(bd.Width), height=bd.Height;
  if(!Ensure(hudless[slot],format,width,height,D3D12_RESOURCE_FLAG_NONE,kReadable)
      || !Ensure(ui[slot],format,width,height,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,kReadable)
      || !Ensure(persist[0],DXGI_FORMAT_R8_UINT,width,height,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,kReadable)
      || !Ensure(persist[1],DXGI_FORMAT_R8_UINT,width,height,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,kReadable))
    return nullptr;
  usedReference=reference && area.width==width && area.height==height
      && Ensure(referenceCopy,format,width,height,D3D12_RESOURCE_FLAG_NONE,kReadable);
  // Coverage persistence and stillness persistence count different things.
  if(usedReference!=referenceHistory) history=false;
  referenceHistory=usedReference;
  // The previous slot holds the previous real frame only when that frame was
  // recorded at this size (each slot is recreated on its own first use after a
  // resize; that alone must not restart the history again).
  const bool continuous=history && historyWidth==width && historyHeight==height && hudless[previous];
  HRESULT hr=allocators[slot]->Reset();
  if(SUCCEEDED(hr)) hr=list->Reset(allocators[slot].Get(),pipeline.Get());
  if(FAILED(hr)) { Fail(hr); return nullptr; }
  open=true;
  const auto readBack=D3D12_RESOURCE_STATES(kReadable|D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(list.Get(),backbuffer,state,readBack);
  Transition(list.Get(),hudless[slot].Get(),kReadable,D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(hudless[slot].Get(),backbuffer);
  Transition(list.Get(),hudless[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,kReadable);
  if(usedReference) {
    // The game's resource is only read here and returned to its own state.
    Transition(list.Get(),reference,referenceState,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(),referenceCopy.Get(),kReadable,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{referenceCopy.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    D3D12_TEXTURE_COPY_LOCATION src{reference,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    const D3D12_BOX box{area.x,area.y,0,area.x+area.width,area.y+area.height,1};
    list->CopyTextureRegion(&dst,0,0,0,&src,&box);
    Transition(list.Get(),referenceCopy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,kReadable);
    Transition(list.Get(),reference,D3D12_RESOURCE_STATE_COPY_SOURCE,referenceState);
  }

  auto cpu=heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=SIZE_T(slot)*6*descriptorSize;
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=UINT64(slot)*6*descriptorSize;
  const auto srv=[&](ID3D12Resource* resource,DXGI_FORMAT viewFormat) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=viewFormat; view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(resource,&view,cpu); cpu.ptr+=descriptorSize;
  };
  const auto uav=[&](ID3D12Resource* resource,DXGI_FORMAT viewFormat) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=viewFormat; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource,nullptr,&view,cpu); cpu.ptr+=descriptorSize;
  };
  auto* persistIn=persist[persistIndex].Get();
  auto* persistOut=persist[persistIndex^1].Get();
  srv(backbuffer,format);
  srv(continuous?hudless[previous].Get():backbuffer,format);
  srv(persistIn,DXGI_FORMAT_R8_UINT);
  srv(usedReference?referenceCopy.Get():backbuffer,format);
  uav(persistOut,DXGI_FORMAT_R8_UINT);
  uav(ui[slot].Get(),format);
  Transition(list.Get(),persistOut,kReadable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list.Get(),ui[slot].Get(),kReadable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[]={heap.Get()};
  list->SetDescriptorHeaps(1,heaps);
  list->SetComputeRootSignature(root.Get());
  // Without a previous frame the history starts empty. Stillness needs that
  // previous frame, so an unbounded threshold keeps every pixel transparent;
  // coverage does not, and counts from this frame.
  const float tolerance=usedReference?kCoverageTolerance:kTolerance;
  const uint32_t threshold=usedReference?kReferenceThreshold:continuous?kThreshold:0xFFFFFFFFu;
  const uint32_t constants[6]={width,height,threshold,
      *reinterpret_cast<const uint32_t*>(&tolerance),continuous?0u:1u,usedReference?1u:0u};
  list->SetComputeRoot32BitConstants(0,6,constants,0);
  list->SetComputeRootDescriptorTable(1,gpu);
  list->Dispatch((width+7)/8,(height+7)/8,1);
  Transition(list.Get(),ui[slot].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,kReadable);
  Transition(list.Get(),persistOut,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,kReadable);
  Transition(list.Get(),backbuffer,readBack,state);
  persistIndex^=1;
  history=true; historyWidth=width; historyHeight=height;
  *hudlessOut=hudless[slot].Get();
  *uiOut=ui[slot].Get();
  return list.Get();
}
bool UiMask::Submit(ID3D12CommandQueue* queue) {
  if(!open) return Fail(E_UNEXPECTED);
  open=false;
  HRESULT hr=list->Close();
  if(FAILED(hr)) { history=false; return Fail(hr); }
  ID3D12CommandList* lists[]={list.Get()};
  queue->ExecuteCommandLists(1,lists);
  fenceValues[slot]=nextFence++;
  hr=queue->Signal(fence.Get(),fenceValues[slot]);
  return SUCCEEDED(hr)?true:Fail(hr);
}
bool UiMask::Drain(uint32_t timeoutMs) {
  if(!fence || nextFence==1) return true;
  return WaitFence(fence.Get(),nextFence-1,event,timeoutMs);
}
}
