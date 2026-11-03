// SPDX-License-Identifier: MIT
#pragma once
// 1.8.1: the XMX generator's decisions, free of Direct3D objects so the gate can
// check them (xmx_generator.hpp needs a game and libxess_fg.dll).
#include <dxgiformat.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace mfgunlock::xmx::policy {
// EvaluateFeature has no queue to wait on, so age stands in for a fence: far more
// real frames than any game keeps in flight.
constexpr uint64_t kRetireAfterPairs = 16;
// XeFG's contexts are not destroyed (the function's contract is unverified), so
// a game that keeps resizing stops generating instead of filling the VRAM.
constexpr uint32_t kMaxContexts = 16;
// A longer gap between real frames is a pause, not a pair to interpolate.
constexpr uint64_t kStalePairMs = 500;

// Keeps resources alive until the commands that used them have finished.
template <class T>
class RetireQueue {
  struct Entry { T item; uint64_t due; };
  std::vector<Entry> entries;

 public:
  void Push(T item, uint64_t pair) { entries.push_back({std::move(item), pair + kRetireAfterPairs}); }
  void Collect(uint64_t pair) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.due <= pair; }),
                  entries.end());
  }
  size_t Size() const { return entries.size(); }
};

struct ContextShape {
  const void* device;
  uint32_t width, height, format, flags;
  friend constexpr bool operator==(const ContextShape&, const ContextShape&) = default;
};

constexpr bool PairIsStale(uint64_t lastCaptureMs, uint64_t nowMs) {
  return lastCaptureMs != 0 && nowMs - lastCaptureMs > kStalePairMs;
}

// DLSS-G's frames of one pair mapped onto what the model can produce. Frames past
// the model's limit show the last one generated.
struct FramePlan {
  uint32_t count, index;
  bool repeat, limited;
};
constexpr FramePlan PlanFrames(uint32_t requested, uint32_t index, uint32_t maxFrames) {
  const uint32_t wanted = requested ? requested : 1;
  const uint32_t count = wanted < maxFrames ? wanted : maxFrames;
  const uint32_t position = index < 1 ? 1 : index > wanted ? wanted : index;
  return {count, position < count ? position : count, position > count, wanted > count};
}

// CopyResource needs the same dimensions and the same typeless family.
constexpr int CopyFamily(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
      return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 2;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return 3;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
      return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
      return 5;
    default:
      return 0;
  }
}
constexpr bool CanCopy(uint64_t targetWidth, uint32_t targetHeight, DXGI_FORMAT target, uint64_t sourceWidth,
                       uint32_t sourceHeight, DXGI_FORMAT source) {
  if (targetWidth != sourceWidth || targetHeight != sourceHeight) return false;
  return target == source || (CopyFamily(target) != 0 && CopyFamily(target) == CopyFamily(source));
}

// A provider found by the loader hook or by the scan of modules already mapped.
// Both report the same module, and NGX may use any copy.
enum class SlotAction { Install, Known, Full };
constexpr SlotAction PlanSlot(bool known, int used, int slots) {
  return known ? SlotAction::Known : used < slots ? SlotAction::Install : SlotAction::Full;
}
// Hooks in place, the generator never called, frames generated anyway: NGX runs a
// copy without the hook, so NVIDIA's model is still the one generating.
constexpr bool Bypassed(bool hooked, bool everEvaluated, bool driverGenerating) {
  return hooked && !everEvaluated && driverGenerating;
}

// The part of a texture a game declares valid; a declaration that is absent or
// falls outside the texture means all of it.
struct Area {
  uint32_t x, y, width, height;
  friend constexpr bool operator==(const Area&, const Area&) = default;
};
constexpr Area InputArea(uint32_t textureWidth, uint32_t textureHeight, Area declared) {
  const bool fits = declared.width != 0 && declared.height != 0 &&
                    uint64_t{declared.x} + declared.width <= textureWidth &&
                    uint64_t{declared.y} + declared.height <= textureHeight;
  return fits ? declared : Area{0, 0, textureWidth, textureHeight};
}
// XeFG answers -4 when depth and motion differ in size (Phantom Blade Zero: 1488x836
// and 1485x836), so both describe the area they share. Inputs that already agree
// are not touched by the caller.
constexpr std::pair<Area, Area> SharedArea(Area depth, Area motion) {
  const uint32_t width = depth.width < motion.width ? depth.width : motion.width;
  const uint32_t height = depth.height < motion.height ? depth.height : motion.height;
  return {Area{depth.x, depth.y, width, height}, Area{motion.x, motion.y, width, height}};
}

// Why the last evaluation produced no generated frame.
enum class Idle : int {
  None,
  MissingInputs,
  NoPair,
  GameReset,
  HudlessChanged,
  ExecuteError,
  NoOutput,
  CopyMismatch,
  Count
};
constexpr const char* IdleText(Idle idle) {
  switch (idle) {
    case Idle::MissingInputs: return "the game sent no color, depth or motion input";
    case Idle::NoPair: return "waiting for two real frames in a row";
    case Idle::GameReset: return "the game reset its history";
    case Idle::HudlessChanged: return "the game's HUD-less input came and went";
    case Idle::ExecuteError: return "XeFG returned an error";
    case Idle::NoOutput: return "XeFG produced no frame";
    case Idle::CopyMismatch: return "the game's textures do not match in size or format";
    default: return "";
  }
}
}  // namespace mfgunlock::xmx::policy
