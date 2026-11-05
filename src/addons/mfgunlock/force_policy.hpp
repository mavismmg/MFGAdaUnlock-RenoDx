/*
 * Fixed/Dynamic DLSS-G multiplier request policy.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
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
  kStreamlineBefore27 = 8,
};

// 1.8.3: Streamline added multi-frame generation in 2.7. An older DLSS-G
// wrapper (Ghost of Tsushima 2.4.0, HZD Remastered 2.2.1) never offers more
// than 2x and was not built for a larger numFramesToGenerate: forcing one
// there crashed inside sl.dlss_g. `packed` is runtimeversion::Pack; zero means
// not observed yet, which does not block.
inline constexpr bool StreamlineBlocksMultiFrame(uint64_t packed, uint32_t generated_frames) {
  constexpr uint64_t kFirstMultiFrame = uint64_t{2} << 48u | uint64_t{7} << 32u;
  return packed != 0 && packed < kFirstMultiFrame && generated_frames > 1;
}

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

// A generated-frame count the runtime refused twice (1.5.6). Games call
// slDLSSGSetOptions every frame; without this memory every call repeats the
// refused count and its retry before forwarding the game's own request, three
// Streamline calls instead of one. The refusal stands until a boundary where
// the runtime's answer can change: the game turns frame generation off and on,
// the user changes the multiplier, or the swapchain is recreated. Zero means
// none: an override always generates at least one frame.
class RefusalMemory {
 public:
  [[nodiscard]] bool Refused(uint32_t generated_frames) const {
    const uint32_t refused = refused_.load(std::memory_order_acquire);
    return refused != 0 && refused == generated_frames;
  }
  void Record(uint32_t generated_frames) {
    refused_.store(generated_frames, std::memory_order_release);
  }
  void Clear() { refused_.store(0, std::memory_order_release); }
  [[nodiscard]] uint32_t Value() const {
    return refused_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<uint32_t> refused_{0};
};

}  // namespace mfgunlock::forcepolicy
