/*
 * Conservative Streamline DLSS-G input quality guard.
 * SPDX-License-Identifier: MIT
 *
 * The guard only acts on optional HUD-separation inputs when their metadata
 * proves that they cannot satisfy Streamline's contract. Required color,
 * depth and motion-vector resources are never rewritten here.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sl_consts.h>
#include <sl_core_types.h>

namespace mfgunlock::qualityguard {

enum Issue : uint32_t {
  kNone = 0,
  kHdrFinalColorIsolation = 1u << 0,
  kInvalidOptionalResource = 1u << 1,
  kHudlessExtentMismatch = 1u << 2,
  kHudlessFormatMismatch = 1u << 3,
  kUiExtentMismatch = 1u << 4,
  kUiColorAlphaLowPrecision = 1u << 5,
};

struct OutputDescription {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;

  [[nodiscard]] bool HasDimensions() const { return width != 0 && height != 0; }
  [[nodiscard]] bool HasFormat() const { return format != 0; }
};

struct Assessment {
  uint32_t issues = kNone;
  bool has_hud_separation = false;
  bool suppress_hud_separation = false;
  OutputDescription observed_backbuffer{};
};

inline bool IsHudSeparationType(sl::BufferType type) {
  return type == sl::kBufferTypeHUDLessColor ||
         type == sl::kBufferTypeUIColorAndAlpha ||
         type == sl::kBufferTypeUIAlpha;
}

inline OutputDescription Describe(const sl::ResourceTag& tag, bool* valid = nullptr) {
  bool local_valid = tag.structVersion == sl::kStructVersion1;
  OutputDescription result{};
  if (tag.extent.width != 0 && tag.extent.height != 0) {
    result.width = tag.extent.width;
    result.height = tag.extent.height;
  }

  if (tag.resource != nullptr) {
    local_valid = local_valid && tag.resource->structVersion == sl::kStructVersion1;
    if (!result.HasDimensions()) {
      result.width = tag.resource->width;
      result.height = tag.resource->height;
    }
    result.format = tag.resource->nativeFormat;

    if (tag.extent.width != 0 && tag.extent.height != 0 &&
        tag.resource->width != 0 && tag.resource->height != 0) {
      const uint64_t right = static_cast<uint64_t>(tag.extent.left) + tag.extent.width;
      const uint64_t bottom = static_cast<uint64_t>(tag.extent.top) + tag.extent.height;
      local_valid = local_valid && right <= tag.resource->width &&
                    bottom <= tag.resource->height;
    }
  }
  if (valid != nullptr) *valid = local_valid;
  return result;
}

inline Assessment AssessTags(const sl::ResourceTag* tags, uint32_t count, bool hdr,
                             const OutputDescription& previous_output = {}) {
  Assessment result{};
  if (tags == nullptr || count == 0) return result;

  OutputDescription output = previous_output;
  for (uint32_t i = 0; i < count; ++i) {
    if (tags[i].structVersion != sl::kStructVersion1 ||
        tags[i].type != sl::kBufferTypeBackbuffer) {
      continue;
    }
    bool valid = false;
    const OutputDescription current = Describe(tags[i], &valid);
    if (!valid) continue;
    if (current.HasDimensions()) {
      output.width = current.width;
      output.height = current.height;
    }
    if (current.HasFormat()) output.format = current.format;
  }
  result.observed_backbuffer = output;

  for (uint32_t i = 0; i < count; ++i) {
    const sl::ResourceTag& tag = tags[i];
    if (!IsHudSeparationType(tag.type) || tag.resource == nullptr) continue;
    result.has_hud_separation = true;

    bool valid = false;
    const OutputDescription resource = Describe(tag, &valid);
    if (!valid) result.issues |= kInvalidOptionalResource;
    if (resource.HasDimensions() && output.HasDimensions() &&
        (resource.width != output.width || resource.height != output.height)) {
      result.issues |= tag.type == sl::kBufferTypeHUDLessColor
                           ? kHudlessExtentMismatch
                           : kUiExtentMismatch;
    }
    if (tag.type == sl::kBufferTypeHUDLessColor && resource.HasFormat() &&
        output.HasFormat() && resource.format != output.format) {
      result.issues |= kHudlessFormatMismatch;
    }

    // DXGI_FORMAT_R10G10B10A2_UNORM has only two alpha bits. NVIDIA's DLSS-G
    // guide explicitly rejects it for UIColorAndAlpha because the separation
    // mask needs adequate alpha precision.
    if (tag.type == sl::kBufferTypeUIColorAndAlpha && resource.format == 24) {
      result.issues |= kUiColorAlphaLowPrecision;
    }
  }

  if (hdr && result.has_hud_separation)
    result.issues |= kHdrFinalColorIsolation;
  result.suppress_hud_separation = result.has_hud_separation && result.issues != kNone;
  return result;
}

inline size_t ConstantsCopySize(const sl::Constants& source) {
  if (source.structVersion == sl::kStructVersion1)
    return offsetof(sl::Constants, minRelativeLinearDepthObjectSeparation);
  if (source.structVersion == sl::kStructVersion2) return sizeof(sl::Constants);
  return 0;
}

inline bool CopyConstantsWithReset(const sl::Constants& source, void* destination,
                                   size_t capacity) {
  const size_t bytes = ConstantsCopySize(source);
  if (bytes == 0 || destination == nullptr || capacity < bytes) return false;
  std::memcpy(destination, &source, bytes);
  reinterpret_cast<sl::Constants*>(destination)->reset = sl::Boolean::eTrue;
  return true;
}

inline bool CopyConstantsWithQualityOverrides(const sl::Constants& source,
                                              void* destination, size_t capacity,
                                              bool force_reset,
                                              float depth_separation_override) {
  const size_t bytes = ConstantsCopySize(source);
  if (bytes == 0 || destination == nullptr || capacity < bytes) return false;
  std::memcpy(destination, &source, bytes);
  auto* forwarded = reinterpret_cast<sl::Constants*>(destination);
  if (force_reset) forwarded->reset = sl::Boolean::eTrue;
  if (source.structVersion >= sl::kStructVersion2 &&
      depth_separation_override > 0.0f && depth_separation_override <= 1000.0f) {
    forwarded->minRelativeLinearDepthObjectSeparation = depth_separation_override;
  }
  return true;
}

}  // namespace mfgunlock::qualityguard
