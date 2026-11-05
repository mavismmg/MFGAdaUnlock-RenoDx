#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/force_policy.hpp"
#include "../src/addons/mfgunlock/hdr_compat.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

struct CapturedCall {
  sl::DLSSGMode mode = sl::DLSSGMode::eOff;
  uint32_t generated_frames = 0;
  unsigned int calls = 0;
};

CapturedCall SendDownstream(unsigned int game_multiplier,
                            unsigned int force_multiplier,
                            bool dynamic_controlling,
                            bool frame_generation_enabled) {
  sl::DLSSGOptions game{};
  game.mode = frame_generation_enabled ? sl::DLSSGMode::eOn
                                       : sl::DLSSGMode::eOff;
  game.numFramesToGenerate = game_multiplier - 1;

  const auto decision = mfgunlock::forcepolicy::Resolve(
      game.numFramesToGenerate, force_multiplier, dynamic_controlling,
      frame_generation_enabled);

  sl::DLSSGOptions forwarded{};
  const bool advanced = decision.OverridesGeneratedFrames() ||
                        decision.source ==
                            mfgunlock::forcepolicy::RequestSource::kDynamic;
  if (advanced) {
    CHECK(mfgunlock::hdrcompat::BuildAdvancedOptions(
        game, forwarded, decision.downstream_generated_frames,
        decision.OverridesGeneratedFrames(), false,
        decision.source == mfgunlock::forcepolicy::RequestSource::kDynamic,
        0.0f));
  } else {
    forwarded = game;
  }

  CapturedCall capture{};
  const auto downstream = [&](const sl::DLSSGOptions& received) {
    capture.mode = received.mode;
    capture.generated_frames = received.numFramesToGenerate;
    ++capture.calls;
  };
  downstream(forwarded);
  return capture;
}

int main() {
  using mfgunlock::forcepolicy::RequestSource;
  using mfgunlock::forcepolicy::Resolve;

  // Exhaustive game-menu 2x/3x/4x versus fixed 2x-6x matrix. Assertions are
  // made against the options received by a fake downstream, not UI state.
  for (unsigned int game_multiplier = 2; game_multiplier <= 4;
       ++game_multiplier) {
    for (unsigned int force_multiplier = 2; force_multiplier <= 6;
         ++force_multiplier) {
      const auto decision = Resolve(game_multiplier - 1, force_multiplier,
                                    false, true);
      CHECK(decision.source == RequestSource::kFixedOverride);
      CHECK(decision.OverridesGeneratedFrames() ==
            (game_multiplier != force_multiplier));
      const CapturedCall call = SendDownstream(
          game_multiplier, force_multiplier, false, true);
      CHECK(call.calls == 1);
      CHECK(call.mode == sl::DLSSGMode::eOn);
      CHECK(call.generated_frames == force_multiplier - 1);
    }
  }

  // Force 0 preserves the game's request.
  for (unsigned int game_multiplier = 2; game_multiplier <= 4;
       ++game_multiplier) {
    const auto decision = Resolve(game_multiplier - 1, 0, false, true);
    CHECK(decision.source == RequestSource::kNative);
    const CapturedCall call = SendDownstream(game_multiplier, 0, false, true);
    CHECK(call.calls == 1);
    CHECK(call.mode == sl::DLSSGMode::eOn);
    CHECK(call.generated_frames == game_multiplier - 1);
  }

  // Dynamic has priority over a fixed selection and remains provider-owned.
  const auto dynamic = Resolve(1, 6, true, true);
  CHECK(dynamic.source == RequestSource::kDynamic);
  const CapturedCall dynamic_call = SendDownstream(2, 6, true, true);
  CHECK(dynamic_call.calls == 1);
  CHECK(dynamic_call.mode == sl::DLSSGMode::eDynamic);
  CHECK(dynamic_call.generated_frames == 1);

  // With Dynamic inactive, the same fixed selection is applied.
  const CapturedCall fixed_call = SendDownstream(2, 6, false, true);
  CHECK(fixed_call.mode == sl::DLSSGMode::eOn);
  CHECK(fixed_call.generated_frames == 5);

  // An Off call is never rewritten; the next On call applies the selection.
  const CapturedCall off_call = SendDownstream(4, 2, false, false);
  CHECK(off_call.calls == 1);
  CHECK(off_call.mode == sl::DLSSGMode::eOff);
  CHECK(off_call.generated_frames == 3);
  const CapturedCall on_call = SendDownstream(4, 2, false, true);
  CHECK(on_call.mode == sl::DLSSGMode::eOn);
  CHECK(on_call.generated_frames == 1);

  // Repeated calls and both change directions remain absolute.
  CHECK(SendDownstream(4, 3, false, true).generated_frames == 2);
  CHECK(SendDownstream(4, 3, false, true).generated_frames == 2);
  CHECK(SendDownstream(2, 6, false, true).generated_frames == 5);
  CHECK(SendDownstream(2, 2, false, true).generated_frames == 1);
  CHECK(SendDownstream(4, 2, false, true).generated_frames == 1);
  CHECK(SendDownstream(4, 6, false, true).generated_frames == 5);

  // 1.5.6: a count refused twice is not retried until a reset boundary.
  {
    mfgunlock::forcepolicy::RefusalMemory refusal;
    CHECK(!refusal.Refused(3));
    CHECK(!refusal.Refused(0));  // zero is "no refusal", never a count
    refusal.Record(3);           // 4x refused
    CHECK(refusal.Refused(3));
    CHECK(refusal.Value() == 3);
    CHECK(!refusal.Refused(2));  // another count (3x) is tried normally
    CHECK(!refusal.Refused(4));
    refusal.Record(4);           // the latest refusal replaces the previous
    CHECK(refusal.Refused(4) && !refusal.Refused(3));
    refusal.Clear();             // game off/on, multiplier change, new swapchain
    CHECK(!refusal.Refused(4) && refusal.Value() == 0);
  }

  {
    using mfgunlock::forcepolicy::StreamlineBlocksMultiFrame;
    constexpr auto version = [](uint64_t major, uint64_t minor, uint64_t patch) {
      return major << 48u | minor << 32u | patch << 16u;
    };
    CHECK(StreamlineBlocksMultiFrame(version(2, 4, 0), 2));   // Ghost of Tsushima, 3x
    CHECK(StreamlineBlocksMultiFrame(version(2, 2, 1), 5));   // HZD Remastered, 6x
    CHECK(StreamlineBlocksMultiFrame(version(2, 6, 9), 2));
    CHECK(!StreamlineBlocksMultiFrame(version(2, 4, 0), 1));  // 2x is what it supports
    CHECK(!StreamlineBlocksMultiFrame(version(2, 7, 0), 3));  // TLOU Part II has 2.7.2
    CHECK(!StreamlineBlocksMultiFrame(version(2, 14, 16), 5));
    CHECK(!StreamlineBlocksMultiFrame(0, 3));                 // not observed yet
  }

  std::cout << "force policy tests passed\n";
  return EXIT_SUCCESS;
}
