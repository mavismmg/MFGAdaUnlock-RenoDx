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
#include "i18n/strings.hpp"
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <string>
#include <shellapi.h>

namespace mfgunlock::ui {
// 1.10.0: the method once called XMX (a product name, never translated).
inline constexpr const char* kXemfgDlssgName = "XeMFG DLSS-G";
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
    ImGui::TableSetupColumn(T(Str::kSetting_754b),ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(T(Str::kControl_bf3e),ImGuiTableColumnFlags_WidthStretch);
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
  // 1.10.0: each option as wide as its label plus an even share of the rest ("XeMFG DLSS-G" did not fit a third).
  const float available=(std::max)(1.f,ImGui::GetContentRegionAvail().x),pad=ImGui::GetStyle().FramePadding.x*2.f;
  float widths[16]{},needed=0.f;
  for(int i=0;i<count && i<16;++i) { widths[i]=ImGui::CalcTextSize(labels[i],nullptr,true).x+pad; needed+=widths[i]; }
  const float extra=(available-needed)/float(count),scale=needed>available?available/needed:1.f;
  float offsets[17]{};
  for(int i=0;i<count && i<16;++i) { widths[i]=extra>0.f?widths[i]+extra:widths[i]*scale; offsets[i+1]=offsets[i]+widths[i]; }
  const float rounding=ImGui::GetStyle().FrameRounding,height=ImGui::GetFrameHeight();
  const auto start=ImGui::GetCursorScreenPos();
  const ImVec2 end(start.x+offsets[(std::min)(count,16)],start.y+height);
  auto* draw=ImGui::GetWindowDrawList();
  draw->AddRectFilled(start,end,ImGui::GetColorU32(ImGuiCol_FrameBg),rounding);
  bool changed=false;
  for(int i=0;i<count;++i) {
    if(i) ImGui::SameLine();
    const bool on=i==value;
    // The row's outer corners stay round, so the selection is drawn here and not by the button.
    if(on) {
      const ImVec2 min(start.x+offsets[i],start.y);
      const ImDrawFlags corners=count==1?ImDrawFlags_RoundCornersAll:i==0?ImDrawFlags_RoundCornersLeft:
          i==count-1?ImDrawFlags_RoundCornersRight:ImDrawFlags_RoundCornersNone;
      draw->AddRectFilled(min,ImVec2(min.x+widths[i],end.y),ImGui::GetColorU32(Selected),rounding,corners);
    }
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,on?ImVec4(0,0,0,0):ImVec4(1,1,1,.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,on?ImVec4(0,0,0,0):ImVec4(1,1,1,.12f));
    ImGui::PushStyleColor(ImGuiCol_Text,on?Bright:Muted);
    if(ImGui::Button(labels[i],ImVec2(widths[i],height)) && !on) { value=i; changed=true; }
    ImGui::PopStyleColor(4);
  }
  ImGui::PopStyleVar(2);
  ImGui::EndGroup();
  draw->AddRect(start,end,ImGui::GetColorU32(ImGuiCol_Border),rounding,0,BorderWidth());
  ImGui::PopID();
  return changed;
}
// 1.10.0 (panel reformulation): an on/off switch instead of an On/Off list. One item, so the
// row's tooltip covers it.
inline bool Toggle(const char* id,bool& value) {
  const float height=ImGui::GetFrameHeight()*.8f,width=height*1.85f;
  const float y=ImGui::GetCursorPosY();
  ImGui::SetCursorPosY(y+(ImGui::GetFrameHeight()-height)*.5f);
  const auto at=ImGui::GetCursorScreenPos();
  const bool pressed=ImGui::InvisibleButton(id,ImVec2(width,height));
  if(pressed) value=!value;
  auto* draw=ImGui::GetWindowDrawList();
  const bool hovered=ImGui::IsItemHovered();
  const ImVec4 track=value?Selected:ControlBg;
  draw->AddRectFilled(at,ImVec2(at.x+width,at.y+height),ImGui::GetColorU32(track),height*.5f);
  draw->AddRect(at,ImVec2(at.x+width,at.y+height),ImGui::GetColorU32(hovered?SelectedLine:Line),height*.5f,0,BorderWidth()*.75f);
  const float radius=height*.5f-3.f*Scale();
  const float cx=value?at.x+width-height*.5f:at.x+height*.5f;
  draw->AddCircleFilled(ImVec2(cx,at.y+height*.5f),radius,ImGui::GetColorU32(value?Bright:ImVec4(.42f,.44f,.47f,1)));
  return pressed;
}
// A status under the control it belongs to: a dot (teal active, amber waiting or a problem,
// grey off) and the text, in a quiet box the width of the row.
inline void StatusChip(const char* text,ImVec4 dot=Muted) {
  if(!text || !text[0]) return;
  const float pad=6.f*Scale(),dotSize=ImGui::GetFontSize()*.42f;
  const auto start=ImGui::GetCursorScreenPos();
  const float width=ImGui::GetContentRegionAvail().x;
  auto* draw=ImGui::GetWindowDrawList();
  // Measured first and drawn under the text, without channels: tables already split this list.
  const float textX=pad*3.f+dotSize,wrap=(std::max)(1.f,width-textX-pad);
  const float bottom=start.y+pad*2.f+ImGui::CalcTextSize(text,nullptr,false,wrap).y;
  draw->AddRectFilled(start,ImVec2(start.x+width,bottom),ImGui::GetColorU32(ImVec4(.14f,.145f,.155f,1)),5.f*Scale());
  draw->AddRect(start,ImVec2(start.x+width,bottom),ImGui::GetColorU32(ImVec4(.21f,.22f,.235f,1)),5.f*Scale());
  draw->AddCircleFilled(ImVec2(start.x+pad*2.f+dotSize*.5f,start.y+pad+ImGui::GetTextLineHeight()*.5f),dotSize*.5f,ImGui::GetColorU32(dot));
  ImGui::SetCursorScreenPos(ImVec2(start.x+textX,start.y+pad));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+wrap);
  ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.84f,.86f,.89f,1)); ImGui::TextUnformatted(text); ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::SetCursorScreenPos(ImVec2(start.x,bottom));
  ImGui::Dummy(ImVec2(width,0.f));
}
// A collapsible group, drawn like ReShade's headers ("System check"), with an optional amber tag
// after the title. Opens once by `openByDefault`; afterwards ImGui keeps what the player chose.
inline bool SectionHeader(const char* title,bool openByDefault,const char* tag=nullptr) {
  ImGui::SetNextItemOpen(openByDefault,ImGuiCond_Once);
  const bool open=ImGui::CollapsingHeader(title);
  if(tag && tag[0]) {
    const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
    const auto& style=ImGui::GetStyle();
    const float x=min.x+style.FramePadding.x*3.f+ImGui::GetFontSize()+ImGui::CalcTextSize(title,nullptr,true).x+ImGui::GetFontSize()*.9f;
    const auto size=ImGui::CalcTextSize(tag);
    const float pad=4.f*Scale(),y=(min.y+max.y-size.y)*.5f;
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRect(ImVec2(x-pad,y-pad*.5f),ImVec2(x+size.x+pad,y+size.y+pad*.5f),ImGui::GetColorU32(ImVec4(.42f,.33f,.15f,1)),3.f*Scale());
    draw->AddText(ImVec2(x,y),ImGui::GetColorU32(Warning),tag);
  }
  return open;
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
  const char* labels[]={T(Str::kMultiplier_bc06),T(Str::kBackend_b9ef),T(Str::kRenderer_391e)};
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
  Setting("##backend_selection",T(Str::kFrameGenerationMethod_fd2d),T(Str::kNvidiaDlssGNvidiaS_572e),[&] {
    const char* kMethods[] = {"DLSS-G","XeMFG",kXemfgDlssgName};
    if(Segmented("##backend",choice,kMethods,3))
      saveFailed=!backend::SelectMode(backend::Mode(choice));
  });
  if(saveFailed) Notice(T(Str::kBackendChangeWasNotSaved_1275));
  if(selected==backend::Kind::Invalid) Notice(T(Str::kTheSavedBackendPreferenceIs_f36b));
  if(backend::Effective(selected)!=backend::active || mode!=startup) Notice(T(Str::kSavedRestartTheGameTo_1fe6));
#endif
}
inline void RepositoryButton() {
  static bool failed{};
  if(ImGui::SmallButton(T(Str::kGithubRepository_31c6))) {
    failed=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",
        L"https://github.com/mavismmg/MFGAdaUnlock-RenoDx",nullptr,nullptr,SW_SHOWNORMAL))<=32;
  }
  Tooltip(T(Str::kOpenTheMaintainedMfgadaunlockRepository_71c5));
  if(failed) Notice(T(Str::kCouldNotOpenTheBrowser_1362));
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
  if(!saved.ok) { message=T(Str::kCouldNotSaveTheSupport_c8bc); return; }
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
  ProblemCard card("##previous_session",offerSafe?T(Str::kTheGameMayHaveClosed_2f77)
                                                 :T(Str::kTheLastSessionMayHave_8e69),Warning);
  ImGui::TextWrapped(T(Str::kIfTheGameCrashedSave_aa60));
  if(offerSafe) ImGui::TextWrapped(T(Str::kUSessionsInARow_c7e5),streak);
  ButtonRow actions(offerSafe?3:2);
  if(actions("Save support files",kSaveSupportHelp)) SaveSupportFiles(report());
  if(offerSafe && actions(T(Str::kSafeModeNextStart_8548),T(Str::kYourSettingsAreKeptOnly_ee82))) {
    SupportMessage()=support::RequestSafeMode(backend::Directory())
        ?T(Str::kSafeModeIsSetFor_72f7)
        :T(Str::kCouldNotSetSafeMode_85b8);
  }
  if(actions(T(Str::kDismiss_15f3))) {
    support::g_previous_unclean.store(false,std::memory_order_relaxed);
    support::ResetStreak(backend::Directory());
    SupportMessage().clear();
  }
  SupportResult();
}
// While safe mode is on, every page says so: the options look set but do nothing.
inline void SafeModeNotice() {
  if(!support::g_safe_mode.load(std::memory_order_relaxed)) return;
  ProblemCard card("##safe_mode",T(Str::kSafeMode_691b),Warning);
  ImGui::TextWrapped(T(Str::kThisStartRunsWithoutThe_d4e2));
}
// Version left, repository link right: one footer for both backends.
// 1.8.1: this PC at a glance, on the General page. `sync` is null where it is not known.
inline void SystemCard(const char* sync,bool xmxSelected) {
  // 1.10.0 (panel reformulation): collapsed by default, like the chain and the system check.
  if(!SectionHeader(T(Str::kSystem_84bc),false)) return;
  Card card("##system_card");
  const auto& machine=system::ReadMachine();
  const auto& display=system::g_display;
  const bool scrgb=display.known && system::OutputIsScrgb(display.format);
  Readout("##system_output",T(Str::kDisplayOutput_bcc2),display.known?system::OutputName(display.format,display.windowsHdr):T(Str::kWaitingForTheGame_18de),
      T(Str::kTheFormatTheGameIs_bb8f),display.known?Accent:Muted);
  if(display.known) {
    const std::string resolution=std::to_string(display.width)+"x"+std::to_string(display.height)
        +(display.refreshHz?" @ "+std::to_string(display.refreshHz)+" Hz":"");
    Readout("##system_resolution",T(Str::kResolution_18af),resolution.c_str(),T(Str::kTheGameSOutputAnd_ec20),Accent);
  }
  Readout("##system_gpu","GPU",machine.gpu.c_str(),nullptr,Accent);
  Readout("##system_driver",T(Str::kDriver_52ff),machine.driver.c_str(),nullptr,Accent);
  Readout("##system_windows",T(Str::kWindows_028a),machine.windows.c_str(),nullptr,Accent);
  if(sync) Readout("##system_sync",T(Str::kSync_1bba),sync,nullptr,Accent);
  if(xmxSelected && scrgb) Notice(T(Str::kXmxHasNotBeenTested_6f96));
}
inline constexpr const char* kVersionLine="MFG Unlock | 1.10.1 by mavismmg also known as monet, ayase or woni";
// Rests on the bottom of the panel when the page leaves room; otherwise follows the page.
// 1.10.0 (plan 5.2/5.3): the panel's language. Saved in MFGAdaUnlock.ini under
// [Panel] Language; auto follows ReShade's language, then Windows'.
inline std::filesystem::path LanguageIni() { return backend::Directory()/L"MFGAdaUnlock.ini"; }
inline std::string ReadSavedLanguage() {
  wchar_t value[32]{};
  GetPrivateProfileStringW(L"Panel",L"Language",L"auto",value,32,LanguageIni().c_str());
  char narrow[32]{};
  WideCharToMultiByte(CP_UTF8,0,value,-1,narrow,sizeof(narrow)-1,nullptr,nullptr);
  return narrow;
}
inline bool SaveLanguage(const char* code) {
  wchar_t wide[32]{};
  MultiByteToWideChar(CP_UTF8,0,code,-1,wide,31);
  return WritePrivateProfileStringW(L"Panel",L"Language",wide,LanguageIni().c_str())!=FALSE;
}
inline std::string WindowsLanguage() {
  wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
  if(!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(),SORT_DEFAULT),name,LOCALE_NAME_MAX_LENGTH,0)) return {};
  char narrow[LOCALE_NAME_MAX_LENGTH]{};
  WideCharToMultiByte(CP_UTF8,0,name,-1,narrow,sizeof(narrow)-1,nullptr,nullptr);
  return narrow;
}
#ifndef MFGUNLOCK_UI_PREVIEW
inline std::string ReShadeLanguage() {
  char value[32]=""; size_t size=sizeof(value);
  return reshade::get_config_value(nullptr,"OVERLAY","Language",value,&size)?std::string(value):std::string();
}
// ReShade builds its fonts after add-ons load, so a font set here at start
// applies in this session; set later, it applies from the next start.
inline bool g_language_font_changed=false;
inline void ApplyLanguageFont(i18n::Language language) {
  char current[MAX_PATH]=""; size_t size=sizeof(current);
  if(!reshade::get_config_value(nullptr,"STYLE","Font",current,&size)) current[0]='\0';
  const char* font=i18n::FontToSet(language,current,support::kPanelFontValue,[](const char* path) {
    return GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES;
  });
  if(font==nullptr || _stricmp(font,current)==0) return;
  reshade::set_config_value(nullptr,"STYLE","Font",font);
  g_language_font_changed=true;
  reshade::log::message(reshade::log::level::info,(std::string("mfgunlock: panel language needs ")+font+" as ReShade's font; set.").c_str());
}
// Once, when the addon starts (both backends).
inline void InitLanguage() {
  const auto language=i18n::Resolve(ReadSavedLanguage().c_str(),ReShadeLanguage().c_str(),WindowsLanguage().c_str());
  i18n::Select(language);
  ApplyLanguageFont(language);
}
#endif
// Each panel frame, before any text: the chosen language when the font can
// draw it, otherwise English for this frame (no boxes instead of letters).
inline bool g_language_glyphs_missing=false;
inline void BeginLanguageFrame() {
  const auto selected=i18n::Selected();
  ImFont* font=ImGui::GetFont();
  g_language_glyphs_missing=i18n::NeedsCjkFont(selected) && font!=nullptr &&
      !font->IsGlyphInFont(static_cast<ImWchar>(i18n::ProbeGlyph(selected)));
  i18n::UseForThisFrame(g_language_glyphs_missing?i18n::Language::kEnglish:selected);
}
// The selector, in the footer of both panels.
inline void LanguageSelector() {
  const auto selected=i18n::Selected();
  const auto& current=i18n::kInfo[static_cast<int>(selected)];
  // The names are each in their own language: only drawable ones are listed
  // when the font lacks CJK glyphs, so the list never shows boxes.
  ImFont* font=ImGui::GetFont();
  const auto drawable=[&](const i18n::Info& info) {
    return !i18n::NeedsCjkFont(info.language) || font==nullptr ||
           font->IsGlyphInFont(static_cast<ImWchar>(i18n::ProbeGlyph(info.language)));
  };
  // 1.10.0: no label beside it; the tooltip names it in English too, so a player who picked a
  // language by mistake still finds it.
  ImGui::SetNextItemWidth(ImGui::GetFontSize()*10.f);
  if(ImGui::BeginCombo("##panel_language",drawable(current)?current.name:current.code)) {
    for(const auto& info:i18n::kInfo) {
      std::string label=drawable(info)?info.name:info.code;
      if(info.machine_translated) label+=" *";
      if(ImGui::Selectable(label.c_str(),info.language==selected) && info.language!=selected) {
        i18n::Select(info.language);
        SaveLanguage(info.code);
#ifndef MFGUNLOCK_UI_PREVIEW
        ApplyLanguageFont(info.language);
#endif
      }
    }
    ImGui::EndCombo();
  }
  if(ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize()*34.f);
    if(selected==i18n::Language::kEnglish) ImGui::TextUnformatted(T(Str::kLanguageLabel_lng));
    else {
      // "Language" stays in English next to the translation, for whoever needs to switch back.
      constexpr const char* kEnglishName=" (Language)";
      ImGui::Text("%s%s",T(Str::kLanguageLabel_lng),kEnglishName);
    }
    ImGui::TextUnformatted(T(Str::kLanguageOfThisPanel_lang));
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
  }
  // Shown in English: this note appears exactly when the font cannot draw the
  // chosen language, and T() then answers in English anyway.
  if(g_language_glyphs_missing) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s",T(Str::kRestartTheGameToLoadFont_lang));
  }
}
// 1.10.0 (panel reformulation): the add-on at the left (name and version, the author below,
// smaller), its actions at the right (language, GitHub), one hint line under both.
inline constexpr const char* kVersion="1.10.1";
inline constexpr const char* kAuthorLine="by mavismmg \xc2\xb7 monet \xc2\xb7 ayase \xc2\xb7 woni";
inline void Footer(const char* /*note: one hint line now covers restarts*/=nullptr) {
  const auto& style=ImGui::GetStyle();
  const float line=ImGui::GetTextLineHeightWithSpacing(),buttonRow=ImGui::GetFrameHeightWithSpacing();
  const float github=ImGui::CalcTextSize("GitHub").x+style.FramePadding.x*2.f;
  const float actions=ImGui::GetFontSize()*10.f+style.ItemSpacing.x+github;
  const float width=ImGui::GetContentRegionAvail().x;
  // The language and GitHub sit beside the name and author line only when both fit; otherwise below them.
  const float block=(std::max)(ImGui::CalcTextSize(kName).x+style.ItemSpacing.x+ImGui::CalcTextSize(kVersion).x,
                               ImGui::CalcTextSize(kAuthorLine).x);
  const bool beside=width>actions+block+ImGui::GetFontSize()*2.f;
  // The hint's height as StatusChip draws it (it wraps in a narrow panel).
  const float pad=6.f*Scale(),chipText=pad*3.f+ImGui::GetFontSize()*.42f;
  const float chip=pad*2.f+ImGui::CalcTextSize(T(Str::kFooterHint_p12),nullptr,false,(std::max)(1.f,width-chipText-pad)).y;
  const float height=style.ItemSpacing.y*5.f+line*2.f+(beside?0.f:buttonRow)+chip+4.f*Scale();
  const float spare=ImGui::GetContentRegionAvail().y-height;
  if(spare>0.f) ImGui::SetCursorPosY(ImGui::GetCursorPosY()+spare);
  ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
  const float right=ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x;
  const float top=ImGui::GetCursorPosY();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(kName); ImGui::SameLine(); ImGui::TextColored(Muted,"%s",kVersion);
  ImGui::TextColored(Muted,"%s",kAuthorLine);
  ImGui::EndGroup();
  const float blockBottom=ImGui::GetCursorPosY();
  if(beside) {
    ImGui::SetCursorPos(ImVec2(right-actions,top+(blockBottom-top-ImGui::GetFrameHeight()-style.ItemSpacing.y)*.5f));
  }
  LanguageSelector();
  ImGui::SameLine();
  static bool failed{};
  if(ImGui::Button("GitHub")) {
    failed=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",
        L"https://github.com/mavismmg/MFGAdaUnlock-RenoDx",nullptr,nullptr,SW_SHOWNORMAL))<=32;
  }
  Tooltip(T(Str::kOpenTheMaintainedMfgadaunlockRepository_71c5));
  ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY(),blockBottom));
  if(failed) Notice(T(Str::kCouldNotOpenTheBrowser_1362));
  StatusChip(T(Str::kFooterHint_p12),Muted);
}
}
