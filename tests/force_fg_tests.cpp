// SPDX-License-Identifier: MIT
#include <cstdio>

#include "../src/addons/mfgunlock/force_fg.hpp"

using namespace mfgunlock::forcefg;

static int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

int main() {
  const sl::Feature sr_only[] = {sl::kFeatureDLSS, sl::kFeatureReflex};
  const sl::Feature with_fg[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  const sl::Feature no_sr[] = {sl::kFeatureReflex};

  CHECK(PlanFeatures(sr_only, 2, false, true).decision == Decision::kOff);
  CHECK(PlanFeatures(sr_only, 2, true, false).decision == Decision::kUnknownAbi);
  CHECK(PlanFeatures(with_fg, 4, true, true).decision == Decision::kGameHasFrameGeneration);
  CHECK(PlanFeatures(no_sr, 1, true, true).decision == Decision::kNoSuperResolution);
  CHECK(PlanFeatures(nullptr, 0, true, true).decision == Decision::kAllFeatures);
  CHECK(Added(Decision::kAdded) && !Added(Decision::kAllFeatures) && !Added(Decision::kGameHasFrameGeneration));

  const auto plan = PlanFeatures(sr_only, 2, true, true);
  CHECK(plan.decision == Decision::kAdded);
  // The game's own features first, in its order, then DLSS-G and the missing PCL (Reflex was there).
  CHECK(plan.features.size() == 4);
  CHECK(plan.features[0] == sl::kFeatureDLSS && plan.features[1] == sl::kFeatureReflex);
  CHECK(plan.features[2] == sl::kFeatureDLSS_G && plan.features[3] == sl::kFeaturePCL);

  CHECK(DecideApply(false, false, true, true, false) == Apply::kNotAdded);
  CHECK(DecideApply(true, true, true, true, false) == Apply::kGameControls);
  CHECK(DecideApply(true, false, false, true, false) == Apply::kNoReflex);
  CHECK(DecideApply(true, false, true, false, false) == Apply::kNoFunction);
  CHECK(DecideApply(true, false, true, true, true) == Apply::kAlreadyOn);
  CHECK(DecideApply(true, false, true, true, false) == Apply::kTurnOn);

  // The General page's status (RDR2: no Streamline at all).
  CHECK(PanelStatus(false, false, false, 99999, Decision::kOff, Apply::kNotAdded) == Status::kOff);
  CHECK(PanelStatus(true, false, false, 5000, Decision::kOff, Apply::kNotAdded) == Status::kWaiting);
  CHECK(PanelStatus(true, false, false, 30000, Decision::kOff, Apply::kNotAdded) == Status::kNoStreamline);
  CHECK(PanelStatus(true, true, false, 30000, Decision::kOff, Apply::kNotAdded) == Status::kInitMissed);
  CHECK(PanelStatus(true, true, true, 30000, Decision::kGameHasFrameGeneration, Apply::kNotAdded) ==
        Status::kGameHasFrameGeneration);
  CHECK(PanelStatus(true, true, true, 30000, Decision::kNoSuperResolution, Apply::kNotAdded) == Status::kNoSuperResolution);
  CHECK(PanelStatus(true, true, true, 30000, Decision::kAdded, Apply::kNoReflex) == Status::kNoReflex);
  CHECK(PanelStatus(true, true, true, 30000, Decision::kAdded, Apply::kAlreadyOn) == Status::kActive);
  CHECK(PanelStatus(true, true, true, 30000, Decision::kAdded, Apply::kNotAdded) == Status::kWaiting);

  // 1.11.0 (F2, Ninja Gaiden 4): the game's Streamline has no sl.dlss_g.dll.
  CHECK(DecidePluginSource(true, false, 2, 4, 0, 0) == PluginSource::kGame);
  CHECK(DecidePluginSource(true, true, 2, 4, 2, 14) == PluginSource::kGame);
  CHECK(DecidePluginSource(false, false, 2, 14, 0, 0) == PluginSource::kMissing);
  CHECK(DecidePluginSource(false, true, 2, 14, 2, 14) == PluginSource::kOurs);
  CHECK(DecidePluginSource(false, true, 2, 9, 2, 14) == PluginSource::kOtherVersion);
  CHECK(DecidePluginSource(false, true, 0, 0, 2, 14) == PluginSource::kOtherVersion);  // unknown version: not lent
  CHECK(AfterPluginCheck(Decision::kAdded, PluginSource::kGame) == Decision::kAdded);
  CHECK(AfterPluginCheck(Decision::kAdded, PluginSource::kOurs) == Decision::kAdded);
  CHECK(AfterPluginCheck(Decision::kAdded, PluginSource::kMissing) == Decision::kNoPlugin);
  CHECK(AfterPluginCheck(Decision::kAdded, PluginSource::kOtherVersion) == Decision::kPluginVersion);
  CHECK(AfterPluginCheck(Decision::kGameHasFrameGeneration, PluginSource::kMissing) == Decision::kGameHasFrameGeneration);
  CHECK(!Added(Decision::kNoPlugin) && !Added(Decision::kPluginVersion));
  CHECK(PanelStatus(true, true, true, 30000, Decision::kNoPlugin, Apply::kNotAdded) == Status::kNoPlugin);
  CHECK(PanelStatus(true, true, true, 30000, Decision::kPluginVersion, Apply::kNotAdded) == Status::kNoPlugin);

  // 1.10.1 (plan item 5): Add frame generation's own slInit is recognized; a game's is not.
  {
    sl::Preferences host{};
    host.projectId = kHostProjectId;
    CHECK(IsHostInit(host));
    sl::Preferences game{};
    CHECK(!IsHostInit(game));
    game.projectId = "a2bcbbbe-4c7a-4f2b-9d1b-1d1b1d1b1d1b";
    CHECK(!IsHostInit(game));
  }

  if (failures == 0) std::printf("force_fg: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
