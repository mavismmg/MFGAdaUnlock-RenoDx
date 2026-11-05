// SPDX-License-Identifier: MIT
// Shared player-facing presentation. Styles are scoped to this addon only.
#pragma once
#include <deps/imgui/imgui.h>
#ifndef MFGUNLOCK_UI_PREVIEW
#include <include/reshade.hpp>
#endif
#include "backend_selection.hpp"
#include "support_files.hpp"
#include "system_info.hpp"
#include <algorithm>
#include <cfloat>
#include <string>
#include <shellapi.h>

namespace mfgunlock::ui {
inline constexpr ImVec4 Accent{0.39f,0.84f,0.71f,1.f};
inline constexpr ImVec4 Muted{0.65f,0.72f,0.82f,1.f};
inline constexpr ImVec4 Warning{1.f,0.77f,0.35f,1.f};
inline constexpr ImVec4 Bright{.93f,.95f,.98f,1.f};
inline constexpr ImVec4 Danger{1.f,.48f,.45f,1.f};
// ReShade's blue: anything that is not a health status (green, yellow, red).
inline constexpr ImVec4 Selected{.165f,.29f,.47f,1.f};
inline constexpr ImVec4 SelectedLine{.29f,.47f,.72f,1.f};
inline constexpr const char* kName="MFG Unlock";
inline constexpr ImVec4 CardBg{.125f,.13f,.14f,1.f};
inline constexpr ImVec4 ControlBg{.095f,.1f,.108f,1.f};
inline constexpr ImVec4 Line{.25f,.27f,.29f,1.f};
inline float Scale() { return ImGui::GetFontSize()/16.f; }
inline float BorderWidth() { return (std::max)(1.5f,1.75f*Scale()); }
// 1.8.1: grey cards and controls on ReShade's own window background.
struct Theme {
  Theme() {
    const float scale=Scale();
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.91f,.925f,.937f,1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,CardBg);
    ImGui::PushStyleColor(ImGuiCol_Border,Line);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ControlBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.16f,.17f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.19f,.2f,.215f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,SelectedLine);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,SelectedLine);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.16f,.17f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.2f,.215f,.23f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,Selected);
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.16f,.17f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.2f,.215f,.23f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,Selected);
    ImGui::PushStyleColor(ImGuiCol_Tab,CardBg);
    ImGui::PushStyleColor(ImGuiCol_TabHovered,ImVec4(.2f,.215f,.23f,1));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,ImVec4(.16f,.17f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,Muted);
    ImGui::PushStyleColor(ImGuiCol_Separator,Line);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,9.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(9.f*scale,5.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(9.f*scale,6.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(5.f*scale,5.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(11.f*scale,9.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding,0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,BorderWidth());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,BorderWidth());
  }
  ~Theme() { ImGui::PopStyleVar(9); ImGui::PopStyleColor(20); }
};
inline void Tooltip(const char* text) {
  if(!text || !text[0] || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip|ImGuiHoveredFlags_AllowWhenDisabled)) return;
  if(ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize()*34.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
  }
}
inline void Description(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text,Muted); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor();
}
inline void Section(const char* title,const char* description=nullptr) {
  ImGui::Spacing(); ImGui::TextUnformatted(title);
  if(description) Description(description);
}
// ReShade ImGui cards. Read-only overview cards explicitly reject input/focus.
struct Card {
  bool visible;
  Card(const char* id,ImGuiWindowFlags flags=0) {
    visible=ImGui::BeginChild(id,ImVec2(0,0),ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|flags);
  }
  ~Card() { ImGui::EndChild(); ImGui::Spacing(); }
  explicit operator bool() const { return visible; }
};
struct NoNavigation {
  NoNavigation() { ImGui::PushItemFlag(ImGuiItemFlags_NoNav,true); }
  ~NoNavigation() { ImGui::PopItemFlag(); }
};
// Label left, control right, two equal columns. Help appears when hovering
// the label or the control, so rows stay aligned without per-row markers.
template<class F> void Setting(const char* id,const char* title,const char* description,F control) {
  const auto label=[&] {
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+(std::max)(1.f,ImGui::GetContentRegionAvail().x));
    ImGui::TextUnformatted(title); ImGui::PopTextWrapPos(); Tooltip(description);
  };
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*28.f) {
    label(); ImGui::SetNextItemWidth(-1); control(); Tooltip(description); return;
  }
  if(ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) {
    ImGui::TableSetupColumn("Setting",ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Control",ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextColumn(); label();
    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); control(); Tooltip(description); ImGui::EndTable();
  }
}
inline void Notice(const char* text,ImVec4 color=Warning) {
  ImGui::PushStyleColor(ImGuiCol_Text,color); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor();
}
// A short choice shown whole, instead of a list to open. Fills the row.
inline bool Segmented(const char* id,int& value,const char* const* labels,int count) {
  ImGui::PushID(id);
  // One item to the caller: a row's tooltip covers every option, not just the last.
  ImGui::BeginGroup();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.f);
  const float width=(std::max)(1.f,ImGui::GetContentRegionAvail().x/float(count));
  const float rounding=ImGui::GetStyle().FrameRounding,height=ImGui::GetFrameHeight();
  const auto start=ImGui::GetCursorScreenPos();
  const ImVec2 end(start.x+width*float(count),start.y+height);
  auto* draw=ImGui::GetWindowDrawList();
  draw->AddRectFilled(start,end,ImGui::GetColorU32(ImGuiCol_FrameBg),rounding);
  bool changed=false;
  for(int i=0;i<count;++i) {
    if(i) ImGui::SameLine();
    const bool on=i==value;
    // The row's outer corners stay round, so the selection is drawn here and not by the button.
    if(on) {
      const ImVec2 min(start.x+width*float(i),start.y);
      const ImDrawFlags corners=count==1?ImDrawFlags_RoundCornersAll:i==0?ImDrawFlags_RoundCornersLeft:
          i==count-1?ImDrawFlags_RoundCornersRight:ImDrawFlags_RoundCornersNone;
      draw->AddRectFilled(min,ImVec2(min.x+width,end.y),ImGui::GetColorU32(Selected),rounding,corners);
    }
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,on?ImVec4(0,0,0,0):ImVec4(1,1,1,.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,on?ImVec4(0,0,0,0):ImVec4(1,1,1,.12f));
    ImGui::PushStyleColor(ImGuiCol_Text,on?Bright:Muted);
    if(ImGui::Button(labels[i],ImVec2(width,height)) && !on) { value=i; changed=true; }
    ImGui::PopStyleColor(4);
  }
  ImGui::PopStyleVar(2);
  ImGui::EndGroup();
  draw->AddRect(start,end,ImGui::GetColorU32(ImGuiCol_Border),rounding,0,BorderWidth());
  ImGui::PopID();
  return changed;
}
inline void Readout(const char* id,const char* title,const char* value,const char* tooltip=nullptr,ImVec4 color=Muted) {
  Setting(id,title,tooltip,[&] {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text,color); ImGui::TextWrapped("%s",value); ImGui::PopStyleColor();
  });
}
// Advanced groups open in place, aligned with the rows around them. The ID
// is the label, identical to the CollapsingHeader it replaces.
inline bool Expander(const char* label) {
  ImGui::Separator();
  return ImGui::TreeNodeEx(label,ImGuiTreeNodeFlags_NoTreePushOnOpen|ImGuiTreeNodeFlags_SpanAvailWidth|ImGuiTreeNodeFlags_FramePadding);
}
// 1.8.1: the page tabs are taller than other controls. ImGui measures the bar and each
// tab from FramePadding, so it holds until EndTabs.
inline bool BeginTabs(const char* id) {
  const auto padding=ImGui::GetStyle().FramePadding;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(padding.x,12.f*Scale()));
  const bool open=ImGui::BeginTabBar(id);
  if(!open) ImGui::PopStyleVar();
  return open;
}
inline void EndTabs() { ImGui::EndTabBar(); ImGui::PopStyleVar(); }
// Equal-width tabs that fill the row. Measure before BeginTabs.
inline float TabWidth(int count) {
  const float spacing=ImGui::GetStyle().ItemInnerSpacing.x;
  return (std::max)(1.f,float(int((ImGui::GetContentRegionAvail().x-spacing*float(count-1))/float(count))));
}
// ImGui draws tab labels left-aligned; leading spaces center them while
// "###label" keeps the tab ID independent of the padding.
inline bool TabItem(const char* label,float width) {
  const float pad=(width-ImGui::CalcTextSize(label).x)*.5f-ImGui::GetStyle().FramePadding.x;
  std::string text(pad>0.f?size_t(pad/ImGui::CalcTextSize(" ").x):0u,' ');
  text.append(label).append("###").append(label);
  ImGui::SetNextItemWidth(width);
  const bool open=ImGui::BeginTabItem(text.c_str());
  // Blue under the selected tab, a quiet line under the rest. ImGui only offers one on top.
  const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
  const float thickness=open?3.f*Scale():2.f*Scale();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x,max.y-thickness*.5f),ImVec2(max.x,max.y-thickness*.5f),
      ImGui::GetColorU32(open?SelectedLine:Line),thickness);
  return open;
}
// A row of equal-width buttons. Narrow windows stack them at full width.
class ButtonRow {
  int index{}; float width{}; bool stacked{};
public:
  explicit ButtonRow(int count) {
    const float spacing=ImGui::GetStyle().ItemSpacing.x,avail=ImGui::GetContentRegionAvail().x;
    stacked=avail<ImGui::GetFontSize()*11.f*float(count);
    width=stacked?-FLT_MIN:(avail-spacing*float(count-1))/float(count);
  }
  bool operator()(const char* label,const char* tooltip=nullptr) {
    if(index++ && !stacked) ImGui::SameLine();
    const bool pressed=ImGui::Button(label,ImVec2(width,0)); Tooltip(tooltip); return pressed;
  }
};
inline void StatusDot(ImVec4 color) {
  const float size=ImGui::GetFontSize()*.5f;
  const auto at=ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x+size*.5f,at.y+ImGui::GetTextLineHeight()*.5f),size*.5f,ImGui::GetColorU32(color));
  ImGui::Dummy(ImVec2(size,ImGui::GetTextLineHeight())); ImGui::SameLine(0,ImGui::GetStyle().ItemInnerSpacing.x);
}
inline void Centered(const char* text,ImVec4 color) {
  const float avail=ImGui::GetContentRegionAvail().x,width=ImGui::CalcTextSize(text).x;
  if(width<avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(avail-width)*.5f);
  ImGui::TextColored(color,"%s",text);
}
inline void Hero(const char* multiplier,const char* backend,const char* renderer,const char* state,bool active) {
  const float right=ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x;
  ImGui::TextUnformatted(kName);
  const float stateWidth=ImGui::CalcTextSize(state).x+ImGui::GetFontSize()*.5f+ImGui::GetStyle().ItemInnerSpacing.x;
  if(ImGui::GetContentRegionAvail().x>stateWidth+ImGui::GetFontSize()*10.f) {
    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),right-stateWidth));
  }
  StatusDot(active?Accent:Warning);
  ImGui::PushTextWrapPos(); ImGui::TextColored(active?Accent:Warning,"%s",state); ImGui::PopTextWrapPos();
  const char* values[]={multiplier,backend,renderer};
  const char* labels[]={"Multiplier","Backend","Renderer"};
  // These are readouts, including the child-container item added by EndChild.
  // Keep real settings outside this scope so keyboard navigation still works.
  NoNavigation summaries;
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    Card card("##compact_overview",ImGuiWindowFlags_NoInputs);
    if(ImGui::BeginTable("##compact_summary",2,ImGuiTableFlags_SizingStretchSame)) {
      for(int i=0;i<3;++i) {
        ImGui::TableNextColumn(); Description(labels[i]);
        ImGui::TableNextColumn(); ImGui::TextColored(Bright,"%s",values[i]);
      }
      ImGui::EndTable();
    }
    return;
  }
  if(ImGui::BeginTable("##mfg_summary_cards",3,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) {
    for(int i=0;i<3;++i) {
      ImGui::TableNextColumn(); ImGui::PushID(i);
      ImGui::BeginChild("summary",ImVec2(0,ImGui::GetTextLineHeight()*2.f+ImGui::GetStyle().ItemSpacing.y+ImGui::GetStyle().WindowPadding.y*2),ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding,
          ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoInputs);
      Centered(values[i],Bright); Centered(labels[i],Muted);
      ImGui::EndChild(); ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
}
inline void BackendSelector() {
#ifdef MFGUNLOCK_NATIVE_XEMFG
  static bool saveFailed{};
  static const auto startup=backend::ReadMode();
  const auto selected=backend::Read(); const auto mode=backend::ReadMode(); int choice=int(mode);
  Setting("##backend_selection","Frame generation method","NVIDIA DLSS-G: NVIDIA's generator.\n"
      "Intel XeMFG (CUDA): Intel's model on NVIDIA Tensor Cores, with Intel's presentation.\n"
      "XeMFG via DLSS-G (XMX): Intel's model in DLSS-G's presentation. The game still reports DLSS-G, but Intel's model generates the frames. Experimental; needs Intel's files in the game folder.\n"
      "Requires a game restart.",[&] {
    constexpr const char* kMethods[]={"DLSS-G","XeMFG","XMX"};
    if(Segmented("##backend",choice,kMethods,3))
      saveFailed=!backend::SelectMode(backend::Mode(choice));
  });
  if(saveFailed) Notice("Backend change was not saved. Check write access to the game folder.");
  if(selected==backend::Kind::Invalid) Notice("The saved backend preference is not valid; NVIDIA DLSS-G is used. Select a method to save a valid one.");
  if(backend::Effective(selected)!=backend::active || mode!=startup) Notice("Saved. Restart the game to switch frame generation.");
#endif
}
inline void RepositoryButton() {
  static bool failed{};
  if(ImGui::SmallButton("GitHub repository")) {
    failed=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",
        L"https://github.com/mavismmg/MFGAdaUnlock-RenoDx",nullptr,nullptr,SW_SHOWNORMAL))<=32;
  }
  Tooltip("Open the maintained MFGAdaUnlock repository in your default browser.");
  if(failed) Notice("Could not open the browser. Repository: github.com/mavismmg/MFGAdaUnlock-RenoDx");
}
// 1.8.1: a problem the player should act on, with its evidence one click away.
class ProblemCard {
public:
  ProblemCard(const char* id,const char* title,ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Border,color);
    ImGui::BeginChild(id,ImVec2(0,0),ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(color,"%s",title);
  }
  ~ProblemCard() { ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::Spacing(); }
};
inline std::string& SupportMessage() { static std::string message; return message; }
inline void SaveSupportFiles(const std::string& report) {
  const auto saved=support::Save(backend::Directory(),report);
  auto& message=SupportMessage();
  if(!saved.ok) { message="Could not save the support files. Check write access to the game folder."; return; }
  ShellExecuteW(nullptr,L"open",saved.folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
  message="Saved in "+saved.folder.filename().string()+" with "+std::to_string(saved.copied)
      +" log and settings file(s). These may include folder names from your PC; check them before posting publicly.";
}
inline void SupportResult() { if(!SupportMessage().empty()) Description(SupportMessage().c_str()); }
inline constexpr const char* kSaveSupportHelp="Copies the logs, the settings and this report into a new folder beside the game, then opens it.";
template<class Report> void SupportActions(Report report) {
  ButtonRow actions(2);
  if(actions("Save support files",kSaveSupportHelp)) SaveSupportFiles(report());
  if(actions("Copy report","Copies this report as text, to paste into a bug report.")) ImGui::SetClipboardText(report().c_str());
  SupportResult();
}
// After a session without a clean exit. Its logs may be overwritten by the next launch.
// Safe mode covers the DLSS-G backend's options only, so XeMFG does not offer it.
template<class Report> void SessionNotice(Report report,bool safeModeApplies=true) {
  if(!support::g_previous_unclean.load(std::memory_order_relaxed)) return;
  const uint32_t streak=support::g_unclean_streak.load(std::memory_order_relaxed);
  const bool offerSafe=safeModeApplies && streak>=support::kSafeModeOffer;
  ProblemCard card("##previous_session",offerSafe?"The game may have closed unexpectedly several times in a row"
                                                 :"The last session may have closed unexpectedly",Warning);
  ImGui::TextWrapped("If the game crashed, save the logs now and attach them to your report.");
  if(offerSafe) ImGui::TextWrapped("%u sessions in a row did not end cleanly. Safe mode starts the game once without "
      "the DLSS preset, XMX and the forced multiplier, to see whether one of them is the cause.",streak);
  ButtonRow actions(offerSafe?3:2);
  if(actions("Save support files",kSaveSupportHelp)) SaveSupportFiles(report());
  if(offerSafe && actions("Safe mode next start","Your settings are kept. Only the next start runs without these options.")) {
    SupportMessage()=support::RequestSafeMode(backend::Directory())
        ?"Safe mode is set for the next start. Close the game and start it again."
        :"Could not set safe mode. Check write access to the game folder.";
  }
  if(actions("Dismiss")) {
    support::g_previous_unclean.store(false,std::memory_order_relaxed);
    support::ResetStreak(backend::Directory());
    SupportMessage().clear();
  }
  SupportResult();
}
// While safe mode is on, every page says so: the options look set but do nothing.
inline void SafeModeNotice() {
  if(!support::g_safe_mode.load(std::memory_order_relaxed)) return;
  ProblemCard card("##safe_mode","Safe mode",Warning);
  ImGui::TextWrapped("This start runs without the DLSS preset, XMX and the forced multiplier. "
      "Your settings are kept and return on the next start.");
}
// Version left, repository link right: one footer for both backends.
// 1.8.1: this PC at a glance, on the General page. `sync` is null where it is not known.
inline void SystemCard(const char* sync,bool xmxSelected) {
  Section("System");
  Card card("##system_card");
  const auto& machine=system::ReadMachine();
  const auto& display=system::g_display;
  const bool scrgb=display.known && system::OutputIsScrgb(display.format);
  Readout("##system_output","Display output",display.known?system::OutputName(display.format,display.windowsHdr):"Waiting for the game",
      "The format the game is presenting in, read with the HDR mode of this monitor in Windows.",display.known?Accent:Muted);
  if(display.known) {
    const std::string resolution=std::to_string(display.width)+"x"+std::to_string(display.height)
        +(display.refreshHz?" @ "+std::to_string(display.refreshHz)+" Hz":"");
    Readout("##system_resolution","Resolution",resolution.c_str(),"The game's output and the monitor's refresh rate.",Accent);
  }
  Readout("##system_gpu","GPU",machine.gpu.c_str(),nullptr,Accent);
  Readout("##system_driver","Driver",machine.driver.c_str(),nullptr,Accent);
  Readout("##system_windows","Windows",machine.windows.c_str(),nullptr,Accent);
  if(sync) Readout("##system_sync","Sync",sync,nullptr,Accent);
  if(xmxSelected && scrgb) Notice("XMX has not been tested with scRGB output, and it has no conversion for it like XeMFG does.");
}
inline constexpr const char* kVersionLine="MFG Unlock | 1.8.4 by mavismmg also known as monet, ayase or woni";
// Rests on the bottom of the panel when the page leaves room; otherwise follows the page.
inline void Footer(const char* note=nullptr) {
  const char* version=kVersionLine;
  const auto& style=ImGui::GetStyle();
  const float button=ImGui::CalcTextSize("GitHub repository").x+style.FramePadding.x*2.f;
  const bool oneLine=ImGui::GetContentRegionAvail().x>ImGui::CalcTextSize(version).x+button+ImGui::GetFontSize();
  const float line=ImGui::GetTextLineHeightWithSpacing(),buttonRow=ImGui::GetFrameHeightWithSpacing();
  const float height=style.ItemSpacing.y*3.f+buttonRow+line*((oneLine?1.f:2.f)+(note?1.f:0.f));
  const float spare=ImGui::GetContentRegionAvail().y-height;
  if(spare>0.f) ImGui::SetCursorPosY(ImGui::GetCursorPosY()+spare);
  ImGui::Spacing(); ImGui::Separator();
  Description(version);
  if(oneLine) {
    ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x-button);
  }
  RepositoryButton();
  Description("Hover a setting to see what it does.");
  if(note) Description(note);
}
}
