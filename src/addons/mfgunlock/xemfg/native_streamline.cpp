// SPDX-License-Identifier: MIT
#include "native_internal.hpp"
#include "vendor/sdk_pacing.h"
#include "native_frame_packet.hpp"
#include "native_ffx.hpp"
#include "native_ngx.hpp"
#include "native_synthetic.hpp"
#include "native_constants.hpp"
#include "native_input_policy.hpp"
#include "log_budget.hpp"
#include "native_profiles.hpp"
#include "../backend_selection.hpp"
#include "native_continuity.hpp"
#include "../latency_marker_policy.hpp"
#include <cmath>
#include <array>
#include <algorithm>
#include <psapi.h>
#include <intrin.h>
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif

namespace mfgunlock::xemfg::native {
namespace {
std::recursive_mutex discovery;
PFun_slGetFeatureFunction* getFeature{};
PFun_slSetTag* setTag{};
PFun_slSetTagForFrame* setTagFrame{};
PFun_slSetConstants* setConstants{};
PFun_slEvaluateFeature* evaluate{};
PFun_slDLSSGSetOptions* setOptions{};
PFun_slDLSSGGetState* getState{};
PFun_slReflexSetOptions* reflexOptions{};
PFun_slReflexSleep* reflexSleep{};
PFun_slPCLSetMarker* pclMarker{};
// Public pre-PCL Streamline ABI: 32-bit ReflexMarker and FrameToken reference.
// Keep a separate original: some plugins export both entrypoints with distinct bodies.
using LegacyMarker=sl::Result(uint32_t,const sl::FrameToken&);
LegacyMarker* legacyMarker{};
void* markerEntries[2]{};
PFun_slInit* initialize{};
PFun_slSetD3DDevice* setDevice{};
PFun_slShutdown* shutdown{};
PFun_slIsFeatureLoaded* getLoaded{};
PFun_slSetFeatureLoaded* setLoaded{};
PFun_slIsFeatureSupported* isSupported{};
PFun_slGetFeatureRequirements* requirements{};
std::atomic<bool> lifecycleApi{},presentationIsolated{},logicalLoaded{true},stopping{};

// slSetFeatureLoaded rebuilds Streamline's hook lists and is not thread safe.
// Use ONLY the game's startup API boundary, never a worker or Present callback.
bool IsolatePresentation(const char* boundary,bool captureRequest) {
  if(single_overlay::native::InsideLoader() || !lifecycleApi || !getLoaded || !setLoaded || Active()) return false;
  bool loaded{};
  const auto queried=getLoaded(sl::kFeatureDLSS_G,loaded);
  const auto result=queried==sl::Result::eOk?setLoaded(sl::kFeatureDLSS_G,false):queried;
  if(captureRequest && queried==sl::Result::eOk) logicalLoaded=loaded;
  bool physical=true;
  const auto verified=result==sl::Result::eOk?getLoaded(sl::kFeatureDLSS_G,physical):result;
  presentationIsolated=result==sl::Result::eOk && verified==sl::Result::eOk && !physical;
  xess_mod::WriteLog("native_presentation_isolation","\"boundary\":\""+std::string(boundary)
      +"\",\"result\":"+std::to_string(uint32_t(result))+",\"isolated\":"+(presentationIsolated.load()?"true":"false")
      +",\"game_loaded\":"+(logicalLoaded.load()?"true":"false"));
  return presentationIsolated;
}
sl::Result InitHook(const sl::Preferences& preferences,uint64_t version) {
  std::lock_guard guard(discovery);
  stopping=false; presentationIsolated=false;
  const auto result=initialize(preferences,version);
  LogStreamlineVersion(version);
  if(result==sl::Result::eOk) IsolatePresentation("slInit",true);
  return result;
}
sl::Result DeviceHook(void* device) {
  std::lock_guard guard(discovery);
  // Covers integrations that initialized SL before the addon was loaded.
  // This explicit initialization call belongs to the host's quiescent startup.
  LogStreamlineVersion(0);
  if(!presentationIsolated && !stopping && !Snapshot().frames)
    IsolatePresentation("slSetD3DDevice",true);
  return setDevice(device);
}
sl::Result ShutdownHook() {
  stopping=true;
  // Drain discovery, then release the lock before SL joins its plugin workers.
  // A plugin's shutdown may itself resolve a function on another thread.
  { std::lock_guard guard(discovery); presentationIsolated=false; }
  xess_mod::WriteLog("native_streamline_shutdown","\"phase\":\"drained\"");
  ExportCompatibilityReport("shutdown");
  ClearRetainedNative();
  const auto result=shutdown();
  xess_mod::WriteLog("native_streamline_shutdown","\"phase\":\"complete\",\"result\":"+std::to_string(uint32_t(result)));
  return result;
}
sl::Result SetLoadedHook(sl::Feature feature,bool enabled) {
  std::lock_guard guard(discovery);
  if(feature!=sl::kFeatureDLSS_G || stopping || !getLoaded) return setLoaded(feature,enabled);
  const auto result=setLoaded(feature,false);
  if(result==sl::Result::eOk) {
    bool physical=true;
    const auto verified=getLoaded(feature,physical);
    logicalLoaded=enabled; presentationIsolated=verified==sl::Result::eOk && !physical;
    if(!enabled) { State().requested=false; State().streamlineGeneration=false; }
  }
  return result;
}
sl::Result GetLoadedHook(sl::Feature feature,bool& enabled) {
  std::lock_guard guard(discovery);
  const auto result=getLoaded(feature,enabled);
  if(result==sl::Result::eOk && feature==sl::kFeatureDLSS_G && presentationIsolated)
    enabled=logicalLoaded;
  return result;
}
// A structure refused for its type, version or extension chain. Grouped per
// API/version so a game repeating the call every frame logs once.
void StructEvent(const char* event,const char* api,const sl::BaseStructure& value,const char* reason) {
  static PatternBudget budget(48);
  const bool chained=value.next!=nullptr;
  const auto key=std::string(event)+'/'+api+'/'+std::to_string(value.structVersion)+'/'+(chained?"1":"0")+'/'+reason;
  if(const auto count=budget.Admit(key))
    xess_mod::WriteLog(event,"\"api\":\""+std::string(api)+"\",\"version\":"+std::to_string(value.structVersion)
        +",\"next\":"+(chained?"true":"false")+",\"reason\":\""+reason+"\",\"occurrence\":"+std::to_string(count));
}
void StructRejected(const char* api,const sl::BaseStructure& value,const char* reason) { StructEvent("native_struct_rejected",api,value,reason); }
// A newer or chained structure accepted through its known prefix.
void StructAdapted(const char* api,const sl::BaseStructure& value,const char* reason) { StructEvent("native_struct_adapted",api,value,reason); }

// Observation only: separates "the game hid FG" from "FG inputs were refused".
void FeatureQuery(const char* api,sl::Feature feature,sl::Result result,sl::Result native) {
  static PatternBudget budget(32);
  const auto key=std::string(api)+'/'+std::to_string(uint32_t(feature))+'/'+std::to_string(uint32_t(result))+'/'+std::to_string(uint32_t(native));
  if(const auto count=budget.Admit(key))
    xess_mod::WriteLog("native_feature_query","\"api\":\""+std::string(api)+"\",\"feature\":"+std::to_string(uint32_t(feature))
        +",\"result\":"+std::to_string(uint32_t(result))+",\"native_result\":"+std::to_string(uint32_t(native))
        +",\"occurrence\":"+std::to_string(count));
}
// RTX 20/30 (plan 2.5): Streamline refuses DLSS-G on adapters without the
// NVIDIA frame-generation hardware, so the game hides its FG option. With
// XeMFG selected (this bridge only runs then), generation is Intel's: present
// DLSS-G as supported for adapter/driver refusals only. Other refusals, such
// as a missing plugin file or OS, are returned unchanged.
bool XeMfgCoversRefusal(sl::Feature feature,sl::Result result) noexcept {
  return feature==sl::kFeatureDLSS_G && (result==sl::Result::eErrorAdapterNotSupported
      || result==sl::Result::eErrorNoSupportedAdapterFound || result==sl::Result::eErrorFeatureNotSupported
      || result==sl::Result::eErrorDriverOutOfDate);
}
// Set when the DLSS-G plugin exposes no option/state functions; the bridge
// then implements both itself and NVIDIA generation is absent by construction.
std::atomic<bool> syntheticGeneration{};
// Counts every tag call per buffer type and viewport, before any filtering.
void CountTags(const std::shared_ptr<Output>& o,uint32_t viewport,const sl::ResourceTag* tags,uint32_t count) {
  if(!o || !tags) return;
  std::lock_guard guard(o->io);
  if(o->retired) return;
  for(uint32_t i=0;i<count && i<64;++i) ++o->tagTypeCounts[(std::min)(uint32_t(tags[i].type),63u)];
  bool counted{};
  for(auto& slot:o->tagViewports) {
    if(slot.count && slot.viewport==viewport) { ++slot.count; counted=true; break; }
    if(!slot.count) { slot={viewport,1}; counted=true; break; }
  }
  if(!counted) ++o->otherViewportTags;
  if(++o->taggedCalls==240) { LogTagHistogram(*o,"steady"); ExportCompatibilityReport("steady"); }
}
// First constants of each output, verbatim: the conventions a new game uses.
void SampleConstants(Output& o,const sl::Constants& c,uint32_t id,uint32_t issues) {
  if(o.constantsSampled) return;
  o.constantsSampled=true;
  // Streamline marks unset values with INVALID_FLOAT (FLT_MAX).
  const auto f=[](float v) { return ConstantFinite(v)?std::to_string(v):std::string("null"); };
  const auto b=[](sl::Boolean v) { return v==sl::eTrue?std::string("true"):v==sl::eFalse?std::string("false"):std::string("\"invalid\""); };
  xess_mod::WriteLog("native_constants_sample","\"output\":"+std::to_string(o.diagnosticOutput)+",\"id\":"+std::to_string(id)
      +",\"version\":"+std::to_string(c.structVersion)+",\"issues\":"+std::to_string(issues)
      +",\"mvec_scale\":["+f(c.mvecScale.x)+','+f(c.mvecScale.y)+"],\"jitter\":["+f(c.jitterOffset.x)+','+f(c.jitterOffset.y)
      +"],\"camera_near\":"+f(c.cameraNear)+",\"camera_far\":"+f(c.cameraFar)+",\"camera_fov\":"+f(c.cameraFOV)
      +",\"camera_aspect\":"+f(c.cameraAspectRatio)+",\"depth_inverted\":"+b(c.depthInverted)
      +",\"camera_motion_included\":"+b(c.cameraMotionIncluded)+",\"motion_3d\":"+b(c.motionVectors3D)
      +",\"motion_jittered\":"+b(c.motionVectorsJittered)+",\"motion_dilated\":"+b(c.motionVectorsDilated)
      +",\"orthographic\":"+b(c.orthographicProjection)+",\"reset\":"+b(c.reset));
}
// Persist a convention the game's constants contradicted (plan 3.3). The
// next Intel output (swapchain recreation or the next launch of this exact
// executable) initializes with it. Explicit INI values are never touched.
void LearnConventions(std::optional<bool> invert,std::optional<bool> jitter) {
  auto& s=State();
  {
    std::lock_guard guard(s.lock);
    if(invert) s.invert=*invert;
    if(jitter) s.jitter=*jitter;
  }
  const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
  bool saved=config::Save(ini,{{L"XeFG.Learned",L"Executable",s.executableIdentity.c_str()}});
  if(invert) saved=saved && config::Save(ini,{{L"XeFG.Learned",L"InvertedDepth",*invert?L"1":L"0"}});
  if(jitter) saved=saved && config::Save(ini,{{L"XeFG.Learned",L"JitteredMotion",*jitter?L"1":L"0"}});
  xess_mod::WriteLog("native_convention_learned",std::string(invert?"\"inverted_depth\":"+std::string(*invert?"true":"false")+",":"")
      +(jitter?"\"jittered_motion\":"+std::string(*jitter?"true":"false")+",":"")+"\"saved\":"+(saved?"true":"false")
      +",\"applies\":\"next_output\"");
  Report("Camera conventions learned from the game; they apply when the game recreates its display output or on the next launch");
}
sl::Result SupportedHook(sl::Feature feature,const sl::AdapterInfo& adapter) {
  const auto native=isSupported(feature,adapter);
  const auto result=XeMfgCoversRefusal(feature,native)?sl::Result::eOk:native;
  FeatureQuery("slIsFeatureSupported",feature,result,native);
  return result;
}
sl::Result RequirementsHook(sl::Feature feature,sl::FeatureRequirements& value) {
  const auto native=requirements(feature,value);
  const auto result=XeMfgCoversRefusal(feature,native)?sl::Result::eOk:native;
  FeatureQuery("slGetFeatureRequirements",feature,result,native);
  return result;
}

// Viewport selection by complete inputs (user decision 2026-10-22, the
// menu-viewport-first case): the first FG request selects a viewport, and the
// selection moves at a Present boundary to another requesting viewport that
// sends depth, motion and constants when the selected one never has, or has
// sent none for 30 frames.
struct ViewportInputs { uint32_t viewport{UINT32_MAX}, mode{}, lastInputs{}, lastConstants{}; bool complete{}; };
std::mutex viewportLock;
std::array<ViewportInputs,8> viewportInputs;
ViewportInputs* ViewportSlot(uint32_t v) {
  for(auto& slot:viewportInputs) if(slot.viewport==v) return &slot;
  for(auto& slot:viewportInputs) if(slot.viewport==UINT32_MAX) { slot={}; slot.viewport=v; return &slot; }
  return nullptr;
}
void ObserveViewport(uint32_t v,uint32_t id,bool inputs,bool constants) {
  if(!id) return;
  std::lock_guard guard(viewportLock);
  auto* slot=ViewportSlot(v); if(!slot) return;
  if(inputs) slot->lastInputs=id;
  if(constants) slot->lastConstants=id;
  if(slot->lastInputs && slot->lastConstants && (slot->lastInputs>slot->lastConstants?slot->lastInputs-slot->lastConstants:slot->lastConstants-slot->lastInputs)<=1)
    slot->complete=true;
}
void RecordViewportTags(uint32_t v,uint32_t id,const sl::ResourceTag* tags,uint32_t count) {
  if(!tags || count>64) return;
  bool depth{},motion{};
  for(uint32_t i=0;i<count;++i) {
    if(!tags[i].resource || !tags[i].resource->native) continue;
    depth|=tags[i].type==sl::kBufferTypeDepth; motion|=tags[i].type==sl::kBufferTypeMotionVectors;
  }
  if(depth && motion) ObserveViewport(v,id,true,false);
}
bool SelectViewport(uint32_t v) {
  auto& s=State(); auto expected=UINT32_MAX;
  s.viewport.compare_exchange_strong(expected,v);
  return s.viewport.load()==v;
}
bool SelectedViewport(uint32_t v) {return State().viewport.load()==v;}
struct InputLease {
  std::shared_ptr<Output> output;
  uint64_t epoch{};
  InputLease():output(Active()) {
    if(output) {std::lock_guard guard(output->io);epoch=output->epoch;}
  }
  bool Current() const {
    return output && compatibility::CurrentLease(epoch,output->epoch,output->retired) && output->fg && !output->deviceLost;
  }
};
void TraceInput(const char* api,uint32_t id,uint32_t viewport=UINT32_MAX,int marker=-1) {
  if(!TraceEnabled() || id%64>=2)return;
  xess_mod::WriteLog("native_input_call","\"api\":\""+std::string(api)+"\",\"id\":"+std::to_string(id)
      +",\"viewport\":"+std::to_string(viewport)+",\"marker\":"+std::to_string(marker)+",\"thread\":"+std::to_string(GetCurrentThreadId()));
}
std::optional<xefg_swapchain_resource_type_t> ResourceType(sl::BufferType type) {
  switch(type) {
    case sl::kBufferTypeDepth: return XEFG_SWAPCHAIN_RES_DEPTH;
    case sl::kBufferTypeMotionVectors: return XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    case sl::kBufferTypeHUDLessColor: return XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
    case sl::kBufferTypeUIColorAndAlpha: return XEFG_SWAPCHAIN_RES_UI;
    default: return {};
  }
}
// Streamline DLSS-G input adapter (plan 5.1): translates slSetTag into
// adapter-neutral packet inputs; validation and tagging live in native_frame_packet.
packet::Lifecycle PacketLifecycle(sl::ResourceLifecycle lifecycle) {
  if(lifecycle==sl::eValidUntilPresent) return packet::Lifecycle::UntilPresent;
  if(lifecycle==sl::eOnlyValidNow || lifecycle==sl::eValidUntilEvaluate) return packet::Lifecycle::OnlyNow;
  return packet::Lifecycle::Invalid;
}
void Tags(const InputLease& lease,uint32_t id,uint32_t viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command) {
  RecordViewportTags(viewport,id,tags,count);
  if(!id || !tags || count>64 || !SelectedViewport(viewport)) return;
  auto o=lease.output; if(!o) return;
  std::lock_guard lock(o->io);
  if(!lease.Current()) {++State().staleInputs;return;}
  auto* frame=FindFrame(*o,id); if(!frame) return;
  frame->viewport=viewport;
  for(uint32_t i=0;i<count;++i) {
    const auto& t=tags[i];
    const DejitterRegion extent{t.extent.left,t.extent.top,t.extent.width,t.extent.height};
    if(t.type==sl::kBufferTypeBackbuffer) { packet::SubmitBackbufferRegion(*o,*frame,id,extent); continue; }
    auto type=ResourceType(t.type); if(!type) continue;
    packet::Resource input;
    input.type=*type; input.extent=extent; input.rawLifecycle=uint32_t(t.lifecycle);
    input.lifecycle=PacketLifecycle(t.lifecycle); input.producer=reinterpret_cast<IUnknown*>(command);
    input.tagVersion=t.structVersion;
    if(t.resource && t.resource->native) {
      input.native=reinterpret_cast<IUnknown*>(t.resource->native);
      input.state=t.resource->state; input.resourceVersion=t.resource->structVersion;
      // A newer ResourceTag is read through its V1 prefix (append-only ABI).
      input.headerValid=t.structType==sl::ResourceTag::s_structType && t.structVersion>=1 && ResourceHeaderSupported(*t.resource)
          && t.resource->type==sl::ResourceType::eTex2d;
      if(input.headerValid && input.state!=UINT_MAX) {
        if(t.structVersion>1) StructAdapted("slSetTag",t,"v1_prefix_read");
        if(t.resource->structVersion>1) StructAdapted("sl::Resource",*t.resource,"v1_prefix_read");
      }
    } else if(t.resource) input.resourceVersion=t.resource->structVersion;
    packet::SubmitResource(*o,*frame,id,input);
  }
}
void RejectUpstreamInput(const InputLease& lease,uint32_t id,uint32_t viewport,sl::Result result,const char* api) {
  if(!id || !SelectedViewport(viewport)) return;
  if(auto output=lease.output) {
    std::lock_guard lock(output->io);
    if(!lease.Current()) {++State().staleInputs;return;}
    if(auto* frame=FindFrame(*output,id)) {
      frame->failed=true;
      if(frame->firstFailure==compatibility::Failure::None)frame->firstFailure=compatibility::Failure::InputRejected;
    }
    static std::atomic<uint32_t> messages{};
    if(messages.fetch_add(1)<8) xess_mod::WriteLog("native_input_api_error","\"api\":\""+std::string(api)
        +"\",\"id\":"+std::to_string(id)+",\"result\":"+std::to_string(uint32_t(result)));
  }
}
// 1.7 (plan F): a Vulkan game has no D3D12 output to feed; its Streamline
// inputs pass on untouched (the Vulkan host reads DLSS-SR's from NGX).
#ifdef MFGUNLOCK_XEMFG_VULKAN
#define MFG_VULKAN_PASS(call) if(vulkan::InstanceSeen()) return call;
#else
#define MFG_VULKAN_PASS(call)
#endif
sl::Result TagHook(const sl::ViewportHandle& v,const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c) {
  MFG_VULKAN_PASS(setTag(v,t,n,c))
  const InputLease lease;
  const auto id=State().recording.load();
  TraceInput("slSetTag",id,uint32_t(v));
  const auto r=setTag(v,t,n,c);
  CountTags(lease.output,uint32_t(v),t,n);
  if(r==sl::Result::eOk) Tags(lease,id,uint32_t(v),t,n,c);
  else RejectUpstreamInput(lease,id,uint32_t(v),r,"slSetTag");
  return r;
}
sl::Result TagFrameHook(const sl::FrameToken& f,const sl::ViewportHandle& v,
    const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c) {
  MFG_VULKAN_PASS(setTagFrame(f,v,t,n,c))
  const InputLease lease;
  TraceInput("slSetTagForFrame",FrameId(uint32_t(f)),uint32_t(v));
  const auto id=FrameId(uint32_t(f)); const auto r=setTagFrame(f,v,t,n,c);
  CountTags(lease.output,uint32_t(v),t,n);
  if(r==sl::Result::eOk) Tags(lease,id,uint32_t(v),t,n,c);
  else RejectUpstreamInput(lease,id,uint32_t(v),r,"slSetTagForFrame");
  return r;
}
sl::Result ConstantsHook(const sl::Constants& c,const sl::FrameToken& f,const sl::ViewportHandle& v) {
  MFG_VULKAN_PASS(setConstants(c,f,v))
  const InputLease lease;
  TraceInput("slSetConstants",FrameId(uint32_t(f)),uint32_t(v));
  const auto r=setConstants(c,f,v); const auto id=FrameId(uint32_t(f));
  if(r!=sl::Result::eOk) { RejectUpstreamInput(lease,id,uint32_t(v),r,"slSetConstants");return r; }
  std::optional<bool> learnInvert,learnJitter;
  ObserveViewport(uint32_t(v),id,false,true);
  if(SelectedViewport(uint32_t(v))) if(auto o=lease.output) {
    std::lock_guard lock(o->io);
    if(!lease.Current()) {++State().staleInputs;return r;}
    if(auto* frame=FindFrame(*o,id)) {
      xefg_swapchain_frame_constant_data_t data{};
      const auto issues=TranslateConstants(c,o->invert,o->jitter,data);
      packet::Conventions stated;
      if(c.depthInverted!=sl::eInvalid) stated.depthInverted=c.depthInverted==sl::eTrue;
      if(c.motionVectorsJittered!=sl::eInvalid) stated.motionJittered=c.motionVectorsJittered==sl::eTrue;
      const auto learn=packet::SubmitConstants(*frame,issues,data,stated);
      learnInvert=learn.depthInverted; learnJitter=learn.motionJittered;
      if(frame->constantIssues&ConstantMetadata)
        StructRejected("slSetConstants",c,c.structType!=sl::Constants::s_structType?"type":"version");
      else {
        if(c.structVersion>2) StructAdapted("slSetConstants",c,"v2_prefix_read");
        SampleConstants(*o,c,id,frame->constantIssues);
      }
      if(frame->constantIssues!=o->lastConstantIssues && o->constantLogBudget<12) {
        o->lastConstantIssues=frame->constantIssues; ++o->constantLogBudget;
        std::string flags;
        if(!(frame->constantIssues&ConstantMetadata)) flags=",\"camera_motion\":"+std::to_string(int(c.cameraMotionIncluded))
            +",\"motion_3d\":"+std::to_string(int(c.motionVectors3D))+",\"inverted\":"+std::to_string(int(c.depthInverted))
            +",\"jittered\":"+std::to_string(int(c.motionVectorsJittered))+",\"reset\":"+std::to_string(int(c.reset));
        xess_mod::WriteLog("native_constants_status","\"id\":"+std::to_string(id)+",\"issues\":"+std::to_string(frame->constantIssues)
            +",\"version\":"+std::to_string(c.structVersion)+flags);
      }
    }
  }
  if(learnInvert || learnJitter) LearnConventions(learnInvert,learnJitter);
  return r;
}
sl::Result OptionsHook(const sl::ViewportHandle& v,const sl::DLSSGOptions& opts) {
  // Only V1's mode is read; the native call receives the bridge's own V1.
  if(opts.structType!=sl::DLSSGOptions::s_structType || opts.structVersion<1) {
    StructRejected("slDLSSGSetOptions",opts,opts.structType!=sl::DLSSGOptions::s_structType?"type":"version");
    return sl::Result::eErrorUnsupportedInterface;
  }
  if(opts.structVersion>5) StructAdapted("slDLSSGSetOptions",opts,"v1_prefix_read");
  auto& s=State();
  // OFF from a menu/secondary viewport cannot claim the selected FG output.
  static std::atomic<uint32_t> optionBudget{};
  if(optionBudget.fetch_add(1)<16) xess_mod::WriteLog("native_viewport_options","\"viewport\":"+std::to_string(uint32_t(v))
      +",\"mode\":"+std::to_string(uint32_t(opts.mode))+",\"selected\":"+std::to_string(s.viewport.load()));
  { std::lock_guard guard(viewportLock); if(auto* slot=ViewportSlot(uint32_t(v))) slot->mode=uint32_t(opts.mode); }
  if(opts.mode!=sl::DLSSGMode::eOff) SelectViewport(uint32_t(v)); // a second requester waits for the selection rule
  // A known V1 prefix is enough to turn NVIDIA generation off. Do not copy
  // the newer structure from an older/smaller allocation supplied by a game.
  sl::DLSSGOptions disabled; disabled.structVersion=1; disabled.mode=sl::DLSSGMode::eOff;
  // Without a DLSS-G plugin there is no NVIDIA generation to turn off.
  const auto r=setOptions?setOptions(v,disabled):sl::Result::eOk;
  if(!SelectedViewport(uint32_t(v))) return r;
  s.nativeOff=r==sl::Result::eOk;
  s.requested=logicalLoaded && opts.mode!=sl::DLSSGMode::eOff;
#ifdef MFGUNLOCK_XEMFG_VULKAN
  vulkan::SetGameFrameGeneration(opts.mode!=sl::DLSSGMode::eOff); // the Vulkan host follows the game's switch
#endif
  s.streamlineGeneration=s.requested.load();
  static std::atomic<uint64_t> previous{UINT64_MAX};
  const uint64_t signature=uint64_t(uint32_t(opts.mode))|(uint64_t(uint32_t(r))<<32);
  if(previous.exchange(signature)!=signature)
    xess_mod::WriteLog("native_game_options","\"viewport\":"+std::to_string(uint32_t(v))+",\"mode\":"+std::to_string(uint32_t(opts.mode))
        +",\"version\":"+std::to_string(opts.structVersion)+",\"native_disable_result\":"+std::to_string(uint32_t(r)));
  return r;
}
sl::Result StateHook(const sl::ViewportHandle& v,sl::DLSSGState& state,const sl::DLSSGOptions*) {
  // Without a DLSS-G plugin the state starts from the game's default
  // structure; the fields below are filled by the bridge.
  if(!getState) state.status=sl::DLSSGStatus::eOk;
  const auto r=getState?getState(v,state,nullptr):sl::Result::eOk; if(r!=sl::Result::eOk) return r;
  // Fields are written only up to the known V4 layout; newer ones stay as
  // Streamline filled them.
  if(state.structType!=sl::DLSSGState::s_structType || state.structVersion<1) {
    StructRejected("slDLSSGGetState",state,state.structType!=sl::DLSSGState::s_structType?"type":"version");
    return r;
  }
  if(state.structVersion>4) StructAdapted("slDLSSGGetState",state,"v4_prefix_written");
  const auto status=Snapshot();
  const auto originalStatus=uint32_t(state.status);
  // The NVIDIA engine is deliberately disabled. Its Reflex/backbuffer hooks
  // do not own the Intel output, so their absence is not a XeMFG failure.
  // Preserve all other errors; never mask format, resolution or input faults.
  if(State().nativeOff && status.output) {
    constexpr auto nativeOnly=uint32_t(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)
        |uint32_t(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled);
    state.status=sl::DLSSGStatus(originalStatus&~nativeOnly);
  }
  static std::atomic<uint64_t> previous{UINT64_MAX};
  const uint64_t signature=uint64_t(originalStatus)|(uint64_t(uint32_t(state.status))<<32);
  if(previous.exchange(signature)!=signature)
    xess_mod::WriteLog("native_game_state","\"native_status\":"+std::to_string(originalStatus)+",\"reported_status\":"+std::to_string(uint32_t(state.status)));
  state.numFramesActuallyPresented=status.submitted;
  if(state.structVersion>=2) state.numFramesToGenerateMax=5;
  if(state.structVersion>=4) state.bIsDynamicMFGSupported=sl::eFalse;
  return r;
}
sl::Result ReflexOptionsHook(const sl::ReflexOptions& opts) {
  // Only a foreign structure is refused. Streamline structures are append-only,
  // so a newer version starts with the known V1 fields.
  if(opts.structType!=sl::ReflexOptions::s_structType || opts.structVersion<1) {
    StructRejected("slReflexSetOptions",opts,opts.structType!=sl::ReflexOptions::s_structType?"type":"version");
    return sl::Result::eErrorUnsupportedInterface;
  }
  auto& s=State();
  const bool native=s.latencyOutput.load()==LatencyOutput::Reflex;
  if(opts.structVersion>1 || opts.next) StructAdapted("slReflexSetOptions",opts,native?"forwarded_unchanged":"v1_prefix_forwarded");
  // The original setter stays on the game's calling thread. Native output
  // forwards the game's structure untouched, newer fields and chain included.
  // XeLL output sends the known V1 prefix with the game's extension chain and
  // suppresses the native limiter; a V1 structure is valid for every 2.x SDK.
  sl::ReflexOptions forwarded;
  memcpy(static_cast<void*>(&forwarded),&opts,sizeof(forwarded));
  forwarded.structVersion=1; forwarded.next=opts.next;
  if(!native) { forwarded.mode=sl::ReflexMode::eOff; forwarded.frameLimitUs=0; }
  const auto result=native?reflexOptions(opts):reflexOptions(forwarded);
  s.reflexOptionsReady=result==sl::Result::eOk;
  s.reflexOff=result==sl::Result::eOk && forwarded.mode==sl::ReflexMode::eOff;
  s.gameReflexMode=uint32_t(opts.mode);
  s.limitUs=opts.frameLimitUs;
  return result;
}
sl::Result AssociateSourceSleep(const std::shared_ptr<Output>& o,uint32_t source);
sl::Result SleepHook(const sl::FrameToken& f) {
  MFG_VULKAN_PASS(reflexSleep(f))
  const auto source=FrameId(uint32_t(f));
  TraceInput("slReflexSleep",source);
  auto o=Active(); if(!o) return reflexSleep(f);
  // Native Reflex output (1.5.6): every game wait reaches the original
  // slReflexSleep with its own token, on its own thread, and the game gets its
  // result. Only the XeLL frame association below can be refused; a game that
  // passes a constant token (Plague Tale) is then covered by the simulation
  // boundary, as on the XeLL route.
  if(State().latencyOutput.load()==LatencyOutput::Reflex) {
    const auto native=reflexSleep(f);
    if(native==sl::Result::eOk) AssociateSourceSleep(o,source);
    return native;
  }
  return AssociateSourceSleep(o,source);
}
sl::Result AssociateSourceSleep(const std::shared_ptr<Output>& o,uint32_t source) {
  uint32_t target{};uint64_t epoch{};
  {
    std::lock_guard guard(o->io);
    // Refused associations never reach XeLL, so they must not disable the
    // per-frame simulation boundary that covers the same frames.
    const auto rejected=[&](const char* reason,uint32_t observed) {
      ++o->rejectedExplicitSleeps;
      if(o->sleepRejectLogBudget++<8)
        xess_mod::WriteLog("native_sleep_rejected","\"source\":"+std::to_string(source)
            +",\"reason\":\""+reason+"\",\"observed\":"+std::to_string(observed));
      return sl::Result::eErrorInvalidState;
    };
    auto* frame=FindFrame(*o,source);
    if(o->retired) return sl::Result::eErrorInvalidState;
    if(!frame) return rejected("frame_unavailable",0);
    if(!frame->sourceSleepResolved) {
      const auto selected=latencymarkers::SourceSleepFrame(source,frame->observedMarkers);
      if(!selected) return rejected("inside_simulation",frame->observedMarkers);
      // A next simulation that has already started cannot receive this wait.
      // Duplicates reuse the first resolved target and never dispatch twice.
      if(selected!=source) {
        const auto* next=o->frames.Find(selected);
        if(next && (next->observedMarkers || next->sleeping || next->sleepInFlight))
          return rejected("next_simulation_started",frame->observedMarkers);
      }
      frame->sourceSleepResolved=true;frame->sourceSleepTarget=selected;
      o->explicitSleepObserved=true;
      if(selected!=source && o->sleepAssociationLogBudget++<8)
        xess_mod::WriteLog("native_sleep_association","\"source\":"+std::to_string(source)
            +",\"target\":"+std::to_string(selected)+",\"observed\":"+std::to_string(frame->observedMarkers));
    }
    target=frame->sourceSleepTarget;
    epoch=o->epoch;
  }
  State().inputTick=GetTickCount64();
  // Only the XeLL frame association follows the simulation after this wait.
  return SleepSourceFrame(o,target,nullptr,nullptr,epoch);
}
void CaptureMarker(sl::PCLMarker marker,const sl::FrameToken& f) {
  MFG_VULKAN_PASS(void())
  TraceInput("latency_marker",FrameId(uint32_t(f)),UINT32_MAX,int(marker));
  const auto id=FrameId(uint32_t(f)); auto& s=State(); const auto value=uint32_t(marker);
  s.streamlineMarkerTick=GetTickCount64();
  if(marker==sl::PCLMarker::eRenderSubmitStart) s.recording=id;
  if(marker==sl::PCLMarker::ePresentStart) s.presenting=id;
  if(auto o=Active()) {
    const auto mapped=marker==sl::PCLMarker::eControllerInputSample?uint32_t(XELL_INPUT_SAMPLE):value;
    bool boundarySleep{},boundaryLog{},optionalBeforeSleep{};uint64_t epoch{};
    if(mapped<=6) {
      std::lock_guard guard(o->io);
      if(!o->retired) if(auto* frame=FindFrame(*o,id)) {
        const auto previous=frame->observedMarkers;
        if(mapped==XELL_SIMULATION_START && !(previous&1u) && !frame->sleeping) {
          if(o->unsleptSimulationStarts<8) ++o->unsleptSimulationStarts;
          // A real simulation boundary is a legal XeLL sleep site for a frame
          // without a completed wait: no slReflexSleep call, or one placed where
          // it cannot be translated (inside simulation, after the next start).
          // Accepted explicit waits mark the frame sleeping and skip this path.
          // Do not replay timestamps, sleep after simulation began, or
          // override native Reflex output.
          // XeLL route: XeLL paces, so native Reflex must be off. Reflex route:
          // the game's own waits pace; this only associates the frame (1.5.6).
          const auto route=s.latencyOutput.load();
          boundarySleep=o->unsleptSimulationStarts>=8 && !(previous&63u)
              && !o->sleepActive && !frame->sleepInFlight && s.requested && s.nativeOff
              && ((route==LatencyOutput::XeLL && s.reflexOff) || route==LatencyOutput::Reflex);
          if(boundarySleep) {epoch=o->epoch;boundaryLog=o->sleepAssociationLogBudget++<8;}
          else if(s.requested && o->unsleptSimulationStarts>=8 && o->boundarySkipLogBudget++<4)
            xess_mod::WriteLog("native_simulation_boundary_skipped","\"id\":"+std::to_string(id)
                +",\"observed\":"+std::to_string(previous)+",\"sleep_active\":"+std::to_string(o->sleepActive)
                +",\"in_flight\":"+std::to_string(frame->sleepInFlight)+",\"native_off\":"+std::to_string(s.nativeOff.load())
                +",\"reflex_off\":"+std::to_string(s.reflexOff.load())+",\"xell\":"+std::to_string(s.latencyOutput.load()==LatencyOutput::XeLL));
        }
        optionalBeforeSleep=mapped==XELL_INPUT_SAMPLE && !frame->sleeping;
        frame->observedMarkers|=1u<<mapped;
      }
    }
    if(boundarySleep) {
      const auto result=SleepSourceFrame(o,id,nullptr,nullptr,epoch);
      if(boundaryLog)
        xess_mod::WriteLog("native_simulation_boundary_sleep","\"id\":"+std::to_string(id)
            +",\"result\":"+std::to_string(uint32_t(result)));
      if(result==sl::Result::eOk) s.inputTick=GetTickCount64();
    }
    // INPUT_SAMPLE is optional. An earlier real sample cannot be replayed
    // with a fabricated timestamp after sleep, nor should it poison the
    // mandatory simulation/render/present sequence that follows.
    if(optionalBeforeSleep) return;
    ForwardSourceMarker(o,id,mapped);
  }
}
sl::Result MarkerHook(sl::PCLMarker marker,const sl::FrameToken& f) {
  CaptureMarker(marker,f);
  return pclMarker(marker,f); // Preserve native PCL/Reflex markers; the selected output owns the sleep.
}
sl::Result LegacyMarkerHook(uint32_t marker,const sl::FrameToken& frame) {
  if(marker<uint32_t(sl::PCLMarker::eMaximum)) CaptureMarker(static_cast<sl::PCLMarker>(marker),frame);
  return legacyMarker(marker,frame);
}
sl::Result EvaluateHook(sl::Feature feature,const sl::FrameToken& f,const sl::BaseStructure** inputs,
    uint32_t count,sl::CommandBuffer* command) {
  MFG_VULKAN_PASS(evaluate(feature,f,inputs,count,command))
  if(inputs && count<=16 && (feature==sl::kFeatureReflex || feature==sl::kFeaturePCL)) {
    for(uint32_t i=0;i<count;++i) {
      const auto* in=inputs[i]; if(!in || in->structVersion!=1) continue;
      if(in->structType==sl::ReflexHelper::s_structType) {
        const auto marker=sl::MarkerUnderlying(*static_cast<const sl::ReflexHelper*>(in));
        // Only named public marker values are admitted. Unknown historical
        // sleep sentinels are not guessed from a private numeric constant.
        if(marker<uint32_t(sl::PCLMarker::eMaximum)) CaptureMarker(static_cast<sl::PCLMarker>(marker),f);
      } else if(in->structType==sl::PCLHelper::s_structType) {
        CaptureMarker(static_cast<const sl::PCLHelper*>(in)->get(),f);
      }
    }
  }
  return evaluate(feature,f,inputs,count,command);
}
bool Feature(const char* name,void* entry) {
  // Keep these plugin bodies original. Resolver-returned wrappers belong to
  // the game, while independent overlay consumers retain the native entry.
  if(!strcmp(name,"slDLSSGSetOptions") || !strcmp(name,"slDLSSGGetState")) {
    auto** slot=!strcmp(name,"slDLSSGSetOptions")?reinterpret_cast<void**>(&setOptions):reinterpret_cast<void**>(&getState);
    if(*slot)return *slot==entry;
    HMODULE pin{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry),&pin))return false;
    *slot=entry;return true;
  }
  if(strcmp(name,"slPCLSetMarker")==0 || strcmp(name,"slReflexSetMarker")==0) {
    const bool legacy=strcmp(name,"slReflexSetMarker")==0;
    auto** target=legacy?reinterpret_cast<void**>(&legacyMarker):reinterpret_cast<void**>(&pclMarker);
    if(*target) return markerEntries[legacy]==entry;
    HMODULE pin{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry),&pin)) return false;
    const auto other=legacy?reinterpret_cast<void*>(pclMarker):reinterpret_cast<void*>(legacyMarker);
    bool installed{};
    if(other && markerEntries[!legacy]==entry) { *target=other; installed=true; }
    else installed=Hook(entry,legacy?reinterpret_cast<void*>(&LegacyMarkerHook):reinterpret_cast<void*>(&MarkerHook),target);
    if(installed) {
      markerEntries[legacy]=entry;
      xess_mod::WriteLog("native_marker_hook",std::string("\"api\":\"")+name+"\",\"aliased\":"+(other && markerEntries[!legacy]==entry?"true":"false"));
    }
    return installed;
  }
#define H(n,f) if(strcmp(name,#n)==0) return Hook(entry,reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&n));
  // Function pointer variable names intentionally differ from exported names.
#undef H
#define H(n,f,p) if(strcmp(name,n)==0) { \
  HMODULE pin{}; \
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN, \
      reinterpret_cast<LPCWSTR>(entry),&pin)) return false; \
  return Hook(entry,reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p)); }
  H("slReflexSetOptions",ReflexOptionsHook,reflexOptions) H("slReflexSleep",SleepHook,reflexSleep)
#undef H
  return false;
}
sl::Result ResolveHook(sl::Feature feature,const char* name,void*& fn) {
  // The host owns the shutdown contract. Do not acquire our discovery lock or
  // publish new detours from a plugin's teardown callback.
  if(stopping) return getFeature(feature,name,fn);
  std::lock_guard guard(discovery);
  const auto r=getFeature(feature,name,fn);
  const bool generationName=name && (!strcmp(name,"slDLSSGSetOptions") || !strcmp(name,"slDLSSGGetState"));
  if(!stopping && generationName && (r!=sl::Result::eOk || !fn) && feature==sl::kFeatureDLSS_G
      && r!=sl::Result::eErrorNotInitialized) {
    // No DLSS-G plugin functions (RTX 20/30): the bridge owns both entries.
    syntheticGeneration=true;
    static std::atomic<bool> logged{};
    if(!logged.exchange(true)) xess_mod::WriteLog("native_synthetic_generation_api","\"native_result\":"+std::to_string(uint32_t(r)));
    fn=!strcmp(name,"slDLSSGSetOptions")?reinterpret_cast<void*>(&OptionsHook):reinterpret_cast<void*>(&StateHook);
    return sl::Result::eOk;
  }
  if(!stopping && r==sl::Result::eOk && name && fn) {
    const bool generation=!strcmp(name,"slDLSSGSetOptions") || !strcmp(name,"slDLSSGGetState");
    if(generation) {
      // Observer modules from profiles.json keep the original functions.
      HMODULE caller{};
      if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(_ReturnAddress()),&caller) && caller)
        for(const auto& name:CurrentPolicy().observerModules)
          if(GetModuleHandleW(name.c_str())==caller) { ++State().externalResolutions; return r; }
    }
    const bool captured=Feature(name,fn);
    if(generation) {
      if(!captured) {fn=nullptr;return sl::Result::eErrorUnsupportedInterface;}
      fn=!strcmp(name,"slDLSSGSetOptions")?reinterpret_cast<void*>(&OptionsHook):reinterpret_cast<void*>(&StateHook);
    }
  }
  return r;
}
} // namespace
void LearnStatedConventions(std::optional<bool> invert,std::optional<bool> jitter) { LearnConventions(invert,jitter); }
void LogTagHistogram(Output& o,const char* phase) {
  if(o.histogramLogged && !strcmp(phase,"steady")) return;
  if(!o.taggedCalls) return;
  o.histogramLogged=true;
  std::string types,viewports;
  for(uint32_t type=0;type<64;++type) if(o.tagTypeCounts[type])
    types+=std::string(types.empty()?"":",")+"{\"type\":"+std::to_string(type)+",\"count\":"+std::to_string(o.tagTypeCounts[type])+"}";
  for(const auto& slot:o.tagViewports) if(slot.count)
    viewports+=std::string(viewports.empty()?"":",")+"{\"viewport\":"+std::to_string(slot.viewport)+",\"count\":"+std::to_string(slot.count)+"}";
  xess_mod::WriteLog("native_tag_histogram","\"output\":"+std::to_string(o.diagnosticOutput)+",\"phase\":\""+phase
      +"\",\"calls\":"+std::to_string(o.taggedCalls)+",\"types\":["+types+"],\"viewports\":["+viewports
      +"],\"other_viewport_calls\":"+std::to_string(o.otherViewportTags));
}
// Streamline isolated from presentation, or an output of an adapter without
// a Streamline presentation proxy: FidelityFX (5.3) or armed DLSS-SR (5.4).
bool OwnsPresentationHooks() noexcept {
  return (lifecycleApi && presentationIsolated && !stopping) || ffx::CreatingOutput() || synthetic::Owner() || ngx::Armed();
}
bool StreamlineStopping() noexcept { return stopping; }
// The detours below use the Streamline 2.x ABI. A 1.x interposer exports
// slSetTag/slSetConstants/slEvaluateFeature with other signatures (the
// conformance fixture crashed the process before this gate), and has no
// slGetFeatureFunction. Such a module never receives a detour.
bool StreamlineAbi2(HMODULE module) noexcept {
  const auto version=FileVersion(module);
  const bool resolver=GetProcAddress(module,"slGetFeatureFunction")!=nullptr;
  const bool legacy=version.rfind("1.",0)==0 || version.rfind("0.",0)==0;
  if(resolver && !legacy) return true;
  static std::atomic<bool> logged{};
  if(!logged.exchange(true))
    xess_mod::WriteLog("native_streamline_unsupported_abi","\"interposer\":\""+version+"\",\"resolver\":"+(resolver?"true":"false")
        +",\"detours\":0");
  return false;
}
void CaptureStreamlineStartup() {
  // No SDK entry calls here. These exports can be captured before slInit even
  // when the host's interposer is only mapped during the loader's attach phase.
  auto module=GetModuleHandleW(L"sl.interposer.dll"); if(!module || !StreamlineAbi2(module)) return;
  HMODULE pin{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pin) || pin!=module) return;
#define H(n,f,p) if(!p) Hook(reinterpret_cast<void*>(GetProcAddress(module,n)),reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p));
  H("slIsFeatureLoaded",GetLoadedHook,getLoaded)
  H("slSetFeatureLoaded",SetLoadedHook,setLoaded)
  H("slShutdown",ShutdownHook,shutdown)
  H("slSetD3DDevice",DeviceHook,setDevice)
  H("slInit",InitHook,initialize)
  H("slGetFeatureFunction",ResolveHook,getFeature)
  H("slIsFeatureSupported",SupportedHook,isSupported)
  H("slGetFeatureRequirements",RequirementsHook,requirements)
#undef H
  // Incomplete installation must not disable NVIDIA or create an Intel output
  // without the query/control/shutdown hooks needed to keep that owner safe.
  lifecycleApi=getLoaded && setLoaded && shutdown && (initialize || setDevice);
}
void DiscoverStreamline() {
  if(single_overlay::native::InsideLoader() || stopping) return;
  std::lock_guard guard(discovery);
  if(stopping) return;
  CaptureStreamlineStartup();
  auto module=GetModuleHandleW(L"sl.interposer.dll"); if(!module || !StreamlineAbi2(module)) return;
  HMODULE pinned{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module) return;
#define H(n,f,p) if(!p) Hook(reinterpret_cast<void*>(GetProcAddress(module,n)),reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p));
  H("slGetFeatureFunction",ResolveHook,getFeature)
  H("slSetTag",TagHook,setTag) H("slSetTagForFrame",TagFrameHook,setTagFrame)
  H("slSetConstants",ConstantsHook,setConstants)
  H("slEvaluateFeature",EvaluateHook,evaluate)
#undef H
  // A late-load notification can discover the interposer before the game's
  // slInit. Resolving functions then would query an uninitialized SDK; the
  // resolver hook catches the game's own queries after slInit instead.
  bool loadedState{};
  const bool initialized=!getLoaded || getLoaded(sl::kFeatureDLSS_G,loadedState)!=sl::Result::eErrorNotInitialized;
  if(getFeature && initialized) {
    struct Entry {sl::Feature feature; const char* name;};
    for(const auto& e:{Entry{sl::kFeatureDLSS_G,"slDLSSGSetOptions"},Entry{sl::kFeatureDLSS_G,"slDLSSGGetState"},
        Entry{sl::kFeatureReflex,"slReflexSetOptions"},Entry{sl::kFeatureReflex,"slReflexSleep"},
        Entry{sl::kFeaturePCL,"slPCLSetMarker"},Entry{sl::kFeatureReflex,"slReflexSetMarker"}}) {
      void* fn{}; const auto found=getFeature(e.feature,e.name,fn);
      if(found==sl::Result::eOk && fn) Feature(e.name,fn);
      // An initialized SDK without DLSS-G functions (RTX 20/30): synthetic API.
      else if(e.feature==sl::kFeatureDLSS_G) {
        if(!syntheticGeneration.exchange(true)) xess_mod::WriteLog("native_synthetic_generation_api","\"native_result\":"+std::to_string(uint32_t(found)));
      }
    }
  }
  auto& s=State(); std::lock_guard lock(s.lock);
  s.status.streamline=((setOptions && getState) || syntheticGeneration) && reflexOptions && reflexSleep
      && (pclMarker || legacyMarker || evaluate) && setConstants && (setTag || setTagFrame);
  s.hookCapabilities=(uint32_t(setOptions && getState)<<0)|(uint32_t(reflexOptions && reflexSleep)<<1)
      |(uint32_t(pclMarker || legacyMarker)<<2)|(uint32_t(evaluate!=nullptr)<<3)
      |(uint32_t(setConstants!=nullptr)<<4)|(uint32_t(setTag!=nullptr)<<5)|(uint32_t(setTagFrame!=nullptr)<<6);
}
// Plan 4.2: converts this frame's staged motion on the presentation queue and
// tags the unjittered copy (ONLY_NOW: Intel copies it on the same list).
bool SubmitDejitteredMotion(Output& o,const Frame& f,uint32_t id) {
  const DejitterVector current{f.data.jitterOffsetX,f.data.jitterOffsetY};
  // Without the previous frame's jitter the difference is unknown; XeFG
  // resets history on that discontinuity, so a zero difference is safe.
  const bool continuous=o.lastJitterId && id==o.lastJitterId+1;
  DejitterVector delta{};
  bool ok=f.motionSource && (!continuous || DejitterDelta(o.lastJitter,current,
      {f.data.motionVectorScaleX,f.data.motionVectorScaleY},f.motionWidth,f.motionHeight,delta));
  ID3D12Resource* output{};
  auto* list=ok?o.dejitter->Convert(f.motionSource.Get(),f.motionState,f.motionRegion,delta,&output):nullptr;
  int tag=XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  if(list) {
    xefg_swapchain_d3d12_resource_data_t motion{};
    motion.type=XEFG_SWAPCHAIN_RES_MOTION_VECTOR; motion.pResource=output; motion.validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;
    motion.incomingState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    motion.resourceSize={f.motionRegion.width,f.motionRegion.height};
    {
      RuntimeCall trace("xefg_tag_resource",o.diagnosticOutput,id);
      tag=State().api.xefgSwapChainD3D12TagFrameResource(o.fg,list,id,&motion); trace.Result(tag);
    }
    ok=o.dejitter->Submit(o.queue.Get()) && tag>=0; // the open list is always submitted
  } else ok=false;
  if(o.dejitterLogBudget<4 || (!ok && o.dejitterLogBudget<12)) {
    ++o.dejitterLogBudget;
    xess_mod::WriteLog("native_dejitter","\"id\":"+std::to_string(id)+",\"ok\":"+(ok?"true":"false")
        +",\"continuous\":"+(continuous?"true":"false")+",\"tag_result\":"+std::to_string(tag)
        +",\"hr\":"+std::to_string(int32_t(o.dejitter->LastError()))
        +",\"delta\":["+std::to_string(delta.x)+","+std::to_string(delta.y)+"]");
  }
  return ok;
}
// Called at the source Present boundary, before admission (see ViewportInputs).
void ReselectViewport(Output& o,uint32_t id) {
  auto& s=State();
  std::lock_guard guard(viewportLock);
  const auto selected=s.viewport.load();
  if(selected==UINT32_MAX || !id) return;
  const ViewportInputs* current{}; const ViewportInputs* candidate{};
  for(const auto& slot:viewportInputs) {
    if(slot.viewport==UINT32_MAX) continue;
    if(slot.viewport==selected) { current=&slot; continue; }
    if(slot.mode!=uint32_t(sl::DLSSGMode::eOff) && slot.complete && slot.lastInputs+2>=id && slot.lastConstants+2>=id) candidate=&slot;
  }
  if(!candidate) return;
  const bool never=!current || !current->complete;
  const bool quiet=current && current->complete && id>std::max(current->lastInputs,current->lastConstants)+30;
  if(!never && !quiet) return;
  s.viewport=candidate->viewport;
  s.requested=logicalLoaded && candidate->mode!=uint32_t(sl::DLSSGMode::eOff);
  s.streamlineGeneration=s.requested.load();
  o.viewportReset=true;
  xess_mod::WriteLog("native_viewport_switch","\"from\":"+std::to_string(selected)+",\"to\":"+std::to_string(candidate->viewport)
      +",\"id\":"+std::to_string(id)+",\"reason\":\""+(never?"selected_without_inputs":"selected_quiet")+"\"");
}
void BeforePresent(const std::shared_ptr<Output>& o) {
  auto& s=State(); auto& a=s.api; std::lock_guard lock(o->io);
  {
    // 1.5.7: smoothed interval between source Presents (display-cap notice).
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    const auto previous=o->lastSourcePresentQpc; o->lastSourcePresentQpc=now.QuadPart;
    const auto us=previous?QpcMicroseconds(now.QuadPart-previous):0;
    const auto smoothed=s.sourceIntervalUs.load();
    if(us && us<250000) s.sourceIntervalUs=smoothed?uint32_t((uint64_t(smoothed)*7+us)/8):uint32_t(us);
    else if(us) s.sourceIntervalUs=0; // a pause or a loading screen: start again
  }
  ReselectViewport(*o,s.presenting.load());
  if(o->retired || !o->fg) return;
  if(o->deviceLost) {o->blocker=compatibility::Failure::DeviceLost;Report(compatibility::Description(o->blocker));return;}
  const auto id=s.presenting.load(); auto* frame=o->frames.Find(id);
  s.lastHudIssues=frame?frame->hudIssues:0;
  s.lastHudInputs=frame?frame->resources&((1u<<XEFG_SWAPCHAIN_RES_HUDLESS_COLOR)|(1u<<XEFG_SWAPCHAIN_RES_UI)):0;
  s.lastHudFiltered=frame?frame->hudFiltered:0; s.lastHudCopied=frame?frame->hudCopied:0;
  const bool hudless=frame && !frame->failed
      && (frame->resources&(1u<<XEFG_SWAPCHAIN_RES_HUDLESS_COLOR));
  // 1.7 (plan E.2): with HUD stabilization on, the game's HUD tags were held
  // back (SubmitResource), so `hudless` is false and the addon's inputs apply.
  const bool stabilized=!hudless && frame && !frame->failed && UiStabilization()
      && StabilizeHud(*o,*frame,id);
  const bool compositionReady=ApplyUiComposition(*o,hudless || stabilized,stabilized);
  constexpr uint32_t required=(1u<<XEFG_SWAPCHAIN_RES_DEPTH)|(1u<<XEFG_SWAPCHAIN_RES_MOTION_VECTOR);
  const bool nativeReflex=s.latencyOutput.load()==LatencyOutput::Reflex;
  const bool latencyReady=s.reflexOptionsReady && (nativeReflex?s.gameReflexMode!=0:s.reflexOff.load());
  // Present identity belongs to the source frame, including passthrough.
  // Missing FG inputs must not leave the Intel proxy on the previous ID.
  const auto presentIdResult=id && id>o->last?a.xefgSwapChainSetPresentId(o->fg,id):XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  constexpr uint32_t beforePresent=(1u<<XELL_SIMULATION_START)|(1u<<XELL_SIMULATION_END)
      |(1u<<XELL_RENDERSUBMIT_START)|(1u<<XELL_PRESENT_START);
  const compatibility::Admission requirements{
    .requested=s.requested,.output=true,.ownsPresentation=OwnsPresentationHooks(),.deviceLost=o->deviceLost,.formatSupported=o->compatible,
    .frame=frame!=nullptr,.presentIdentity=presentIdResult>=0 && id>o->last,.nativeOff=s.nativeOff,.latencyReady=latencyReady,
    .depth=frame && (frame->resources&(1u<<XEFG_SWAPCHAIN_RES_DEPTH)),
    .motion=frame && (frame->resources&(1u<<XEFG_SWAPCHAIN_RES_MOTION_VECTOR)) && frame->motionWidth && frame->motionHeight,
    .constants=frame && frame->constants,.constantsValid=frame && frame->valid,.rejected=frame && frame->failed,
    .sourceWait=frame && frame->sleeping && frame->latencyOutput==s.latencyOutput.load(),
    .requiredMarkers=frame && (frame->markers&beforePresent)==beforePresent,.composition=compositionReady,
    .firstInputFailure=frame?frame->firstFailure:compatibility::Failure::None,
    .presentationSupported=o->presentationSupported};
  o->blocker=compatibility::Blocker(requirements);
  bool ready=o->blocker==compatibility::Failure::None;
  if(ready) {
    auto& f=*frame;
    // RenderSubmit END can legally arrive after Present starts or returns.
    // Admission requires the observed frame's starts, simulation and sleep;
    // END continues to be forwarded at its actual arrival in CaptureMarker.
    ready=f.constants && f.valid && !f.failed && f.sleeping && f.latencyOutput==s.latencyOutput.load() && (f.markers&beforePresent)==beforePresent
        && (f.resources&required)==required && f.motionWidth && f.motionHeight;
    if(ready) {
      auto data=f.data; data.motionVectorScaleX*=f.motionWidth; data.motionVectorScaleY*=f.motionHeight;
      data.resetHistory|=o->last==0 || id!=o->last+1 || o->viewportReset;
      o->viewportReset=false;
      xefg_swapchain_d3d12_resource_data_t region{}; region.type=XEFG_SWAPCHAIN_RES_BACKBUFFER;
      region.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT; region.resourceSize={o->width,o->height};
      if(f.backbufferRegion.width) {
        region.resourceBase={f.backbufferRegion.x,f.backbufferRegion.y};
        region.resourceSize={f.backbufferRegion.width,f.backbufferRegion.height};
      }
      ready=a.xefgSwapChainD3D12TagFrameResource(o->fg,nullptr,id,&region)>=0
          && (!o->dejitter || SubmitDejitteredMotion(*o,f,id))
          && packet::SubmitPendingHud(*o,f,id)
          && a.xefgSwapChainTagFrameConstants(o->fg,id,&data)>=0;
    }
  }
  if(frame && frame->constants) {
    o->lastJitter={frame->data.jitterOffsetX,frame->data.jitterOffsetY}; o->lastJitterId=id;
  }
  if(ready!=o->enabled) {
    if(a.xefgSwapChainSetEnabled(o->fg,ready?1:0)!=0) ready=false;
    o->enabled=ready;
    o->appliedCount=0;
  }
  const auto count=s.multiplier.load()-1;
  if(ready && count!=o->appliedCount) {
    ready=a.xefgSwapChainSetNumInterpolatedFrames(o->fg,count)==0;
    if(ready) o->appliedCount=count;
    else { a.xefgSwapChainSetEnabled(o->fg,0); o->enabled=false; }
  }
  if(!ready && o->blocker==compatibility::Failure::None)o->blocker=compatibility::Failure::SdkSubmission;
  const auto* f=frame;
  if(id && !ready && o->bypassPresentLogBudget<8) {
    ++o->bypassPresentLogBudget;
    xess_mod::WriteLog("native_bypass_present_id","\"id\":"+std::to_string(id)+",\"result\":"+std::to_string(presentIdResult));
  }
  const uint32_t admission=(f?f->resources:0)|((f?f->markers:0)<<8)
      |(uint32_t(f&&f->constants)<<16)|(uint32_t(f&&f->valid)<<17)
      |(uint32_t(f&&f->failed)<<18)|(uint32_t(f&&f->sleeping)<<19)
      |(uint32_t(s.nativeOff.load())<<20)|(uint32_t(s.reflexOff.load())<<21);
  const auto tick=GetTickCount64();
  uint32_t framesSoFar{};
  { std::lock_guard guard(s.lock); framesSoFar=s.status.frames; }
  if(framesSoFar<4 || (admission!=o->lastAdmission && tick-o->admissionTick>=1000)) {
    o->lastAdmission=admission; o->admissionTick=tick;
    xess_mod::WriteLog("native_frame_admission","\"id\":"+std::to_string(id)
        +",\"native_off\":"+std::to_string(s.nativeOff.load())+",\"requested\":"+std::to_string(s.requested.load())
        +",\"reflex_off\":"+std::to_string(s.reflexOff.load())
        +",\"resources\":"+std::to_string(f?f->resources:0)+",\"markers\":"+std::to_string(f?f->markers:0)
        +",\"constants\":"+std::to_string(f?f->constants:0)+",\"valid\":"+std::to_string(f?f->valid:0)
        +",\"failed\":"+std::to_string(f?f->failed:0)+",\"sleep\":"+std::to_string(f?f->sleeping:0)
        +",\"blocker\":\""+compatibility::Name(o->blocker)+"\",\"observed_markers\":"+std::to_string(f?f->observedMarkers:0)
        +",\"first_input_failure\":\""+compatibility::Name(f?f->firstFailure:compatibility::Failure::None)+"\"");
  }
  { std::lock_guard guard(s.lock); s.status.inputs=ready; s.status.enabled=ready;
    s.status.multiplier=s.multiplier; ++s.status.frames; if(!ready) ++s.status.rejected; }
  if(ready) Report(nativeReflex?"XeMFG active: Streamline inputs and native Reflex; pacing state is reported separately":
      "XeMFG active: Streamline inputs and XeLL; pacing state is reported separately");
  else Report(compatibility::Description(o->blocker));
  o->last=id;
}
// 1.5.5: one native_summary per [Diagnostics] SummaryIntervalMs (10 s) while
// this output presents. Per-event logs keep only their first entries; the
// summary carries the counts, and when it stops, presentation stopped.
static void WriteSummary(Output& o,const xefg_swapchain_present_status_t& status,int queryResult) {
  auto& s=State(); const auto tick=GetTickCount64();
  uint32_t frames{},withoutFg{};
  { std::lock_guard guard(s.lock); frames=s.status.frames; withoutFg=s.status.rejected; }
  const uint64_t tagsRejected=s.tagsRejected,tagsRemoved=s.tagsRemoved,reservedStates=s.reservedStates,hudFiltered=s.hudRejected;
  const auto baseline=[&] {
    o.summaryTick=tick; o.summaryFrames=frames; o.summaryRejectedFrames=withoutFg; o.summarySleeps=o.rejectedExplicitSleeps;
    o.summaryTagsRejected=tagsRejected; o.summaryTagsRemoved=tagsRemoved; o.summaryReservedStates=reservedStates;
    o.summaryHudFiltered=hudFiltered;
    o.summaryPresentUs=0; o.summaryPresentCalls=0; o.summarySleepUs=0; o.summarySleepCalls=0;
  };
  if(!o.summaryTick) { baseline(); return; }
  if(tick-o.summaryTick<s.summaryIntervalMs.load()) return;
  const auto sync=s.gameSyncInterval.load();
  const auto presents=o.summaryPresentCalls.load();
  const auto flip=sdk_pacing::Snapshot(o.fg);
  xess_mod::WriteLog("native_summary","\"output\":"+std::to_string(o.diagnosticOutput)+",\"interval_ms\":"+std::to_string(tick-o.summaryTick)
      +",\"frames\":"+std::to_string(uint32_t(frames-uint32_t(o.summaryFrames)))
      +",\"frames_without_fg\":"+std::to_string(uint32_t(withoutFg-uint32_t(o.summaryRejectedFrames)))
      +",\"fg_enabled\":"+std::to_string(o.enabled)+",\"requested\":"+std::to_string(s.requested.load())
      +",\"blocker\":\""+compatibility::Name(o.blocker)+"\""
      +",\"frames_per_present\":"+std::to_string(queryResult==0?status.framesPresented:0)+",\"sdk_result\":"+std::to_string(status.frameGenResult)
      +",\"tags_rejected\":"+std::to_string(tagsRejected-o.summaryTagsRejected)+",\"tags_removed\":"+std::to_string(tagsRemoved-o.summaryTagsRemoved)
      +",\"hud_filtered\":"+std::to_string(hudFiltered-o.summaryHudFiltered)
      +",\"reserved_states\":"+std::to_string(reservedStates-o.summaryReservedStates)
      +",\"sleeps_rejected\":"+std::to_string(o.rejectedExplicitSleeps-o.summarySleeps)
      +",\"game_sync_interval\":"+(sync==UINT32_MAX?std::string("-1"):std::to_string(sync))
      +",\"force_vsync_off\":"+std::to_string(ForceVSyncOff())
      +",\"source_interval_us\":"+std::to_string(s.sourceIntervalUs.load())
      +",\"sleep_us\":"+std::to_string(o.summarySleepCalls?o.summarySleepUs/o.summarySleepCalls:0)
      +",\"present_call_us\":"+std::to_string(presents?o.summaryPresentUs.load()/presents:0)
      +",\"latency_output\":"+std::to_string(uint32_t(s.latencyOutput.load()))
      +",\"native_buffers\":"+std::to_string(flip.nativeBuffers)+",\"native_latency\":"+std::to_string(flip.nativeLatency)
      +",\"flip_metered\":"+std::to_string(flip.metered)+",\"queue_ahead\":"+std::to_string(flip.queueAheadActive));
  baseline();
}
void AfterPresent(const std::shared_ptr<Output>& o,CpuProbe* probe,HRESULT result) {
  std::lock_guard lock(o->io); xefg_swapchain_present_status_t status{};
  if(o->retired || !o->fg) return;
  // Log before querying SDK status: retain the original failing Present and
  // the device's more specific reason without swallowing/retrying the error.
  if(FAILED(result) && o->presentFailureLogBudget<4) {
    ++o->presentFailureLogBudget;
    const auto removed=o->device?o->device->GetDeviceRemovedReason():E_NOINTERFACE;
    xess_mod::WriteLog("native_present_failure","\"output\":"+std::to_string(o->diagnosticOutput)
        +",\"frame\":"+std::to_string(o->last)+",\"present_hresult\":"+std::to_string(uint32_t(result))
        +",\"device_removed_reason\":"+std::to_string(uint32_t(removed))
        +",\"fg_enabled\":"+std::to_string(o->enabled)+",\"epoch\":"+std::to_string(o->epoch));
    RecordDeviceFailure(*o,result);
  }
  if(o->deviceLost) {
    if(probe && probe->active) {probe->sample.frame=o->last;probe->sample.epoch=o->epoch;
      probe->sample.output=o->diagnosticOutput;probe->sample.presentResult=result;
      probe->sample.sdkQueryResult=E_ABORT;probe->Finish();}
    Report(compatibility::Description(o->blocker));return;
  }
  const auto queryResult=State().api.xefgSwapChainGetLastPresentStatus(o->fg,&status);
  if(queryResult==0) {
    auto& s=State(); std::lock_guard guard(s.lock);
    s.status.submitted=status.framesPresented; s.status.sdkResult=status.frameGenResult;
  }
  // Keep late PresentEnd markers for the preceding frame, but bound memory.
  o->frames.Prune(o->last);
  // 1.5.7: refresh of the display showing this output, every 2 s (display cap).
  if(const auto tick=GetTickCount64(); tick-o->refreshTick>=2000) {
    o->refreshTick=tick;
    const uint32_t refresh=WindowRefreshMilliHz(o->hwnd);
    if(State().displayRefreshMilliHz.exchange(refresh)!=refresh || !o->displayCapLogged) {
      o->displayCapLogged=true;
      xess_mod::WriteLog("native_display_cap","\"output\":"+std::to_string(o->diagnosticOutput)+",\"refresh_mhz\":"+std::to_string(refresh)
          +",\"multiplier\":"+std::to_string(State().multiplier.load())+",\"enabled\":"+std::to_string(State().displayCap.load())
          +",\"interval_us\":"+std::to_string(DisplayCapIntervalUs(refresh,State().multiplier.load())));
    }
  }
  WriteSummary(*o,status,queryResult);
  if(probe && probe->active) {
    auto& sample=probe->sample; sample.frame=o->last; sample.epoch=o->epoch; sample.output=o->diagnosticOutput;
    sample.presentResult=result; sample.admitted=o->enabled; sample.requestedOutputs=State().multiplier;
    sample.sdkOutputs=status.framesPresented; sample.sdkResult=status.frameGenResult; sample.sdkEnabled=status.isFrameGenEnabled!=0;
    sample.sdkQueryResult=queryResult;
    if(auto* frame=o->frames.Find(o->last)) sample.sleepUs=frame->sleepUs;
    probe->Finish();
  }
}
}
