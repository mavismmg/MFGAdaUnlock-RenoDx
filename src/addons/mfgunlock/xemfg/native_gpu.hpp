// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <windows.h>
#include <cstdint>

namespace mfgunlock::xemfg::native {
inline void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
  if(from==to) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
  list->ResourceBarrier(1,&barrier);
}
// True once the fence reached `value`. The event alone proves nothing: after a
// timed-out wait the late completion still sets it, and it would end the next
// wait on another value early, with that value's allocator still in use.
inline bool WaitFence(ID3D12Fence* fence,uint64_t value,HANDLE event,uint32_t timeoutMs) {
  const auto begin=GetTickCount64();
  for(;;) {
    if(fence->GetCompletedValue()>=value) return true;
    const auto elapsed=GetTickCount64()-begin;
    if(elapsed>=timeoutMs || !event || FAILED(fence->SetEventOnCompletion(value,event))) return false;
    if(WaitForSingleObject(event,DWORD(timeoutMs-elapsed))==WAIT_FAILED) return false;
  }
}
}
