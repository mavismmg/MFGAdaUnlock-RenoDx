// SPDX-License-Identifier: MIT
#include "../src/addons/mfgunlock/xemfg/profiles.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace mfgunlock::xemfg::profiles;
namespace { void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); } }
int main(int argc,char** argv) {
  try {
    Require(argc==2,"usage: xemfg_profiles_tests <embedded profiles.json>");
    std::ifstream file(argv[1],std::ios::binary); std::stringstream text; text<<file.rdbuf();
    Rules embedded; std::string error;
    Require(Parse(text.str(),"embedded",embedded,error) && error.empty(),"shipped profiles.json does not parse");
    for(const auto& rule:embedded) Require(!rule.id.empty() && !rule.evidence.empty(),"every shipped rule needs an id and its evidence");
    // Defaults apply everywhere; module rules only while that module is mapped.
    Context game{L"Game.exe",{}};
    auto policy=Resolve(embedded,game);
    Require(Contains(policy.conflictingModules,L"mfgunlocker.asi"),"default conflicts missing");
    Require(!policy.retainNativeOutput.value_or(false),"overlay policy applied without the overlay");
    game.modules={L"GameOverlayRenderer64.dll"};
    policy=Resolve(embedded,game);
    Require(policy.retainNativeOutput.value_or(false) && Contains(policy.observerModules,L"gameoverlayrenderer64.dll"),"overlay rule not matched case-insensitively");
    // Observed conventions moved from code (plan 3.4); unknown games rely on learning.
    Require(Resolve(embedded,Context{L"tlou-ii.exe",{}}).depthInverted==false && Resolve(embedded,Context{L"TLOU-II-L.EXE",{}}).depthInverted==false
        && Resolve(embedded,Context{L"AlanWake2.exe",{}}).depthInverted==false
        && Resolve(embedded,Context{L"Darktide.exe",{}}).depthInverted==false,"observed conventional depth profiles");
    Require(Resolve(embedded,Context{L"DS2.exe",{}}).motionJittered==true,"observed jitter profile");
    // AC Black Flag Resynced loses the device on the first Present of a reused
    // retained output; the executable rule wins over the overlay rule.
    Require(!Resolve(embedded,Context{L"ACBlackFlag.exe",{L"gameoverlayrenderer64.dll"}}).retainNativeOutput.value_or(true),
        "AC Black Flag kept native output retention under the Steam overlay");
    Require(!Resolve(embedded,Context{L"re9.exe",{}}).depthInverted && !Resolve(embedded,Context{L"re9.exe",{}}).motionJittered,"unobserved game received a convention");
    // A missing DLSS-G plugin is accepted for the one game with evidence, not by default.
    Require(Resolve(embedded,Context{L"Ghost-Win64-Shipping.exe",{}}).acceptMissingDlssgPlugin==true,"Titans of the Tide lost its profile");
    Require(!Resolve(embedded,Context{L"Game.exe",{L"GameOverlayRenderer64.dll"}}).acceptMissingDlssgPlugin.value_or(false),"another game accepts a missing DLSS-G plugin");
    Require(AcceptMissingDlssgPlugin(true,true,true,false),"the evidenced case was refused");
    Require(!AcceptMissingDlssgPlugin(false,true,true,false),"accepted without a profile");
    Require(!AcceptMissingDlssgPlugin(true,false,true,false),"accepted with a DLSS-G plugin present");
    Require(!AcceptMissingDlssgPlugin(true,true,false,false),"accepted before the plugins loaded");
    Require(!AcceptMissingDlssgPlugin(true,true,true,true),"took presentation from the game's own XeFG");
    // The game-folder file overrides scalar fields and extends lists.
    Rules local;
    Require(Parse(R"({"schema":1,"profiles":[{"id":"local","match":{"executable":"game.exe"},"evidence":"user",
      "retain_native_output":false,"observer_modules":["Other.dll"],"depth_inverted":false}]})","override",local,error),"valid override refused");
    auto merged=embedded; merged.insert(merged.end(),local.begin(),local.end());
    policy=Resolve(merged,game);
    Require(!policy.retainNativeOutput.value_or(true) && Contains(policy.observerModules,L"other.dll")
        && Contains(policy.observerModules,L"gameoverlayrenderer64.dll") && policy.depthInverted==false,"override did not merge");
    Require(Contains(policy.applied,"local") && policy.sources.back()=="override","applied profile not recorded");
    // Invalid files are refused as a whole, never partially applied.
    Rules broken;
    Require(!Parse("{\"schema\":1,\"profiles\":[{\"id\":}]}","override",broken,error) && broken.empty() && !error.empty(),"invalid JSON accepted");
    Require(!Parse(R"({"schema":2,"profiles":[]})","override",broken,error) && broken.empty(),"unknown schema accepted");
    Require(!Parse(R"({"schema":1,"profiles":[{"id":"x","match":{},"evidence":"e","enable_unverified_path":true}]})","override",broken,error)
        && broken.empty(),"unknown policy key accepted");
    std::cout<<"PASS: shipped profiles, defaults, module and executable matching, override merge and refusal of invalid files\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
