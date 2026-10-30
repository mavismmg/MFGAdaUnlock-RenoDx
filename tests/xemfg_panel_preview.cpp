// SPDX-License-Identifier: MIT
// Renders the production XeMFG widgets with synthetic status, without a game,
// a GPU driver, ReShade injection, or global keyboard/mouse input.
#include "../src/addons/mfgunlock/xemfg/native_internal.hpp"
#include "../src/addons/mfgunlock/xemfg/native_overlay.hpp"
#include "../src/addons/mfgunlock/xemfg/backend_api.hpp"
#include "../src/addons/mfgunlock/xemfg/vendor/cuda/cuda_route.h"
#include "../src/addons/mfgunlock/xemfg/vendor/sdk_pacing.h"
#include "../src/addons/mfgunlock/panel_ui.hpp"
#include <deps/imgui/imgui_internal.h>
#include <wincodec.h>
#include <cmath>
#include <iostream>

namespace xess_mod { void WriteLog(const char*,const std::string&) noexcept {} }
namespace mfgunlock::xemfg {
bool ModelStartupFailed() noexcept { return false; }
bool RuntimeFilesRejected(RuntimeFileReport (&)[2]) noexcept { return false; }
void SetGameMultiplier(uint32_t) noexcept {}
LogStatus LogSnapshot() noexcept { return {}; }
bool TryModelStatus(Status& out) { out.model=out.initialized=out.enabled=1; return true; }
namespace native {
OverlayStatus OverlaySnapshot() noexcept { return {1,0,5500,0}; }
OverlayInputStatus OverlayInputSnapshot() noexcept { return {4,2,0}; }
Bridge& State() {static Bridge state; return state;}
Status Snapshot(){ return State().status; }
// DLSS-SR opt-in (1.5.1): the preview shows a Streamline game, where it is never offered.
namespace ngx { bool Configured() noexcept { return false; } bool Suggested() noexcept { return false; }
bool Save(bool) { return true; } int Saved() noexcept { return -1; } }
bool OwnsPresentationHooks() noexcept {return true;}
bool StreamlineStopping() noexcept {return false;}
bool D3D12DiagnosticsForced() {return false;}
Output::~Output()=default;
std::shared_ptr<Output> Active() {
  static auto output=std::make_shared<Output>();
  if(!output->window) output->window=std::make_shared<WindowControl>();
  output->uiCompositionAvailable=true;
  return output;
}
WindowStatus WindowControl::Snapshot() { WindowStatus value; value.available=value.borderless=value.coversMonitor=true; value.mode=DisplayMode::NativeBorderless; return value; }
void WindowControl::Request(bool,bool) {}
}
}
namespace xefg_cuda {
bool TrySnapshot(Status& out) noexcept { out.state=State::Active; out.major=8; out.minor=9; out.layers=17; out.launches=260040; return true; }
}
namespace sdk_pacing {
void RequestQueueAhead(bool) noexcept {} void RequestNativeLatency(uint32_t) noexcept {}
void RequestFlipMetering(bool) noexcept {}
void RequestSdkDeadlines(bool) noexcept {}
FlipStatus Snapshot(void*) noexcept {
  FlipStatus status; status.requested=status.enabled=status.available=status.installed=status.registered=status.queue=status.configured=status.metered=true;
  status.outputs=4; status.calls=3500; return status;
}
}
namespace {
void Check(HRESULT value) { if(FAILED(value)) throw std::runtime_error("WIC failure"); }
void Save(const std::filesystem::path& path,std::vector<unsigned char>& pixels,int width,int height) {
  for(size_t i=0;i<pixels.size();i+=4) std::swap(pixels[i],pixels[i+2]);
  ComPtr<IWICImagingFactory> factory;
  Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
  ComPtr<IWICStream> stream; Check(factory->CreateStream(&stream)); Check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
  ComPtr<IWICBitmapEncoder> encoder; Check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));
  Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> frame; Check(encoder->CreateNewFrame(&frame,nullptr)); Check(frame->Initialize(nullptr));
  Check(frame->SetSize(width,height)); auto format=GUID_WICPixelFormat32bppBGRA; Check(frame->SetPixelFormat(&format));
  Check(frame->WritePixels(height,width*4,UINT(pixels.size()),pixels.data())); Check(frame->Commit()); Check(encoder->Commit());
}
void Raster(ImDrawData* data,const unsigned char* tex,int tw,int th,std::vector<unsigned char>& pixels,int width,int height) {
  for(const auto* list:data->CmdLists) for(const auto& cmd:list->CmdBuffer) {
    if(cmd.UserCallback) continue;
    for(unsigned n=0;n+2<cmd.ElemCount;n+=3) {
      const auto& a=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+n]+cmd.VtxOffset];
      const auto& b=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+n+1]+cmd.VtxOffset];
      const auto& c=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+n+2]+cmd.VtxOffset];
      const float area=(b.pos.y-c.pos.y)*(a.pos.x-c.pos.x)+(c.pos.x-b.pos.x)*(a.pos.y-c.pos.y);
      if(std::abs(area)<1e-6f) continue;
      const int left=std::max({0,int(std::floor(std::min({a.pos.x,b.pos.x,c.pos.x}))),int(std::ceil(cmd.ClipRect.x))});
      const int right=std::min({width,int(std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x}))),int(cmd.ClipRect.z)});
      const int top=std::max({0,int(std::floor(std::min({a.pos.y,b.pos.y,c.pos.y}))),int(std::ceil(cmd.ClipRect.y))});
      const int bottom=std::min({height,int(std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y}))),int(cmd.ClipRect.w)});
      for(int y=top;y<bottom;++y) for(int x=left;x<right;++x) {
        const float u=((b.pos.y-c.pos.y)*(x+.5f-c.pos.x)+(c.pos.x-b.pos.x)*(y+.5f-c.pos.y))/area;
        const float v=((c.pos.y-a.pos.y)*(x+.5f-c.pos.x)+(a.pos.x-c.pos.x)*(y+.5f-c.pos.y))/area;
        const float w=1-u-v; if(u<0 || v<0 || w<0) continue;
        const int tx=std::clamp(int((a.uv.x*u+b.uv.x*v+c.uv.x*w)*tw),0,tw-1);
        const int ty=std::clamp(int((a.uv.y*u+b.uv.y*v+c.uv.y*w)*th),0,th-1);
        const auto* t=tex+(ty*tw+tx)*4;
        const float alpha=(float(a.col>>24)*u+float(b.col>>24)*v+float(c.col>>24)*w)*t[3]/65025.f;
        auto* p=pixels.data()+(y*width+x)*4;
        for(int channel=0;channel<3;++channel) {
          const float color=float((a.col>>(channel*8))&255)*u+float((b.col>>(channel*8))&255)*v+float((c.col>>(channel*8))&255)*w;
          p[channel]=static_cast<unsigned char>(color*t[channel]/255.f*alpha+p[channel]*(1-alpha));
        }
      }
    }
  }
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    if(argc<4 || argc>6) throw std::runtime_error("usage: preview output.png page(0..3) width [font-size] [expanded|performance|tooltip|multiplier]");
    const std::wstring scenario=argc==6?argv[5]:L"";
    const int page=std::stoi(argv[2]),width=std::stoi(argv[3]),height=1250;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.IniFilename=nullptr;
    io.DisplaySize=ImVec2(float(width),float(height)); io.DeltaTime=1.f/60.f;
    if(scenario==L"navigation") io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig font; font.SizePixels=argc>=5?std::stof(argv[4]):16.f; io.Fonts->AddFontDefault(&font);
    unsigned char* tex{}; int tw{},th{}; io.Fonts->GetTexDataAsRGBA32(&tex,&tw,&th); io.Fonts->SetTexID(1);
    auto& style=ImGui::GetStyle(); style.WindowRounding=10; style.WindowPadding=ImVec2(16,14);
    style.Colors[ImGuiCol_WindowBg]=ImVec4(.085f,.095f,.12f,1);
    mfgunlock::backend::active=mfgunlock::backend::Kind::XeMfg; mfgunlock::backend::Select(mfgunlock::backend::active);
    using namespace mfgunlock::xemfg::native;
    LoadControls(); auto& state=State(); state.status.enabled=state.status.output=state.status.xell=state.status.streamline=true;
    state.status.submitted=4; state.status.frames=5500; state.multiplier=4; state.reflexOff=true;
    state.latencySleeps=5500; state.latencyMarkers=33000; state.gameReflexMode=1; state.latencyModeUpdates=1;
    ImVec2 click{}; unsigned navigationFrames{},controlFrames{}; std::string lastNavigationWindow;
    for(int frame=0;frame<48;++frame) {
      if(frame==2) io.AddMousePosEvent(click.x,click.y);
      if(frame==4 || frame==6) io.AddMouseButtonEvent(0,frame==4);
      if(scenario==L"multiplier" && (frame==12 || frame==14)) io.AddMouseButtonEvent(0,frame==12);
      if(scenario==L"navigation" && frame>=8) {
        io.AddKeyEvent(ImGuiKey_Tab,frame%4==0);
        io.AddKeyEvent(ImGuiKey_Enter,frame==10);
        io.AddKeyEvent(ImGuiKey_Home,frame==37);
      }
      state.inputTick=state.outputTick=GetTickCount64();
      ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
      ImGui::Begin("Layout preview - synthetic status",nullptr,ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);
      { mfgunlock::ui::Theme theme;
        if(scenario==L"navigation") {
          mfgunlock::ui::Hero("4x","XeMFG","DX12","Generating frames",true);
          mfgunlock::ui::Card controls("##keyboard_controls");
          static int selected{};
          mfgunlock::ui::Setting("##keyboard_multiplier","Frame multiplier","Keyboard navigation remains available in real controls.",[&] {
            ImGui::Combo("##keyboard_combo",&selected,"2x\0" "3x\0" "4x\0");
          });
          static bool enabled{}; ImGui::Checkbox("Driver flip metering",&enabled);
        } else Draw();
      }
      if(scenario==L"navigation" && GImGui->NavWindow) {
        if(GImGui->NavId) ++navigationFrames;
        const auto* name=GImGui->NavWindow->Name;
        if(lastNavigationWindow!=name) { lastNavigationWindow=name; std::cout<<"Navigation window: "<<name<<'\n'; }
        if(strstr(name,"/summary_") || strstr(name,"##compact_overview"))
          throw std::runtime_error("read-only overview captured keyboard navigation");
        if(GImGui->NavId) ++controlFrames;
      }
      for(auto* window:GImGui->Windows)
        if(window->Active && (window->Flags&ImGuiWindowFlags_ChildWindow)
            && (strstr(window->Name,"/summary_") || strstr(window->Name,"##compact_overview"))
            && (window->Flags&ImGuiWindowFlags_NoInputs)!=ImGuiWindowFlags_NoInputs)
          throw std::runtime_error("read-only card accepts input");
      if(scenario==L"navigation" && frame<2) {
        if(auto* table=GImGui->Tables.GetByKey(ImGui::GetID("##mfg_summary_cards"))) click=table->OuterRect.GetCenter();
        else { ImGui::PushID("##compact_overview");
          if(auto* compact=GImGui->Tables.GetByKey(ImGui::GetID("##compact_summary"))) click=compact->OuterRect.GetCenter();
          ImGui::PopID(); }
      }
      auto* bar=GImGui->TabBars.GetByKey(ImGui::GetID("##xemfg_pages"));
      if(bar && bar->Tabs.Size==4) click=ImVec2(bar->BarRect.Min.x+bar->Tabs[page].Offset+bar->Tabs[page].Width*.5f,bar->BarRect.GetCenter().y);
      if(frame==9 && bar && bar->SelectedTabId!=bar->Tabs[page].ID) throw std::runtime_error("requested preview tab was not selected");
      auto groupWindow=[&](const char* group) -> ImGuiWindow* {
        for(auto* window:GImGui->Windows)
          if((window->Flags&ImGuiWindowFlags_ChildWindow) && strstr(window->Name,group)) return window;
        return nullptr;
      };
      if(frame>=8) {
        const char* group=scenario==L"expanded"?"##xe_latency":scenario==L"performance"?"##xe_model_details":"##xe_presentation";
        const char* heading=scenario==L"expanded"?"Advanced latency routing":scenario==L"performance"?"Performance capture (advanced)":"Advanced pacing comparison";
        if(scenario==L"expanded" || scenario==L"performance" || scenario==L"pacing")
          if(auto* window=groupWindow(group)) window->StateStorage.SetInt(window->GetID(heading),1);
        if(scenario==L"tooltip" || scenario==L"multiplier") {
          auto* window=groupWindow("##xemfg_generation");
          auto* table=window?GImGui->Tables.GetByKey(window->GetID(scenario==L"multiplier"?"##xe_multiplier_row":"##backend_selection")):nullptr;
          if(table && table->ColumnsCount==2)
            io.AddMousePosEvent((table->Columns[1].MinX+table->Columns[1].MaxX)*.5f,table->OuterRect.GetCenter().y);
        }
      }
      if(frame==47 && (scenario==L"expanded" || scenario==L"performance" || scenario==L"pacing")) {
        const char* group=scenario==L"expanded"?"##xe_latency":scenario==L"performance"?"##xe_model_details":"##xe_presentation";
        const char* heading=scenario==L"expanded"?"Advanced latency routing":scenario==L"performance"?"Performance capture (advanced)":"Advanced pacing comparison";
        auto* window=groupWindow(group);
        if(!window || !window->StateStorage.GetInt(window->GetID(heading))) throw std::runtime_error("advanced group was not expanded");
      }
      if(frame==47 && scenario==L"multiplier" && GImGui->OpenPopupStack.empty()) throw std::runtime_error("multiplier popup did not open");
      if(frame==47 && scenario==L"dependencies" && !groupWindow("##intel_dependencies")) throw std::runtime_error("dependency controls were not expanded");
      if(frame==47 && scenario==L"tooltip") {
        bool found=false; for(auto* window:GImGui->Windows) if(window->Active && (window->Flags&ImGuiWindowFlags_Tooltip)) found=true;
        if(!found) throw std::runtime_error("control tooltip did not appear");
      }
      ImGui::End(); ImGui::Render();
    }
    std::vector<unsigned char> pixels(size_t(width)*height*4,255);
    if(scenario==L"navigation" && (!navigationFrames || !controlFrames)) throw std::runtime_error("keyboard test did not reach real settings: "+std::to_string(navigationFrames)+"/"+std::to_string(controlFrames));
    Raster(ImGui::GetDrawData(),tex,tw,th,pixels,width,height);
    std::filesystem::create_directories(std::filesystem::path(argv[1]).parent_path()); Save(argv[1],pixels,width,height);
    ImGui::DestroyContext(); std::cout<<"Rendered actual XeMFG widgets with synthetic status\n"; return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
