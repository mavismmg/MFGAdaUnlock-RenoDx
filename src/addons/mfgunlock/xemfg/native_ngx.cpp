// SPDX-License-Identifier: MIT
#include "nr_bridge.hpp"
#include "native_ngx.hpp"
#include "native_frame_packet.hpp"
#include "native_constants.hpp"
#include "native_synthetic.hpp"
#include "../backend_selection.hpp"
#include <nvsdk_ngx.h>
#include <DirectXMath.h>
#include <map>
#include <mutex>
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif

namespace mfgunlock::xemfg::native::ngx {
namespace {
using CreateFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using EvaluateFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
CreateFn create{}; EvaluateFn evaluate{}; ReleaseFn release{};
std::mutex lock;
std::map<const NVSDK_NGX_Handle*,int> superSampling; // opaque NGX handle -> DLSS create flags
uint32_t logBudget{};
bool Enabled() {
  static const bool enabled=[] {
    const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
    return GetPrivateProfileIntW(L"Adapters",L"DlssSr",0,ini.c_str())!=0;
  }();
  return enabled;
}
template<class T> T Read(const NVSDK_NGX_Parameter& p,const char* name,T fallback) {
  T value=fallback;
  return NVSDK_NGX_SUCCEED(p.Get(name,&value))?value:fallback;
}
packet::Resource Input(xefg_swapchain_resource_type_t type,ID3D12Resource* resource,ID3D12GraphicsCommandList* list,
    const DejitterRegion& extent) {
  packet::Resource input;
  input.type=type; input.native=resource; input.headerValid=resource!=nullptr;
  // DLSS reads its inputs as shader resources during evaluation.
  input.state=uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  input.lifecycle=packet::Lifecycle::OnlyNow; input.rawLifecycle=uint32_t(input.lifecycle);
  input.producer=list; input.extent=extent;
  return input;
}
void Feed(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter& p) {
  int flags{};
  {
    std::lock_guard guard(lock);
    const auto found=superSampling.find(handle);
    if(found==superSampling.end()) return;
    flags=found->second;
  }
  const auto o=Active();
  const auto id=o?synthetic::SubmitFrame(o,synthetic::kDlssSr):0;
  if(!id) return;
  auto& s=State();
  if(!s.requested.load()) { s.requested=true; s.nativeOff=true; s.reflexOptionsReady=true; s.reflexOff=true; }
  ID3D12Resource* depth=Read<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Depth,nullptr);
  ID3D12Resource* motion=Read<ID3D12Resource*>(p,NVSDK_NGX_Parameter_MotionVectors,nullptr);
  uint32_t width=Read<unsigned int>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,0u);
  uint32_t height=Read<unsigned int>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,0u);
  if((!width || !height) && depth) { const auto d=depth->GetDesc(); width=uint32_t(d.Width); height=d.Height; }
  const DejitterRegion depthArea{Read<unsigned int>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,0u),
      Read<unsigned int>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,0u),width,height};
  // Without MVLowRes the vectors are at output resolution: the whole texture.
  DejitterRegion motionArea{Read<unsigned int>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,0u),
      Read<unsigned int>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,0u),width,height};
  if(!(flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) && motion) motionArea={0,0,0,0};
  const float motionWidth=float(motionArea.width?motionArea.width:(motion?uint32_t(motion->GetDesc().Width):width));
  const float motionHeight=float(motionArea.height?motionArea.height:(motion?motion->GetDesc().Height:height));
  xefg_swapchain_frame_constant_data_t data{};
  packet::Conventions stated;
  const bool inverted=flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  const bool jittered=flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
  stated.depthInverted=inverted; stated.motionJittered=jittered;
  uint32_t issues{};
  if(inverted!=o->invert) issues|=ConstantDepth;
  if(jittered!=o->jitter) issues|=ConstantJitterMode;
  // NGX carries no camera: an assumed 60 degree projection with the game's
  // depth direction, and an identity view (camera motion is in the vectors).
  constexpr float kFov=1.0471976f,kNear=.1f,kFar=10000.f;
  if(width && height) {
    DirectX::XMFLOAT4X4 projection;
    DirectX::XMStoreFloat4x4(&projection,DirectX::XMMatrixPerspectiveFovLH(kFov,float(width)/float(height),
        inverted?kFar:kNear,inverted?kNear:kFar));
    memcpy(data.projectionMatrix,&projection,sizeof(data.projectionMatrix));
  } else issues|=ConstantProjection;
  for(unsigned i=0;i<16;++i) data.viewMatrix[i]=i%5==0?1.f:0.f;
  data.jitterOffsetX=Read<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);
  data.jitterOffsetY=Read<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
  // DLSS scales vectors to pixels; this pipeline expects pixels per motion size.
  data.motionVectorScaleX=Read<float>(p,NVSDK_NGX_Parameter_MV_Scale_X,1.f)/motionWidth;
  data.motionVectorScaleY=Read<float>(p,NVSDK_NGX_Parameter_MV_Scale_Y,1.f)/motionHeight;
  data.resetHistory=Read<int>(p,NVSDK_NGX_Parameter_Reset,0)!=0;
  for(float v:data.projectionMatrix) if(!ConstantFinite(v)) issues|=ConstantProjection;
  if(!ConstantFinite(data.jitterOffsetX) || !ConstantFinite(data.jitterOffsetY)) issues|=ConstantJitter;
  if(!ConstantFinite(data.motionVectorScaleX) || !ConstantFinite(data.motionVectorScaleY)) issues|=ConstantMotionScale;
  packet::Conventions learn;
  {
    std::lock_guard io(o->io);
    if(auto* frame=FindFrame(*o,id)) {
      frame->viewport=0;
      packet::SubmitResource(*o,*frame,id,Input(XEFG_SWAPCHAIN_RES_DEPTH,depth,list,depthArea));
      packet::SubmitResource(*o,*frame,id,Input(XEFG_SWAPCHAIN_RES_MOTION_VECTOR,motion,list,motionArea));
      learn=packet::SubmitConstants(*frame,issues,data,stated);
      if(logBudget<4 || (issues && logBudget<12)) {
        ++logBudget;
        xess_mod::WriteLog("native_ngx_evaluate","\"id\":"+std::to_string(id)+",\"issues\":"+std::to_string(issues)
            +",\"resources\":"+std::to_string(frame->resources)+",\"render_width\":"+std::to_string(width)
            +",\"render_height\":"+std::to_string(height)+",\"flags\":"+std::to_string(flags));
      }
    }
  }
  if(learn.depthInverted || learn.motionJittered) LearnStatedConventions(learn.depthInverted,learn.motionJittered);
}
NVSDK_NGX_Result NVSDK_CONV CreateHook(ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** out) {
  const auto result=create(list,feature,params,out);
  // 1.12.1 (D3): every feature, for DLSS 5 (nr_bridge.hpp).
  if(NVSDK_NGX_SUCCEED(result) && out && *out)
    if(const auto created=nrbridge::g_created.load(std::memory_order_acquire)) created(int(feature),params,*out);
  if(NVSDK_NGX_SUCCEED(result) && feature==NVSDK_NGX_Feature_SuperSampling && out && *out) {
    const int flags=params?Read<int>(*params,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,0):0;
    { std::lock_guard guard(lock); superSampling[*out]=flags; }
    xess_mod::WriteLog("native_ngx_feature","\"handle\":"+std::to_string(uintptr_t(*out))+",\"flags\":"+std::to_string(flags));
  }
  return result;
}
NVSDK_NGX_Result NVSDK_CONV EvaluateHook(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params,PFN_NVSDK_NGX_ProgressCallback callback) {
  // DLSS-SR itself runs first and unchanged; its inputs are still valid on
  // the same list afterwards, where the volatile copies are recorded.
  const auto result=evaluate(list,handle,params,callback);
  if(NVSDK_NGX_SUCCEED(result) && list && handle && params) {
    Feed(list,handle,*params);
    // 1.12.1 (D3): DLSS 5 after the upscaler, on the same list, after XeFG's copies of the guides.
    if(const auto evaluated=nrbridge::g_evaluated.load(std::memory_order_acquire)) evaluated(list,handle,params);
  }
  return result;
}
NVSDK_NGX_Result NVSDK_CONV ReleaseHook(NVSDK_NGX_Handle* handle) {
  if(handle) { std::lock_guard guard(lock); superSampling.erase(handle); }
  if(handle)
    if(const auto released=nrbridge::g_released.load(std::memory_order_acquire)) released(handle);
  return release(handle);
}
}
bool Armed() noexcept { return Enabled() && !GetModuleHandleW(L"sl.interposer.dll"); }
namespace { std::atomic<int> saved{-1}; }
bool Configured() noexcept { return Enabled(); }
bool Suggested() noexcept {
  return !Enabled() && !GetModuleHandleW(L"sl.interposer.dll") && GetModuleHandleW(L"_nvngx.dll")
      && !synthetic::Owner() && !Active();
}
bool Save(bool enabled) {
  if(backend::Directory().empty()
      || !config::Save(backend::Directory()/L"MFGAdaUnlock.ini",{{L"Adapters",L"DlssSr",enabled?L"1":L"0"}})) return false;
  saved=enabled?1:0;
  xess_mod::WriteLog("native_dlss_sr_saved","\"enabled\":"+std::string(enabled?"true":"false")+",\"applies\":\"next_launch\"");
  return true;
}
int Saved() noexcept { return saved.load(); }
void OutputCreated(const std::shared_ptr<Output>& o) {
  if(Armed() && !synthetic::Owner()) synthetic::Begin(o,synthetic::kDlssSr);
}
void Discover() {
#ifdef MFGUNLOCK_XEMFG_VULKAN
  vulkan::DiscoverNgx();
#endif
  if(!Enabled()) return;
  static std::mutex once; std::lock_guard guard(once);
  HMODULE module=GetModuleHandleW(L"_nvngx.dll");
  if(!module) return;
  HMODULE pinned{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pinned)) return;
  const auto entry=[&](const char* function) { return reinterpret_cast<void*>(GetProcAddress(module,function)); };
  const bool hooks[]={Hook(entry("NVSDK_NGX_D3D12_CreateFeature"),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&create)),
      Hook(entry("NVSDK_NGX_D3D12_EvaluateFeature"),reinterpret_cast<void*>(&EvaluateHook),reinterpret_cast<void**>(&evaluate)),
      Hook(entry("NVSDK_NGX_D3D12_ReleaseFeature"),reinterpret_cast<void*>(&ReleaseHook),reinterpret_cast<void**>(&release))};
  uint32_t mask{}; for(unsigned i=0;i<3;++i) mask|=uint32_t(hooks[i])<<i;
  static uint32_t logged{};
  if(logged!=mask+1) {
    logged=mask+1;
    xess_mod::WriteLog("native_ngx_hooks","\"version\":\""+FileVersion(module)+"\",\"hooked\":"+(mask==7?"true":"false")
        +",\"mask\":"+std::to_string(mask));
  }
}
}
// Panel hint for conformance: bit 0 opted in, bit 1 suggested, bit 2 saved this session.
extern "C" __declspec(dllexport) uint32_t MFGAXeDlssSrHint() {
  namespace ngx=mfgunlock::xemfg::native::ngx;
  return uint32_t(ngx::Configured())|uint32_t(ngx::Suggested())<<1|uint32_t(ngx::Saved()>=0)<<2;
}
extern "C" __declspec(dllexport) bool MFGAXeDlssSrSave(bool enabled) { return mfgunlock::xemfg::native::ngx::Save(enabled); }
