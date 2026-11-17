// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (plan XMX/Vulkan/VRAM, item 4): NVIDIA's DLSS frame generation in a
// game that integrates Streamline for DLSS Super Resolution but not for frame
// generation. [DLSSG] ForceFrameGeneration=1, off by default.
//
// Such a game already gives Streamline what DLSS-G reads: depth and motion
// vector tags and the per-frame camera constants (slSetConstants serves every
// feature). So nothing is invented here:
//   1. slInit: when the game asks for DLSS SR and not DLSS-G, DLSS-G is added
//      to the features Streamline loads (with Reflex and PCL, which DLSS-G
//      needs, if they are missing). A game that asks for DLSS-G is never
//      touched, so its own frame generation (and XMX in it) is unchanged.
//   2. Each slSetConstants: while the game itself never called
//      slDLSSGSetOptions, frame generation is turned on for that viewport at
//      the panel's multiplier, through the addon's own SetOptions path.
//   3. Without the game's Reflex (it never asked for slReflexSetOptions):
//      DLSS-G presents and paces through Reflex, so since 1.10.1 (plan item 2,
//      user-approved) the addon turns Reflex on and marks the game's own frame
//      tokens (framecount.hpp, reflexdrive). Until it can (it needs the game's
//      Present on Streamline's proxy) DLSS-G stays off and the panel says so.
// With the HUD-less image missing, DLSS-G interpolates the HUD, as in any game
// that does not tag one. Games that use NGX without Streamline are out of scope.
// Independent of JavieEm's GPL "force FG": only Streamline's public API.
#include <sl.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace mfgunlock::forcefg {

// 1.10.1 (plan item 5): the project id Add frame generation's own Streamline passes to slInit
// (sl_host.hpp, xemfg/sl_vulkan.cpp). Its slInit is not the game's: the Metro Exodus report
// read it as "the game asks for DLSS frame generation itself".
inline constexpr const char* kHostProjectId = "3c5b0a51-6f2e-4b8d-9d17-4f0b6e2a8c19";
inline bool IsHostInit(const sl::Preferences& pref) {
  return pref.structType == sl::Preferences::s_structType && pref.projectId != nullptr &&
         std::strcmp(pref.projectId, kHostProjectId) == 0;
}

enum class Decision : uint8_t { kOff, kGameHasFrameGeneration, kNoSuperResolution, kUnknownAbi, kAdded, kAllFeatures };

constexpr const char* DecisionText(Decision decision) {
  switch (decision) {
    case Decision::kOff: return "off";
    case Decision::kGameHasFrameGeneration: return "not needed: the game asks for DLSS frame generation itself";
    case Decision::kNoSuperResolution: return "not available: the game does not ask Streamline for DLSS Super Resolution";
    case Decision::kUnknownAbi: return "not applied: unknown slInit preferences layout";
    case Decision::kAllFeatures: return "not applied: the game loads every Streamline feature, so it may have its own frame generation";
    default: return "DLSS frame generation added to the game's Streamline features";
  }
}

struct Plan {
  Decision decision = Decision::kOff;
  std::vector<sl::Feature> features;  // the list to load when decision is kAdded
};

inline bool Has(const sl::Feature* features, uint32_t count, sl::Feature wanted) {
  for (uint32_t i = 0; features != nullptr && i < count; ++i)
    if (features[i] == wanted) return true;
  return false;
}

// Pure, for the test.
// Only when the addon added DLSS-G: a game that loads every feature may have its
// own frame generation turned off in its menu, which must stay off.
constexpr bool Added(Decision decision) { return decision == Decision::kAdded; }

inline Plan PlanFeatures(const sl::Feature* features, uint32_t count, bool enabled, bool known_abi) {
  Plan plan;
  if (!enabled) return plan;
  if (!known_abi) {
    plan.decision = Decision::kUnknownAbi;
    return plan;
  }
  // No list: Streamline loads every feature it has, so DLSS-G is there already.
  if (features == nullptr || count == 0) {
    plan.decision = Decision::kAllFeatures;
    return plan;
  }
  if (Has(features, count, sl::kFeatureDLSS_G)) {
    plan.decision = Decision::kGameHasFrameGeneration;
    return plan;
  }
  if (!Has(features, count, sl::kFeatureDLSS)) {
    plan.decision = Decision::kNoSuperResolution;
    return plan;
  }
  plan.decision = Decision::kAdded;
  plan.features.assign(features, features + count);
  plan.features.push_back(sl::kFeatureDLSS_G);
  if (!Has(features, count, sl::kFeatureReflex)) plan.features.push_back(sl::kFeatureReflex);
  if (!Has(features, count, sl::kFeaturePCL)) plan.features.push_back(sl::kFeaturePCL);
  return plan;
}

// Whether to turn frame generation on from slSetConstants. Pure, for the test.
enum class Apply : uint8_t { kNotAdded, kGameControls, kNoReflex, kNoFunction, kAlreadyOn, kTurnOn };
constexpr Apply DecideApply(bool added, bool game_called_set_options, bool game_uses_reflex, bool have_set_options,
                            bool applied_for_this_multiplier) {
  if (!added) return Apply::kNotAdded;
  if (game_called_set_options) return Apply::kGameControls;
  if (!game_uses_reflex) return Apply::kNoReflex;
  if (!have_set_options) return Apply::kNoFunction;
  return applied_for_this_multiplier ? Apply::kAlreadyOn : Apply::kTurnOn;
}

constexpr const char* ApplyText(Apply apply) {
  switch (apply) {
    case Apply::kNotAdded: return "";
    case Apply::kGameControls: return "the game turns DLSS frame generation on and off itself";
    case Apply::kNoReflex: return "not turned on yet: the game does not use Reflex, which DLSS frame generation needs; MFG Unlock turns Reflex on once it has found the game's Present (see the log)";
    case Apply::kNoFunction: return "not turned on: Streamline did not give its DLSS-G options function";
    case Apply::kAlreadyOn: return "on";
    default: return "turning on";
  }
}

// 1.10.0: what the General page says under the switch. A game that never loads
// Streamline (RDR2 calls DLSS through NGX directly) never reaches slInit, so the
// absence of Streamline after the game has been up a while is a status of its own.
enum class Status : uint8_t {
  kOff, kWaiting, kNoStreamline, kInitMissed, kGameHasFrameGeneration, kAllFeatures, kNoSuperResolution, kNoReflex,
  kActive
};
constexpr uint64_t kNoStreamlineAfterMs = 20000;
constexpr Status PanelStatus(bool enabled, bool streamline_loaded, bool init_seen, uint64_t uptime_ms,
                             Decision decision, Apply apply) {
  if (!enabled) return Status::kOff;
  if (!init_seen) {
    if (streamline_loaded) return Status::kInitMissed;
    return uptime_ms >= kNoStreamlineAfterMs ? Status::kNoStreamline : Status::kWaiting;
  }
  switch (decision) {
    case Decision::kGameHasFrameGeneration: return Status::kGameHasFrameGeneration;
    case Decision::kAllFeatures: return Status::kAllFeatures;
    case Decision::kNoSuperResolution: return Status::kNoSuperResolution;
    case Decision::kAdded: break;
    default: return Status::kWaiting;
  }
  switch (apply) {
    case Apply::kNoReflex: return Status::kNoReflex;
    case Apply::kGameControls: return Status::kGameHasFrameGeneration;
    case Apply::kAlreadyOn: case Apply::kTurnOn: return Status::kActive;
    default: return Status::kWaiting;
  }
}

}  // namespace mfgunlock::forcefg
