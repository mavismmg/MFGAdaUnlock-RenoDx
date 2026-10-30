// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "native_window.hpp"
namespace mfgunlock::xemfg::native {
enum class LatencyInput : uint32_t { Auto, StreamlineReflex };
enum class LatencyOutput : uint32_t { Auto, XeLL, Reflex };
struct ControlSettings {
  uint32_t version{5};
  LatencyInput input{LatencyInput::Auto};
  LatencyOutput output{LatencyOutput::Auto};
  bool queueAhead{true};
  bool flipMetering{true}, forceBorderless{}, uiComposition{},sdkDeadlines{};
  bool forceVSyncOff{};
  // 1.5.7: limit the source rate to the display refresh / multiplier (XeLL output).
  bool displayCap{};
  // 1.5.8: an exclusive fullscreen request on the XeMFG output is served as
  // borderless (off by default).
  bool exclusiveAsBorderless{};
  // 1.7 (plan E.2): without HUD-less color from the game, compose unchanged
  // back-buffer pixels as UI (off by default).
  bool uiStabilization{};
};
struct ControlStatus {
  uint32_t version{5};
  ControlSettings settings;
  WindowStatus window;
  bool reflexObserved{}, xellObserved{}, nativeReflexDisabled{};
  uint32_t gameReflexMode{};
  uint64_t sleeps{}, markers{};
  uint64_t modeUpdates{}, lastSleepUs{};
  bool uiCompositionAvailable{}, uiCompositionApplied{}, uiCompositionKnown{};
  int uiCompositionResult{};
  uint64_t uiCompositionUpdates{};
  bool uiStabilized{};
  bool modelPort{}, modelInitialized{}, modelUpdating{}, cudaUpdating{}, settingsSaved{true},invalidConfiguration{};
  uint32_t cudaState{}, cudaLayers{};
  int cudaMajor{}, cudaMinor{};
  uint64_t cudaLaunches{}, cudaFallbacks{};
  bool flipAvailable{}, flipInstalled{}, flipRegistered{}, flipQueue{}, flipBusy{};
  bool flipRequested{}, flipEnabled{}, flipConfigured{}, flipMetered{}, flipRefused{};
  uint32_t flipOutputs{}, flipFeedback{};
  int flipResult{};
  uint64_t flipCalls{};
  bool sdkDeadlinesRequested{},sdkDeadlinesActive{};
  bool queueAheadActive{}; uint64_t queueAheadDeadlines{};
  uint32_t hudIssues{},hudInputs{};
  uint32_t hudFiltered{},hudCopied{}; // 1.5.7: per kind, 1u<<xefg type
  uint64_t hudRejected{};
  char cudaReason[128]{};
  LatencyOutput activeLatencyOutput{LatencyOutput::XeLL};
  bool latencyRestartPending{},reflexOutputObserved{},reflexOptionsReady{};
  uint64_t reflexSleeps{},xellSleeps{};
  uint32_t gameSyncInterval{UINT32_MAX};
  // 1.5.7: display refresh (mHz, 0 unknown), smoothed source interval and the
  // display cap's minimum source interval (0 when off or unavailable).
  uint32_t displayRefreshMilliHz{}, sourceIntervalUs{}, displayCapIntervalUs{};
  // 1.5.8: the game's exclusive fullscreen is currently served as borderless;
  // exclusive requests served so far and Alt+Enter presses held back.
  bool exclusiveServed{};
  uint64_t exclusiveRequests{}, altEnterBlocked{};
};
void LoadControls();
ControlSettings ControlsSettings();
// Lock-free read of ControlSettings::forceVSyncOff for the Present path.
bool ForceVSyncOff() noexcept;
// Lock-free read of ControlSettings::exclusiveAsBorderless for swapchain calls.
bool ExclusiveAsBorderless() noexcept;
// Lock-free read of ControlSettings::uiStabilization for the Present and tag paths.
bool UiStabilization() noexcept;
bool ConfigureControls(const ControlSettings& settings);
ControlStatus ControlsSnapshot();
enum class PanelPage { General, Quality, Latency, Diagnostics };
void DrawControls(PanelPage page);
}
