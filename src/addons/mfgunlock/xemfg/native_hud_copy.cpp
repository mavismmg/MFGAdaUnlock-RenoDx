// SPDX-License-Identifier: MIT
#include "native_hud_copy.hpp"
#include "native_gpu.hpp"

using Microsoft::WRL::ComPtr;
namespace mfgunlock::xemfg::native {
HudCopy::~HudCopy() { Drain(1000); if(event) CloseHandle(event); }
bool HudCopy::Initialize(ID3D12Device* target) {
  device=target;
  HRESULT hr=S_OK;
  for(auto& allocator:allocators)
    if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))) return Fail(hr);
  if(FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[0].Get(),nullptr,IID_PPV_ARGS(&list)))) return Fail(hr);
  list->Close();
  if(FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return Fail(hr);
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  return event?true:Fail(HRESULT_FROM_WIN32(GetLastError()));
}
ID3D12Resource* HudCopy::Record(ID3D12GraphicsCommandList* target,ID3D12Resource* source,D3D12_RESOURCE_STATES state,
    const DejitterRegion& region,DXGI_FORMAT format,unsigned kind,uint32_t frame) {
  if(!target || !source || kind>1 || !region.width || !region.height) { Fail(E_INVALIDARG); return nullptr; }
  auto& texture=textures[kind][frame%kSlots];
  if(!Ensure(texture,format,region.width,region.height,D3D12_RESOURCE_FLAG_NONE)) return nullptr;
  // The copy rests in COMMON: CopyTextureRegion promotes it implicitly to
  // COPY_DEST and an explicit barrier returns it, as MotionDejitter::Capture.
  Transition(target,source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
  D3D12_TEXTURE_COPY_LOCATION src{source,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
  const D3D12_BOX box{region.x,region.y,0,region.x+region.width,region.y+region.height,1};
  target->CopyTextureRegion(&dst,0,0,0,&src,&box);
  Transition(target,source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
  Transition(target,texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
  return texture.Get();
}
bool HudCopy::Ensure(ComPtr<ID3D12Resource>& texture,DXGI_FORMAT format,uint32_t width,uint32_t height,D3D12_RESOURCE_FLAGS flags) {
  if(texture) {
    const auto d=texture->GetDesc();
    if(d.Format==format && d.Width==width && d.Height==height && d.Flags==flags) return true;
    texture.Reset();
  }
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height;
  d.DepthOrArraySize=1; d.MipLevels=1; d.Format=format; d.SampleDesc.Count=1; d.Flags=flags;
  const HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture));
  return SUCCEEDED(hr)?true:Fail(hr);
}
ID3D12Resource* HudCopy::Target(unsigned kind,uint32_t frame,DXGI_FORMAT format,uint32_t width,uint32_t height,D3D12_RESOURCE_FLAGS flags) {
  if(kind>1 || !width || !height) { Fail(E_INVALIDARG); return nullptr; }
  auto& texture=targets[kind][frame%kSlots];
  return Ensure(texture,format,width,height,flags)?texture.Get():nullptr;
}
ID3D12GraphicsCommandList* HudCopy::Begin() {
  if(open) { Fail(E_UNEXPECTED); return nullptr; }
  slot=(slot+1)%kSlots;
  if(!WaitFence(fence.Get(),fenceValues[slot],event,250)) { Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT)); return nullptr; }
  HRESULT hr=allocators[slot]->Reset();
  if(SUCCEEDED(hr)) hr=list->Reset(allocators[slot].Get(),nullptr);
  if(FAILED(hr)) { Fail(hr); return nullptr; }
  open=true;
  return list.Get();
}
bool HudCopy::Submit(ID3D12CommandQueue* queue) {
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
bool HudCopy::Drain(uint32_t timeoutMs) {
  if(!fence || nextFence==1) return true;
  return WaitFence(fence.Get(),nextFence-1,event,timeoutMs);
}
}
