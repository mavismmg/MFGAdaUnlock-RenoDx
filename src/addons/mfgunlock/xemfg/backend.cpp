// SPDX-License-Identifier: MIT
// OptiScaler transport -> Intel XeSS-FG model port. The transport owns input
// resources, XeLL markers/sleep and its UI. This companion owns only the port,
// count admission and the extra-output pacing for the admitted provider pair.
#include "../backend_selection.hpp"
#include "backend_api.hpp"
#include "module_snapshot.hpp"
#include "async_log.hpp"
#include "vendor/port/port_support.h"
#include "vendor/port/model_port.h"
#include "vendor/port/shader_port.h"
#include "vendor/host_unlock.h"
#include "vendor/sdk_pacing.h"
#include "vendor/cuda/cuda_route.h"
#include "vendor/adapter_discovery.h"
#include "embedded_xefg_cuda.generated.h"
#include "vendor/third_party/minhook/include/MinHook.h"
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <psapi.h>
#ifdef MFGUNLOCK_NATIVE_XEMFG
#include "native_profiles.hpp"
#endif
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif
#include <intrin.h>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <span>

namespace {
using Handle = xefg_swapchain_handle_t;
using Result = xefg_swapchain_result_t;
constexpr auto success = XEFG_SWAPCHAIN_RESULT_SUCCESS;
constexpr auto unsupported = XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
HMODULE self{}, host{}, fg{}, ll{};
std::recursive_mutex mutex;
// Plan 5.2: the user's generated-frame count for game-owned contexts (0: the
// game's own). A game context initializing on this thread creates its
// swapchain through DXGI; the native bridge must not wrap that one.
std::atomic<uint32_t> userGenerated{0};
thread_local bool gameInitializing{};
// Lock-free (read from DXGI hooks while an SDK init may hold `mutex` and
// wait for its own worker thread): live game-owned contexts and game inits.
std::atomic<int> gameContexts{0}, gameInits{0};
// Game-owned contexts whose initialization succeeded: they present. A probe
// context that is created and never initialized (Mafia) owns nothing.
std::set<Handle> initializedGame;
std::once_flag initializeOnce;
std::once_flag startupOnce;
HMODULE bootstrapFg{};
bool bootstrapCreate{};
std::atomic<bool> startupFailed{};
bool InsideLoader() noexcept {
  using Query = BOOLEAN (NTAPI*)();
  const auto query = reinterpret_cast<Query>(GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlIsThreadWithinLoaderCallout"));
  return !query || query(); // Unknown context never authorizes SDK entry calls.
}
void CompleteStartup() noexcept;
mfgunlock::xemfg::AsyncLog& LogQueue() { static auto* queue=new mfgunlock::xemfg::AsyncLog; return *queue; }
void StartLog() {
  // CompleteStartup is outside the loader. The same process-lifetime policy
  // as the installed hooks protects the worker; no DllMain join or file I/O.
  HMODULE pinned{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&StartLog),&pinned)) return;
  const auto path=mfgunlock::backend::Directory()/L"MFGAdaUnlock-XeMFG.log";
  auto file=std::make_shared<std::ofstream>();
  LogQueue().Start([file,path](const char* bytes,size_t count) {
    if(!file->is_open()) file->open(path,std::ios::app|std::ios::binary);
    file->write(bytes,std::streamsize(count)); file->flush(); return bool(*file);
  });
}
struct Context {
  ComPtr<ID3D12Device> device;
  uint64_t generation{};
  bool initialized{}, enabled{};
  uint32_t generated{};
  bool hostOwner{};
  uint32_t gameRequested{}; // the game's own count request (plan 5.2)
  uint64_t statusTick{};
  uint32_t statusSamples{};
  xefg_swapchain_present_status_t lastStatus{};
};
struct State {
  mfgunlock::xemfg::Status status;
  std::map<Handle, Context> contexts;
  std::set<Handle> nativeContexts;
  std::unique_ptr<ShaderPort> shaders;
  std::unique_ptr<ModelPort> model;
  std::unique_ptr<xess_host_unlock::Unlock> unlock;
  std::span<const unsigned char> cudaFatbin; // Resource bytes in the process-pinned companion.
  bool attempted{}, deferred{}, rejected{}, publishing{};
  uint64_t generation{};
};
// Once hooks exist, callbacks, payloads and provider stay alive until exit.
State& Get() { static State* state = new State; return *state; }
void Detail(const std::string& value) {
  auto& s = Get().status;
  strncpy_s(s.detail, value.c_str(), _TRUNCATE);
}
std::filesystem::path ModulePath(HMODULE module) {
  std::wstring p(32768,L'\0');
  const auto n=GetModuleFileNameW(module,p.data(),DWORD(p.size()));
  if (!n || n>=p.size()) throw std::runtime_error("module_path");
  p.resize(n); return p;
}
std::string HashFile(const std::filesystem::path& path) {
  std::ifstream f(path,std::ios::binary);
  if (!f) throw std::runtime_error("hash_file_open");
  sha256_util::Hasher h; std::array<char,65536> b{};
  while (f) { f.read(b.data(),b.size()); if (f.gcount() && !h.Update(b.data(),size_t(f.gcount()))) throw std::runtime_error("hash_update"); }
  sha256_util::Digest d{};
  if (f.bad() || !h.Finish(d)) throw std::runtime_error("hash_finish");
  return sha256_util::LowerHex(d);
}
bool HostAddress(const void* p) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  return host && p && VirtualQuery(p,&m,sizeof(m))==sizeof(m)
      && m.AllocationBase==host && m.Type==MEM_IMAGE;
}
bool SdkAddress(const void* p) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  return p && VirtualQuery(p,&m,sizeof(m))==sizeof(m) && m.Type==MEM_IMAGE
      && (m.AllocationBase==fg || m.AllocationBase==ll);
}
// 1.8.6: modules a profile names as this game's XeFG integration (Elden Ring's
// ERSS calls XeFG from zzz_ERSS.dll, not from eldenring.exe). Lower-case
// file names, set once in Start() under `mutex`.
std::vector<std::wstring> gameXefgModules;
bool GameAddress(const void* p) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  if(!p || VirtualQuery(p,&m,sizeof(m))!=sizeof(m) || m.Type!=MEM_IMAGE) return false;
  if(m.AllocationBase==GetModuleHandleW(nullptr)) return true;
  if(gameXefgModules.empty()) return false;
  wchar_t path[MAX_PATH]{};
  if(!GetModuleFileNameW(static_cast<HMODULE>(m.AllocationBase),path,MAX_PATH)) return false;
  const wchar_t* name=wcsrchr(path,L'\\'); name=name?name+1:path;
  std::wstring lower(name); for(auto& c:lower) c=static_cast<wchar_t>(towlower(c));
  return std::find(gameXefgModules.begin(),gameXefgModules.end(),lower)!=gameXefgModules.end();
}
std::string CallerName(const void* p) noexcept {
  try {
    MEMORY_BASIC_INFORMATION m{};
    if(!p || VirtualQuery(p,&m,sizeof(m))!=sizeof(m) || m.Type!=MEM_IMAGE) return "unknown";
    return ModulePath(static_cast<HMODULE>(m.AllocationBase)).filename().string();
  } catch(...) { return "unknown"; }
}
// Respect XeLL entry redirects that belong to OptiScaler; no second sleep hook.
bool HostRedirect(const void* entry) noexcept {
  auto* p=static_cast<const unsigned char*>(entry);
  for (unsigned i=0; p && i<4; ++i) {
    unsigned char bytes[6]{}; SIZE_T n{};
    if (!ReadProcessMemory(GetCurrentProcess(),p,bytes,6,&n) || n!=6) return false;
    int32_t rel{};
    if (bytes[0]==0xe9) { memcpy(&rel,bytes+1,4); p+=5+rel; }
    else if (bytes[0]==0xff && bytes[1]==0x25) {
      memcpy(&rel,bytes+2,4); uintptr_t target{};
      if (!ReadProcessMemory(GetCurrentProcess(),p+6+rel,&target,8,&n) || n!=8) return false;
      p=reinterpret_cast<const unsigned char*>(target);
    } else return false;
    if (HostAddress(p)) return true;
  }
  return false;
}
bool IsOptiScaler10(HMODULE module) {
  return mfgunlock::xemfg::module_snapshot::IsOptiScaler10(module);
}

uint32_t Vendor(ID3D12Device* device) {
  if (!device) return 0;
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
      || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))) return 0;
  DXGI_ADAPTER_DESC1 desc{};
  return SUCCEEDED(adapter->GetDesc1(&desc)) ? desc.VendorId : 0;
}
void Pin(HMODULE module) {
  HMODULE pinned{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module) throw std::runtime_error("module_pin");
}
void Prepare(ID3D12Device* device) {
  auto& s=Get(); if (s.attempted) return;
  // Unadmitted callers retain the native SDK. Never publish over their live
  // contexts. Admitted game/host consumers share the port from their first
  // Create, even when the game's capability context precedes the host output.
  if(!s.nativeContexts.empty()) {
    Detail("Native XeFG 2x: waiting for earlier native contexts to retire");
    if(!s.deferred) xess_mod::WriteLog("port_deferred","\"reason\":\"native_contexts_live\",\"contexts\":"+std::to_string(s.nativeContexts.size()));
    s.deferred=true; return;
  }
  s.attempted=true; s.deferred=false;
  s.publishing=true;
  struct PublicationScope { bool& flag; ~PublicationScope() { flag=false; } } publication{s.publishing};
  try {
    // 1.8 (plan A.6): comparison of Intel's own 2x model with the t=1/2 frame of the
    // ported model. Off by default; it runs the existing native 2x route.
    if (GetPrivateProfileIntW(L"XeFG",L"XeFGStock2xModel",0,(ModulePath(host).parent_path()/L"MFGAdaUnlock.ini").c_str())==1)
      throw std::runtime_error("stock_2x_model_requested");
    if (Vendor(device)!=0x10de) throw std::runtime_error("first_build_targets_nvidia_only");
    if (!xess_layout::Pair(fg,ll)) throw std::runtime_error("provider_layout_rejected");
    auto fatbin=xess_payload::Read(self,xefg_cuda::embedded::fatbin);
    // Publish only the true multi-frame model. Never repeat the stock 2x model
    // at the wrong phases as an alternative unlock.
    auto shaders=std::make_unique<ShaderPort>(fg,self);
    auto model=std::make_unique<ModelPort>(fg,true,true);
    auto unlock=std::make_unique<xess_host_unlock::Unlock>(fg,ll,5,false);
    s.shaders=std::move(shaders); s.model=std::move(model); s.unlock=std::move(unlock);
    s.cudaFatbin=std::move(fatbin);
    s.status.model=1;
    Detail("Intel multi-frame model ready");
  } catch (const std::exception& e) {
    s.rejected=true; Detail(std::string("Native XeFG 2x fallback: ")+e.what());
    xess_mod::WriteLog("port_rejected","\"reason\":\""+escape(e.what())+"\"");
  }
}
void ConfigureFlip() noexcept {
  static std::once_flag once;
  std::call_once(once,[] {
    try {
      const auto ini=ModulePath(host).parent_path()/
#ifdef MFGUNLOCK_NATIVE_XEMFG
          L"MFGAdaUnlock.ini";
#else
          L"OptiScaler.ini";
#endif
      wchar_t setting[32]{};
      GetPrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",L"auto",setting,32,ini.c_str());
      const bool enabled=_wcsicmp(setting,L"off") && _wcsicmp(setting,L"false") && wcscmp(setting,L"0");
      wchar_t system[1024]{};
      if (!GetSystemDirectoryW(system,1024)) throw std::runtime_error("system_directory");
      const auto dll=std::filesystem::path(system)/L"nvapi64.dll";
      const auto dispatcher=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!dispatcher || !adapter_discovery::NvapiOwnerModule(system,dispatcher)) throw std::runtime_error("NVAPI_owner");
      using Query=void*(__cdecl*)(uint32_t);
      const auto query=reinterpret_cast<Query>(GetProcAddress(dispatcher,"nvapi_QueryInterface"));
      if (!adapter_discovery::OwnedCode(dispatcher,reinterpret_cast<void*>(query))) throw std::runtime_error("NVAPI_query");
      auto resolve=[&](uint32_t id) {
        auto target=query(id); adapter_discovery::ModuleReference owner;
        if (!target || !owner.Acquire(target) || !adapter_discovery::NvapiOwnerModule(system,owner.module)
            || !adapter_discovery::OwnerFactoryMatches(owner.module,id,target)) throw std::runtime_error("NVAPI_function_owner");
        Pin(owner.module); return target;
      };
      auto init=reinterpret_cast<int(__cdecl*)()>(resolve(0x0150e828u));
      const auto set=reinterpret_cast<sdk_pacing::SetFlipConfigFunction>(resolve(0xf3148c42u));
      if (init()!=0) throw std::runtime_error("NVAPI_initialize");
      Pin(dispatcher);
      auto lead=GetPrivateProfileIntW(L"XeFG",L"XeFGFlipMeteringLeadUs",2500,ini.c_str());
      if (lead>8000) lead=2500;
      sdk_pacing::SetFlipMetering(set,enabled,int64_t(lead)*1000);
      xess_mod::WriteLog("flip_metering_available","\"lead_us\":"+std::to_string(lead));
    } catch(const std::exception& e) {
      sdk_pacing::SetFlipMetering(nullptr,false,0);
      xess_mod::WriteLog("flip_metering_unavailable","\"fallback\":\"SDK_extra_pacing\",\"reason\":\""+escape(e.what())+"\"");
    } catch(...) { sdk_pacing::SetFlipMetering(nullptr,false,0); }
  });
}
// 1.7 (plan J): dashdogy's v1.7.0 flip queue (vendor/sdk_pacing_meter.h). His
// own standalone D3D12 host keeps it off: without OptiScaler the game's
// hand-overs follow the queue's own releases and the clock locks below the
// game's rate (his Subnautica 2 measurement: 100-110 fps against DLSS MFG's
// ~180). NVIDIA's flip metering and the CPU pacer space the outputs instead,
// as for DLSS-G; that is this host's measured default as well (plan G). The
// pacing component itself starts with the queue on, so it is always set here.
// [XeFG] XeFGFlipQueue=1 enables it for comparison, with XeFG's native swap
// chain raised to XeFGNativeBuffers (default 10, at most 16) back buffers.
struct FlipQueueChoice { bool enabled{}; uint32_t buffers{}; };
FlipQueueChoice ConfigureFlipQueue() noexcept {
  static std::once_flag once;
  static FlipQueueChoice choice;
  std::call_once(once,[] {
    try {
      const auto ini=ModulePath(host).parent_path()/
#ifdef MFGUNLOCK_NATIVE_XEMFG
          L"MFGAdaUnlock.ini";
#else
          L"OptiScaler.ini";
#endif
      choice.enabled=GetPrivateProfileIntW(L"XeFG",L"XeFGFlipQueue",0,ini.c_str())!=0;
      auto percent=GetPrivateProfileIntW(L"XeFG",L"XeFGFlipQueueBufferPct",50,ini.c_str());
      if(percent>100) percent=50;
      wchar_t first[32]{};
      GetPrivateProfileStringW(L"XeFG",L"XeFGFlipQueueFirst",L"ready",first,32,ini.c_str());
      const bool firstReady=_wcsicmp(first,L"buffer")!=0;
      auto buffers=GetPrivateProfileIntW(L"XeFG",L"XeFGNativeBuffers",10,ini.c_str());
      if(buffers<2 || buffers>16) buffers=10;
      choice.buffers=choice.enabled?buffers:0;
      sdk_pacing::SetFlipQueueFirst(firstReady);
      sdk_pacing::SetFlipQueue(choice.enabled,int(percent));
      xess_mod::WriteLog("flip_queue_config","\"enabled\":"+std::string(choice.enabled?"true":"false")
          +",\"buffer_percent\":"+std::to_string(percent)+",\"first\":\""+(firstReady?"ready":"buffer")
          +"\",\"native_buffers\":"+std::to_string(choice.buffers));
    } catch(...) { sdk_pacing::SetFlipQueue(false,50); choice={}; }
  });
  return choice;
}
#define API(name) decltype(&name) original_##name{}
API(xefgSwapChainD3D12CreateContext);
API(xefgSwapChainD3D12InitFromSwapChain);
API(xefgSwapChainD3D12InitFromSwapChainDesc);
API(xefgSwapChainDestroy);
API(xefgSwapChainSetEnabled);
API(xefgSwapChainSetNumInterpolatedFrames);
API(xefgSwapChainGetLastPresentStatus);
#undef API
__declspec(noinline) Result Create(ID3D12Device* device, Handle* output) {
  // The public entry is captured early, but SDK code/metadata is never called
  // from an OptiScaler DllMain callback. The first real client call can join
  // the worker here before a context consumes the model or count settings.
  if (InsideLoader()) return unsupported;
  const auto caller=_ReturnAddress();
  CompleteStartup();
  std::lock_guard lock(mutex); auto& s=Get();
  if (!s.status.hooks) return original_xefgSwapChainD3D12CreateContext(device,output);
  if(s.publishing) return unsupported; // Do not create a native context reentrantly during publication.
  // SDK self-calls retain their original forwarding/lifecycle semantics.
  if(SdkAddress(caller)) return original_xefgSwapChainD3D12CreateContext(device,output);
  const bool hostCall=HostAddress(caller),gameCall=GameAddress(caller);
  if (!hostCall && !gameCall) {
    if (s.model) return unsupported;
    const auto r=original_xefgSwapChainD3D12CreateContext(device,output);
    if(r==success && output && *output) s.nativeContexts.insert(*output);
    xess_mod::WriteLog("native_context_create","\"result\":"+std::to_string(r)+",\"caller\":\""+escape(CallerName(caller))+"\",\"handle\":"+std::to_string(output&&r==success?uintptr_t(*output):0));
    return r;
  }
  if (!device || !output) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  if(s.contexts.size()>=8) return unsupported;
  const bool first=!s.attempted && s.nativeContexts.empty();
  Prepare(device);
  if(first && s.model) xess_mod::WriteLog("model_prepared_before_create","\"owner\":\""+std::string(hostCall?"host":"game")+"\",\"caller\":\""+escape(CallerName(caller))+"\"");
  if(hostCall && s.model && !xess_mod::VulkanHostDevice(device)) {
    // Bind the optional GPU route to the actual output device, not an earlier
    // game capability probe. Other devices retain the profiled DXIL path.
    xefg_cuda::SetLog(&xess_mod::WriteLog);
    std::string reason;
    xefg_cuda::Install(device,fg,s.cudaFatbin,reason);
  }
  const auto r=original_xefgSwapChainD3D12CreateContext(device,output);
  s.status.result=r;
  if (r==success && *output && s.model) {
    Context c{device,++s.generation}; c.hostOwner=hostCall;
    s.contexts.emplace(*output,std::move(c));
    s.status.contexts=uint32_t(s.contexts.size());
  }
  else if(r==success && *output) s.nativeContexts.insert(*output);
  xess_mod::WriteLog("context_create","\"result\":"+std::to_string(r)+",\"mfg_model\":"+(s.model?"true":"false")+",\"owner\":\""+(hostCall?"host":"game")+"\",\"handle\":"+std::to_string(r==success?uintptr_t(*output):0));
  return r;
}
// A context created by any caller but our host (the game or another layer).
bool GameOwned(Handle h) {
  std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
  return (it!=s.contexts.end() && !it->second.hostOwner) || s.nativeContexts.contains(h);
}
template<class Init> Result Initialize(Handle handle, Init call) {
  std::lock_guard lock(mutex); auto& s=Get();
  const auto r=call(); s.status.result=r;
  if(r==success && GameOwned(handle) && initializedGame.insert(handle).second) ++gameContexts;
  auto it=s.contexts.find(handle);
  if (r!=success || it==s.contexts.end()) return r;
  auto& c=it->second;
  if (c.initialized) sdk_pacing::Unregister(handle,c.generation);
  c.initialized=false;
  std::string failure;
  if(xess_mod::VulkanHostDevice(c.device.Get())) {
    // 1.7 (plan F): the xvk device has no DXGI queue for NVAPI's flip
    // configuration or the flip queue; XeLL and the CPU pacer space the outputs.
    sdk_pacing::SetFlipMetering(nullptr,false,0);
    sdk_pacing::SetFlipQueue(false,50);
    xess_mod::WriteLog("flip_metering_unavailable","\"fallback\":\"SDK_extra_pacing\",\"reason\":\"vulkan_host\"");
  } else {
    ConfigureFlip();
    ConfigureFlipQueue();
  }
  if (!sdk_pacing::Install(fg,failure) || !sdk_pacing::Register(handle,c.generation,c.device.Get(),failure)) {
    Detail("XeMFG disabled: extra-output pacing unavailable: "+failure);
    original_xefgSwapChainSetEnabled(handle,0);
    s.status.initialized=0; s.status.enabled=0;
    xess_mod::WriteLog("pacing_rejected","\"reason\":\""+escape(failure)+"\"");
    return unsupported;
  }
  c.initialized=true; s.status.initialized=1;
  Detail("XeMFG ready (2x-6x)");
  xess_mod::WriteLog("context_initialized","\"paced\":true,\"owner\":\""+std::string(c.hostOwner?"host":"game")+"\",\"generation\":"+std::to_string(c.generation));
  return r;
}
// A game-owned context is initialized with room for the user's multiplier.
const xefg_swapchain_d3d12_init_params_t* GameParams(Handle h,const xefg_swapchain_d3d12_init_params_t* p,
    xefg_swapchain_d3d12_init_params_t& raised) {
  if(!p || !userGenerated.load() || p->maxInterpolatedFrames>=5) return p;
  {
    std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
    if(it==s.contexts.end() || it->second.hostOwner) return p; // only the game's model contexts
  }
  raised=*p; raised.maxInterpolatedFrames=5;
  xess_mod::WriteLog("game_init_raised","\"handle\":"+std::to_string(uintptr_t(h))+",\"requested_max\":"+std::to_string(p->maxInterpolatedFrames)+",\"max\":5");
  return &raised;
}
struct GameInitScope {
  bool previous{gameInitializing};
  bool counted{};
  explicit GameInitScope(bool game):counted(game) { if(game) { gameInitializing=true; ++gameInits; } }
  ~GameInitScope() { gameInitializing=previous; if(counted) --gameInits; }
};
__declspec(noinline) Result InitChain(Handle h, ID3D12CommandQueue* q,const xefg_swapchain_d3d12_init_params_t* p) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainD3D12InitFromSwapChain(h,q,p);
  xefg_swapchain_d3d12_init_params_t raised; const auto* params=GameParams(h,p,raised);
  GameInitScope scope(GameOwned(h));
  return Initialize(h,[&] { return original_xefgSwapChainD3D12InitFromSwapChain(h,q,params); });
}
__declspec(noinline) Result InitDesc(Handle h,HWND w,const DXGI_SWAP_CHAIN_DESC1* d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,
                ID3D12CommandQueue* q,IDXGIFactory2* factory,const xefg_swapchain_d3d12_init_params_t* p) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainD3D12InitFromSwapChainDesc(h,w,d,f,q,factory,p);
  xefg_swapchain_d3d12_init_params_t raised; const auto* params=GameParams(h,p,raised);
  GameInitScope scope(GameOwned(h));
  return Initialize(h,[&] {
    // 1.7 (plan J): with the flip queue on, XeFG's native swap chain gets the
    // queue's back buffers. The native build hands XeFG its own factory
    // (native_factory.cpp OutputFactory), which raises the count itself;
    // upstream's NativeBuffersScope only accepts DXGI's own factory table.
    std::optional<sdk_pacing::NativeBuffersScope> buffers;
#ifndef MFGUNLOCK_NATIVE_XEMFG
    const auto queue=ConfigureFlipQueue();
    if(queue.enabled && queue.buffers>=2 && factory) {
      buffers.emplace(factory,fg,queue.buffers);
      if(buffers->Reason()) xess_mod::WriteLog("native_buffers_scope","\"applied\":false,\"reason\":\""+std::string(buffers->Reason())+"\"");
    }
#endif
    return original_xefgSwapChainD3D12InitFromSwapChainDesc(h,w,d,f,q,factory,params);
  });
}
void ApplyGameCount(Handle h);
__declspec(noinline) Result Enable(Handle h,uint32_t enabled) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainSetEnabled(h,enabled);
  std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
  if (it!=s.contexts.end() && enabled && !it->second.initialized) return unsupported;
  auto r=original_xefgSwapChainSetEnabled(h,enabled);
  if (r==success && it!=s.contexts.end()) {
    it->second.enabled=enabled!=0; s.status.enabled=enabled!=0;
    sdk_pacing::Reset(h,it->second.generation);
  }
  if(r==success && enabled) ApplyGameCount(h);
  s.status.result=r; return r;
}
__declspec(noinline) Result Count(Handle h,uint32_t count) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainSetNumInterpolatedFrames(h,count);
  std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
  // Plan 5.2: a game-owned context generates the user's count, not the game's.
  if(it!=s.contexts.end() && !it->second.hostOwner && count) {
    it->second.gameRequested=count;
    if(const auto user=userGenerated.load()) {
      xess_mod::WriteLog("game_multiplier_applied","\"handle\":"+std::to_string(uintptr_t(h))
          +",\"requested\":"+std::to_string(count)+",\"applied\":"+std::to_string(user));
      count=user;
    }
  }
  if (it!=s.contexts.end() && (!count || count>5 || !it->second.initialized)) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  const auto r=original_xefgSwapChainSetNumInterpolatedFrames(h,count);
  if (r==success && it!=s.contexts.end()) {
    if (it->second.generated!=count) sdk_pacing::Reset(h,it->second.generation);
    it->second.generated=count; s.status.generated=count;
  }
  if(it!=s.contexts.end()) xess_mod::WriteLog("count_result","\"owner\":\""+std::string(it->second.hostOwner?"host":"game")+"\",\"generated\":"+std::to_string(count)+",\"result\":"+std::to_string(r));
  s.status.result=r; return r;
}
// A changed user multiplier reaches an enabled game context on the game's
// own thread, at its next status or enable call.
void ApplyGameCount(Handle h) {
  std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
  const auto user=userGenerated.load();
  if(it==s.contexts.end() || it->second.hostOwner || !it->second.initialized || !it->second.gameRequested
      || !user || it->second.generated==user) return;
  const auto r=original_xefgSwapChainSetNumInterpolatedFrames(h,user);
  if(r==success) { sdk_pacing::Reset(h,it->second.generation); it->second.generated=user; s.status.generated=user; }
  xess_mod::WriteLog("game_multiplier_applied","\"handle\":"+std::to_string(uintptr_t(h))
      +",\"requested\":"+std::to_string(it->second.gameRequested)+",\"applied\":"+std::to_string(user)+",\"live\":true,\"result\":"+std::to_string(r));
}
Result LastStatus(Handle h,xefg_swapchain_present_status_t* p) {
  ApplyGameCount(h);
  const auto r=original_xefgSwapChainGetLastPresentStatus(h,p);
  if (r==success && p) {
    std::lock_guard lock(mutex); auto& s=Get();
    auto it=s.contexts.find(h);
    if (it!=s.contexts.end() && it->second.hostOwner) {
      s.status.presented=p->framesPresented; s.status.result=p->frameGenResult;
      s.status.enabled=p->isFrameGenEnabled;
      s.status.tensor_launches=xefg_cuda::Snapshot().launches;
      auto& c=it->second;
      const auto tick=GetTickCount64();
      const bool changed=p->framesPresented!=c.lastStatus.framesPresented
          || p->isFrameGenEnabled!=c.lastStatus.isFrameGenEnabled || p->frameGenResult!=c.lastStatus.frameGenResult;
      // A bounded startup sample, then only changes. SDK presentation status
      // is not evidence of monitor scanout, FPS or physical latency.
      if(tick-c.statusTick>=1000 && (c.statusSamples<30 || changed)) {
        c.statusTick=tick; ++c.statusSamples; c.lastStatus=*p;
        xess_mod::WriteLog("output_status","\"handle\":"+std::to_string(uintptr_t(h))
            +",\"frames_presented\":"+std::to_string(p->framesPresented)+",\"enabled\":"+std::to_string(p->isFrameGenEnabled)
            +",\"interpolation_result\":"+std::to_string(p->frameGenResult)+",\"tensor_launches\":"+std::to_string(s.status.tensor_launches));
      }
    }
  }
  return r;
}
__declspec(noinline) Result Destroy(Handle h) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainDestroy(h);
  std::lock_guard lock(mutex); auto& s=Get(); const auto it=s.contexts.find(h);
  const auto r=original_xefgSwapChainDestroy(h); // SDK joins its workers first.
  if(r==success && initializedGame.erase(h)) --gameContexts;
  if(r==success && s.nativeContexts.erase(h)) {
    xess_mod::WriteLog("native_context_destroy","\"handle\":"+std::to_string(uintptr_t(h))+",\"remaining\":"+std::to_string(s.nativeContexts.size()));
  }
  if (r==success && it!=s.contexts.end()) {
    sdk_pacing::Unregister(h,it->second.generation); s.contexts.erase(it);
    s.status.contexts=uint32_t(s.contexts.size());
    if (s.contexts.empty()) {
      s.status.initialized=s.status.enabled=s.status.presented=0;
      xefg_cuda::ForgetLayers();
    }
  }
  return r;
}
#ifdef MFGUNLOCK_NATIVE_XEMFG
// 1.5.5: a refused pair names each file's path, hash, version and state, so a
// report shows which file is missing or different (both were one reason code).
constexpr const char* kPinnedProvider="ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27";
constexpr const char* kPinnedLatency="d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7";
constexpr const char* kOlderLatency="61aeeab7fdcfb6d1742f86c14c68c663d759607d01a6132645c857291eaebb86";
std::mutex runtimeFilesLock;
mfgunlock::xemfg::RuntimeFileReport runtimeFiles[2]{};
bool runtimeFilesRejected{};
std::string FileVersion(const std::filesystem::path& path) {
  DWORD unused{}; const auto size=GetFileVersionInfoSizeW(path.c_str(),&unused); if(!size) return {};
  std::vector<char> data(size); VS_FIXEDFILEINFO* info{}; UINT length{};
  if(!GetFileVersionInfoW(path.c_str(),0,size,data.data()) || !VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length) || length<sizeof(*info)) return {};
  return std::to_string(HIWORD(info->dwFileVersionMS))+'.'+std::to_string(LOWORD(info->dwFileVersionMS))+'.'+std::to_string(HIWORD(info->dwFileVersionLS))+'.'+std::to_string(LOWORD(info->dwFileVersionLS));
}
struct RuntimeFile { mfgunlock::xemfg::RuntimeFileState state{}; std::string sha256,version; };
RuntimeFile InspectRuntimeFile(const std::filesystem::path& path,bool latency) {
  using State=mfgunlock::xemfg::RuntimeFileState;
  RuntimeFile file;
  if(path.empty()) { file.state=State::Missing; return file; }
  try { file.sha256=HashFile(path); } catch(...) { file.state=State::Unreadable; return file; }
  file.version=FileVersion(path);
  file.state=file.sha256==(latency?kPinnedLatency:kPinnedProvider)?State::Pinned:
      latency && file.sha256==kOlderLatency?State::AcceptedOlder:State::Different;
  return file;
}
const char* RuntimeStateName(mfgunlock::xemfg::RuntimeFileState state) {
  using State=mfgunlock::xemfg::RuntimeFileState;
  return state==State::Missing?"missing":state==State::Pinned?"pinned":state==State::AcceptedOlder?"accepted_older":
      state==State::Different?"different":state==State::Unreadable?"unreadable":"unknown";
}
void RecordRuntimeFiles(const std::filesystem::path& folder,const std::filesystem::path& fgPath,const RuntimeFile& fg,
    const std::filesystem::path& llPath,const RuntimeFile& ll) {
  const auto field=[](const char* name,const std::filesystem::path& path,const RuntimeFile& file) {
    return std::string("\"")+name+"\":{\"path\":\""+escape(path.string())+"\",\"state\":\""+RuntimeStateName(file.state)
        +"\",\"version\":\""+escape(file.version)+"\",\"sha256\":\""+file.sha256+"\"}";
  };
  xess_mod::WriteLog("runtime_files","\"folder\":\""+escape(folder.string())+"\","+field("libxess_fg.dll",fgPath,fg)+","+field("libxell.dll",llPath,ll)
      +",\"supported_provider\":\"1.3.1.78\",\"supported_latency\":\"1.3.2.10\"");
  std::lock_guard guard(runtimeFilesLock);
  const RuntimeFile* files[2]={&fg,&ll};
  for(int i=0;i<2;++i) { runtimeFiles[i]={}; runtimeFiles[i].state=files[i]->state; strncpy_s(runtimeFiles[i].version,files[i]->version.c_str(),_TRUNCATE); }
  runtimeFilesRejected=true;
}
#endif
void Start() {
  std::lock_guard lock(mutex);
  if (mfgunlock::backend::Read()!=mfgunlock::backend::Kind::XeMfg) { Detail("DLSS-G selected; XeMFG companion inactive"); return; }
#ifdef MFGUNLOCK_NATIVE_XEMFG
  host=self; // Explicitly admitted in-process transport, never an OptiScaler impersonation.
  {
    const auto policy=mfgunlock::xemfg::native::CurrentPolicy();
    gameXefgModules=policy.gameXefgModules;
    for(const auto& name:gameXefgModules) {
      std::string ascii; for(const auto c:name) ascii+=static_cast<char>(c);
      xess_mod::WriteLog("native_game_xefg_module","\"module\":\""+ascii+"\"");
    }
  }
  // Bridges that patch the same Intel runtime come from profiles.json.
  for(const auto& name:mfgunlock::xemfg::native::CurrentPolicy().conflictingModules)
    if(GetModuleHandleW(name.c_str())) {
      std::string ascii; for(const auto c:name) ascii+=static_cast<char>(c); // profile names are ASCII
      throw std::runtime_error("other_mfg_plugin_present:"+ascii);
    }
  std::array<HMODULE,2048> modules{}; DWORD bytes{};
  if(!EnumProcessModules(GetCurrentProcess(),modules.data(),DWORD(sizeof(modules)),&bytes)
      || bytes>sizeof(modules)) throw std::runtime_error("module_inventory");
  for(size_t i=0;i<bytes/sizeof(HMODULE);++i)
    if(IsOptiScaler10(modules[i])) throw std::runtime_error("remove_OptiScaler_loader_before_native_bridge");
#else
  // Legacy OptiScaler companion build: no profiles are linked here.
  if (GetModuleHandleW(L"MFGUnlocker.asi")) throw std::runtime_error("other_mfg_plugin_present");
  std::array<HMODULE,2048> modules{}; DWORD bytes{};
  if (!EnumProcessModules(GetCurrentProcess(),modules.data(),DWORD(sizeof(modules)),&bytes) || bytes>sizeof(modules)) throw std::runtime_error("module_inventory");
  for (size_t i=0;i<bytes/sizeof(HMODULE);++i) if (IsOptiScaler10(modules[i])) {
    if (host && host!=modules[i]) throw std::runtime_error("multiple_optiscaler_hosts"); host=modules[i];
  }
  if (!host) throw std::runtime_error("OptiScaler_10_required");
#endif
#ifdef MFGUNLOCK_NATIVE_XEMFG
  const auto dir=mfgunlock::backend::Directory();
  // Some mod loaders map the game's exact DLLs from _storage_. Prefer the
  // already mapped instance; the native API must use this same validated pair.
  const auto runtimePath=[&](const wchar_t* name,HMODULE mapped) {
    if(mapped) return ModulePath(mapped);
    for(const auto& parent : {dir,dir/L"_storage_"}) {
      const auto candidate=parent/name;
      if(std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return std::filesystem::path{};
  };
  const auto mappedFg=bootstrapCreate?bootstrapFg:GetModuleHandleW(L"libxess_fg.dll");
  const auto mappedLl=GetModuleHandleW(L"libxell.dll");
  const auto fgPath=runtimePath(L"libxess_fg.dll",mappedFg), llPath=runtimePath(L"libxell.dll",mappedLl);
  // Both files are inspected before refusing, so the log and the panel name
  // every problem. The reason codes are unchanged for reports and the panel.
  const auto fgFile=InspectRuntimeFile(fgPath,false), llFile=InspectRuntimeFile(llPath,true);
  using FileState=mfgunlock::xemfg::RuntimeFileState;
  const bool fgUsable=fgFile.state==FileState::Pinned;
  const bool llUsable=llFile.state==FileState::Pinned || llFile.state==FileState::AcceptedOlder;
  if(!fgUsable || !llUsable) {
    RecordRuntimeFiles(dir,fgPath.empty()?dir/L"libxess_fg.dll":fgPath,fgFile,llPath.empty()?dir/L"libxell.dll":llPath,llFile);
    if(fgFile.state==FileState::Missing) throw std::runtime_error("missing_Intel_runtime:libxess_fg.dll");
    if(llFile.state==FileState::Missing) throw std::runtime_error("missing_Intel_runtime:libxell.dll");
    if(fgFile.state==FileState::Unreadable || llFile.state==FileState::Unreadable) throw std::runtime_error("hash_file_open");
    throw std::runtime_error("unreviewed_Intel_runtime_pair");
  }
  const auto fh=fgFile.sha256, lh=llFile.sha256;
#else
  const auto dir=ModulePath(host).parent_path();
  const auto fgPath=std::filesystem::exists(dir/L"OptiScaler/libxess_fg.dll")?dir/L"OptiScaler/libxess_fg.dll":dir/L"libxess_fg.dll";
  const auto llPath=std::filesystem::exists(dir/L"OptiScaler/libxell.dll")?dir/L"OptiScaler/libxell.dll":dir/L"libxell.dll";
  const auto fh=HashFile(fgPath), lh=HashFile(llPath);
  if (fh!="ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27"
      || (lh!="61aeeab7fdcfb6d1742f86c14c68c663d759607d01a6132645c857291eaebb86"
          && lh!="d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7")) throw std::runtime_error("unreviewed_Intel_runtime_pair");
#endif
#ifdef MFGUNLOCK_NATIVE_XEMFG
  fg=mappedFg?mappedFg:LoadLibraryExW(fgPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  ll=mappedLl?mappedLl:LoadLibraryExW(llPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
  fg=LoadLibraryExW(fgPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  ll=LoadLibraryExW(llPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
  if (!fg || !ll) throw std::runtime_error("Intel_runtime_load");
  if (bootstrapCreate && bootstrapFg!=fg) throw std::runtime_error("bootstrap_provider_differs");
#ifdef MFGUNLOCK_NATIVE_XEMFG
  // LoadLibrary hooks may return a byte-identical mapped copy with another path.
  // Revalidate its own file and then the loaded instruction profiles below.
  if(HashFile(ModulePath(fg))!=fh || HashFile(ModulePath(ll))!=lh)
    throw std::runtime_error("loaded_runtime_identity_differs");
  xess_mod::WriteLog("runtime_selection","\"provider_path\":\""+escape(ModulePath(fg).string())+
      "\",\"xell_path\":\""+escape(ModulePath(ll).string())+"\",\"provider_sha256\":\""+fh+"\",\"xell_sha256\":\""+lh+"\"");
#else
  if (!std::filesystem::equivalent(ModulePath(fg),fgPath)
      || !std::filesystem::equivalent(ModulePath(ll),llPath)) throw std::runtime_error("loaded_runtime_path_differs");
#endif
  Pin(self); Pin(host); Pin(fg); Pin(ll);
  xess_layout::entryRedirectFilter.store(&HostRedirect);
  if (!xess_layout::Pair(fg,ll)) throw std::runtime_error("Intel_instruction_profile_rejected");
  // Verify all private writers before public entrypoints change. Publication
  // itself is deferred until OptiScaler's first real CreateContext call.
  const auto mh=MH_Initialize();
  if (mh!=MH_OK && mh!=MH_ERROR_ALREADY_INITIALIZED) throw std::runtime_error("hook_init");
  struct Hook { const char* name; void* target; void** original; };
#define H(name,fn) {#name,reinterpret_cast<void*>(&fn),reinterpret_cast<void**>(&original_##name)}
  const Hook hooks[]={H(xefgSwapChainD3D12CreateContext,Create),H(xefgSwapChainD3D12InitFromSwapChain,InitChain),
    H(xefgSwapChainD3D12InitFromSwapChainDesc,InitDesc),H(xefgSwapChainDestroy,Destroy),
    H(xefgSwapChainSetEnabled,Enable),H(xefgSwapChainSetNumInterpolatedFrames,Count),H(xefgSwapChainGetLastPresentStatus,LastStatus)};
#undef H
  std::vector<void*> targets;
  for (const auto& h:hooks) {
    if (bootstrapCreate && h.original==reinterpret_cast<void**>(&original_xefgSwapChainD3D12CreateContext)) continue;
    void* target=reinterpret_cast<void*>(GetProcAddress(fg,h.name));
    // OptiScaler's GameSetEnabled (among others) branches on its caller.
    // Chaining through our wrapper turns a host output call into a game call.
    // Keep the host's entry untouched, as with its XeLL redirects.
    if(target && HostRedirect(target)) {
      *h.original=target;
      xess_mod::WriteLog("host_entry_preserved","\"api\":\""+std::string(h.name)+"\"");
      continue;
    }
    if (!target || MH_CreateHook(target,h.target,h.original)!=MH_OK) {
      for (void* t:targets) MH_RemoveHook(t);
      throw std::runtime_error("SDK_hook_preflight");
    }
    targets.push_back(target);
  }
  for (void* t:targets) if (MH_QueueEnableHook(t)!=MH_OK) throw std::runtime_error("SDK_hook_queue");
  if (MH_ApplyQueued()!=MH_OK) throw std::runtime_error("SDK_hook_commit");
  Get().status.hooks=1;
  Detail("SDK hooks ready; waiting for XeFG output context");
  xess_mod::WriteLog("ready","\"provider_sha256\":\""+fh+"\",\"xell_sha256\":\""+lh+"\"");
}
void CompleteStartup() noexcept {
  // Do not enter call_once while inside the loader: another initializer may
  // need that same loader before it can complete.
  if (InsideLoader()) return;
  std::call_once(startupOnce,[] {
    try { Start(); }
    catch (const std::exception& e) { std::lock_guard lock(mutex); Detail(e.what()); startupFailed=true; xess_mod::WriteLog("startup_rejected","\"reason\":\""+escape(e.what())+"\""); }
    catch (...) { std::lock_guard lock(mutex); Detail("unexpected_startup_failure"); startupFailed=true; }
    // Start discovery/publication before creating an additional thread. Startup
    // records already fit in the bounded queue; also drain a rejected startup.
    try { StartLog(); } catch(...) {}
  });
}
bool CaptureEarlyCreate() noexcept {
  // Only inspect a mapped public export. In the initial dependency graph the
  // Intel image may already be mapped even though its CRT/DllMain has not run.
  // Calling GetVersion, touching its catalogs or loading another SDK here is
  // unsafe (the X1 Cyberpunk dump reproduced exactly that failure).
  const auto module=GetModuleHandleW(L"libxess_fg.dll");
  if (!module) return false;
  void* target=reinterpret_cast<void*>(GetProcAddress(module,"xefgSwapChainD3D12CreateContext"));
  const auto mh=MH_Initialize();
  if (!target || (mh!=MH_OK && mh!=MH_ERROR_ALREADY_INITIALIZED)) return false;
  for (const auto owner : {self,module}) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(owner),&pinned) || pinned!=owner) return false;
  }
  if (MH_CreateHook(target,reinterpret_cast<void*>(&Create),
      reinterpret_cast<void**>(&original_xefgSwapChainD3D12CreateContext))!=MH_OK) return false;
  bootstrapFg=module;
  if (MH_EnableHook(target)!=MH_OK) return false; // Retain any published trampoline.
  bootstrapCreate=true;
  return true;
}
DWORD WINAPI StartupWorker(void* reference) {
  // Windows starts this thread after the process's DLL initialization phase.
  // No wait/join is performed by InitializeASI or DllMain.
  CompleteStartup();
  FreeLibraryAndExitThread(static_cast<HMODULE>(reference),0);
}
} // namespace

namespace xess_mod {
namespace { std::atomic<ID3D12Device*> vulkanHostDevice{}; }
HMODULE Self() noexcept { return self; }
bool ActiveSdkPair(HMODULE& outFg,HMODULE& outLl) noexcept {
  CompleteStartup();
  std::lock_guard lock(mutex);
  if(!Get().status.hooks || !fg || !ll) return false;
  outFg=fg; outLl=ll; return true;
}
bool AdoptVulkanHostPair(HMODULE hostFg,HMODULE hostLl) noexcept {
  std::lock_guard lock(mutex);
  return Get().status.hooks && hostFg && hostFg==fg && hostLl==ll;
}
bool HostRedirectedExport(HMODULE module,const char* name) noexcept {
  return module && name && HostRedirect(reinterpret_cast<const void*>(GetProcAddress(module,name)));
}
void SetVulkanHostDevice(ID3D12Device* device) noexcept { vulkanHostDevice.store(device,std::memory_order_release); }
bool VulkanHostDevice(const ID3D12Device* device) noexcept {
  return device && device==vulkanHostDevice.load(std::memory_order_acquire);
}
void WriteLog(const char* stage,const std::string& fields) noexcept {
  try { LogQueue().Push(GetTickCount64(),GetCurrentProcessId(),stage,fields); } catch(...) {}
}
}
namespace mfgunlock::xemfg {
LogStatus LogSnapshot() noexcept {
  const auto stats=LogQueue().Snapshot();
  return {stats.pending,stats.dropped,stats.batches,stats.failed,stats.started};
}
}
extern "C" __declspec(dllexport) bool MFGAXeLogFlush(uint32_t timeoutMs) {
  // Explicit diagnostic/test boundary only; never called by frame hooks.
  return LogQueue().Flush((std::min)(timeoutMs,2000u));
}
extern "C" __declspec(dllexport) void InitializeASI() {
  std::call_once(initializeOnce,[] {
    if (!InsideLoader()) { CompleteStartup(); return; }
    if (mfgunlock::backend::Read()!=mfgunlock::backend::Kind::XeMfg) {
      std::lock_guard lock(mutex); Detail("DLSS-G selected; XeMFG companion inactive"); return;
    }
    if (!CaptureEarlyCreate()) {
      std::lock_guard lock(mutex); Detail("Early XeFG Create entry unavailable; native startup retained");
#ifndef MFGUNLOCK_NATIVE_XEMFG
      return;
#endif
    }
    { std::lock_guard lock(mutex); Detail("Waiting for DLL initialization to finish"); }
    HMODULE reference{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(self),&reference)) return;
    const auto thread=CreateThread(nullptr,0,&StartupWorker,reference,0,nullptr);
    if (thread) CloseHandle(thread);
    else FreeLibrary(reference); // First captured Create can still complete startup.
  });
}
extern "C" __declspec(dllexport) bool MFGAXeGetStatus(mfgunlock::xemfg::Status* out,uint32_t size) {
  if (!out || size!=sizeof(*out)) return false;
  std::lock_guard lock(mutex); auto& s=Get();
  s.status.tensor_launches=xefg_cuda::Snapshot().launches; *out=s.status; return true;
}
#ifdef MFGUNLOCK_NATIVE_XEMFG
namespace mfgunlock::xemfg {
bool TryModelStatus(Status& out) {
  std::unique_lock guard(mutex,std::try_to_lock);
  if(!guard.owns_lock()) return false;
  out=Get().status; return true;
}
bool NativeRuntimeModules(NativeRuntimePair& out) {
  std::lock_guard guard(mutex);
  if(!Get().status.hooks) return false;
  out={fg,ll}; return fg && ll;
}
bool ModelStartupFailed() noexcept { return startupFailed.load(); }
bool RuntimeFilesRejected(RuntimeFileReport (&out)[2]) noexcept {
  std::lock_guard guard(runtimeFilesLock);
  if(!runtimeFilesRejected) return false;
  out[0]=runtimeFiles[0]; out[1]=runtimeFiles[1]; return true;
}
void SetGameMultiplier(uint32_t multiplier) noexcept {
  userGenerated=multiplier>=2 && multiplier<=6?multiplier-1:0;
#ifdef MFGUNLOCK_XEMFG_VULKAN
  native::vulkan::SetFrames(multiplier);
#endif
}
bool GameContextInitializing() noexcept { return gameInitializing || gameInits.load()>0; }
bool GameOwnsFrameGeneration() noexcept { return GameContextInitializing() || gameContexts.load()>0; }
uint32_t FlipQueueNativeBuffers() noexcept { return ConfigureFlipQueue().buffers; }
// 1.8 (plan D.2): the XMX mode publishes the same ported model and count unlock as
// the swap chain route, for XeFG's own context. Once per process, before its Init.
bool PublishMultiFrameModel(HMODULE fg,HMODULE ll,HMODULE payload,std::string& error) noexcept {
  try {
    static auto shaders=std::make_unique<ShaderPort>(fg,payload);
    static auto model=std::make_unique<ModelPort>(fg,true,true);
    static auto unlock=std::make_unique<xess_host_unlock::Unlock>(fg,ll,5,false);
    return shaders && model && unlock;
  } catch(const std::exception& e) { error=e.what(); }
  catch(...) { error="unknown"; }
  return false;
}
void AttachModel(HMODULE module) {
  self=module;
  InitializeASI();
}
bool EnsureModel() {
  CompleteStartup();
  std::lock_guard lock(mutex);
  return Get().status.hooks!=0;
}
}
#else
BOOL WINAPI DllMain(HMODULE module,DWORD reason,LPVOID) {
  if (reason==DLL_PROCESS_ATTACH) { self=module; DisableThreadLibraryCalls(module); }
  return TRUE;
}
#endif
