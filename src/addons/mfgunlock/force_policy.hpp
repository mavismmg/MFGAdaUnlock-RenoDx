/*
 * Fixed/Dynamic DLSS-G multiplier request policy.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace mfgunlock::forcepolicy {

enum class RequestSource : uint32_t {
  kNative = 0,
  kFixedOverride = 1,
  kDynamic = 2,
};

enum class FixedOverrideStatus : uint32_t {
  kNative = 0,
  kPending = 1,
  kApplied = 2,
  kRejected = 3,
  kBlockedByPacing = 4,
  kUnsupportedAbi = 5,
  kDynamicPriority = 6,
  kMatchedGameRequest = 7,
};

struct RequestDecision {
  RequestSource source = RequestSource::kNative;
  uint32_t game_generated_frames = 0;
  uint32_t downstream_generated_frames = 0;

  [[nodiscard]] constexpr bool OverridesGeneratedFrames() const {
    return source == RequestSource::kFixedOverride &&
           downstream_generated_frames != game_generated_frames;
  }
};

inline constexpr bool IsFixedMultiplier(unsigned int multiplier) {
  return multiplier >= 2 && multiplier <= 6;
}

// Dynamic owns multiplier selection whenever it is actively eligible. A valid
// fixed selection is otherwise absolute: it replaces rather than merely raises
// the game's numFramesToGenerate request. Frame Generation Off is always
// forwarded unchanged and the selection is applied on the next enabled call.
inline constexpr RequestDecision Resolve(uint32_t game_generated_frames,
                                         unsigned int force_multiplier,
                                         bool dynamic_controlling,
                                         bool frame_generation_enabled) {
  RequestDecision decision{
      RequestSource::kNative, game_generated_frames, game_generated_frames};
  if (!frame_generation_enabled) return decision;
  if (dynamic_controlling) {
    decision.source = RequestSource::kDynamic;
    return decision;
  }
  if (IsFixedMultiplier(force_multiplier)) {
    decision.source = RequestSource::kFixedOverride;
    decision.downstream_generated_frames = force_multiplier - 1;
  }
  return decision;
}

}  // namespace mfgunlock::forcepolicy
