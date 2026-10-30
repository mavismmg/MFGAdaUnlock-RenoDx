// SPDX-License-Identifier: MIT
// Own COM identity outside the Intel proxy. ReShade wraps the inner DXGI
// chain, so its effects/overlay run on the final output, after interpolation.
#include "native_internal.hpp"
#include "native_synthetic.hpp"
namespace mfgunlock::xemfg::native {
constexpr DXGI_FORMAT kScrgbFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
bool PrepareScrgb(Output& o,IDXGISwapChain4* chain) {
  HRESULT hr=S_OK;
  if(!o.scrgbEncoder) {
    auto encoder=std::make_unique<ScrgbEncoder>();
    if(!encoder->Initialize(o.device.Get())) hr=encoder->LastError(); else o.scrgbEncoder=std::move(encoder);
  }
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if(SUCCEEDED(hr)) hr=chain->GetDesc1(&desc);
  if(SUCCEEDED(hr)) hr=chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
  o.scrgbBuffers.clear();
  // The game's view of the chain: FP16 buffers that behave like swapchain
  // buffers (render-target capable, resting in PRESENT).
  for(UINT i=0;SUCCEEDED(hr) && i<desc.BufferCount;++i) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=desc.Width; d.Height=desc.Height;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=kScrgbFormat; d.SampleDesc.Count=1;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|((desc.BufferUsage&DXGI_USAGE_UNORDERED_ACCESS)?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> buffer;
    hr=o.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_PRESENT,nullptr,IID_PPV_ARGS(&buffer));
    if(SUCCEEDED(hr)) o.scrgbBuffers.push_back(std::move(buffer));
  }
  o.scrgb=SUCCEEDED(hr);
  if(!o.scrgb) o.scrgbBuffers.clear();
  xess_mod::WriteLog("native_scrgb_ready","\"output\":"+std::to_string(o.diagnosticOutput)+",\"result\":"+std::to_string(int32_t(hr))
      +",\"buffers\":"+std::to_string(o.scrgbBuffers.size())+",\"width\":"+std::to_string(desc.Width)+",\"height\":"+std::to_string(desc.Height));
  return o.scrgb;
}
class Swapchain final : public IDXGISwapChain4 {
  std::atomic<ULONG> references{1};
  ComPtr<IDXGISwapChain4> chain;
  std::shared_ptr<Output> owner;
  // scRGB mode keeps the Intel chain in HDR10; a resize to another format
  // leaves it, a resize to FP16 enters it.
  DXGI_FORMAT IntelFormat(DXGI_FORMAT requested) {
    if(requested==kScrgbFormat || (requested==DXGI_FORMAT_UNKNOWN && owner->scrgb)) return ScrgbEncoder::kTargetFormat;
    return requested;
  }
  void ResizedScrgb(HRESULT hr,DXGI_FORMAT requested) {
    if(FAILED(hr)) return;
    std::lock_guard lock(owner->io);
    if(requested==kScrgbFormat || (requested==DXGI_FORMAT_UNKNOWN && owner->scrgb)) PrepareScrgb(*owner,chain.Get());
    else if(requested!=DXGI_FORMAT_UNKNOWN) { owner->scrgb=false; owner->scrgbBuffers.clear(); }
  }
  // The game's buffer becomes Intel's backbuffer: encoded before XeFG reads it.
  void EncodeScrgb() {
    const UINT index=chain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> target;
    bool ok=index<owner->scrgbBuffers.size() && SUCCEEDED(chain->GetBuffer(index,IID_PPV_ARGS(&target)))
        && owner->scrgbEncoder->Encode(owner->queue.Get(),owner->scrgbBuffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,
            target.Get(),D3D12_RESOURCE_STATE_PRESENT);
    if(owner->scrgbLogBudget<4 || (!ok && owner->scrgbLogBudget<12)) {
      ++owner->scrgbLogBudget;
      xess_mod::WriteLog("native_scrgb","\"index\":"+std::to_string(index)+",\"ok\":"+(ok?"true":"false")
          +",\"hr\":"+std::to_string(int32_t(owner->scrgbEncoder->LastError())));
    }
  }
  void Resized(HRESULT hr) {
    if(FAILED(hr)) return;
    const auto display=ReadFullscreen(chain.Get());
    if(owner->window) {
      owner->window->ObserveFullscreen(display.known,display.exclusive);
    }
    std::lock_guard lock(owner->io);
    owner->presentationSupported=display.known && !display.exclusive;
    DXGI_SWAP_CHAIN_DESC1 desc{}; chain->GetDesc1(&desc);
    owner->width=desc.Width; owner->height=desc.Height; owner->format=desc.Format;
    owner->compatible=desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM
        || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;
    ++owner->epoch; owner->frames.clear(); owner->last=0; owner->enabled=false;
    owner->uiCompositionKnown=false; owner->uiCompositionAvailable=false;
    State().api.xefgSwapChainSetEnabled(owner->fg,0);
  }
public:
  Swapchain(ComPtr<IDXGISwapChain4> value,std::shared_ptr<Output> o):chain(std::move(value)),owner(std::move(o)){}
  ~Swapchain() {
    // Retire the presentation owner now, even when an in-flight source sleep
    // or diagnostic snapshot still pins Output and its separate XeLL context.
    // The versioned peer handshake runs while the Intel public proxy is still
    // owned. Retirement either releases it before Destroy or quarantines both.
    owner->RetirePresentation(std::move(chain));
    { auto& s=State(); std::lock_guard lock(s.lock);
      if(s.output.lock()==owner) {
        s.output.reset(); s.hasOutput=false; s.status.output=s.status.xell=s.status.enabled=s.status.inputs=false;
        s.status.submitted=1; s.status.sdkResult=0;
        s.viewport=UINT32_MAX; s.recording=0; s.presenting=0;
      }
    }
    owner.reset();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
    if(!out) return E_POINTER; *out=nullptr;
    if(id==__uuidof(IUnknown)||id==__uuidof(IDXGIObject)||id==__uuidof(IDXGIDeviceSubObject)
        ||id==__uuidof(IDXGISwapChain)||id==__uuidof(IDXGISwapChain1)||id==__uuidof(IDXGISwapChain2)
        ||id==__uuidof(IDXGISwapChain3)||id==__uuidof(IDXGISwapChain4)) {
      *out=static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
    }
    // Do not leak an interface that bypasses Present and lifetime ownership.
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { auto n=--references; if(!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a,UINT b,const void* c) override { return chain->SetPrivateData(a,b,c); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a,const IUnknown* b) override { return chain->SetPrivateDataInterface(a,b); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a,UINT* b,void* c) override { return chain->GetPrivateData(a,b,c); }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID a,void** b) override { return chain->GetParent(a,b); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID a,void** b) override { return chain->GetDevice(a,b); }
  HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {
    if(flags&DXGI_PRESENT_TEST) return chain->Present(sync,flags);
    return SourcePresent(sync,flags,nullptr,false);
  }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT a,REFIID b,void** c) override {
    if(owner->scrgb) {
      if(!c) return E_POINTER;
      *c=nullptr;
      return a<owner->scrgbBuffers.size()?owner->scrgbBuffers[a]->QueryInterface(b,c):DXGI_ERROR_INVALID_CALL;
    }
    return chain->GetBuffer(a,b,c);
  }
  // 1.5.8: XeFG does not support exclusive fullscreen. With the option on, a
  // request for it is served as borderless: the Intel chain stays windowed,
  // the window covers the monitor, and the game is told it is in fullscreen.
  // Leaving restores the window. A chain already exclusive (option turned on
  // later) keeps the original path until the game leaves it.
  bool ServeExclusive(BOOL fullscreen,IDXGIOutput* target) {
    if(!owner->exclusiveServed && (!fullscreen || !ExclusiveAsBorderless())) return false;
    if(ReadFullscreen(chain.Get()).exclusive) return false;
    { std::lock_guard lock(owner->io); owner->exclusiveTarget=fullscreen?target:nullptr; }
    const bool was=owner->exclusiveServed.exchange(fullscreen!=FALSE);
    if(fullscreen && !was) ++State().exclusiveRequests;
    if(owner->window) owner->window->Request(fullscreen!=FALSE);
    xess_mod::WriteLog("native_exclusive_as_borderless","\"output\":"+std::to_string(owner->diagnosticOutput)
        +",\"fullscreen\":"+(fullscreen?"true":"false")+",\"window\":"+(owner->window?"true":"false"));
    return true;
  }
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a,IDXGIOutput* b) override {
    if(ServeExclusive(a,b)) return S_OK;
    const auto hr=chain->SetFullscreenState(a,b);
    if(SUCCEEDED(hr)) {
      const auto display=ReadFullscreen(chain.Get());
      if(owner->window) owner->window->ObserveFullscreen(display.known,display.exclusive);
      std::lock_guard lock(owner->io);
      const bool supported=display.known && !display.exclusive;
      if(supported!=owner->presentationSupported) {
        owner->presentationSupported=supported;
        ++owner->epoch;owner->frames.clear();owner->last=0;
      }
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a,IDXGIOutput** b) override {
    if(!owner->exclusiveServed) return chain->GetFullscreenState(a,b);
    if(a) *a=TRUE;
    if(b) {
      { std::lock_guard lock(owner->io); *b=owner->exclusiveTarget.Get(); }
      if(*b) (*b)->AddRef();
      else if(FAILED(chain->GetContainingOutput(b))) *b=nullptr;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a) override {
    const auto hr=chain->GetDesc(a);
    if(SUCCEEDED(hr) && a && owner->scrgb) a->BufferDesc.Format=kScrgbFormat;
    if(SUCCEEDED(hr) && a && owner->exclusiveServed) a->Windowed=FALSE;
    return hr;
  }
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a,UINT b,UINT c,DXGI_FORMAT d,UINT e) override {
    auto hr=chain->ResizeBuffers(a,b,c,IntelFormat(d),e); Resized(hr); ResizedScrgb(hr,d); return hr;
  }
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a) override {
    // Served borderless: the window keeps covering the monitor; no mode change.
    if(owner->exclusiveServed) return a?S_OK:DXGI_ERROR_INVALID_CALL;
    return chain->ResizeTarget(a);
  }
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** a) override { return chain->GetContainingOutput(a); }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* a) override { return chain->GetFrameStatistics(a); }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* a) override { return chain->GetLastPresentCount(a); }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* a) override {
    const auto hr=chain->GetDesc1(a); if(SUCCEEDED(hr) && a && owner->scrgb) a->Format=kScrgbFormat; return hr;
  }
  HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a) override {
    const auto hr=chain->GetFullscreenDesc(a); if(SUCCEEDED(hr) && a && owner->exclusiveServed) a->Windowed=FALSE; return hr;
  }
  HRESULT STDMETHODCALLTYPE GetHwnd(HWND* a) override { return chain->GetHwnd(a); }
  HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID a,void** b) override { return chain->GetCoreWindow(a,b); }
  HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) override {
    if(flags&DXGI_PRESENT_TEST) return chain->Present1(sync,flags,params);
    return SourcePresent(sync,flags,params,true);
  }
  HRESULT SourcePresent(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params,bool extended) {
    const auto frame=State().presenting.load();
    // XeFG presents generated frames with the game's sync interval; game VSync
    // (Elden Ring has no option for it) keeps flip metering and extra pacing off.
    State().gameSyncInterval.store(sync,std::memory_order_relaxed);
    if(sync && State().requested.load()) {
      static std::atomic<bool> reported{};
      if(!reported.exchange(true)) xess_mod::WriteLog("native_game_vsync","\"sync_interval\":"+std::to_string(sync)
          +",\"force_vsync_off\":"+std::to_string(ForceVSyncOff()));
    }
    if(sync && ForceVSyncOff()) {
      static std::atomic<bool> logged{};
      if(!logged.exchange(true)) xess_mod::WriteLog("native_vsync_forced_off","\"game_sync_interval\":"+std::to_string(sync));
      sync=0;
    }
    RuntimeCall whole("source_present",owner->diagnosticOutput,frame);
    CpuProbe probe; std::unique_lock lock(owner->presents,std::defer_lock);
    { RuntimeCall trace("present_lock",owner->diagnosticOutput,frame);lock.lock(); }
    probe.Locked();
    if(GpuTraceEnabled() && !owner->gpuTrace) owner->gpuTrace=std::make_unique<QueueTrace>(owner->device.Get(),owner->queue.Get(),owner->diagnosticOutput);
    if(owner->gpuTrace) owner->gpuTrace->Begin(frame);
    if(owner->scrgb) { RuntimeCall trace("scrgb_encode",owner->diagnosticOutput,frame);EncodeScrgb(); }
    synthetic::BeforeSourcePresent(owner);
    { RuntimeCall trace("prepare_present",owner->diagnosticOutput,frame);BeforePresent(owner); }
    probe.Prepared();
    HRESULT hr;
    LARGE_INTEGER presentBegin{},presentEnd{}; QueryPerformanceCounter(&presentBegin);
    { RuntimeCall trace("xefg_present",owner->diagnosticOutput,frame);
      hr=extended?chain->Present1(sync,flags,params):chain->Present(sync,flags);trace.Result(hr); }
    QueryPerformanceCounter(&presentEnd);
    owner->summaryPresentUs+=QpcMicroseconds(presentEnd.QuadPart-presentBegin.QuadPart); ++owner->summaryPresentCalls;
    probe.Returned();
    if(owner->gpuTrace) owner->gpuTrace->End();
    { RuntimeCall trace("after_present",owner->diagnosticOutput,frame);AfterPresent(owner,&probe,hr); }
    synthetic::AfterSourcePresent(owner);
    whole.Result(hr);return hr;
  }
  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return chain->IsTemporaryMonoSupported(); }
  HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** a) override { return chain->GetRestrictToOutput(a); }
  HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* a) override { return chain->SetBackgroundColor(a); }
  HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* a) override { return chain->GetBackgroundColor(a); }
  HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION a) override { return chain->SetRotation(a); }
  HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* a) override { return chain->GetRotation(a); }
  HRESULT STDMETHODCALLTYPE SetSourceSize(UINT a,UINT b) override { return chain->SetSourceSize(a,b); }
  HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* a,UINT* b) override { return chain->GetSourceSize(a,b); }
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT a) override { return chain->SetMaximumFrameLatency(a); }
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* a) override { return chain->GetMaximumFrameLatency(a); }
  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return chain->GetFrameLatencyWaitableObject(); }
  HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* a) override { return chain->SetMatrixTransform(a); }
  HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* a) override { return chain->GetMatrixTransform(a); }
  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return chain->GetCurrentBackBufferIndex(); }
  // In scRGB mode the game's linear BT.709 space is what the encoder expects;
  // the Intel chain itself stays in HDR10.
  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE a,UINT* b) override {
    if(owner->scrgb && b && a==DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) { *b=DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT; return S_OK; }
    return chain->CheckColorSpaceSupport(a,b);
  }
  HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE a) override {
    if(owner->scrgb) return a==DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709?S_OK:DXGI_ERROR_UNSUPPORTED;
    return chain->SetColorSpace1(a);
  }
  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT a,UINT b,UINT c,DXGI_FORMAT d,UINT e,const UINT* f,IUnknown*const* g) override {
    auto hr=chain->ResizeBuffers1(a,b,c,IntelFormat(d),e,f,g); Resized(hr); ResizedScrgb(hr,d); return hr;
  }
  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE a,UINT b,void* c) override { return chain->SetHDRMetaData(a,b,c); }
};
IDXGISwapChain1* Wrap(ComPtr<IDXGISwapChain4> chain,std::shared_ptr<Output> owner) {
  return new Swapchain(std::move(chain),std::move(owner));
}
}
