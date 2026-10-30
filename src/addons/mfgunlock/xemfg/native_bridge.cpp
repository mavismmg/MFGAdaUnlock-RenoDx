// SPDX-License-Identifier: MIT
// Independent Streamline/DXGI transport. No OptiScaler source is used here.
#include "native_internal.hpp"
#include "native_session.hpp"
#include "log_budget.hpp"
#include "native_profiles.hpp"
#include "native_overlay.hpp"
#include "native_factory.hpp"
#include "native_continuity.hpp"
#include "native_reflex_profile.hpp"
#include "backend_api.hpp"
#include "native_ffx.hpp"
#include "native_ngx.hpp"
#include "../backend_selection.hpp"
#include "vendor/third_party/minhook/include/MinHook.h"
#include <deps/imgui/imgui.h>
#include <reshade.hpp>
#include <set>
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif

namespace mfgunlock::xemfg::native {
// Intel excludes CreateContext, Init and Destroy from its thread-safe API
// contract. Also serialize output selection with initialization: two factory
// threads must not both observe an empty Active() before publishing an owner.
static std::recursive_mutex sdkLifecycle;
Bridge& State() { static auto* s=new Bridge; return *s; }
Status Snapshot() { auto& s=State(); std::lock_guard lock(s.lock); auto status=s.status; status.outputPresents=s.outputPresents; return status; }
void Report(const char* reason) {
  auto& s=State(); std::lock_guard lock(s.lock);
  if(strcmp(s.status.detail,reason)==0) return;
  strncpy_s(s.status.detail,reason,_TRUNCATE);
  xess_mod::WriteLog("native_bridge","\"detail\":\""+escape(reason)+"\"");
}
std::shared_ptr<Output> Active() {
  auto& s=State(); std::lock_guard lock(s.lock); return s.output.lock();
}
uint32_t WindowRefreshMilliHz(HWND window) noexcept {
  const auto monitor=window?MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST):nullptr;
  if(!monitor) return 0;
  MONITORINFOEXW info{}; info.cbSize=sizeof(info); DEVMODEW mode{}; mode.dmSize=sizeof(mode);
  return GetMonitorInfoW(monitor,&info) && EnumDisplaySettingsW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode)
      && mode.dmDisplayFrequency>1 && mode.dmDisplayFrequency<=1000?mode.dmDisplayFrequency*1000:0;
}
// 1.7.1 (plan B.3): the display cap of a Vulkan game's window; the panel reads the refresh.
uint32_t DisplayCapIntervalForWindow(HWND window) noexcept {
  State().displayRefreshMilliHz=WindowRefreshMilliHz(window);
  return DisplayCapIntervalForOutput();
}
bool Hook(void* entry,void* replacement,void** original) {
  static std::recursive_mutex lock; std::lock_guard guard(lock);
  static std::set<void*> installed;
  if(!entry) return false;
  if(installed.contains(entry)) return *original!=nullptr;
  const auto init=MH_Initialize();
  if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
  if(MH_CreateHook(entry,replacement,original)!=MH_OK) return false;
  if(MH_EnableHook(entry)!=MH_OK) { MH_RemoveHook(entry); *original=nullptr; return false; }
  installed.insert(entry); return true;
}
bool Api::Load() {
  if(!EnsureModel()) return false;
  NativeRuntimePair selected;
  if(!NativeRuntimeModules(selected)) return false;
  auto fg=static_cast<HMODULE>(selected.frameGeneration),ll=static_cast<HMODULE>(selected.latency);
  fgModule=fg; llModule=ll;
#define F(n,m) n=reinterpret_cast<decltype(n)>(GetProcAddress(m,#n)); if(!n) return false;
  F(xefgSwapChainD3D12CreateContext,fg) F(xefgSwapChainD3D12InitFromSwapChainDesc,fg)
  F(xefgSwapChainD3D12GetSwapChainPtr,fg) F(xefgSwapChainD3D12TagFrameResource,fg)
  F(xefgSwapChainTagFrameConstants,fg) F(xefgSwapChainSetPresentId,fg)
  F(xefgSwapChainSetEnabled,fg) F(xefgSwapChainSetNumInterpolatedFrames,fg)
  F(xefgSwapChainSetLatencyReduction,fg) F(xefgSwapChainGetLastPresentStatus,fg)
  F(xefgSwapChainDestroy,fg) F(xefgSwapChainSetUiCompositionState,fg)
  F(xefgSwapChainSetLoggingCallback,fg)
  F(xellD3D12CreateContext,ll) F(xellSetSleepMode,ll) F(xellSleep,ll)
  F(xellAddMarkerData,ll) F(xellDestroyContext,ll)
#undef F
  xellSetLoggingCallback=reinterpret_cast<decltype(xellSetLoggingCallback)>(GetProcAddress(ll,"xellSetLoggingCallback"));
  xellGetVersion=reinterpret_cast<decltype(xellGetVersion)>(GetProcAddress(ll,"xellGetVersion"));
  return true;
}
void Output::RetirePresentation(ComPtr<IDXGISwapChain4> publicProxy) {
  bool pendingSleep{};
  {
    // Drain all admitted source Present/input calls, then close admission and
    // invalidate in-flight sleep tokens. External cooperation, COM Release and
    // SDK Destroy run outside these locks: REF may call back on another thread.
    std::lock_guard presenting(presents);
    std::lock_guard inputs(io);
    if(retired) return;
    retired=true; enabled=false; ++epoch;
    pendingSleep=sleepActive;
    LogTagHistogram(*this,"retired");
  }
  ExportCompatibilityReport("retired");
  auto& a=State().api;
  if(window) window->Retire();
  if(fg && publicProxy) {
    try {reframeworkPeer=reframework::Resolve(reframeworkPeer);} // also discovers late loads
    catch(...) {
      quarantinedProxy=std::move(publicProxy);State().retirementFailed=true;
      Report("REFramework discovery failed during XeMFG retirement; restart required");return;
    }
    RuntimeCall trace("reframework_pre_retire",diagnosticOutput);
    const auto decision=reframework::Prepare(reframeworkPeer,publicProxy.Get(),fg,hwnd);
    trace.Result(int(decision),!reframework::Safe(decision));
    const auto peer=reframework::Snapshot();
    xess_mod::WriteLog("native_reframework_retire","\"output\":"+std::to_string(diagnosticOutput)
        +",\"v1\":"+(peer.v1?"true":"false")+",\"decision\":"+std::to_string(int(decision))
        +",\"raw_result\":"+std::to_string(peer.rawResult)+",\"path\":\""+escape(peer.path)+"\",\"sha256\":\""+peer.sha256+"\"");
    if(!reframework::Safe(decision)) {
      quarantinedProxy=std::move(publicProxy);
      State().retirementFailed=true;
      Report("REFramework did not authorize XeMFG output retirement; restart the game before creating another output");
      return; // no proxy Release, disable, Destroy, or automatic retry
    }
  }
  publicProxy.Reset(); // release the public proxy before destroying its context
  if(fg) {
    std::lock_guard lifecycle(sdkLifecycle);
    RuntimeCall trace("xefg_destroy",diagnosticOutput);
    const auto disabled=a.xefgSwapChainSetEnabled(fg,0);
    const auto result=a.xefgSwapChainDestroy(fg);
    ConfirmNativeRetirement(hwnd,result==0);
    if(result!=0)State().retirementFailed=true;
    trace.Result(result);
    xess_mod::WriteLog("native_output_retired","\"output\":"+std::to_string(diagnosticOutput)
        +",\"disable_result\":"+std::to_string(disabled)+",\"destroy_result\":"+std::to_string(result)
        +",\"sleep_in_flight\":"+(pendingSleep?"true":"false"));
    if(result==0) {std::lock_guard inputs(io);fg=nullptr;}
  }
}
Output::~Output() {
  RetirePresentation();
  auto& a=State().api;
  if(fg) {
      // Preserve every dependency if the SDK refuses retirement. Destruction
      // must not convert a leaked SDK reference into a freed XeLL/device.
      struct Pending {
        ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGIFactory2> factory; xell_context_handle_t ll;
        ComPtr<IDXGISwapChain4> proxy; std::shared_ptr<reframework::Peer> peer;
        FrameStore<Frame> frames;
      };
      new Pending{device,queue,factory,ll,std::move(quarantinedProxy),std::move(reframeworkPeer),std::move(frames)};
      xess_mod::WriteLog("native_retirement_pending","\"context\":"+std::to_string(uintptr_t(fg)));
      return;
  }
  if(ll) { std::lock_guard lifecycle(sdkLifecycle);a.xellDestroyContext(ll); }
}
HRESULT CreateOutput(IDXGIFactory2* factory,IUnknown* source,HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
    IDXGIOutput* restrictOutput,IDXGISwapChain1** result) {
  RuntimeCall createTrace("create_output");
  if(!result || !desc || !hwnd || restrictOutput) return E_NOINTERFACE;
  // 1.5.8: with Exclusive fullscreen as borderless, a swapchain created in
  // exclusive fullscreen becomes a windowed Intel output served as borderless.
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowedFull{};
  const bool serveExclusive=full && !full->Windowed && ExclusiveAsBorderless();
  if(serveExclusive) { windowedFull=*full; windowedFull.Windowed=TRUE; full=&windowedFull; }
  if(full && !full->Windowed) {
    Report("XeMFG is unavailable in exclusive fullscreen; the game's display mode was preserved");
    return E_NOINTERFACE;
  }
  // FP16 is scRGB: XeFG supports HDR only as RGB10A2 HDR10, so the Intel chain
  // is created in that format and the game keeps an FP16 view (plan 4.4).
  const bool scrgb=desc->Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
  if(desc->Format!=DXGI_FORMAT_R10G10B10A2_UNORM && desc->Format!=DXGI_FORMAT_R8G8B8A8_UNORM
      && desc->Format!=DXGI_FORMAT_B8G8R8A8_UNORM && !scrgb) {
    Report("XeMFG: unsupported swapchain format");
    return E_NOINTERFACE;
  }
  DXGI_SWAP_CHAIN_DESC1 intelDesc=*desc;
  if(scrgb) intelDesc.Format=ScrgbEncoder::kTargetFormat;
  // Capability/identity discovery is confined to output creation/retirement.
  // No REFramework is loaded and no per-frame probing or hashing is added.
  if(State().retirementFailed) {Report("XeMFG output retirement was not confirmed; restart required");return E_FAIL;}
  auto peer=reframework::Resolve();
  if(!reframework::Safe(peer->info.result)) {
    Report("XeMFG cannot identify a single REFramework peer safely; keeping the original presentation path");return E_NOINTERFACE;
  }
  if(peer->info.present) xess_mod::WriteLog("native_reframework_capability","\"v1\":"+(peer->info.v1?std::string("true"):std::string("false"))
      +",\"known_monitor_issue\":"+(peer->info.knownMonitorIssue?"true":"false")
      +",\"path\":\""+escape(peer->info.path)+"\",\"sha256\":\""+peer->info.sha256+"\",\"version\":\""+escape(peer->info.version)+"\"");
  std::unique_lock lifecycle(sdkLifecycle,std::defer_lock);
  { RuntimeCall trace("sdk_lifecycle_lock");lifecycle.lock(); }
  if(State().retirementFailed) {Report("XeMFG context retirement failed; restart the game before creating another output");return E_FAIL;}
  if(Active()) return E_NOINTERFACE; // One selected Streamline viewport/output in this implementation.
  if(!OwnsPresentationHooks()) {
    Report("XeMFG needs early Streamline initialization to own presentation; restart with the addon loaded before slInit");
    return E_NOINTERFACE;
  }
  ComPtr<ID3D12CommandQueue> queue;
  if(!source || FAILED(source->QueryInterface(IID_PPV_ARGS(&queue)))) return E_NOINTERFACE;
  ComPtr<ID3D12CommandQueue> transportQueue;
  ComPtr<IDXGIFactory2> transportFactory;
  // The public unwrapping contracts identify the same underlying queue and
  // factory. Retain those identities, not a presentation wrapper owned by a
  // different layer, inside Intel's long-lived context.
  if(!single_overlay::native::Unwrap(queue.Get(),transportQueue)
      || !single_overlay::native::Unwrap(factory,transportFactory)) {
    Report("XeMFG cannot identify the presentation queue/factory safely"); return E_NOINTERFACE;
  }
  // FinalOutputFactory observes the SDK call explicitly, including another
  // thread/vtable implementation. Keeping an outer SL factory here would
  // return another proxy and prevent admission of the native final renderer.
  const bool queueChanged=queue.Get()!=transportQueue.Get(),factoryChanged=factory!=transportFactory.Get();
  queue=std::move(transportQueue);
  if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return E_NOINTERFACE;
  auto& s=State();
  if(!s.api.Load()) { Report("XeMFG provider pair or model hooks unavailable"); return E_NOINTERFACE; }
  auto o=std::make_shared<Output>(); o->queue=queue;o->hwnd=hwnd;
  o->reframeworkPeer=std::move(peer);
  static std::atomic<uint64_t> outputGeneration{0}; o->diagnosticOutput=++outputGeneration;
  LogModules(o->diagnosticOutput);
  xess_mod::WriteLog("native_transport_identity","\"output\":"+std::to_string(o->diagnosticOutput)
      +",\"queue_unwrapped\":"+(queueChanged?"true":"false")+",\"factory_unwrapped\":"+(factoryChanged?"true":"false"));
  ComPtr<ID3D12Device> device;
  if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))
      || !single_overlay::native::Unwrap(device.Get(),o->device)) return E_NOINTERFACE;
  o->invert=s.invert; o->jitter=s.jitter; o->width=desc->Width; o->height=desc->Height; o->format=intelDesc.Format;
#if !defined(XESS_INIT_PROBE)
  if(o->jitter) {
    // The portable program set has no d243=1 (jittered) reconstruction variant
    // (decoder key 35112441167526893749578). Jittered vectors are converted by
    // our own pass and XeFG runs its unjittered program (plan 4.2).
    auto dejitter=std::make_unique<MotionDejitter>();
    const bool initialized=dejitter->Initialize(o->device.Get());
    xess_mod::WriteLog("native_dejitter_ready","\"output\":"+std::to_string(o->diagnosticOutput)
        +",\"result\":"+std::to_string(initialized?0:int32_t(dejitter->LastError())));
    if(!initialized) { Report("XeMFG: the motion-vector dejitter pass could not be created; inspect the XeMFG log"); return E_NOINTERFACE; }
    o->dejitter=std::move(dejitter);
  }
#endif
  auto& a=s.api;
  if(a.xellD3D12CreateContext(o->device.Get(),&o->ll)!=XELL_RESULT_SUCCESS) {
    Report("XeLL D3D12 context creation failed"); return E_NOINTERFACE;
  }
  {
    // Warnings and errors explain refused sleeps and markers (for example
    // "Sleep() called after markers"); debug level only with stall tracing.
    xell_version_t version{};
    const auto versionResult=a.xellGetVersion?a.xellGetVersion(&version):XELL_RESULT_ERROR_NOT_IMPLEMENTED;
    const auto logging=a.xellSetLoggingCallback?a.xellSetLoggingCallback(o->ll,TraceEnabled()?XELL_LOGGING_LEVEL_DEBUG:XELL_LOGGING_LEVEL_WARNING,
        [](const char* message,xell_logging_level_t level) {
          static PatternBudget budget(48);
          const std::string text=message?message:"";
          if(const auto count=budget.Admit(PatternBudget::Pattern(text)))
            xess_mod::WriteLog("native_xell_log","\"level\":"+std::to_string(level)+",\"message\":\""+escape(text)
                +"\",\"occurrence\":"+std::to_string(count)+",\"fg_requested\":"+(State().requested.load()?"true":"false"));
        }):XELL_RESULT_ERROR_NOT_IMPLEMENTED;
    xess_mod::WriteLog("native_xell_context","\"output\":"+std::to_string(o->diagnosticOutput)
        +",\"version\":\""+(versionResult==XELL_RESULT_SUCCESS?std::to_string(version.major)+'.'+std::to_string(version.minor)+'.'+std::to_string(version.patch):std::string("unavailable"))
        +"\",\"logging_result\":"+std::to_string(logging));
  }
  xell_sleep_params_t sleep{}; sleep.bLowLatencyMode=s.latencyOutput==LatencyOutput::Reflex?0:1;
  if(a.xellSetSleepMode(o->ll,&sleep)!=XELL_RESULT_SUCCESS) return E_NOINTERFACE;
  o->sleepConfigured=true; o->sleepLowLatency=sleep.bLowLatencyMode!=0; ++s.latencyModeUpdates;
  if(a.xefgSwapChainD3D12CreateContext(o->device.Get(),&o->fg)!=0) return E_NOINTERFACE;
  a.xefgSwapChainSetLoggingCallback(o->fg,TraceEnabled()?XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG:XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING,
      [](const char* message,xefg_swapchain_logging_level_t level,void*) {
        static PatternBudget budget(48);
        if(const auto count=AdmitSdkMessage(budget,message))
          xess_mod::WriteLog("native_sdk_log","\"level\":"+std::to_string(level)+",\"message\":\""+escape(message?message:"")
              +"\",\"occurrence\":"+std::to_string(count));
      },nullptr);
  const bool nativeReflex=s.latencyOutput==LatencyOutput::Reflex;
  if(nativeReflex && !reflex_profile::Available(a.fgModule)) {
    Report("Native Reflex output unavailable for this XeFG profile; select XeLL and restart");
    return E_NOINTERFACE;
  }
  const auto latencyResult=a.xefgSwapChainSetLatencyReduction(o->fg,
      nativeReflex?reflex_profile::ExternalSelector():static_cast<void*>(o->ll));
  if(latencyResult!=0) return E_NOINTERFACE;
  xess_mod::WriteLog("native_latency_binding",std::string("\"output\":\"")+(nativeReflex?"Reflex":"XeLL")+
      "\",\"external_profile\":"+(nativeReflex?"true":"false")+",\"result\":"+std::to_string(latencyResult));
  xefg_swapchain_d3d12_init_params_t init{};
  init.maxInterpolatedFrames=5;
  init.initFlags=(o->invert?XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH:0)
      |(o->jitter && !o->dejitter?XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV:0);
  init.uiMode=XEFG_SWAPCHAIN_UI_MODE_AUTO;
  o->factory=FinalOutputFactory(transportFactory.Get());
  const auto code=[&] {
    RuntimeCall trace("xefg_init",o->diagnosticOutput);
    const auto r=a.xefgSwapChainD3D12InitFromSwapChainDesc(o->fg,hwnd,&intelDesc,full,queue.Get(),o->factory.Get(),&init);
    trace.Result(r);return r;
  }();
  if(code!=0) {
    xess_mod::WriteLog("native_swapchain_init_failed","\"result\":"+std::to_string(code));
    Report("XeFG swapchain initialization failed; inspect the XeMFG log"); return E_NOINTERFACE;
  }
  xess_mod::WriteLog("native_swapchain_init","\"output\":"+std::to_string(o->diagnosticOutput)+",\"init_flags\":"+std::to_string(init.initFlags)
      +",\"motion_dejitter\":"+(o->dejitter?"true":"false")+",\"scrgb\":"+(scrgb?"true":"false"));
  ComPtr<IDXGISwapChain4> chain;
  if(a.xefgSwapChainD3D12GetSwapChainPtr(o->fg,IID_PPV_ARGS(&chain))!=0) return E_FAIL;
  DXGI_SWAP_CHAIN_DESC1 actual{};
  if(SUCCEEDED(chain->GetDesc1(&actual))) { o->width=actual.Width; o->height=actual.Height; o->format=actual.Format; }
  if(scrgb && !PrepareScrgb(*o,chain.Get())) { Report("XeMFG: the scRGB to HDR10 conversion could not be created; inspect the XeMFG log"); return E_NOINTERFACE; }
  using Debug=xefg_swapchain_result_t(*)(xefg_swapchain_handle_t,uint32_t,uint32_t,void*);
  if(auto debug=reinterpret_cast<Debug>(GetProcAddress(a.fgModule,"xefgSwapChainEnableDebugFeature")))
    debug(o->fg,0,0,nullptr);
  // Keep Intel's default (composition disabled). The user's opt-in preference
  // is consumed at Present, after the matching frame's inputs are known.
  a.xefgSwapChainSetEnabled(o->fg,0);
  BOOL exclusive{}; const bool known=SUCCEEDED(chain->GetFullscreenState(&exclusive,nullptr));
  SetLastError(0); o->window=WindowControl::Bind(hwnd,false,known,exclusive!=FALSE);
  if(!o->window) o->windowBindError=GetLastError();
  xess_mod::WriteLog("native_window_bind","\"hwnd\":"+std::to_string(uintptr_t(hwnd))
      +",\"thread\":"+std::to_string(GetCurrentThreadId())+",\"available\":"+(o->window?"true":"false")
      +",\"error\":"+std::to_string(o->windowBindError));
  if(o->window) o->window->BlockAltEnter(ExclusiveAsBorderless());
  if(serveExclusive) {
    o->exclusiveServed=true; ++s.exclusiveRequests;
    if(o->window) o->window->Request(true);
    xess_mod::WriteLog("native_exclusive_as_borderless","\"output\":"+std::to_string(o->diagnosticOutput)
        +",\"fullscreen\":true,\"at_creation\":true,\"window\":"+(o->window?"true":"false"));
  }
  *result=Wrap(std::move(chain),o);
  { std::lock_guard guard(s.lock); s.output=o; s.hasOutput=true; s.status.output=true; s.status.xell=true; }
  if(!ffx::CreatingOutput()) ngx::OutputCreated(o);
  Report("XeMFG swapchain ready; waiting for Streamline inputs and XeLL markers");
  return S_OK;
}

using FactoryCreate=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,
    const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
using LegacyFactoryCreate=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
static FactoryCreate originalFactory{};
static LegacyFactoryCreate originalLegacyFactory{};
static thread_local bool internalCreate{};
static thread_local HWND creatingWindow{};
struct CreationScope {
  bool previousInternal{internalCreate};HWND previousWindow{creatingWindow};
  explicit CreationScope(HWND window) { internalCreate=true; creatingWindow=window; }
  ~CreationScope() { internalCreate=previousInternal; creatingWindow=previousWindow; }
};
HRESULT CreateFinalNativeOutput(IDXGIFactory2* factory,IUnknown* queue,HWND window,const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGIOutput* restrictOutput,IDXGISwapChain1** out) {
  CreationScope scope(nullptr); // Also guards calls on a separate SDK thread.
  auto result=ReuseNativeOutput(queue,window,desc,out);
  if(result==DXGI_ERROR_NOT_FOUND) result=factory->CreateSwapChainForHwnd(queue,window,desc,full,restrictOutput,out);
  if(SUCCEEDED(result)) WrapFinalOutput(queue,window,out);
  return result;
}
static HRESULT STDMETHODCALLTYPE FactoryHook(IDXGIFactory2* f,IUnknown* d,HWND w,
    const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
    IDXGIOutput* restrictOutput,IDXGISwapChain1** out) {
  // Plan 5.2: the game's own XeFG creating its swapchain; never stack a second Intel output.
  if(GameOwnsFrameGeneration()) return originalFactory(f,d,w,desc,full,restrictOutput,out);
#ifdef MFGUNLOCK_XEMFG_VULKAN
  // 1.7 (plan F): a Vulkan game's DXGI chains are the driver's presentation.
  if(vulkan::GameUsesVulkan() || vulkan::InstanceSeen()) {
    const auto hr=originalFactory(f,d,w,desc,full,restrictOutput,out);
    HMODULE caller{}; wchar_t name[MAX_PATH]{};
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(_ReturnAddress()),&caller)) GetModuleFileNameW(caller,name,MAX_PATH);
    const wchar_t* leaf=wcsrchr(name,L'\\'); leaf=leaf?leaf+1:name;
    std::string module; for(const wchar_t* c=leaf;*c;++c) module+=*c<0x80?char(*c):'?';
    static std::atomic<int> budget{};
    if(budget.fetch_add(1)<16)
      xess_mod::WriteLog("vk_game_dxgi_swapchain","\"result\":"+std::to_string(uint32_t(hr))+",\"window\":"+std::to_string(uintptr_t(w))
          +",\"width\":"+std::to_string(desc?desc->Width:0)+",\"height\":"+std::to_string(desc?desc->Height:0)
          +",\"buffers\":"+std::to_string(desc?desc->BufferCount:0)+",\"format\":"+std::to_string(desc?int(desc->Format):0)
          +",\"caller\":\""+module+"\",\"internal\":"+(internalCreate?"true":"false"));
    return hr;
  }
#endif
  if(internalCreate) {
    const auto hr=originalFactory(f,d,w,desc,full,restrictOutput,out);
    if(SUCCEEDED(hr) && creatingWindow==w) WrapFinalOutput(d,w,out);
    return hr;
  }
  CreationScope scope(w);
  DiscoverStreamline();
  const auto hr=CreateOutput(f,d,w,desc,full,restrictOutput,out);
  if(hr!=E_NOINTERFACE) return hr;
  // Unsupported integration remains native with our NVIDIA requests forced off.
  // It is explicitly reported; never announce an active XeMFG output here.
  creatingWindow=nullptr; // Native fallback must not become Intel's final output.
  ClearRetainedNative(w);
  return originalFactory(f,d,w,desc,full,restrictOutput,out);
}
static HRESULT STDMETHODCALLTYPE LegacyFactoryHook(IDXGIFactory* factory,IUnknown* source,
    DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out) {
  if(GameOwnsFrameGeneration()) return originalLegacyFactory(factory,source,desc,out);
#ifdef MFGUNLOCK_XEMFG_VULKAN
  if(vulkan::GameUsesVulkan()) return originalLegacyFactory(factory,source,desc,out);
#endif
  if(internalCreate) {
    const auto hr=originalLegacyFactory(factory,source,desc,out);
    // Intel normally creates its native output through ForHwnd. Keep final
    // ReShade rendering intact if a runtime uses the legacy entrypoint too.
    if(SUCCEEDED(hr) && out && *out && desc && creatingWindow==desc->OutputWindow) {
      ComPtr<IDXGISwapChain1> modern;
      if(SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&modern)))) {
        WrapFinalOutput(source,desc->OutputWindow,modern.GetAddressOf());
        (*out)->Release(); *out=modern.Detach();
      }
    }
    return hr;
  }
  // Preserve native validation and the exact caller arguments for unsupported
  // integrations. A legacy API does not imply a D3D11 device: D3D12 supplies
  // its direct command queue to this entrypoint as well.
  if(!desc || !out || !desc->OutputWindow)
    return originalLegacyFactory(factory,source,desc,out);
  CreationScope scope(desc->OutputWindow);
  ComPtr<IDXGIFactory2> modernFactory;
  if(FAILED(factory->QueryInterface(IID_PPV_ARGS(&modernFactory))))
    return originalLegacyFactory(factory,source,desc,out);
  DXGI_SWAP_CHAIN_DESC1 modern{};
  modern.Width=desc->BufferDesc.Width; modern.Height=desc->BufferDesc.Height;
  modern.Format=desc->BufferDesc.Format; modern.SampleDesc=desc->SampleDesc;
  modern.BufferUsage=desc->BufferUsage; modern.BufferCount=desc->BufferCount;
  modern.Scaling=DXGI_SCALING_STRETCH; modern.SwapEffect=desc->SwapEffect;
  modern.AlphaMode=DXGI_ALPHA_MODE_IGNORE; modern.Flags=desc->Flags;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC full{};
  full.RefreshRate=desc->BufferDesc.RefreshRate;
  full.ScanlineOrdering=desc->BufferDesc.ScanlineOrdering;
  full.Scaling=desc->BufferDesc.Scaling; full.Windowed=desc->Windowed;
  DiscoverStreamline();
  ComPtr<IDXGISwapChain1> replacement;
  const auto hr=CreateOutput(modernFactory.Get(),source,desc->OutputWindow,&modern,&full,nullptr,&replacement);
  xess_mod::WriteLog("native_legacy_factory_create","\"result\":"+std::to_string(hr)
      +",\"replaced\":"+(SUCCEEDED(hr)?"true":"false")
      +",\"format\":"+std::to_string(modern.Format)+",\"flags\":"+std::to_string(modern.Flags));
  if(hr==E_NOINTERFACE) {
    creatingWindow=nullptr;
    ClearRetainedNative(desc->OutputWindow);
    return originalLegacyFactory(factory,source,desc,out);
  }
  if(FAILED(hr)) return hr;
  return replacement.CopyTo(out);
}
bool Install() {
  if(single_overlay::native::InsideLoader() || StreamlineStopping()) return false;
  DiscoverStreamline();
  static std::mutex installLock; std::lock_guard guard(installLock);
  if(originalFactory && originalLegacyFactory) return true;
  ComPtr<IDXGIFactory2> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  auto** table=*reinterpret_cast<void***>(factory.Get());
  if(!single_overlay::native::PinInterface(factory.Get(),15)) {
    Report("XeMFG factory hook unavailable; restart with early ReShade addon loading"); return false;
  }
  const bool legacy=originalLegacyFactory || Hook(table[10],reinterpret_cast<void*>(&LegacyFactoryHook),reinterpret_cast<void**>(&originalLegacyFactory));
  const bool hwnd=originalFactory || Hook(table[15],reinterpret_cast<void*>(&FactoryHook),reinterpret_cast<void**>(&originalFactory));
  if(!legacy || !hwnd) {
    Report("XeMFG factory entrypoint hook incomplete; restart with early ReShade addon loading"); return false;
  }
  xess_mod::WriteLog("native_factory_hooks","\"legacy\":true,\"hwnd\":true");
  std::lock_guard lock(State().lock); State().status.factoryHook=true; return true;
}
static bool forceDred{},debugLayer{};
bool D3D12DiagnosticsForced() { return forceDred || debugLayer; }
static bool CreateDeviceEvent(reshade::api::device_api api,uint32_t& version) {
#ifdef MFGUNLOCK_XEMFG_VULKAN
  // 1.7 (plan F): vkCreateInstance; the Vulkan host's hooks go in before the device.
  if(api==reshade::api::device_api::vulkan) return vulkan::BeforeInstance(version);
#else
  (void)version;
#endif
  if(api!=reshade::api::device_api::d3d12 || !D3D12DiagnosticsForced()) return false;
  // Must run before D3D12CreateDevice; both settings apply to later devices only.
  HRESULT dred=E_ABORT,layer=E_ABORT;
  if(forceDred) {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    dred=D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if(SUCCEEDED(dred)) {
      settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
  }
  if(debugLayer) {
    ComPtr<ID3D12Debug> debug;
    layer=D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if(SUCCEEDED(layer)) debug->EnableDebugLayer();
  }
  xess_mod::WriteLog("native_d3d12_diagnostics","\"dred\":"+std::to_string(uint32_t(dred))+",\"debug_layer\":"+std::to_string(uint32_t(layer)));
  return false;
}
static void CALLBACK D3D12Message(D3D12_MESSAGE_CATEGORY category,D3D12_MESSAGE_SEVERITY severity,D3D12_MESSAGE_ID id,LPCSTR text,void*) {
  // Games emit steady performance warnings; keep the budget for errors.
  if(severity>D3D12_MESSAGE_SEVERITY_ERROR) return;
  static std::atomic<uint32_t> budget{};
  if(budget++>=64) return;
  xess_mod::WriteLog("native_d3d12_message","\"severity\":"+std::to_string(int(severity))+",\"category\":"+std::to_string(int(category))
      +",\"id\":"+std::to_string(int(id))+",\"text\":\""+escape(text?text:"")+"\"");
}
static void DeviceEvent(reshade::api::device* device) {
  if(device->get_api()!=reshade::api::device_api::d3d12) return;
  Install();
  if(!debugLayer) return;
  ComPtr<ID3D12InfoQueue1> queue; DWORD cookie{};
  const auto* native=reinterpret_cast<ID3D12Device*>(device->get_native());
  const auto result=native?const_cast<ID3D12Device*>(native)->QueryInterface(IID_PPV_ARGS(&queue)):E_POINTER;
  const auto registered=SUCCEEDED(result)?queue->RegisterMessageCallback(&D3D12Message,D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&cookie):result;
  xess_mod::WriteLog("native_d3d12_info_queue","\"result\":"+std::to_string(uint32_t(registered)));
}
static void OutputPresent(reshade::api::command_queue*,reshade::api::swapchain*,const reshade::api::rect*,
    const reshade::api::rect*,uint32_t,const reshade::api::rect*) {
  if(State().hasOutput.load(std::memory_order_relaxed)) State().outputPresents.fetch_add(1,std::memory_order_relaxed);
}
static DWORD WINAPI DiscoverWorker(void*) {
  LogSession();
  LogProfiles();
  StartTraceWorker();
  for(unsigned i=0;i<80 && !StreamlineStopping();++i) { DiscoverStreamline(); Install(); ffx::Discover(); ngx::Discover(); Sleep(250); }
  return 0;
}
// Late interposer loads. The notification runs under the loader lock, so it
// only signals; this thread installs the detours after the load completes
// (DiscoverStreamline pins the module, which waits for the loader).
namespace loader {
struct UnicodeString { USHORT Length,MaximumLength; PWSTR Buffer; };
struct NotificationData { ULONG Flags; const UnicodeString* FullDllName; const UnicodeString* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using Callback=VOID(CALLBACK*)(ULONG,const NotificationData*,PVOID);
using Register=LONG(NTAPI*)(ULONG,Callback,PVOID,PVOID*);
constexpr ULONG Loaded=1;
HANDLE signal{};
VOID CALLBACK Notify(ULONG reason,const NotificationData* data,PVOID) {
  if(reason!=Loaded || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;
  // Streamline and the FidelityFX loader (plan 5.3) are both input providers.
  const auto* s=data->BaseDllName;
  for(const wchar_t* name:{L"sl.interposer.dll",L"amd_fidelityfx_loader_dx12.dll",L"amd_fidelityfx_dx12.dll",L"_nvngx.dll"}) {
    const auto length=wcslen(name)*sizeof(wchar_t);
    if(s->Length==length && _wcsnicmp(s->Buffer,name,length/sizeof(wchar_t))==0) SetEvent(signal);
  }
}
DWORD WINAPI Worker(void*) {
  while(!StreamlineStopping() && WaitForSingleObject(signal,INFINITE)==WAIT_OBJECT_0) {
    xess_mod::WriteLog("native_interposer_loaded","\"phase\":\"notification\"");
    DiscoverStreamline(); Install(); ffx::Discover(); ngx::Discover();
  }
  return 0;
}
void Start() {
  const auto ntdll=GetModuleHandleW(L"ntdll.dll");
  const auto registerNotification=ntdll?reinterpret_cast<Register>(GetProcAddress(ntdll,"LdrRegisterDllNotification")):nullptr;
  if(!registerNotification || !(signal=CreateEventW(nullptr,FALSE,FALSE,nullptr))) return;
  PVOID cookie{};
  if(registerNotification(0,&Notify,nullptr,&cookie)<0) return;
  // The registration lives for the process, like the pinned addon.
  if(auto thread=CreateThread(nullptr,0,&Worker,nullptr,0,nullptr)) CloseHandle(thread);
}
}
void Attach(HMODULE module) {
  CaptureStreamlineStartup();
  auto& s=State(); auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
  ConfigureNativeContinuity(int(GetPrivateProfileIntW(L"Compatibility",L"RetainNativeOutput",UINT(-1),ini.c_str())));
  s.multiplier=std::clamp(GetPrivateProfileIntW(L"XeFG",L"Multiplier",4,ini.c_str()),2u,6u);
  SetGameMultiplier(s.multiplier);
  wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));
  // Conventions (plans 3.3/3.4): explicit INI > learned for this exact executable >
  // profiles.json > SDK default (inverted depth, unjittered motion). Identity is
  // name and size; a game update relearns once.
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  GetFileAttributesExW(executable,GetFileExInfoStandard,&attributes);
  s.executableIdentity=std::filesystem::path(executable).filename().wstring()+L"|"
      +std::to_wstring((uint64_t(attributes.nFileSizeHigh)<<32)|attributes.nFileSizeLow);
  wchar_t learnedFor[1024]{};
  GetPrivateProfileStringW(L"XeFG.Learned",L"Executable",L"",learnedFor,DWORD(std::size(learnedFor)),ini.c_str());
  const bool learnedHere=s.executableIdentity==learnedFor;
  const auto convention=[&](const wchar_t* key,bool fallback,bool& explicitValue) {
    const int value=int(GetPrivateProfileIntW(L"XeFG",key,-1,ini.c_str())); // UINT: -1 means absent
    explicitValue=value>=0;
    if(explicitValue) return value!=0;
    const int learned=learnedHere?int(GetPrivateProfileIntW(L"XeFG.Learned",key,-1,ini.c_str())):-1;
    return learned>=0?learned!=0:fallback;
  };
  const auto profile=CurrentPolicy();
  s.invert=convention(L"InvertedDepth",profile.depthInverted.value_or(true),s.explicitInvert);
  s.jitter=convention(L"JitteredMotion",profile.motionJittered.value_or(false),s.explicitJitter);
  LoadControls();
  reframework::SetKnownMonitorIssues(CurrentPolicy().knownBadReframework);
  SetTimingEnabled(GetPrivateProfileIntW(L"Diagnostics",L"CpuTiming",0,ini.c_str())!=0);
  ConfigureTrace(GetPrivateProfileIntW(L"Diagnostics",L"StallTrace",0,ini.c_str())!=0,
      GetPrivateProfileIntW(L"Diagnostics",L"GpuTiming",0,ini.c_str())!=0);
  s.summaryIntervalMs=std::clamp(GetPrivateProfileIntW(L"Diagnostics",L"SummaryIntervalMs",10000,ini.c_str()),100u,600000u);
  forceDred=GetPrivateProfileIntW(L"Diagnostics",L"ForceDRED",0,ini.c_str())!=0;
  debugLayer=GetPrivateProfileIntW(L"Diagnostics",L"D3D12DebugLayer",0,ini.c_str())!=0;  AttachModel(module);
#ifdef MFGUNLOCK_XEMFG_VULKAN
  vulkan::InstallDiagnostics();
#endif
  reshade::register_event<reshade::addon_event::create_device>(&CreateDeviceEvent);
  reshade::register_event<reshade::addon_event::init_device>(&DeviceEvent);
  reshade::register_event<reshade::addon_event::present>(&OutputPresent);
  // Bounded discovery only; the resolver catches feature queries after this
  // bootstrap window. No module scan is added to generated-frame Present.
  HMODULE pin{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pin);
  if(auto worker=CreateThread(nullptr,0,&DiscoverWorker,nullptr,0,nullptr)) CloseHandle(worker);
  loader::Start();
}

}
extern "C" __declspec(dllexport) bool MFGAXeNativeStatus(mfgunlock::xemfg::native::Status* out,uint32_t size) {
  if(!out || size!=sizeof(*out)) return false;
  *out=mfgunlock::xemfg::native::Snapshot(); return true;
}
extern "C" __declspec(dllexport) void MFGAXeNativeRefresh() {
  if(auto o=mfgunlock::xemfg::native::Active()) mfgunlock::xemfg::native::AfterPresent(o);
}
