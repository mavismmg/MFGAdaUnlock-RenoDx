// SPDX-License-Identifier: MIT
#include "native_scrgb.hpp"
#include "native_gpu.hpp"
#include "native_scrgb_vs.h" // fxc /Fh output: g_native_scrgb_vs
#include "native_scrgb_ps.h" // fxc /Fh output: g_native_scrgb_ps

using Microsoft::WRL::ComPtr;
namespace mfgunlock::xemfg::native {
ScrgbEncoder::~ScrgbEncoder() { Drain(1000); if(event) CloseHandle(event); }
bool ScrgbEncoder::Initialize(ID3D12Device* target) {
  device=target;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable={1,&range};
  parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC desc{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob,errors;
  HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
  if(SUCCEEDED(hr)) hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));
  if(FAILED(hr)) return Fail(hr);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature=root.Get();
  pso.VS={g_native_scrgb_vs,sizeof(g_native_scrgb_vs)};
  pso.PS={g_native_scrgb_ps,sizeof(g_native_scrgb_ps)};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask=UINT_MAX;
  pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable=TRUE;
  pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets=1; pso.RTVFormats[0]=kTargetFormat; pso.SampleDesc.Count=1;
  if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipeline)))) return Fail(hr);
  D3D12_DESCRIPTOR_HEAP_DESC vd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kSlots,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  if(FAILED(hr=device->CreateDescriptorHeap(&vd,IID_PPV_ARGS(&views)))) return Fail(hr);
  D3D12_DESCRIPTOR_HEAP_DESC td{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,kSlots,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
  if(FAILED(hr=device->CreateDescriptorHeap(&td,IID_PPV_ARGS(&targets)))) return Fail(hr);
  viewSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  targetSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for(auto& allocator:allocators)
    if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))) return Fail(hr);
  if(FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[0].Get(),nullptr,IID_PPV_ARGS(&list)))) return Fail(hr);
  list->Close();
  if(FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return Fail(hr);
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  return event?true:Fail(HRESULT_FROM_WIN32(GetLastError()));
}
bool ScrgbEncoder::Encode(ID3D12CommandQueue* queue,ID3D12Resource* source,D3D12_RESOURCE_STATES sourceState,
    ID3D12Resource* target,D3D12_RESOURCE_STATES targetState) {
  const auto out=target->GetDesc();
  slot=(slot+1)%kSlots;
  if(!WaitFence(fence.Get(),fenceValues[slot],event,250)) return Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
  HRESULT hr=allocators[slot]->Reset();
  if(SUCCEEDED(hr)) hr=list->Reset(allocators[slot].Get(),pipeline.Get());
  if(FAILED(hr)) return Fail(hr);
  auto view=views->GetCPUDescriptorHandleForHeapStart(); view.ptr+=SIZE_T(slot)*viewSize;
  auto gpuView=views->GetGPUDescriptorHandleForHeapStart(); gpuView.ptr+=UINT64(slot)*viewSize;
  auto rtv=targets->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=SIZE_T(slot)*targetSize;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
  srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels=1;
  device->CreateShaderResourceView(source,&srv,view);
  D3D12_RENDER_TARGET_VIEW_DESC rt{}; rt.Format=kTargetFormat; rt.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(target,&rt,rtv);
  const auto readable=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  Transition(list.Get(),source,sourceState,readable);
  Transition(list.Get(),target,targetState,D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12DescriptorHeap* heaps[]={views.Get()};
  list->SetDescriptorHeaps(1,heaps);
  list->SetGraphicsRootSignature(root.Get());
  list->SetGraphicsRootDescriptorTable(0,gpuView);
  const D3D12_VIEWPORT viewport{0,0,float(out.Width),float(out.Height),0,1};
  const D3D12_RECT scissor{0,0,LONG(out.Width),LONG(out.Height)};
  list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
  list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3,1,0,0);
  Transition(list.Get(),target,D3D12_RESOURCE_STATE_RENDER_TARGET,targetState);
  Transition(list.Get(),source,readable,sourceState);
  if(FAILED(hr=list->Close())) return Fail(hr);
  ID3D12CommandList* lists[]={list.Get()};
  queue->ExecuteCommandLists(1,lists);
  fenceValues[slot]=nextFence++;
  hr=queue->Signal(fence.Get(),fenceValues[slot]);
  return SUCCEEDED(hr)?true:Fail(hr);
}
bool ScrgbEncoder::Drain(uint32_t timeoutMs) {
  if(!fence || nextFence==1) return true;
  return WaitFence(fence.Get(),nextFence-1,event,timeoutMs);
}
}
