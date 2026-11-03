// SPDX-License-Identifier: MIT
#pragma once
// Declarative compatibility profiles. A profile only adjusts policy: it never
// enables a path whose contract is not implemented and verified. The shipped
// profiles.json is embedded; MFGAdaUnlock.profiles.json in the game folder is
// applied after it. An invalid file is refused as a whole.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>
namespace mfgunlock::xemfg::profiles {
inline std::wstring Lower(std::wstring text) { std::transform(text.begin(),text.end(),text.begin(),[](wchar_t c){return wchar_t(std::towlower(c));}); return text; }
inline std::wstring Wide(const std::string& text) { return std::wstring(text.begin(),text.end()); } // names are ASCII
struct Rule {
  std::string id,evidence,source;
  std::wstring executable,module; // lower case; empty matches everything
  std::optional<bool> retainNativeOutput,depthInverted,motionJittered,acceptMissingDlssgPlugin;
  std::vector<std::wstring> observerModules,conflictingModules;
  std::vector<std::string> knownBadReframework;
};
using Rules=std::vector<Rule>;
struct Context { std::wstring executable; std::vector<std::wstring> modules; };
struct Policy {
  std::optional<bool> retainNativeOutput,depthInverted,motionJittered,acceptMissingDlssgPlugin;
  std::vector<std::wstring> observerModules,conflictingModules;
  std::vector<std::string> knownBadReframework,applied,sources;
};
// A Streamline with its plugins loaded and no DLSS-G among them has nothing to
// isolate from presentation. Only a profile opts in: every other game keeps the
// refusal. The error alone is not enough, since before the plugins load it could
// still change, and a game presenting through its own XeFG keeps its presentation.
constexpr bool AcceptMissingDlssgPlugin(bool profileAllows,bool featureMissing,bool pluginsLoaded,bool gameOwnsGeneration) {
  return profileAllows && featureMissing && pluginsLoaded && !gameOwnsGeneration;
}
inline bool Contains(const std::vector<std::wstring>& list,const std::wstring& value) {
  const auto key=Lower(value); return std::find(list.begin(),list.end(),key)!=list.end();
}
inline bool Contains(const std::vector<std::string>& list,const std::string& value) {
  return std::find(list.begin(),list.end(),value)!=list.end();
}
inline bool Parse(const std::string& text,const std::string& source,Rules& out,std::string& error) {
  out.clear(); error.clear();
  try {
    const auto root=nlohmann::json::parse(text);
    if(root.value("schema",0)!=1 || !root.contains("profiles") || !root["profiles"].is_array()) { error="unsupported schema"; return false; }
    static const char* known[]={"id","match","evidence","retain_native_output","depth_inverted","motion_jittered",
        "observer_modules","conflicting_modules","known_bad_reframework_sha256","accept_missing_dlssg_plugin"};
    Rules rules;
    for(const auto& item:root["profiles"]) {
      for(const auto& [key,value]:item.items())
        if(std::find_if(std::begin(known),std::end(known),[&](const char* k){return key==k;})==std::end(known)) {
          error="unknown key "+key; return false;
        }
      Rule rule; rule.source=source;
      rule.id=item.at("id").get<std::string>(); rule.evidence=item.at("evidence").get<std::string>();
      if(rule.id.empty() || rule.evidence.empty()) { error="profile without id or evidence"; return false; }
      const auto& match=item.at("match");
      if(match.contains("executable")) rule.executable=Lower(Wide(match["executable"].get<std::string>()));
      if(match.contains("module")) rule.module=Lower(Wide(match["module"].get<std::string>()));
      if(item.contains("retain_native_output")) rule.retainNativeOutput=item["retain_native_output"].get<bool>();
      if(item.contains("depth_inverted")) rule.depthInverted=item["depth_inverted"].get<bool>();
      if(item.contains("motion_jittered")) rule.motionJittered=item["motion_jittered"].get<bool>();
      if(item.contains("accept_missing_dlssg_plugin")) rule.acceptMissingDlssgPlugin=item["accept_missing_dlssg_plugin"].get<bool>();
      for(const auto& m:item.value("observer_modules",nlohmann::json::array())) rule.observerModules.push_back(Lower(Wide(m.get<std::string>())));
      for(const auto& m:item.value("conflicting_modules",nlohmann::json::array())) rule.conflictingModules.push_back(Lower(Wide(m.get<std::string>())));
      for(const auto& h:item.value("known_bad_reframework_sha256",nlohmann::json::array())) rule.knownBadReframework.push_back(h.get<std::string>());
      rules.push_back(std::move(rule));
    }
    out=std::move(rules); return true;
  } catch(const std::exception& e) { error=e.what(); return false; }
}
// Rules apply in order; a later rule overrides scalar fields and extends lists.
inline Policy Resolve(const Rules& rules,const Context& context) {
  Policy policy;
  const auto exe=Lower(context.executable);
  std::vector<std::wstring> modules; for(const auto& m:context.modules) modules.push_back(Lower(m));
  const auto add=[](auto& to,const auto& from) { for(const auto& v:from) if(std::find(to.begin(),to.end(),v)==to.end()) to.push_back(v); };
  for(const auto& rule:rules) {
    if(!rule.executable.empty() && rule.executable!=exe) continue;
    if(!rule.module.empty() && std::find(modules.begin(),modules.end(),rule.module)==modules.end()) continue;
    if(rule.retainNativeOutput) policy.retainNativeOutput=rule.retainNativeOutput;
    if(rule.depthInverted) policy.depthInverted=rule.depthInverted;
    if(rule.motionJittered) policy.motionJittered=rule.motionJittered;
    if(rule.acceptMissingDlssgPlugin) policy.acceptMissingDlssgPlugin=rule.acceptMissingDlssgPlugin;
    add(policy.observerModules,rule.observerModules); add(policy.conflictingModules,rule.conflictingModules);
    add(policy.knownBadReframework,rule.knownBadReframework);
    policy.applied.push_back(rule.id); policy.sources.push_back(rule.source);
  }
  return policy;
}
}
