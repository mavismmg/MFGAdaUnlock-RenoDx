// SPDX-License-Identifier: MIT
// Observation only. No resource, constant, option or pacing overrides.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include <sl.h>
#include <sl_dlss_g.h>

namespace mfgdiagnostics {

constexpr uint64_t kUnknown = UINT64_MAX;
enum class Kind { options, constants, tag, state, output, tag_batch };

// Native resource/command/swapchain addresses are opaque identity tokens only.
// Export replaces them with session-local IDs; they are never dereferenced later.
struct Event {
  Kind kind{};
  uint64_t generation = 0;
  uint64_t begin = 0, end = 0;
  uint32_t thread = 0, viewport = UINT32_MAX, frame = UINT32_MAX;
  uint32_t result = UINT32_MAX;
  uint64_t version = 0;
  bool recognized = false;
  // Schema 1 readers use indexes 0-23. Later observer-only metadata is appended
  // so old captures and comparison tooling remain compatible.
  std::array<uint64_t, 32> values{};
  std::array<float, 112> floats{};

  Event(Kind type = Kind::tag) : kind(type) {
    values.fill(kUnknown);
    floats.fill(std::numeric_limits<float>::quiet_NaN());
  }
};

inline Event SnapshotOptions(const sl::DLSSGOptions& source) {
  Event event(Kind::options);
  event.version = source.structVersion;
  event.values[23] = source.next != nullptr;
  if (source.structType != sl::DLSSGOptions::s_structType ||
      source.structVersion < 1 || source.structVersion > 5) return event;
  event.recognized = true;
  event.values[0] = static_cast<uint32_t>(source.mode);
  event.values[1] = source.numFramesToGenerate;
  event.values[2] = static_cast<uint32_t>(source.flags);
  event.values[3] = source.dynamicResWidth;
  event.values[4] = source.dynamicResHeight;
  event.values[5] = source.numBackBuffers;
  event.values[6] = source.mvecDepthWidth;
  event.values[7] = source.mvecDepthHeight;
  event.values[8] = source.colorWidth;
  event.values[9] = source.colorHeight;
  event.values[10] = source.colorBufferFormat;
  event.values[11] = source.mvecBufferFormat;
  event.values[12] = source.depthBufferFormat;
  event.values[13] = source.hudLessBufferFormat;
  event.values[14] = source.uiBufferFormat;
  if (source.structVersion >= 3)
    event.values[15] = static_cast<uint32_t>(source.queueParallelismMode);
  if (source.structVersion >= 4)
    event.values[16] = static_cast<unsigned char>(source.enableUserInterfaceRecomposition);
  if (source.structVersion >= 5) event.floats[0] = source.dynamicTargetFrameRate;
  return event;
}

inline Event SnapshotConstants(const sl::Constants& source) {
  Event event(Kind::constants);
  event.version = source.structVersion;
  event.values[23] = source.next != nullptr;
  if (source.structType != sl::Constants::s_structType ||
      source.structVersion < 1 || source.structVersion > 2) return event;
  event.recognized = true;
  const sl::float4x4* matrices[] = {&source.cameraViewToClip, &source.clipToCameraView,
      &source.clipToLensClip, &source.clipToPrevClip, &source.prevClipToClip};
  for (size_t m = 0; m < 5; ++m) {
    for (size_t row = 0; row < 4; ++row) {
      const auto& v = matrices[m]->row[row];
      const size_t at = m * 16 + row * 4;
      event.floats[at] = v.x;
      event.floats[at + 1] = v.y;
      event.floats[at + 2] = v.z;
      event.floats[at + 3] = v.w;
    }
  }
  event.floats[80] = source.jitterOffset.x;
  event.floats[81] = source.jitterOffset.y;
  event.floats[82] = source.mvecScale.x;
  event.floats[83] = source.mvecScale.y;
  event.floats[84] = source.cameraPinholeOffset.x;
  event.floats[85] = source.cameraPinholeOffset.y;
  const sl::float3* vectors[] = {&source.cameraPos, &source.cameraUp,
                               &source.cameraRight, &source.cameraFwd};
  for (size_t i = 0; i < 4; ++i) {
    event.floats[86 + i * 3] = vectors[i]->x;
    event.floats[87 + i * 3] = vectors[i]->y;
    event.floats[88 + i * 3] = vectors[i]->z;
  }
  event.floats[98] = source.cameraNear;
  event.floats[99] = source.cameraFar;
  event.floats[100] = source.cameraFOV;
  event.floats[101] = source.cameraAspectRatio;
  event.floats[102] = source.motionVectorsInvalidValue;
  if (source.structVersion >= 2)
    event.floats[103] = source.minRelativeLinearDepthObjectSeparation;
  event.values[0] = static_cast<unsigned char>(source.depthInverted);
  event.values[1] = static_cast<unsigned char>(source.cameraMotionIncluded);
  event.values[2] = static_cast<unsigned char>(source.motionVectors3D);
  event.values[3] = static_cast<unsigned char>(source.reset);
  event.values[4] = static_cast<unsigned char>(source.orthographicProjection);
  event.values[5] = static_cast<unsigned char>(source.motionVectorsDilated);
  event.values[6] = static_cast<unsigned char>(source.motionVectorsJittered);
  return event;
}

inline Event SnapshotTag(const sl::ResourceTag& source, const sl::CommandBuffer* commands) {
  Event event(Kind::tag);
  event.version = source.structVersion;
  event.values[23] = source.next != nullptr;
  if (source.structType != sl::ResourceTag::s_structType || source.structVersion != 1)
    return event;
  event.recognized = true;
  event.values[0] = source.type;
  event.values[1] = static_cast<uint32_t>(source.lifecycle);
  event.values[2] = source.resource != nullptr;
  event.values[9] = source.extent.left;
  event.values[10] = source.extent.top;
  event.values[11] = source.extent.width;
  event.values[12] = source.extent.height;
  event.values[13] = reinterpret_cast<uintptr_t>(commands);
  if (source.resource == nullptr) return event;
  const auto& resource = *source.resource;
  event.values[22] = resource.structVersion;
  if (resource.structType != sl::Resource::s_structType || resource.structVersion != 1)
    return event;
  event.values[3] = resource.native != nullptr;
  event.values[4] = reinterpret_cast<uintptr_t>(resource.native);
  event.values[5] = resource.width;
  event.values[6] = resource.height;
  event.values[7] = resource.nativeFormat;
  event.values[8] = resource.state;
  event.values[21] = resource.next != nullptr;
  return event;
}

inline Event SnapshotState(const sl::DLSSGState& source, sl::Result result) {
  Event event(Kind::state);
  event.result = static_cast<uint32_t>(result);
  event.version = source.structVersion;
  if (result != sl::Result::eOk || source.structType != sl::DLSSGState::s_structType ||
      source.structVersion < 1 || source.structVersion > 4) return event;
  event.recognized = true;
  event.values[0] = static_cast<uint32_t>(source.status);
  event.values[1] = source.numFramesActuallyPresented;
  if (source.structVersion >= 2) event.values[2] = source.numFramesToGenerateMax;
  return event;
}

// Readers never wait on the capture/UI/export thread. Missing events remain
// explicit; a partial capture must not be treated as a synchronization proof.
template <size_t Capacity>
class Capture {
 public:
  std::atomic<uint64_t> active{0};
  std::atomic<uint32_t> dropped{0};
  std::atomic_bool full{false};
  std::atomic<uint64_t> deadline{0};
  std::mutex mutex;
  std::vector<Event> events;
  uint64_t generation = 0;

  void Start(uint64_t now_ms, uint64_t duration_ms) {
    std::lock_guard lock(mutex);
    active.store(0, std::memory_order_release);
    events.clear();
    events.reserve(Capacity); // Allocation only when starting, never in hooks.
    dropped.store(0, std::memory_order_relaxed);
    full.store(false, std::memory_order_relaxed);
    deadline.store(now_ms + duration_ms, std::memory_order_relaxed);
    active.store(++generation, std::memory_order_release);
  }

  uint64_t Ticket(uint64_t now_ms) {
    uint64_t ticket = active.load(std::memory_order_acquire);
    if (ticket != 0 && now_ms >= deadline.load(std::memory_order_relaxed)) {
      active.compare_exchange_strong(ticket, 0, std::memory_order_acq_rel);
      return 0;
    }
    return ticket;
  }

  void Submit(Event event) {
    if (event.generation == 0 || active.load(std::memory_order_acquire) != event.generation)
      return;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (active.load(std::memory_order_acquire) != event.generation) return;
    if (events.size() == Capacity) {
      full.store(true, std::memory_order_relaxed);
      active.store(0, std::memory_order_release);
      return;
    }
    events.push_back(event);
  }

  void Stop() { active.store(0, std::memory_order_release); }
};

} // namespace mfgdiagnostics
