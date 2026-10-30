// SPDX-License-Identifier: MIT
// Support export contains metadata only. Serialization and file hashing run
// on request or on a worker thread at lifecycle points, never in Present.
#include "native_internal.hpp"
#include "backend_api.hpp"
#include "../backend_selection.hpp"
#include "vendor/sha256_util.h"
#ifdef MFGUNLOCK_XEMFG_VULKAN
#include "native_vulkan.hpp"
#endif
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <array>
#include <cstring>
namespace mfgunlock::xemfg::native {
using Json=nlohmann::ordered_json;
namespace {
// 1.7.1: a Vulkan game has no DX12 output; the host's state is what explains it.
Json VulkanReport() {
#ifdef MFGUNLOCK_XEMFG_VULKAN
  const auto vk=vulkan::Status();
  return {{"game",vk.game},{"active",vk.active},{"frames",vk.frames},{"input_frames",vk.inputFrames},
      {"base_fps_milli",vk.baseFpsMilli},{"output_fps_milli",vk.outputFpsMilli},{"reason",vk.reason},
      {"dynamic",vk.dynamic},{"fps_limit",vk.fpsLimit},{"game_latency",vk.gameLatency}};
#else
  return {{"game",false}};
#endif
}
Json Module(HMODULE module) {
  if(!module)return {{"state","unavailable"}};
  wchar_t path[32768]{};
  if(!GetModuleFileNameW(module,path,DWORD(std::size(path))))return {{"state","unavailable"}};
  const std::filesystem::path file(path);
  Json value={{"file",file.filename().string()},{"state","observed"}};
  HANDLE handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(handle==INVALID_HANDLE_VALUE) {value["sha256"]=nullptr;return value;}
  sha256_util::Hasher hash;std::array<unsigned char,65536> bytes{};DWORD count{};bool okay=true;
  while(okay) {
    if(!ReadFile(handle,bytes.data(),DWORD(bytes.size()),&count,nullptr)) {okay=false;break;}
    if(!count)break;
    okay=hash.Update(bytes.data(),count);
  }
  CloseHandle(handle);sha256_util::Digest digest{};
  if(okay && hash.Finish(digest))value["sha256"]=sha256_util::LowerHex(digest);
  else value["sha256"]=nullptr;
  return value;
}
}
std::string CompatibilityReport(const char* exportReason) {
  auto& s=State();const auto status=Snapshot();
  Json report={{"schema","mfgunlock.compatibility"},{"version",1},{"build","1.7.0"},
      {"export_reason",exportReason?exportReason:"user"},
      {"vulkan",VulkanReport()},
      {"adapter",status.output?"streamline_d3d12":"none"},{"fallback","xemfg_selected_passthrough"},
      {"status",status.detail},
      {"displayed_frames_measured",false},{"physical_latency_measured",false},
      {"hooks",s.hookCapabilities.load()},{"stale_inputs_discarded",s.staleInputs.load()},
      {"external_observer_resolutions",s.externalResolutions.load()}};
  report["modules"]={{"frame_generation",Module(s.api.fgModule)},{"latency",Module(s.api.llModule)},
      {"streamline",Module(GetModuleHandleW(L"sl.interposer.dll"))}};
  report["streamline"]={{"sdk_version",StreamlineSdkVersion()},{"sdk_magic",StreamlineSdkMagic()},
      {"interposer_version",FileVersion(GetModuleHandleW(L"sl.interposer.dll"))}};
  report["capabilities"]=Json::object();
  // ordered_json uses vector storage: adding a sibling can invalidate a
  // reference to this child. Look it up for each explicit export field.
  const auto capability=[&](const char* key,compatibility::Evidence state) {report["capabilities"][key]=compatibility::Name(state);};
  const auto hooks=s.hookCapabilities.load();
  capability("tag_api",hooks&((1u<<5)|(1u<<6))?compatibility::Evidence::Confirmed:compatibility::Evidence::Unavailable);
  capability("constant_api",hooks&(1u<<4)?compatibility::Evidence::Confirmed:compatibility::Evidence::Unavailable);
  capability("latency_api",hooks&(1u<<1)?compatibility::Evidence::Confirmed:compatibility::Evidence::Unavailable);
  capability("presentation_ownership",OwnsPresentationHooks()?compatibility::Evidence::Confirmed:compatibility::Evidence::Unavailable);
  report["lifecycle"]=StreamlineStopping()?"stopping":status.output?"awaiting_frame":"discovering";
  if(auto o=Active()) {
    // A support export cannot wait behind a blocked SDK call. Report the
    // missing snapshot explicitly instead of freezing the ReShade panel.
    std::unique_lock guard(o->io,std::try_to_lock);
    if(!guard.owns_lock()) {report["snapshot"]="busy";return report.dump(2);}
    report["snapshot"]="available";
    report["output"]={{"generation",o->diagnosticOutput},{"epoch",o->epoch},{"width",o->width},
        {"height",o->height},{"format",uint32_t(o->format)},{"depth_inverted",o->invert},{"jittered_motion",o->jitter},{"motion_dejitter",o->dejitter!=nullptr},
        {"device_lost",o->deviceLost},{"present_hresult",uint32_t(o->presentFailure)},
        {"device_removed_reason",uint32_t(o->removedReason)},
        {"windowed_presentation_supported",o->presentationSupported},
        {"dred_query_hresult",uint32_t(o->dredResult)},{"dred_nodes",o->dredNodes},
        {"dred_unfinished_lists",o->dredUnfinished}};
    report["source_wait"]={{"explicit_accepted",o->explicitSleepObserved},
        {"explicit_rejected",o->rejectedExplicitSleeps},{"unslept_simulation_starts",o->unsleptSimulationStarts}};
    report["blocker"]=compatibility::Name(o->blocker);
    report["lifecycle"]=o->deviceLost?"device_lost":o->retired?"retired":o->enabled?"active":"passthrough";
    const auto* frame=o->frames.Find(o->last);
    const auto observed=[&](bool available,bool invalid=false) {return invalid?compatibility::Evidence::Invalid:
        available?compatibility::Evidence::Confirmed:compatibility::Evidence::Unavailable;};
    capability("depth",observed(frame && (frame->resources&(1u<<XEFG_SWAPCHAIN_RES_DEPTH))));
    capability("motion_vectors",observed(frame && (frame->resources&(1u<<XEFG_SWAPCHAIN_RES_MOTION_VECTOR))));
    capability("constants",observed(frame && frame->constants,frame && frame->constantIssues));
    capability("source_wait",observed(frame && frame->sleeping));
    constexpr uint32_t required=(1u<<XELL_SIMULATION_START)|(1u<<XELL_SIMULATION_END)|(1u<<XELL_RENDERSUBMIT_START)|(1u<<XELL_PRESENT_START);
    capability("latency_markers",observed(frame && (frame->markers&required)==required));
    // Structural acceptance does not establish the pixels' colour encoding.
    capability("hud_encoding",frame && (frame->resources&1u)?compatibility::Evidence::Inferred:compatibility::Evidence::Unavailable);
    if(frame)report["frame"]={{"id",frame->id},{"viewport",frame->viewport},{"resources",frame->resources},
        {"observed_markers",frame->observedMarkers},{"accepted_markers",frame->markers},
        {"constant_issues",frame->constantIssues},{"source_wait_completed",frame->sleeping},
        {"first_input_failure",compatibility::Name(frame->firstFailure)},
        {"rejected_marker",frame->rejectedMarker},{"marker_sdk_result",frame->markerSdkResult},
        {"markers_at_first_rejection",frame->markerObservedBeforeSleep}};
    if(frame) {
      report["frame"]["resource_receipts"]=Json::array();
      for(uint32_t type=0;type<4;++type) {
        const auto& r=frame->resourceReceipts[type];
        if(!r.observed)continue;
        report["frame"]["resource_receipts"].push_back({{"type",type},{"accepted",r.accepted},
            {"width",r.width},{"height",r.height},{"format",r.format},{"incoming_state",r.incomingState},
            {"lifecycle",r.lifecycle},{"sdk_result",r.sdkResult},{"rejection",r.rejection?r.rejection:"none"}});
      }
    }
  } else {report["blocker"]="no_output";report["snapshot"]="no_output";}
  report["limitations"]={"No resource contents, personal paths, pointers or GPU addresses are exported",
      "SDK output counts are not display or scanout measurements",
      "Native XeFG and FSR-FG adapters require their own validated contracts"};
  return report.dump(2);
}
namespace {
void WriteReport(const char* reason) noexcept {
  try {
    const auto directory=backend::Directory(); if(directory.empty()) return;
    const auto text=CompatibilityReport(reason);
    const auto target=directory/L"MFGAdaUnlock-XeMFG-compatibility.json";
    auto temporary=target; temporary+=L".writing";
    {
      std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
      out.write(text.data(),std::streamsize(text.size()));
      if(!out) return;
    }
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) DeleteFileW(temporary.c_str());
  } catch(...) {}
}
}
void ExportCompatibilityReport(const char* reason) noexcept {
  if(!strcmp(reason,"shutdown")) { WriteReport(reason); return; }
  static std::atomic<bool> pending{};
  if(pending.exchange(true)) return;
  try {
    // The caller may hold the output lock; the report uses try_lock, so give
    // that frame time to finish. The addon is pinned for the process lifetime.
    std::thread([reason] { Sleep(200); WriteReport(reason); pending=false; }).detach();
  } catch(...) { pending=false; }
}
void RecordDeviceFailure(Output& o,HRESULT present) {
  o.presentFailure=present;
  o.removedReason=o.device?o.device->GetDeviceRemovedReason():S_OK;
  if(FAILED(o.removedReason) || present==DXGI_ERROR_DEVICE_REMOVED || present==DXGI_ERROR_DEVICE_RESET) {
    o.deviceLost=true;o.enabled=false;o.blocker=compatibility::Failure::DeviceLost;
    auto& s=State();std::lock_guard guard(s.lock);s.status.enabled=s.status.inputs=false;s.status.sdkResult=present;
  }
  // Query only after a failure, without enabling DRED or changing device flags.
  // Do not export object names, GPU VAs or proprietary command contents.
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output{};
  uint32_t nodes{},unfinished{};HRESULT dredResult=E_NOINTERFACE;
  if(o.device && SUCCEEDED(o.device.As(&dred))) {
    dredResult=dred->GetAutoBreadcrumbsOutput1(&output);
    if(SUCCEEDED(dredResult))for(auto* node=output.pHeadAutoBreadcrumbNode;node && nodes<64;node=node->pNext) {
      ++nodes;
      unfinished+=node->pLastBreadcrumbValue && *node->pLastBreadcrumbValue<node->BreadcrumbCount;
    }
  }
  o.dredResult=dredResult;o.dredNodes=nodes;o.dredUnfinished=unfinished;
  // Opt-in local diagnostics only: name the unfinished lists, their ops and
  // the page-fault allocations. Default builds keep the metadata-only record.
  if(D3D12DiagnosticsForced() && dred) {
    const auto narrow=[](const wchar_t* w,const char* a) {
      if(a) return std::string(a);
      if(!w) return std::string();
      char out[256]{}; WideCharToMultiByte(CP_UTF8,0,w,-1,out,int(sizeof(out)-1),nullptr,nullptr); return std::string(out);
    };
    uint32_t index{};
    if(SUCCEEDED(dredResult))for(auto* node=output.pHeadAutoBreadcrumbNode;node && index<16;node=node->pNext,++index) {
      const uint32_t last=node->pLastBreadcrumbValue?*node->pLastBreadcrumbValue:0;
      if(last>=node->BreadcrumbCount) continue;
      const int done=last?int(node->pCommandHistory[last-1]):-1, next=int(node->pCommandHistory[last]);
      xess_mod::WriteLog("native_dred_list","\"index\":"+std::to_string(index)
          +",\"list\":\""+escape(narrow(node->pCommandListDebugNameW,node->pCommandListDebugNameA))
          +"\",\"queue\":\""+escape(narrow(node->pCommandQueueDebugNameW,node->pCommandQueueDebugNameA))
          +"\",\"completed\":"+std::to_string(last)+",\"count\":"+std::to_string(node->BreadcrumbCount)
          +",\"last_op\":"+std::to_string(done)+",\"next_op\":"+std::to_string(next));
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    const auto faultResult=dred->GetPageFaultAllocationOutput1(&fault);
    std::string existing,freed; uint32_t count{};
    if(SUCCEEDED(faultResult)) {
      for(auto* a=fault.pHeadExistingAllocationNode;a && count<8;a=a->pNext,++count)
        existing+=(existing.empty()?"\"":",\"")+escape(narrow(a->ObjectNameW,a->ObjectNameA))+"\"";
      count=0;
      for(auto* a=fault.pHeadRecentFreedAllocationNode;a && count<8;a=a->pNext,++count)
        freed+=(freed.empty()?"\"":",\"")+escape(narrow(a->ObjectNameW,a->ObjectNameA))+"\"";
    }
    xess_mod::WriteLog("native_dred_page_fault","\"result\":"+std::to_string(uint32_t(faultResult))
        +",\"va\":"+std::to_string(fault.PageFaultVA)+",\"existing\":["+existing+"],\"recent_freed\":["+freed+"]");
  }
  xess_mod::WriteLog("native_device_failure","\"output\":"+std::to_string(o.diagnosticOutput)
      +",\"epoch\":"+std::to_string(o.epoch)+",\"present_hresult\":"+std::to_string(uint32_t(present))
      +",\"device_removed_reason\":"+std::to_string(uint32_t(o.removedReason))
      +",\"dred_query_result\":"+std::to_string(uint32_t(dredResult))+",\"dred_nodes\":"+std::to_string(nodes)
      +",\"unfinished_breadcrumb_lists\":"+std::to_string(unfinished));
}
}
extern "C" __declspec(dllexport) uint32_t MFGAXeCompatibilityReport(char* destination,uint32_t capacity) {
  try {
    const auto report=mfgunlock::xemfg::native::CompatibilityReport();
    if(destination && capacity>=report.size()+1)memcpy(destination,report.c_str(),report.size()+1);
    return uint32_t(report.size()+1);
  }catch(...) {return 0;}
}
