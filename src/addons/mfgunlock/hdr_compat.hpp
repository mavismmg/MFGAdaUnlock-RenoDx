/*
 * DLSS-G HDR/UI compatibility helpers.
 * SPDX-License-Identifier: MIT
 *
 * Some older integrations provide both HUD-less and UI resources but still
 * submit a pre-v4 DLSSGOptions structure. Current Streamline providers only
 * expose the separate HUD-less/UI interpolation path through the v4
 * enableUserInterfaceRecomposition field. Build a complete local options
 * structure instead of reading fields which did not exist in the caller's ABI
 * or modifying memory owned by the game.
 */

#pragma once

#include <algorithm>
#include <cstdint>

#include <sl_dlss_g.h>

namespace mfgunlock::hdrcompat {

inline bool IsHudSeparationResource(const sl::ResourceTag& tag) {
  if (tag.structVersion != sl::kStructVersion1 || tag.resource == nullptr) return false;
  return tag.type == sl::kBufferTypeHUDLessColor ||
         tag.type == sl::kBufferTypeUIColorAndAlpha ||
         tag.type == sl::kBufferTypeUIAlpha;
}

inline bool HasHudSeparationResources(const sl::ResourceTag* tags, uint32_t count) {
  if (tags == nullptr) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (IsHudSeparationResource(tags[i])) return true;
  }
  return false;
}

inline bool BuildAdvancedOptions(const sl::DLSSGOptions& source,
                                 sl::DLSSGOptions& destination,
                                 uint32_t generated_frames,
                                 bool override_generated_frames,
                                 bool enable_ui_recomposition,
                                 bool enable_dynamic_mfg,
                                 float dynamic_target_frame_rate) {
  const size_t version = source.structVersion;
  if (version < sl::kStructVersion1 || version > sl::kStructVersion5) return false;
  if (dynamic_target_frame_rate < 0.0f || dynamic_target_frame_rate > 1000.0f)
    return false;

  destination = sl::DLSSGOptions{};
  destination.next = source.next;
  const size_t required_version = enable_dynamic_mfg
                                      ? size_t{sl::kStructVersion5}
                                      : (enable_ui_recomposition
                                             ? size_t{sl::kStructVersion4}
                                             : size_t{sl::kStructVersion1});
  destination.structVersion = (std::max)(version, required_version);

  // Version 1.
  destination.mode = enable_dynamic_mfg && source.mode != sl::DLSSGMode::eOff
                         ? sl::DLSSGMode::eDynamic
                         : source.mode;
  destination.numFramesToGenerate =
      override_generated_frames ? generated_frames : source.numFramesToGenerate;
  destination.flags = source.flags;
  destination.dynamicResWidth = source.dynamicResWidth;
  destination.dynamicResHeight = source.dynamicResHeight;
  destination.numBackBuffers = source.numBackBuffers;
  destination.mvecDepthWidth = source.mvecDepthWidth;
  destination.mvecDepthHeight = source.mvecDepthHeight;
  destination.colorWidth = source.colorWidth;
  destination.colorHeight = source.colorHeight;
  destination.colorBufferFormat = source.colorBufferFormat;
  destination.mvecBufferFormat = source.mvecBufferFormat;
  destination.depthBufferFormat = source.depthBufferFormat;
  destination.hudLessBufferFormat = source.hudLessBufferFormat;
  destination.uiBufferFormat = source.uiBufferFormat;
  destination.onErrorCallback = source.onErrorCallback;

  if (version >= sl::kStructVersion2) destination.bReserved15 = source.bReserved15;
  if (version >= sl::kStructVersion3)
    destination.queueParallelismMode = source.queueParallelismMode;
  destination.enableUserInterfaceRecomposition =
      enable_ui_recomposition ? sl::eTrue :
      (version >= sl::kStructVersion4
           ? source.enableUserInterfaceRecomposition
           : sl::eFalse);
  if (version >= sl::kStructVersion5)
    destination.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
  if (enable_dynamic_mfg)
    destination.dynamicTargetFrameRate = dynamic_target_frame_rate;
  return true;
}

inline bool BuildUiRecompositionOptions(const sl::DLSSGOptions& source,
                                        sl::DLSSGOptions& destination,
                                        uint32_t generated_frames,
                                        bool override_generated_frames) {
  return BuildAdvancedOptions(source, destination, generated_frames,
                              override_generated_frames, true, false, 0.0f);
}

inline uint32_t SuppressHudSeparationResources(sl::ResourceTag* tags, uint32_t count) {
  if (tags == nullptr) return 0;
  uint32_t suppressed = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!IsHudSeparationResource(tags[i])) continue;
    tags[i].resource = nullptr;
    ++suppressed;
  }
  return suppressed;
}

}  // namespace mfgunlock::hdrcompat
