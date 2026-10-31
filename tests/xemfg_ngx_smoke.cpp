// SPDX-License-Identifier: MIT
// Plan 5.4: a DLSS-SR game through NGX, with no Streamline in the process and
// no frame generation of its own. With [Adapters] DlssSr=1 its swapchain must
// become the Intel output and each DLSS evaluation must feed one source frame.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include "../src/addons/mfgunlock/xemfg/backend_api.hpp"
#include "../src/addons/mfgunlock/xemfg/native_bridge.hpp"
#include "../src/addons/mfgunlock/xemfg/native_controls.hpp"
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT code,const char* where) { if(FAILED(code)) { std::cerr<<where<<" 0x"<<std::hex<<code<<std::endl; throw std::runtime_error(where); } }
void Require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
template<class T> T Import(HMODULE m,const char* name) { auto p=reinterpret_cast<T>(GetProcAddress(m,name)); Require(p!=nullptr,name); return p; }
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM a,LPARAM b) { return DefWindowProcW(w,m,a,b); }
// A game-side NGX parameter block (NGX defines only the interface).
struct Parameters final : NVSDK_NGX_Parameter {
  std::map<std::string,unsigned long long> integers; std::map<std::string,double> reals; std::map<std::string,void*> pointers;
  void Set(const char* n,unsigned long long v) override { integers[n]=v; }
  void Set(const char* n,float v) override { reals[n]=v; }
  void Set(const char* n,double v) override { reals[n]=v; }
  void Set(const char* n,unsigned int v) override { integers[n]=v; }
  void Set(const char* n,int v) override { integers[n]=static_cast<unsigned long long>(static_cast<long long>(v)); }
  void Set(const char* n,ID3D11Resource* v) override { pointers[n]=v; }
  void Set(const char* n,ID3D12Resource* v) override { pointers[n]=v; }
  void Set(const char* n,void* v) override { pointers[n]=v; }
  template<class T> NVSDK_NGX_Result Integer(const char* n,T* out) const {
    const auto it=integers.find(n); if(it==integers.end() || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out=static_cast<T>(it->second); return NVSDK_NGX_Result_Success;
  }
  template<class T> NVSDK_NGX_Result Real(const char* n,T* out) const {
    const auto it=reals.find(n); if(it==reals.end() || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out=static_cast<T>(it->second); return NVSDK_NGX_Result_Success;
  }
  template<class T> NVSDK_NGX_Result Pointer(const char* n,T** out) const {
    const auto it=pointers.find(n); if(it==pointers.end() || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out=static_cast<T*>(it->second); return NVSDK_NGX_Result_Success;
  }
  NVSDK_NGX_Result Get(const char* n,unsigned long long* v) const override { return Integer(n,v); }
  NVSDK_NGX_Result Get(const char* n,float* v) const override { return Real(n,v); }
  NVSDK_NGX_Result Get(const char* n,double* v) const override { return Real(n,v); }
  NVSDK_NGX_Result Get(const char* n,unsigned int* v) const override { return Integer(n,v); }
  NVSDK_NGX_Result Get(const char* n,int* v) const override { return Integer(n,v); }
  NVSDK_NGX_Result Get(const char* n,ID3D11Resource** v) const override { return Pointer(n,v); }
  NVSDK_NGX_Result Get(const char* n,ID3D12Resource** v) const override { return Pointer(n,v); }
  NVSDK_NGX_Result Get(const char* n,void** v) const override { return Pointer(n,v); }
  void Reset() override { integers.clear(); reals.clear(); pointers.clear(); }
};
using CreateFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using EvaluateFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
}
int main(int argc,char** argv) {
  try {
    const unsigned multiplier=argc>1?unsigned(std::stoul(argv[1])):4;
    bool expectOff=false,exclusive=false,exclusiveFirst=false,exclusiveCreate=false,borderless=false,hdr10=false;
    bool pipelined=false,doubleEvaluate=false;
    for(int i=2;i<argc;++i) {
      const std::string arg=argv[i];
      expectOff|=arg=="--dlss-sr-default-off";
      exclusive|=arg=="--exclusive-transition" || arg=="--exclusive-borderless";
      // The game enters exclusive fullscreen before its first Present, as the
      // Metro log shows (the first admission already refused the display mode).
      exclusiveFirst|=arg=="--exclusive-first" || arg=="--exclusive-borderless-first";
      // 1.5.8: the swapchain is created in exclusive fullscreen.
      exclusiveCreate|=arg=="--exclusive-borderless-create";
      // 1.5.8: [XeFG] ExclusiveAsBorderless=1 serves the request as borderless.
      borderless|=arg.rfind("--exclusive-borderless",0)==0;
      hdr10|=arg=="--hdr10";
      // 1.7.1: RDR2 on D3D12 records a frame's DLSS before the previous frame's Present.
      pipelined|=arg=="--dlss-sr-pipelined";
      // Two DLSS evaluations in one frame are one source frame.
      doubleEvaluate|=arg=="--dlss-sr-double";
    }
    exclusiveFirst|=exclusiveCreate; exclusive|=exclusiveFirst;
    auto reshade=LoadLibraryW(L".\\dxgi.dll"); Require(reshade,"ReShade");
    ComPtr<IDXGIFactory4> factory;
    Check(Import<HRESULT(WINAPI*)(REFIID,void**)>(reshade,"CreateDXGIFactory1")(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de) D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    Require(device!=nullptr,"NVIDIA device");
    auto addon=GetModuleHandleW(L"renodx-mfgunlock-native.addon64"); Require(addon,"ReShade did not load the addon");
    auto modelStatus=Import<mfgunlock::xemfg::GetStatus>(addon,"MFGAXeGetStatus");
    auto bridgeStatus=Import<bool(*)(mfgunlock::xemfg::native::Status*,uint32_t)>(addon,"MFGAXeNativeStatus");
    Require(!GetModuleHandleW(L"sl.interposer.dll"),"Streamline is present in the DLSS-SR scenario");
    // The NGX driver module maps late; the adapter hooks it from the loader worker.
    HMODULE ngx=LoadLibraryW(L"_nvngx.dll"); Require(ngx,"NGX fixture");
    const auto create=Import<CreateFn>(ngx,"NVSDK_NGX_D3D12_CreateFeature");
    const auto evaluate=Import<EvaluateFn>(ngx,"NVSDK_NGX_D3D12_EvaluateFeature");
    const auto release=Import<ReleaseFn>(ngx,"NVSDK_NGX_D3D12_ReleaseFeature");
    const auto evaluations=Import<uint32_t(*)()>(ngx,"FixtureNgxEvaluations");
    const auto* entry=reinterpret_cast<const uint8_t*>(evaluate);
    if(expectOff) {
      // Off by default: NGX stays untouched and the swapchain stays native.
      Sleep(1500);
      Require(entry[0]!=0xE9,"NGX was hooked without [Adapters] DlssSr=1");
      // 1.5.1: no Streamline, NGX mapped, no output: the panel suggests the input.
      const auto hint=Import<uint32_t(*)()>(addon,"MFGAXeDlssSrHint");
      Require(hint()==2u,"DLSS-SR input was not suggested for a DLSS-SR-only game");
      Require(Import<bool(*)(bool)>(addon,"MFGAXeDlssSrSave")(true),"DLSS-SR opt-in was not saved");
      Require(hint()==(2u|4u),"saving the opt-in changed the running adapter");
      Sleep(500);
      Require(entry[0]!=0xE9,"saving the opt-in hooked NGX before a restart");
      D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
      WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.lpszClassName=L"XeMFGNgxOff"; wc.hInstance=GetModuleHandleW(nullptr); RegisterClassW(&wc);
      HWND hwnd=CreateWindowW(wc.lpszClassName,L"XeMFG DLSS-SR off",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
      DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=640; desc.Height=360; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
      desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=3; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
      ComPtr<IDXGISwapChain1> chain; Check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,nullptr,nullptr,&chain),"native swapchain");
      mfgunlock::xemfg::native::Status status{}; bridgeStatus(&status,sizeof(status));
      Require(!status.output,"an Intel output was created without the DLSS-SR opt-in");
      chain.Reset(); DestroyWindow(hwnd);
      Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"log flush");
      std::cout<<"PASS: DLSS-SR adapter is off by default"<<std::endl;
      return 0;
    }
    for(const auto deadline=GetTickCount64()+5000;entry[0]!=0xE9 && GetTickCount64()<deadline;) Sleep(10);
    Require(entry[0]==0xE9,"NGX evaluation was not hooked");
    Require(Import<uint32_t(*)()>(addon,"MFGAXeDlssSrHint")()==1u,"an opted-in game was still offered the DLSS-SR input");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.lpszClassName=L"XeMFGNgxSmoke"; wc.hInstance=GetModuleHandleW(nullptr); RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"XeMFG DLSS-SR adapter smoke",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    Require(hwnd,"window");
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=640; desc.Height=360; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=3; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // RenoDX HDR mods present HDR10 (Metro Exodus field report).
    if(hdr10) desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    if(exclusive) {
      // Metro Exodus field report (1.5.5): the game creates its swapchain
      // windowed and enters exclusive fullscreen afterwards. DXGI only grants
      // that to a visible, foreground window.
      desc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
      // A background process may not take the foreground; joining the current
      // foreground thread's input queue allows it without sending any input.
      ShowWindow(hwnd,SW_SHOWNORMAL);
      const auto foreground=GetWindowThreadProcessId(GetForegroundWindow(),nullptr),self=GetCurrentThreadId();
      if(foreground && foreground!=self) AttachThreadInput(self,foreground,TRUE);
      SetForegroundWindow(hwnd); BringWindowToTop(hwnd); SetFocus(hwnd);
      if(foreground && foreground!=self) AttachThreadInput(self,foreground,FALSE);
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
      std::cout<<"Window foreground="<<(GetForegroundWindow()==hwnd)<<std::endl;
    }
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowed{}; windowed.Windowed=!exclusiveCreate;
    // The window the served borderless state must restore (taken again right
    // before a runtime request, after the game's windowed ResizeTarget).
    RECT originalRect{}; GetWindowRect(hwnd,&originalRect);
    auto originalStyle=GetWindowLongPtrW(hwnd,GWL_STYLE);
    const auto pump=[] { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); };
    using ControlsStatus=bool(*)(mfgunlock::xemfg::native::ControlStatus*,uint32_t);
    const auto controls=Import<ControlsStatus>(addon,"MFGAXeControlsStatus");
    // The window covers its monitor without a caption (served borderless).
    const auto coversMonitor=[&] {
      MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
      RECT now{}; GetWindowRect(hwnd,&now);
      return EqualRect(&now,&monitor.rcMonitor) && !(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_CAPTION);
    };
    ComPtr<IDXGISwapChain1> base; Check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,&windowed,nullptr,&base),"swapchain");
    ComPtr<IDXGISwapChain4> chain; Check(base.As(&chain),"chain4"); base.Reset();
    mfgunlock::xemfg::native::Status ready{}; bridgeStatus(&ready,sizeof(ready));
    Require(ready.output,"armed DLSS-SR swapchain is not the Intel output");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence; uint64_t value{};
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    const auto submit=[&] {
      Check(list->Close(),"close"); ID3D12CommandList* raw=list.Get(); queue->ExecuteCommandLists(1,&raw);
      Check(queue->Signal(fence.Get(),++value),"signal"); while(fence->GetCompletedValue()<value) Sleep(0);
    };
    // Depth (R32) and motion (R16G16) at render resolution, as DLSS-SR inputs.
    ComPtr<ID3D12Resource> depth,motion;
    for(auto item:{std::pair{std::addressof(depth),DXGI_FORMAT_R32_FLOAT},std::pair{std::addressof(motion),DXGI_FORMAT_R16G16_FLOAT}}) {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT}; D3D12_RESOURCE_DESC rd{};
      rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=desc.Width; rd.Height=desc.Height; rd.DepthOrArraySize=1; rd.MipLevels=1;
      rd.Format=item.second; rd.SampleDesc.Count=1; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,
          IID_PPV_ARGS(item.first->ReleaseAndGetAddressOf())),"input texture");
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> rtv; Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"rtv heap");
    Parameters params;
    params.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,int(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    params.Set(NVSDK_NGX_Parameter_Width,desc.Width); params.Set(NVSDK_NGX_Parameter_Height,desc.Height);
    NVSDK_NGX_Handle* feature{};
    Require(NVSDK_NGX_SUCCEED(create(list.Get(),NVSDK_NGX_Feature_SuperSampling,&params,&feature)) && feature,"DLSS-SR feature");
    submit();
    unsigned frame{},evaluated{};
    const auto evaluateFrame=[&] {
      Check(allocator->Reset(),"allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"list reset");
      params.Set(NVSDK_NGX_Parameter_Depth,depth.Get()); params.Set(NVSDK_NGX_Parameter_MotionVectors,motion.Get());
      params.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,evaluated%2?.25f:-.25f); params.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,evaluated%2?-.25f:.25f);
      params.Set(NVSDK_NGX_Parameter_MV_Scale_X,1.f); params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,1.f);
      params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,desc.Width);
      params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,desc.Height);
      params.Set(NVSDK_NGX_Parameter_Reset,int(evaluated==0));
      Require(NVSDK_NGX_SUCCEED(evaluate(list.Get(),feature,&params,nullptr)),"DLSS-SR evaluation");
      ++evaluated; submit();
    };
    const auto presentFrame=[&] {
      Check(allocator->Reset(),"allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"list reset");
      ComPtr<ID3D12Resource> back; Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"back buffer");
      auto handle=rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(back.Get(),nullptr,handle);
      Barrier(list.Get(),back.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float color[]={.1f,.2f,.3f,1.f}; list->ClearRenderTargetView(handle,color,0,nullptr);
      Barrier(list.Get(),back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
      submit(); back.Reset();
      Check(chain->Present(0,0),"Present");
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
    };
    if(pipelined || doubleEvaluate) {
      // The order the D3D12 log of RDR2 implies: two frames' evaluations before
      // their Presents, twice in five (E E P P E E P P E P), so without a queue
      // two of five Presents have no frame and generation never settles.
      constexpr unsigned kCycles=24,kWarmup=4;
      unsigned presents{},ready_{};
      const auto present=[&](unsigned cycle) {
        presentFrame(); bridgeStatus(&ready,sizeof(ready));
        if(cycle>=kWarmup) { ++presents; if(ready.enabled) ++ready_; }
      };
      for(unsigned cycle=0;cycle<kCycles;++cycle) {
        if(doubleEvaluate) { evaluateFrame(); evaluateFrame(); present(cycle); continue; }
        evaluateFrame(); evaluateFrame(); present(cycle); present(cycle);
        evaluateFrame(); evaluateFrame(); present(cycle); present(cycle);
        evaluateFrame(); present(cycle);
      }
      Sleep(200);
      mfgunlock::xemfg::Status ms; modelStatus(&ms,sizeof(ms));
      std::cout<<"DLSS-SR order presents="<<presents<<" ready="<<ready_<<" evaluations="<<evaluations()
          <<" generated="<<ms.generated<<" result="<<ms.result<<" detail="<<ready.detail<<std::endl;
      Require(evaluations()==evaluated,"DLSS-SR itself did not run every evaluation");
      Require(ready_*10>=presents*9,"generation did not stay on through this order of evaluations and Presents");
      Require(ms.generated==multiplier-1 && ms.result==0,"DLSS-SR output presented no generated frames");
      Require(NVSDK_NGX_SUCCEED(release(feature)),"DLSS-SR release");
      chain.Reset(); DestroyWindow(hwnd);
      Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"log flush");
      std::cout<<"PASS: DLSS-SR adapter reached ready"<<std::endl;
      return 0;
    }
    const auto run=[&](unsigned count) {
    unsigned readyFrames{};
    for(const unsigned end=frame+count;frame<end;++frame) {
      Check(allocator->Reset(),"allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"list reset");
      ComPtr<ID3D12Resource> back; Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"back buffer");
      auto handle=rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(back.Get(),nullptr,handle);
      Barrier(list.Get(),back.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float color[]={.1f,.2f,.3f,1.f}; list->ClearRenderTargetView(handle,color,0,nullptr);
      Barrier(list.Get(),back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
      params.Set(NVSDK_NGX_Parameter_Depth,depth.Get()); params.Set(NVSDK_NGX_Parameter_MotionVectors,motion.Get());
      params.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,frame%2?.25f:-.25f); params.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,frame%2?-.25f:.25f);
      params.Set(NVSDK_NGX_Parameter_MV_Scale_X,1.f); params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,1.f);
      params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,desc.Width);
      params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,desc.Height);
      params.Set(NVSDK_NGX_Parameter_Reset,int(frame==0));
      Require(NVSDK_NGX_SUCCEED(evaluate(list.Get(),feature,&params,nullptr)),"DLSS-SR evaluation");
      submit(); back.Reset();
      Check(chain->Present(0,0),"Present");
      bridgeStatus(&ready,sizeof(ready)); if(ready.enabled) ++readyFrames;
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
    }
    return readyFrames;
    };
    unsigned readyFrames=exclusiveFirst?0:run(24);
    mfgunlock::xemfg::native::ControlStatus cs;
    if(exclusiveCreate) {
      // Created in exclusive fullscreen: a windowed Intel output, served borderless.
      pump();
      Require(controls(&cs,sizeof(cs)) && cs.exclusiveServed && cs.exclusiveRequests==1,"exclusive creation was not served as borderless");
      Require(coversMonitor(),"the window does not cover the monitor after an exclusive creation");
      BOOL full{}; chain->GetFullscreenState(&full,nullptr); Require(full,"the game was not told it is in fullscreen");
      // Alt+Enter is held back before DXGI sees it.
      PostMessageW(hwnd,WM_SYSKEYDOWN,VK_RETURN,LPARAM(1u<<29)|1); pump();
      Require(controls(&cs,sizeof(cs)) && cs.altEnterBlocked==1,"Alt+Enter was not held back");
      const auto servedReady=run(24);
      std::cout<<"Exclusive creation served borderless; ready frames="<<servedReady<<std::endl;
      Require(servedReady>=16,"frames were not generated while served borderless");
      Check(chain->SetFullscreenState(FALSE,nullptr),"leave served fullscreen"); pump();
      chain->GetFullscreenState(&full,nullptr); Require(!full,"the game still sees fullscreen after leaving");
      Require(controls(&cs,sizeof(cs)) && !cs.exclusiveServed,"served state kept after leaving");
      readyFrames=run(24);
      exclusive=false; // The transition below is covered by the other cases.
    }
    if(exclusive) {
      Require(exclusiveFirst || readyFrames>=16,"DLSS-SR frames did not reach ready before the transition");
      // A game's sequence: target mode, fullscreen, target again, buffers at the mode size.
      ComPtr<IDXGIOutput> output; Check(chain->GetContainingOutput(&output),"containing output");
      DXGI_OUTPUT_DESC od{}; output->GetDesc(&od);
      DXGI_MODE_DESC mode{}; mode.Width=UINT(od.DesktopCoordinates.right-od.DesktopCoordinates.left);
      mode.Height=UINT(od.DesktopCoordinates.bottom-od.DesktopCoordinates.top); mode.Format=desc.Format;
      DXGI_MODE_DESC closest{}; Check(output->FindClosestMatchingMode(&mode,&closest,nullptr),"closest mode");
      std::cout<<"Mode "<<closest.Width<<"x"<<closest.Height<<" @ "<<closest.RefreshRate.Numerator<<"/"<<closest.RefreshRate.Denominator<<std::endl;
      Check(chain->ResizeTarget(&closest),"ResizeTarget");
      pump(); GetWindowRect(hwnd,&originalRect); originalStyle=GetWindowLongPtrW(hwnd,GWL_STYLE);
      const auto entered=chain->SetFullscreenState(TRUE,output.Get());
      BOOL full{}; chain->GetFullscreenState(&full,nullptr);
      std::cout<<"SetFullscreenState(TRUE)=0x"<<std::hex<<uint32_t(entered)<<std::dec<<" fullscreen="<<full<<std::endl;
      if(SUCCEEDED(entered)) {
        DXGI_MODE_DESC again=closest; again.RefreshRate={};
        Check(chain->ResizeTarget(&again),"exclusive ResizeTarget");
        Check(chain->ResizeBuffers(0,closest.Width,closest.Height,DXGI_FORMAT_UNKNOWN,desc.Flags),"exclusive ResizeBuffers");
      }
      if(borderless) {
        pump();
        Require(entered==S_OK && full,"the served request did not report fullscreen to the game");
        Require(controls(&cs,sizeof(cs)) && cs.exclusiveServed && cs.exclusiveRequests==1,"exclusive request was not served as borderless");
        Require(coversMonitor(),"the window does not cover the monitor while served borderless");
      }
      const auto exclusiveReady=run(24);
      std::cout<<"Exclusive fullscreen ready frames="<<exclusiveReady<<std::endl;
      if(full) {
        Check(chain->SetFullscreenState(FALSE,nullptr),"leave exclusive fullscreen");
        Check(chain->ResizeBuffers(0,desc.Width,desc.Height,DXGI_FORMAT_UNKNOWN,desc.Flags),"windowed ResizeBuffers");
      }
      if(borderless) {
        pump();
        RECT now{}; GetWindowRect(hwnd,&now);
        Require(EqualRect(&now,&originalRect) && GetWindowLongPtrW(hwnd,GWL_STYLE)==originalStyle,"the window was not restored after leaving");
        Require(controls(&cs,sizeof(cs)) && !cs.exclusiveServed,"served state kept after leaving");
      }
      readyFrames=run(24);
      std::cout<<"Windowed again ready frames="<<readyFrames<<std::endl;
      // Without the option, XeFG presents the game's frames only; with it,
      // generation continues in the served borderless window.
      if(borderless) Require(exclusiveReady>=16,"frames were not generated while served borderless");
      else Require(!full || exclusiveReady==0,"frames were generated in exclusive fullscreen");
    }
    mfgunlock::xemfg::Status ms; modelStatus(&ms,sizeof(ms));
    std::cout<<"DLSS-SR ready frames="<<readyFrames<<" evaluations="<<evaluations()<<" presented="<<ms.presented
        <<" generated="<<ms.generated<<" result="<<ms.result<<" detail="<<ready.detail<<std::endl;
    Require(evaluations()==frame,"DLSS-SR itself did not run every frame");
    Require(readyFrames>=16,"DLSS-SR frames did not reach ready");
    // A covered or unfocused window may have one generated frame dropped by DWM,
    // as in the Streamline check of xemfg_native_smoke; all were generated.
    // The exclusive cases show their window; when Windows refuses the mode
    // (window not foreground), DWM may show only the source frame of the last
    // Present (presented=1) while XeFG still generated every frame.
    Require(ms.generated==multiplier-1 && (exclusiveFirst || exclusive || ms.presented>=multiplier-1) && ms.result==0,"DLSS-SR output presented no generated frames");
    Require(NVSDK_NGX_SUCCEED(release(feature)),"DLSS-SR release");
    chain.Reset(); DestroyWindow(hwnd);
    Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"log flush");
    std::cout<<"PASS: DLSS-SR adapter reached ready"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
