// SPDX-License-Identifier: MIT
// Own XeLL translation. A shared owner pins context lifetime; bookkeeping
// locks never span xellSleep, which may need another thread's late marker.
#include "native_internal.hpp"
#include "../latency_marker_policy.hpp"
#include "log_budget.hpp"
#include <chrono>
namespace mfgunlock::xemfg::native {
namespace {
// Grouped by operation, result and whether the game requested FG, so loading
// screens cannot exhaust the log before gameplay refusals are recorded.
void XellFailure(const char* operation,uint64_t output,uint32_t id,int result,uint32_t interval,bool lowLatency) {
  static PatternBudget budget(32);
  const bool requested=State().requested.load();
  const auto count=budget.Admit(std::string(operation)+'/'+std::to_string(result)+'/'+(requested?"1":"0"));
  if(!count) return;
  xess_mod::WriteLog("native_xell_failure","\"operation\":\""+std::string(operation)+"\",\"output\":"+std::to_string(output)
      +",\"id\":"+std::to_string(id)+",\"xell_result\":"+std::to_string(result)
      +",\"interval_us\":"+std::to_string(interval)+",\"low_latency\":"+(lowLatency?"true":"false")
      +",\"fg_requested\":"+(requested?"true":"false")+",\"occurrence\":"+std::to_string(count));
}
}
sl::Result SleepSourceFrame(const std::shared_ptr<Output>& o,uint32_t id,
    PFun_slReflexSleep* nativeSleep,const sl::FrameToken* token,std::optional<uint64_t> expectedEpoch) {
  auto& s=State(); auto& a=s.api;
  uint64_t epoch{};
  const auto route=s.latencyOutput.load();
  const bool reflex=route==LatencyOutput::Reflex;
  // Without the game's native wait (simulation boundary, synthesized frames,
  // or a game wait the caller already sent to Reflex) the Reflex route only
  // associates the frame with XeLL, whose low-latency mode is off there.
  {
    std::lock_guard guard(o->io);
    if(o->retired || o->deviceLost || (expectedEpoch && *expectedEpoch!=o->epoch)) return sl::Result::eErrorInvalidState;
    auto* frame=FindFrame(*o,id); if(!frame) return sl::Result::eErrorInvalidState;
    if(frame->sleeping) return sl::Result::eOk;
    if(o->sleepActive || frame->sleepInFlight) return sl::Result::eErrorInvalidState;
    // XeLL bound to XeFG enters a strict mode after 16 accepted waits and then
    // requires last+1 (libxell 1.3.0.5/1.3.2.10). A wait for an id that is not
    // newer than the last dispatched one, such as a game passing a constant
    // token every frame, would alternate with the real boundary waits and be
    // refused forever. It is never dispatched; the boundary wait covers the frame.
    if(id<=o->lastXellSleep) {
      static PatternBudget budget(8);
      if(const auto count=budget.Admit("stale"))
        xess_mod::WriteLog("native_sleep_rejected","\"source\":"+std::to_string(id)+",\"reason\":\"not_newer_than_dispatched\",\"last\":"
            +std::to_string(o->lastXellSleep)+",\"occurrence\":"+std::to_string(count));
      ++o->rejectedExplicitSleeps;
      return sl::Result::eErrorInvalidState;
    }
    // 1.5.7: the optional display cap joins the game's own limit (XeLL only).
    const auto interval=reflex?0:CombinedIntervalUs(s.limitUs.load(),DisplayCapIntervalForOutput());
    const bool lowLatency=!reflex;
    if(!o->sleepConfigured || o->sleepInterval!=interval || o->sleepLowLatency!=lowLatency) {
      xell_sleep_params_t mode{}; mode.bLowLatencyMode=lowLatency; mode.minimumIntervalUs=interval;
      if(const auto r=a.xellSetSleepMode(o->ll,&mode); r!=XELL_RESULT_SUCCESS) {
        XellFailure("set_sleep_mode",o->diagnosticOutput,id,int(r),interval,lowLatency);
        return sl::Result::eErrorReflexAPI;
      }
      o->sleepConfigured=true; o->sleepInterval=interval; o->sleepLowLatency=lowLatency; ++s.latencyModeUpdates;
      // Games can alternate their limit in menus (STALKER 2: 0 / 16666 us about
      // once per second); every change is applied, the log keeps a budget.
      static PatternBudget budget(16);
      if(const auto count=budget.Admit(std::to_string(interval)+(lowLatency?"/1":"/0")))
        xess_mod::WriteLog("native_sleep_mode","\"interval_us\":"+std::to_string(interval)+",\"low_latency\":"+(lowLatency?"true":"false")
            +",\"occurrence\":"+std::to_string(count));
    }
    epoch=o->epoch; frame->sleepInFlight=true; o->sleepActive=true; o->lastXellSleep=id;
  }
  const auto start=std::chrono::steady_clock::now();
  // Native Reflex is the only latency wait in this route. XeLL sleep with
  // low latency OFF and interval zero retains the separate diagnostic marker context.
  // XeFG is bound to its profiled external-latency path in this route.
  const auto nativeResult=[&] {
    if(!reflex || !nativeSleep || !token) return sl::Result::eOk;
    RuntimeCall trace("reflex_sleep",o->diagnosticOutput,id);
    const auto r=nativeSleep(*token);trace.Result(int(r),r!=sl::Result::eOk);return r;
  }();
  const auto result=[&] {
    if(nativeResult!=sl::Result::eOk) return XELL_RESULT_ERROR_UNKNOWN;
    RuntimeCall trace("xell_sleep",o->diagnosticOutput,id);
    const auto r=a.xellSleep(o->ll,id);trace.Result(r);return r;
  }();
  const auto sleepUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
  s.lastSleepUs=sleepUs;
  {
    std::lock_guard guard(o->io);
    o->sleepActive=false;
    auto* found=o->frames.Find(id);
    if(epoch!=o->epoch || !found) return sl::Result::eErrorInvalidState;
    auto& frame=*found; frame.sleepInFlight=false;
    if(nativeResult!=sl::Result::eOk) { frame.failed=true; return nativeResult; }
    if(result!=XELL_RESULT_SUCCESS) {
      XellFailure("sleep",o->diagnosticOutput,id,int(result),o->sleepInterval,o->sleepLowLatency);
      frame.failed=true; return sl::Result::eErrorReflexAPI;
    }
    frame.sleeping=true; frame.sleepUs=sleepUs; frame.latencyOutput=route; ++s.latencySleeps;
    o->summarySleepUs+=sleepUs; ++o->summarySleepCalls;
    if(reflex) { ++s.reflexSleeps; if(s.gameReflexMode!=0) s.reflexOutputTick=GetTickCount64(); }
    else { ++s.xellSleeps; s.outputTick=GetTickCount64(); }
  }
  return sl::Result::eOk;
}
bool ForwardSourceMarker(const std::shared_ptr<Output>& o,uint32_t id,uint32_t marker) {
  if(marker>6) return false;
  auto& s=State(); std::lock_guard guard(o->io);
  if(o->retired || o->deviceLost) return false;
  auto* frame=FindFrame(*o,id); if(!frame) return false;
  const auto reject=[&](unsigned kind,const char* reason,int result=0) {
    frame->failed=true;
    if(frame->firstFailure==compatibility::Failure::None) {
      frame->firstFailure=kind==0?compatibility::Failure::SourceWait:kind==1?compatibility::Failure::MarkerOrder:compatibility::Failure::MarkerSdk;
      frame->rejectedMarker=marker;frame->markerSdkResult=result;frame->markerObservedBeforeSleep=frame->observedMarkers;
    }
    // SDK refusals are always logged; ordering refusals need stall tracing.
    if((kind==2 || TraceEnabled()) && o->markerRejectLogBudget[kind]<8) {
      ++o->markerRejectLogBudget[kind];
      xess_mod::WriteLog("native_marker_rejected","\"id\":"+std::to_string(id)
          +",\"marker\":"+std::to_string(marker)+",\"reason\":\""+reason+"\",\"sdk_result\":"+std::to_string(result)
          +",\"mask\":"+std::to_string(frame->markers)+",\"sleeping\":"+std::to_string(frame->sleeping)
          +",\"in_flight\":"+std::to_string(frame->sleepInFlight));
    }
    return false;
  };
  // XeLL requires sleep to finish before any marker of the SAME frame.
  // Older frames may still submit their real END timestamp during this sleep.
  if(!frame->sleeping || frame->sleepInFlight) return reject(0,"sleep_not_completed");
  const auto decision=latencymarkers::Classify(frame->markers,marker,frame->sleeping,frame->sleepInFlight);
  if(decision==latencymarkers::Decision::Duplicate) return true;
  // Each END needs its own START; phases may overlap and an old frame's END
  // may arrive after another frame starts. Never fabricate a missing START.
  if(decision==latencymarkers::Decision::Invalid) return reject(1,"marker_order");
  const auto result=s.api.xellAddMarkerData(o->ll,id,static_cast<xell_latency_marker_type_t>(marker));
  if(result!=XELL_RESULT_SUCCESS) return reject(2,"sdk_rejected",int(result));
  ++s.latencyMarkers; frame->markers|=1u<<marker;
  if(marker==XELL_RENDERSUBMIT_END && id<=o->last) {
    std::lock_guard lock(s.lock);
    if(++s.status.lateSubmitEnds<=4)
      xess_mod::WriteLog("native_late_submit_end","\"id\":"+std::to_string(id)+",\"markers\":"+std::to_string(frame->markers));
  }
  return true;
}
}
