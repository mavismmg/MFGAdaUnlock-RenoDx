// SPDX-License-Identifier: MIT
// Shared player-facing presentation. Styles are scoped to this addon only.
#pragma once
#include <deps/imgui/imgui.h>
#ifndef MFGUNLOCK_UI_PREVIEW
#include <include/reshade.hpp>
#endif
#include "backend_selection.hpp"
#include <algorithm>
#include <cfloat>
#include <string>
#include <shellapi.h>

namespace mfgunlock::ui {
inline constexpr ImVec4 Accent{0.39f,0.84f,0.71f,1.f};
inline constexpr ImVec4 Muted{0.65f,0.72f,0.82f,1.f};
inline constexpr ImVec4 Warning{1.f,0.77f,0.35f,1.f};
inline constexpr ImVec4 Bright{.93f,.95f,.98f,1.f};
struct Theme {
  Theme() {
    const float scale=ImGui::GetFontSize()/16.f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.135f,.155f,.19f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.10f,.12f,.15f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.19f,.23f,.28f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.22f,.29f,.32f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,Accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,Accent);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.21f,.28f,.35f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.25f,.36f,.42f,1));
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.22f,.29f,.37f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.19f,.24f,.30f,1));
    ImGui::PushStyleColor(ImGuiCol_Tab,ImVec4(.12f,.14f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,ImVec4(.23f,.31f,.40f,1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,Muted);
    ImGui::PushStyleColor(ImGuiCol_Separator,ImVec4(.22f,.25f,.30f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,4.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,6.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(8.f*scale,4.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8.f*scale,6.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(4.f*scale,4.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10.f*scale,8.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding,4.f*scale);
  }
  ~Theme() { ImGui::PopStyleVar(7); ImGui::PopStyleColor(14); }
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
    visible=ImGui::BeginChild(id,ImVec2(0,0),ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding,
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
// Equal-width tabs that fill the row. Measure before BeginTabBar.
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
  ImGui::SetNextItemWidth(width); return ImGui::BeginTabItem(text.c_str());
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
  ImGui::TextUnformatted("MFG Unlock");
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
        ImGui::TableNextColumn(); ImGui::TextColored(i==0?Accent:Bright,"%s",values[i]);
      }
      ImGui::EndTable();
    }
    return;
  }
  if(ImGui::BeginTable("##mfg_summary_cards",3,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) {
    for(int i=0;i<3;++i) {
      ImGui::TableNextColumn(); ImGui::PushID(i);
      ImGui::BeginChild("summary",ImVec2(0,ImGui::GetTextLineHeight()*2.f+ImGui::GetStyle().ItemSpacing.y+ImGui::GetStyle().WindowPadding.y*2),ImGuiChildFlags_AlwaysUseWindowPadding,
          ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoInputs);
      Centered(values[i],i==0?Accent:Bright); Centered(labels[i],Muted);
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
      "XeFG via DLSS-G (XMX): Intel's model in DLSS-G's presentation. The game still reports DLSS-G, but Intel's model generates the frames. Experimental; needs Intel's files in the game folder.\n"
      "Requires a game restart.",[&] {
    if(ImGui::Combo("##backend",&choice,"NVIDIA DLSS-G\0Intel XeMFG (CUDA)\0XeFG via DLSS-G (XMX)\0"))
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
// Version left, repository link right: one footer for both backends.
inline void Footer(const char* version) {
  ImGui::Spacing(); ImGui::Separator();
  const auto& style=ImGui::GetStyle();
  const float button=ImGui::CalcTextSize("GitHub repository").x+style.FramePadding.x*2.f;
  const bool oneLine=ImGui::GetContentRegionAvail().x>ImGui::CalcTextSize(version).x+button+ImGui::GetFontSize();
  Description(version);
  if(oneLine) {
    ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x-button);
  }
  RepositoryButton();
  Description("Hover a setting to see what it does.");
}
}
