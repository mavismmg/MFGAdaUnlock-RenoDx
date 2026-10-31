// SPDX-License-Identifier: MIT
// DLSS-G overlay (plan 6.6): the MFG Unlock panel of the DLSS-G backend, its
// status texts and controls, the diagnostics report and the backend selector.
//
// A fragment of addon.cpp, included inside its anonymous namespace: it uses
// the configuration and state declared there and is not a standalone header.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "addon_overlay.inl is included only by addon.cpp"
#endif

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kUiPositive{0.35f, 0.82f, 0.47f, 1.0f};
constexpr ImVec4 kUiWarning{1.0f, 0.76f, 0.25f, 1.0f};
constexpr ImVec4 kUiError{1.0f, 0.38f, 0.34f, 1.0f};
constexpr ImVec4 kUiMuted{0.62f, 0.65f, 0.70f, 1.0f};

void HelpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("[?]");
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void SetNextItemWidthWithHelp(const char* visible_label) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float trailing_width =
      ImGui::CalcTextSize(visible_label).x + style.ItemInnerSpacing.x +
      style.ItemSpacing.x + ImGui::CalcTextSize("[?]").x;
  ImGui::SetNextItemWidth(
      (std::max)(1.0f, ImGui::GetContentRegionAvail().x - trailing_width));
}

void TextDisabledWrapped(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void StatusRow(const char* label, const char* value, const ImVec4& color) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", value);
  ImGui::PopStyleColor();
}

void StatusSectionRow(const char* label) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::Separator();
}

const char* SourceCapConfigOriginText(
    mfgunlock::pacing::SourceCapConfigOrigin origin) {
  using Origin = mfgunlock::pacing::SourceCapConfigOrigin;
  switch (origin) {
    case Origin::kConfigured:
      return "Reflex output cap (compatibility key ReflexSourceFpsCap)";
    case Origin::kLegacyDynamic:
      return "migrated in memory from DynamicReflexSourceCap";
    case Origin::kLegacyFixedOutput:
      return "migrated in memory from FixedOutputFpsCap";
    default:
      return "default/off";
  }
}

const char* UserSourceCapStatusText(
    mfgunlock::framecount::internal::UserSourceCapStatus status) {
  using Status =
      mfgunlock::framecount::internal::UserSourceCapStatus;
  switch (status) {
    case Status::kInactiveGameControlled:
      return "Waiting for fresh driver-confirmed MFG (or select fixed/Dynamic MFG)";
    case Status::kSleepNotNative:
      return "Unavailable: Reflex sleep bypassed/replaced; configured cap is not guaranteed";
    case Status::kWaitingForDynamic:
      return "Waiting for Dynamic MFG to become active";
    case Status::kWaitingForReflex:
      return "Pending: game has not exposed Reflex options";
    case Status::kPending:
      return "Pending application at the next safe Reflex update";
    case Status::kActive:
      return "Active";
    case Status::kNativeLimitStricter:
      return "Active; the game's stricter native cap wins";
    case Status::kRejected:
      return "Rejected by Reflex; native settings restored";
    default:
      return "Off";
  }
}

const ImVec4& UserSourceCapColor(
    mfgunlock::framecount::internal::UserSourceCapStatus status) {
  using Status =
      mfgunlock::framecount::internal::UserSourceCapStatus;
  if (status == Status::kActive ||
      status == Status::kNativeLimitStricter)
    return kUiPositive;
  if (status == Status::kRejected) return kUiError;
  if (status == Status::kOff) return kUiMuted;
  return kUiWarning;
}

std::string UserSourceCapSummary(
    const mfgunlock::framecount::internal::UserSourceCapState& state) {
  if (state.configured_fps == 0) return "0 is Off.";
  std::ostringstream summary;
  summary << "Targets up to ~" << state.configured_fps
          << " displayed FPS";
  if (state.estimated_multiplier >= 2 &&
      state.estimated_multiplier <= 6) {
    const float estimated_source =
        static_cast<float>(state.configured_fps) /
        static_cast<float>(state.estimated_multiplier);
    summary << "; ~" << std::fixed << std::setprecision(1)
            << estimated_source << " game-rendered FPS at "
            << state.estimated_multiplier << "x";
  } else {
    summary << "; base FPS depends on the active multiplier";
  }
  return summary.str();
}

std::string AdaptiveComponentStatus(
    mfgunlock::adaptivequality::ComponentVersion expected,
    mfgunlock::adaptivequality::ComponentVersion actual, bool applied) {
  using Version = mfgunlock::adaptivequality::ComponentVersion;
  if (!applied || actual == Version::kNative) return "Native / not applied";
  if (actual == Version::kMixed)
    return "Mixed versions across providers; execution unverified";
  const std::string version =
      mfgunlock::adaptivequality::ComponentVersionName(actual);
  return version + (actual == expected ? " applied; execution unverified"
                                       : " fallback; execution unverified");
}

const ImVec4& AdaptiveComponentColor(
    mfgunlock::adaptivequality::ComponentVersion expected,
    mfgunlock::adaptivequality::ComponentVersion actual, bool applied) {
  using Version = mfgunlock::adaptivequality::ComponentVersion;
  if (!applied || actual == Version::kNative || actual == Version::kMixed)
    return kUiWarning;
  return actual == expected ? kUiPositive : kUiWarning;
}

const char* InputFormatApiName(uint32_t api) {
  switch (api) {
    case 1: return "DXGI";
    case 2: return "Vulkan";
    default: return "Unknown";
  }
}

const char* InputNativeApiName(mfgunlock::inputdiag::NativeApi api) {
  switch (api) {
    case mfgunlock::inputdiag::NativeApi::D3D11: return "D3D11";
    case mfgunlock::inputdiag::NativeApi::D3D12: return "D3D12";
    case mfgunlock::inputdiag::NativeApi::Vulkan: return "Vulkan";
    default: return "Unknown";
  }
}

const char* InputColorSpaceName(uint32_t value) {
  switch (static_cast<reshade::api::color_space>(value)) {
    case reshade::api::color_space::srgb: return "sRGB";
    case reshade::api::color_space::scrgb: return "scRGB linear";
    case reshade::api::color_space::hdr10_pq: return "HDR10 PQ";
    case reshade::api::color_space::hdr10_hlg: return "HDR10 HLG";
    default: return "Unknown";
  }
}

const char* InputBooleanName(int value) {
  switch (value) {
    case 0: return "No";
    case 1: return "Yes";
    default: return "Unspecified";
  }
}

const char* InputDlssgModeName(uint32_t value) {
  switch (static_cast<sl::DLSSGMode>(value)) {
    case sl::DLSSGMode::eOff: return "Off";
    case sl::DLSSGMode::eOn: return "On";
    case sl::DLSSGMode::eAuto: return "Auto";
    case sl::DLSSGMode::eDynamic: return "Dynamic";
    default: return "Unknown";
  }
}

const char* InputLifecycleName(uint32_t value) {
  switch (value) {
    case sl::eOnlyValidNow: return "Now";
    case sl::eValidUntilPresent: return "Until Present";
    case sl::eValidUntilEvaluate: return "Until Evaluate";
    default: return "Unknown";
  }
}

bool KnownInputFloat(float value) {
  return std::isfinite(value) && value != sl::INVALID_FLOAT;
}

std::string FormatMemoryBytes(uint64_t bytes) {
  if (bytes == 0) return "Not reported";
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(bytes >= (1ull << 30) ? 2 : 0)
         << (bytes >= (1ull << 30)
                 ? static_cast<double>(bytes) / static_cast<double>(1ull << 30)
                 : static_cast<double>(bytes) / static_cast<double>(1ull << 20))
         << (bytes >= (1ull << 30) ? " GiB" : " MiB");
  return stream.str();
}

const char* VramReadinessText(mfgunlock::memorypolicy::EstimateReadiness value) {
  using mfgunlock::memorypolicy::EstimateReadiness;
  switch (value) {
    case EstimateReadiness::kReady: return "Ready";
    case EstimateReadiness::kMissingOptions: return "Waiting for DLSS-G options";
    case EstimateReadiness::kMissingColor: return "Waiting for output metadata";
    case EstimateReadiness::kMissingMotionVectors: return "Waiting for motion-vector metadata";
    case EstimateReadiness::kMissingDepth: return "Waiting for depth metadata";
    case EstimateReadiness::kMissingBackBuffers: return "Waiting for swapchain metadata";
    case EstimateReadiness::kMissingUiInputs: return "Waiting for UI resource metadata";
    default: return "Waiting for inputs";
  }
}

mfgunlock::memorypolicy::TextureInfo MemoryTextureInfo(
    const mfgunlock::inputdiag::TagSummary& tag) {
  mfgunlock::memorypolicy::TextureInfo result{};
  result.present = tag.has_resource;
  result.width = tag.native_desc_seen ? tag.native_desc_width
                                      : (tag.width != 0 ? tag.width
                                                        : tag.resource_width);
  result.height = tag.native_desc_seen ? tag.native_desc_height
                                       : (tag.height != 0 ? tag.height
                                                          : tag.resource_height);
  result.format = tag.native_desc_seen ? tag.native_desc_format
                                       : tag.native_format;
  result.lifecycle = tag.lifecycle;
  return result;
}

void QueueProviderVramEstimateFromCapture() {
  // NVIDIA documents this as an occasional, relatively expensive query. Keep
  // it user-visible and exact-stack-only; never run it continuously in Present.
  if (g_is_star_wars_outlaws ||
      !mfgunlock::framecount::g_dlssg_dynamic_version_seen.load(
          std::memory_order_acquire) ||
      !mfgunlock::framecount::g_streamline_dynamic_version_active.load(
          std::memory_order_acquire)) {
    mfgunlock::framecount::g_vram_estimate_status.store(
        static_cast<unsigned int>(
            mfgunlock::framecount::VramEstimateStatus::kUnsupported),
        std::memory_order_release);
    return;
  }

  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  mfgunlock::inputdiag::OutputSnapshot output{};
  const bool output_valid = mfgunlock::inputdiag::SnapshotOutput(output);
  for (size_t index = 0; index < count; ++index) {
    const auto& snapshot = snapshots[index];
    const auto& options = snapshot.forwarded_options;
    if (!options.valid || options.calls == 0) continue;
    mfgunlock::memorypolicy::EstimateInputs input{};
    input.viewport = snapshot.viewport;
    input.options_valid = true;
    input.mode = options.mode;
    input.generated_frames = options.generated_frames;
    input.flags = options.flags;
    input.dynamic_width = options.dynamic_width;
    input.dynamic_height = options.dynamic_height;
    input.back_buffers = options.back_buffers != 0
        ? options.back_buffers
        : g_vram_swapchain_buffers.load(std::memory_order_relaxed);
    input.mvec_depth_width = options.mvec_depth_width;
    input.mvec_depth_height = options.mvec_depth_height;
    input.color_width = options.color_width;
    input.color_height = options.color_height;
    input.color_format = options.color_format;
    input.mvec_format = options.mvec_format;
    input.depth_format = options.depth_format;
    input.hudless_format = options.hudless_format;
    input.ui_format = options.ui_format;
    input.queue_parallelism_mode = options.queue_parallelism_mode;
    input.ui_recomposition = options.ui_recomposition;
    input.dynamic_target_fps = KnownInputFloat(options.dynamic_target_fps)
        ? options.dynamic_target_fps : 0.0f;
    if (output_valid) {
      input.swapchain = {output.width, output.height, output.format,
                         static_cast<uint32_t>(sl::eValidUntilPresent), true};
    }
    input.motion = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::MotionVectors)]);
    input.depth = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::Depth)]);
    input.hudless = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::Hudless)]);
    input.ui = MemoryTextureInfo(snapshot.tags[
        static_cast<size_t>(mfgunlock::inputdiag::Kind::UiColorAlpha)]);
    if (!input.ui.present) {
      input.ui = MemoryTextureInfo(snapshot.tags[
          static_cast<size_t>(mfgunlock::inputdiag::Kind::UiAlpha)]);
    }
    const auto plan = mfgunlock::memorypolicy::BuildEstimatePlan(input);
    if (plan.readiness == mfgunlock::memorypolicy::EstimateReadiness::kReady)
      mfgunlock::framecount::QueueVramEstimate(snapshot.viewport, plan);
    else
      mfgunlock::framecount::SetVramEstimateWaiting(plan.readiness,
                                                    plan.volatile_inputs);
    return;
  }
  mfgunlock::framecount::SetVramEstimateWaiting(
      mfgunlock::memorypolicy::EstimateReadiness::kMissingOptions);
}

void AppendInputDiagnosticsReport(std::ostringstream& report) {
  report << "Streamline input resource capture: "
         << (mfgunlock::inputdiag::g_enabled.load(std::memory_order_relaxed)
                 ? "Enabled" : "Disabled")
         << " (original game tags before addon filtering; no pixel data)\n";
  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  report << "Input capture skipped/truncated: "
         << mfgunlock::inputdiag::g_dropped_contention.load(std::memory_order_relaxed)
         << '/' << mfgunlock::inputdiag::g_truncated_batches.load(
                        std::memory_order_relaxed) << '\n';
  mfgunlock::inputdiag::OutputSnapshot output{};
  if (mfgunlock::inputdiag::SnapshotOutput(output)) {
    report << "Primary swapchain: " << output.width << 'x' << output.height
           << ", format " << output.format << ", color space "
           << InputColorSpaceName(output.color_space) << " ("
           << output.color_space << "), samples/changes " << output.samples
           << '/' << output.changes << '\n';
  } else {
    report << "Primary swapchain: no complete sample\n";
  }
  for (size_t viewport = 0; viewport < count; ++viewport) {
    const auto& snapshot = snapshots[viewport];
    if (snapshot.tag_batches == 0 && snapshot.constants.calls == 0) continue;
    report << "Input viewport " << snapshot.viewport << " ("
           << InputNativeApiName(snapshot.native_api) << "; "
           << InputFormatApiName(snapshot.format_api) << " formats): "
           << snapshot.tag_batches << " tag batches, "
           << snapshot.frame_aware_batches << " frame-aware\n";
    const auto append_options = [&report](
        const char* label,
        const mfgunlock::inputdiag::OptionsSummary& options) {
      if (options.calls == 0) return;
      report << "  " << label << " DLSS-G options: " << options.calls
             << " calls, " << options.changes << " changes, "
             << options.invalid_base_metadata << " invalid";
      if (!options.valid) {
        report << ", no valid sample\n";
        return;
      }
      report << ", struct v" << options.struct_version
             << ", mode " << InputDlssgModeName(options.mode)
             << ", generated frames " << options.generated_frames
             << " (" << (options.generated_frames + 1) << "x)"
             << ", flags 0x" << std::hex << options.flags << std::dec
             << ", color " << options.color_width << 'x'
             << options.color_height << " format " << options.color_format
             << ", MV/depth " << options.mvec_depth_width << 'x'
             << options.mvec_depth_height
             << ", formats MV/depth/HUD-less/UI "
             << options.mvec_format << '/' << options.depth_format << '/'
             << options.hudless_format << '/' << options.ui_format
             << ", UI recomposition "
             << InputBooleanName(options.ui_recomposition);
      if (KnownInputFloat(options.dynamic_target_fps))
        report << ", dynamic target " << options.dynamic_target_fps;
      report << '\n';
    };
    append_options("Game", snapshot.game_options);
    append_options("Forwarded", snapshot.forwarded_options);
    for (size_t index = 0; index < mfgunlock::inputdiag::kKindCount; ++index) {
      const auto& tag = snapshot.tags[index];
      if (tag.sets == 0 && tag.clears == 0) continue;
      report << "  " << mfgunlock::inputdiag::KindName(
          static_cast<mfgunlock::inputdiag::Kind>(index)) << ": "
          << "tag extent " << tag.width << 'x' << tag.height
          << ", Streamline resource " << tag.resource_width << 'x'
          << tag.resource_height << " format " << tag.native_format;
      if (tag.native_desc_seen) {
        report << ", native desc " << tag.native_desc_width << 'x'
               << tag.native_desc_height << " format "
               << tag.native_desc_format;
      } else {
        report << ", native desc unavailable";
      }
      report << ", lifecycle " << InputLifecycleName(tag.lifecycle)
          << ", last state " << (tag.has_resource ? "set" : "cleared")
          << ", sets/clears " << tag.sets << '/' << tag.clears
          << ", invalid base metadata " << tag.invalid_base_metadata
          << ", metadata/native-desc/identity changes "
          << tag.metadata_changes << '/' << tag.native_desc_changes << '/'
          << tag.native_identity_changes;
      if (tag.frame_known) report << ", last frame " << tag.last_frame;
      report << '\n';
    }
    const auto& constants = snapshot.constants;
    if (constants.calls != 0) {
      report << "  Constants: " << constants.calls << " calls, game resets "
             << constants.game_reset_true << ", invalid base metadata "
             << constants.invalid_base_metadata
             << ", struct v" << constants.struct_version
             << ", last frame " << constants.last_frame
             << ", depth inverted " << InputBooleanName(constants.depth_inverted)
             << ", camera motion " << InputBooleanName(constants.camera_motion_included)
             << ", MV 3D/dilated/jittered "
             << InputBooleanName(constants.motion_vectors_3d) << '/'
             << InputBooleanName(constants.motion_vectors_dilated) << '/'
             << InputBooleanName(constants.motion_vectors_jittered) << '\n';
      if (KnownInputFloat(constants.mvec_scale_x) &&
          KnownInputFloat(constants.mvec_scale_y))
        report << "  Motion-vector scale: " << constants.mvec_scale_x
               << ", " << constants.mvec_scale_y << '\n';
      if (KnownInputFloat(constants.jitter_x) &&
          KnownInputFloat(constants.jitter_y))
        report << "  Jitter: " << constants.jitter_x << ", "
               << constants.jitter_y << '\n';
    }
  }
}

void DrawInputDiagnosticsPanel() {
  if (!ImGui::TreeNode("Streamline input resources (read-only)")) return;
  bool enabled = mfgunlock::inputdiag::g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Capture resource metadata this session", &enabled)) {
    if (enabled) mfgunlock::inputdiag::Clear();
    mfgunlock::inputdiag::g_enabled.store(enabled, std::memory_order_release);
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear input samples")) mfgunlock::inputdiag::Clear();
  ImGui::TextDisabled(
      "Game submissions before Quality Guard; no pixel reads or saved addresses.");
  ImGui::TextDisabled(
      "Relevant tag types may also be used by other Streamline features.");
  ImGui::TextDisabled(
      "Native format is API-specific and does not establish scRGB, PQ or SDR encoding.");
  ImGui::TextDisabled("Skipped lock/truncated batches: %llu / %llu.",
      mfgunlock::inputdiag::g_dropped_contention.load(std::memory_order_relaxed),
      mfgunlock::inputdiag::g_truncated_batches.load(std::memory_order_relaxed));
  mfgunlock::inputdiag::OutputSnapshot output{};
  if (mfgunlock::inputdiag::SnapshotOutput(output)) {
    ImGui::TextDisabled(
        "Primary swapchain: %ux%u, format %u, %s; samples/changes %llu/%llu.",
        output.width, output.height, output.format,
        InputColorSpaceName(output.color_space), output.samples,
        output.changes);
  } else {
    ImGui::TextDisabled("Primary swapchain: waiting for a complete sample.");
  }
  if (mfgunlock::inputdiag::g_viewport_overflow.load(std::memory_order_relaxed))
    ImGui::TextColored(kUiWarning, "More than eight viewports; capture incomplete.");
  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  for (size_t viewport = 0; viewport < count; ++viewport) {
    const auto& snapshot = snapshots[viewport];
    if (snapshot.tag_batches == 0 && snapshot.constants.calls == 0) continue;
    ImGui::PushID(static_cast<int>(snapshot.viewport));
    if (ImGui::TreeNode("viewport", "Viewport %u (%s; %s formats): %llu tag batches",
                        snapshot.viewport,
                        InputNativeApiName(snapshot.native_api),
                        InputFormatApiName(snapshot.format_api),
                        snapshot.tag_batches)) {
      ImGui::TextDisabled("Frame-aware tag batches: %llu", snapshot.frame_aware_batches);
      const auto draw_options = [](
          const char* label,
          const mfgunlock::inputdiag::OptionsSummary& options) {
        if (options.calls == 0) return;
        if (!options.valid) {
          ImGui::TextDisabled("%s options: %llu calls; no valid sample (%llu invalid).",
                              label, options.calls,
                              options.invalid_base_metadata);
          return;
        }
        ImGui::Text(
            "%s options v%u: %s, %u generated (%ux), flags 0x%X",
            label, options.struct_version, InputDlssgModeName(options.mode),
            options.generated_frames, options.generated_frames + 1,
            options.flags);
        ImGui::TextDisabled(
            "Color %ux%u fmt %u | MV/depth %ux%u | formats MV/depth/HUD/UI %u/%u/%u/%u",
            options.color_width, options.color_height, options.color_format,
            options.mvec_depth_width, options.mvec_depth_height,
            options.mvec_format, options.depth_format,
            options.hudless_format, options.ui_format);
        ImGui::TextDisabled(
            "UI recomposition %s | calls %llu | changes %llu | invalid %llu",
            InputBooleanName(options.ui_recomposition), options.calls,
            options.changes, options.invalid_base_metadata);
        if (KnownInputFloat(options.dynamic_target_fps))
          ImGui::TextDisabled("Dynamic target: %.4g FPS",
                              options.dynamic_target_fps);
      };
      draw_options("Game", snapshot.game_options);
      draw_options("Forwarded", snapshot.forwarded_options);
      for (size_t index = 0; index < mfgunlock::inputdiag::kKindCount; ++index) {
        const auto& tag = snapshot.tags[index];
        if (tag.sets == 0 && tag.clears == 0) continue;
        ImGui::Separator();
        ImGui::Text("%s: tag %ux%u; Streamline resource %ux%u, format %u",
                    mfgunlock::inputdiag::KindName(
                        static_cast<mfgunlock::inputdiag::Kind>(index)),
                    tag.width, tag.height, tag.resource_width,
                    tag.resource_height, tag.native_format);
        if (tag.native_desc_seen) {
          ImGui::TextDisabled("Native desc: %ux%u, format %u",
                              tag.native_desc_width, tag.native_desc_height,
                              tag.native_desc_format);
        } else {
          ImGui::TextDisabled("Native desc unavailable for this API/resource.");
        }
        ImGui::TextDisabled(
            "Sets %llu | clears %llu | last state %s | lifecycle %s | base concerns %llu",
            tag.sets, tag.clears, tag.has_resource ? "set" : "cleared",
            InputLifecycleName(tag.lifecycle), tag.invalid_base_metadata);
        ImGui::TextDisabled(
            "Metadata changes %llu | native-desc changes %llu | native identity changes %llu",
            tag.metadata_changes, tag.native_desc_changes,
            tag.native_identity_changes);
        if (tag.frame_known)
          ImGui::TextDisabled("Last frame token: %u", tag.last_frame);
      }
      const auto& constants = snapshot.constants;
      if (constants.calls != 0) {
        ImGui::Separator();
        ImGui::Text("Constants v%u: %llu calls, %llu game reset flags",
                    constants.struct_version, constants.calls,
                    constants.game_reset_true);
        ImGui::TextDisabled(
            "Frame %u | depth inverted %s | camera motion %s | MV 3D/dilated/jittered %s/%s/%s",
            constants.last_frame, InputBooleanName(constants.depth_inverted),
            InputBooleanName(constants.camera_motion_included),
            InputBooleanName(constants.motion_vectors_3d),
            InputBooleanName(constants.motion_vectors_dilated),
            InputBooleanName(constants.motion_vectors_jittered));
        if (KnownInputFloat(constants.mvec_scale_x) &&
            KnownInputFloat(constants.mvec_scale_y))
          ImGui::TextDisabled("MV scale: %.4g, %.4g", constants.mvec_scale_x,
                              constants.mvec_scale_y);
        if (KnownInputFloat(constants.jitter_x) &&
            KnownInputFloat(constants.jitter_y))
          ImGui::TextDisabled("Jitter: %.4g, %.4g", constants.jitter_x,
                              constants.jitter_y);
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  ImGui::TreePop();
}

void SettingLabel(const char* label, const char* summary, const char* tooltip,
                  const char* badge = nullptr,
                  const ImVec4& badge_color = kUiMuted) {
  ImGui::AlignTextToFramePadding();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+(std::max)(1.f,ImGui::GetContentRegionAvail().x));
  ImGui::TextUnformatted(label); ImGui::PopTextWrapPos();
  mfgunlock::ui::Tooltip(tooltip);
  if (badge != nullptr) {
    ImGui::SameLine();
    ImGui::TextColored(badge_color, "%s", badge);
  }
  if (summary != nullptr) TextDisabledWrapped(summary);
}

std::string PackedVersionString(uint64_t version, bool seen) {
  if (!seen) return "Not detected";
  std::ostringstream stream;
  stream << ((version >> 48u) & 0xffffu) << '.'
         << ((version >> 32u) & 0xffffu) << '.'
         << ((version >> 16u) & 0xffffu) << '.'
         << (version & 0xffffu);
  return stream.str();
}

const mfgunlock::nvapistatus::Snapshot& ObserveNvapiUiStatus(
    reshade::api::effect_runtime* runtime) {
  static mfgunlock::nvapistatus::Snapshot cached{};
  static ULONGLONG next_sample = 0;
  const ULONGLONG now = GetTickCount64();
  if (now < next_sample) return cached;
  next_sample = now + 500;

  IUnknown* native_device = nullptr;
  if (runtime != nullptr) {
    if (auto* device = runtime->get_device(); device != nullptr) {
      const auto api = device->get_api();
      if ((api == reshade::api::device_api::d3d11 ||
           api == reshade::api::device_api::d3d12) &&
          device->get_native() != 0) {
        native_device = reinterpret_cast<IUnknown*>(
            static_cast<uintptr_t>(device->get_native()));
      }
    }
  }
  cached = mfgunlock::nvapistatus::Observe(native_device,
                                            GetCurrentProcessId());
  return cached;
}

std::string NgxPresetText(uint32_t preset) {
  std::string result;
  if (preset >= 1 && preset <= 26) {
    result = "Preset ";
    result.push_back(static_cast<char>('A' + preset - 1));
  } else if (preset == 0x00fffffeu) {
    result = "Default preset";
  } else if (preset == 0x00ffffffu) {
    result = "Latest preset";
  } else if (preset == 0) {
    result = "Game/provider controlled";
  } else {
    result = "Unknown (" + std::to_string(preset) + ')';
  }
  return result;
}

const char* NgxPerformanceModeText(uint32_t mode) {
  switch (mode) {
    case 0:
      return "Performance";
    case 1:
      return "Balanced";
    case 2:
      return "Quality";
    case 3:
      return "Game controlled";
    case 4:
      return "DLAA";
    case 5:
      return "Ultra Performance";
    case 6:
      return "Custom";
    default:
      return "Unknown mode";
  }
}

std::string ReconstructionPresetText(
    const mfgunlock::nvapistatus::Snapshot& status, bool ray_reconstruction) {
  using namespace mfgunlock::nvapistatus;
  if (status.ngx_override_status != kOk) return "Not reported";

  const uint64_t feedback = ray_reconstruction
                                ? status.ngx.feedback_ray_reconstruction
                                : status.ngx.feedback_super_resolution;
  if (feedback == 0) return "Not detected";
  const bool active = NgxFeatureActive(feedback);
  const bool configured = NgxFeatureConfigured(feedback);
  if (!active && !configured) return "Not active";

  std::ostringstream stream;
  bool wrote_detail = false;
  if (!active) {
    stream << "Configured";
    wrote_detail = true;
  }
  if ((feedback & kNgxOverridePerformanceMode) != 0) {
    stream << NgxPerformanceModeText(status.ngx.performance_mode);
    wrote_detail = true;
  }
  if ((feedback & kNgxOverrideScalingRatio) != 0 &&
      std::isfinite(status.ngx.scaling_ratio) &&
      status.ngx.scaling_ratio > 0.0f) {
    if (wrote_detail) stream << " | ";
    stream << std::fixed << std::setprecision(1)
           << status.ngx.scaling_ratio * 100.0f << "% render scale";
    wrote_detail = true;
  }
  if ((feedback & kNgxOverridePreset) != 0) {
    if (wrote_detail) stream << " | ";
    stream << NgxPresetText(status.ngx.render_preset)
           << " (NVIDIA override)";
    wrote_detail = true;
  }
  if (!wrote_detail) return "Active; game/provider controlled";
  return stream.str();
}

std::string FrameGenerationPresetText(
    const mfgunlock::nvapistatus::Snapshot& status, bool dlssg_seen) {
  using namespace mfgunlock::nvapistatus;
  if (status.ngx_override_status != kOk) return "Not reported";

  const uint64_t feedback = status.ngx.feedback_frame_generation;
  if (feedback == 0 && !dlssg_seen) return "Not detected";
  if ((feedback & kNgxOverridePreset) == 0) {
    if (NgxFeatureActive(feedback) || dlssg_seen)
      return "Game/provider controlled";
    return NgxFeatureConfigured(feedback)
               ? "Configured; waiting for Frame Generation"
               : "Not detected";
  }

  return NgxPresetText(status.ngx.frame_generation_preset) +
         " (NVIDIA override)";
}

// Hero readout while Dynamic MFG is applied: the driver's live multiplier and
// the advertised ceiling, e.g. "3x / 6x". The ceiling alone is still useful
// before the driver reports a live value.
std::string DynamicMultiplierText(unsigned int driver_multiplier,
                                  unsigned int maximum_generated_frames) {
  const std::string live =
      driver_multiplier >= 2 ? std::to_string(driver_multiplier) + "x"
                             : "Auto";
  if (maximum_generated_frames == 0) return live;
  return live + " / " + std::to_string(maximum_generated_frames + 1) + "x";
}

void DrawAdvancedQualityControls(bool adaptive_quality_managed) {
  constexpr const char* kDepthEdgeModes[] = {
      "Game Default", "Mild", "Balanced", "Strong", "Aggressive"};
  int depth_edge_level = static_cast<int>(
      mfgunlock::framecount::g_depth_edge_guard_level.load(
          std::memory_order_relaxed));
  SetNextItemWidthWithHelp("Depth-edge compatibility");
  if (ImGui::Combo("Depth-edge compatibility", &depth_edge_level,
                   kDepthEdgeModes,
                   static_cast<int>(std::size(kDepthEdgeModes)))) {
    mfgunlock::framecount::g_depth_edge_guard_level.store(
        static_cast<unsigned int>(depth_edge_level),
        std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDepthEdgeTuningChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "DepthEdgeGuardLevel", depth_edge_level);
  }
  HelpMarker(
      "Optional compatibility override for the game's linear-depth separation. Leave on Game Default unless a specific game shows unresolved depth-edge artifacts; stronger values may hurt pacing or thin-detail stability.");

  const auto configured_profile =
      mfgunlock::adaptivequality::NormalizeProfile(
          g_configured_adaptive_quality_profile.load(
              std::memory_order_relaxed));
  if (adaptive_quality_managed &&
      configured_profile ==
          mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
      ImGui::TreeNode("Adaptive Quality V3 A/B controls")) {
    ImGui::TextDisabled(
        "Developer comparisons inside the single V3 profile. Requires restart.");
    bool photometric =
        g_configured_adaptive_quality_v3_photometric.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox("Relative luma/chroma confidence", &photometric)) {
      g_configured_adaptive_quality_v3_photometric.store(
          photometric, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3Photometric",
                                photometric ? 1 : 0);
    }
    HelpMarker(
        "Off retains V2 absolute-RGB confidence and arbitration while the rest of V3 remains available.");

    bool directional_border =
        g_configured_adaptive_quality_v3_directional_border.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox("Motion-directional border taper",
                        &directional_border)) {
      g_configured_adaptive_quality_v3_directional_border.store(
          directional_border, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3DirectionalBorder",
                                directional_border ? 1 : 0);
    }
    HelpMarker(
        "Off returns the warp path to V2's symmetric two-pixel border taper.");

    bool oriented_geometry =
        g_configured_adaptive_quality_v3_oriented_geometry.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox("Motion-oriented diagonal geometry",
                        &oriented_geometry)) {
      g_configured_adaptive_quality_v3_oriented_geometry.store(
          oriented_geometry, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3OrientedGeometry",
                                oriented_geometry ? 1 : 0);
    }
    HelpMarker(
        "Off deliberately requests the validated V2 geometry cubin. No additional texture or shared-memory reads are introduced when enabled.");

#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
    ImGui::TextWrapped(
        "Local low-overhead build: Local Stable geometry + V2 Compatibility inpaint. "
        "Confidence history and launch hooks are disabled. Saved temporal research settings are preserved but ignored.");
#else
    constexpr const char* kStabilityModes[] = {
        "Local Stable (3x3 tile)", "Temporal Stable (confidence history)"};
    int stability_mode = static_cast<int>(
        mfgunlock::cudatemporal::NormalizeMode(
            g_configured_adaptive_quality_v3_stability_mode.load(
                std::memory_order_relaxed))) - 1;
    SetNextItemWidthWithHelp("V3.2 silhouette stability");
    if (ImGui::Combo("V3.2 silhouette stability", &stability_mode,
                     kStabilityModes,
                     static_cast<int>(std::size(kStabilityModes)))) {
      const auto selected = mfgunlock::cudatemporal::NormalizeMode(
          static_cast<unsigned int>(stability_mode + 1));
      g_configured_adaptive_quality_v3_stability_mode.store(
          static_cast<unsigned int>(selected), std::memory_order_relaxed);
      reshade::set_config_value(
          nullptr, kConfigSection, "AdaptiveQualityV3StabilityMode",
          static_cast<int>(selected));
    }
    HelpMarker(
        "Temporal Stable stores only one byte of geometry confidence per pixel, direction and symmetric phase bucket. It starts in read-only probe mode and automatically falls back to Local Stable on any CUDA API, module, phase, stream, allocation or 64 MiB validation failure. Requires restart.");
    if (g_configured_adaptive_quality_v3_stability_mode.load(
            std::memory_order_relaxed) !=
        g_adaptive_quality_v3_stability_mode.load(
            std::memory_order_relaxed)) {
      ImGui::TextDisabled("Saved for next launch; restart the game.");
    }

    constexpr const char* kInpaintModes[] = {
        "V2 Compatibility", "Local V3", "Temporal V3"};
    int inpaint_mode = static_cast<int>(
        mfgunlock::cudatemporal::NormalizeInpaintMode(
            g_configured_adaptive_quality_v3_inpaint_mode.load(
                std::memory_order_relaxed)));
    SetNextItemWidthWithHelp("V3.4 inpaint stability");
    if (ImGui::Combo("V3.4 inpaint stability", &inpaint_mode,
                     kInpaintModes,
                     static_cast<int>(std::size(kInpaintModes)))) {
      const auto selected =
          mfgunlock::cudatemporal::NormalizeInpaintMode(
              static_cast<unsigned int>(inpaint_mode));
      g_configured_adaptive_quality_v3_inpaint_mode.store(
          static_cast<unsigned int>(selected), std::memory_order_relaxed);
      reshade::set_config_value(
          nullptr, kConfigSection, "AdaptiveQualityV3InpaintMode",
          static_cast<int>(selected));
    }
    HelpMarker(
        "Temporal V3 is the default when no saved setting exists. Local V3 separates hard rejects without history. Temporal V3 stores one confidence byte per pixel, direction and symmetric phase bucket; color always comes from the current frame. Invalid saved values fall back to V2 Compatibility. Requires restart.");
    if (g_configured_adaptive_quality_v3_inpaint_mode.load(
            std::memory_order_relaxed) !=
        g_adaptive_quality_v3_inpaint_mode.load(
            std::memory_order_relaxed)) {
      ImGui::TextDisabled("Inpaint mode saved for next launch; restart the game.");
    }
#endif
    ImGui::TreePop();
  }

  if (!ImGui::TreeNode("Legacy quality A/B controls")) return;
  ImGui::TextDisabled(
      "For regression testing only. Adaptive Quality preserves these saved choices but ignores them while it is enabled.");
  if (adaptive_quality_managed) {
    ImGui::TextColored(kUiPositive,
                       "Managed by Adaptive Quality Suite");
    ImGui::BeginDisabled();
  }

  bool refined =
      g_configured_quality_refinement.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Legacy smooth-confidence refinement", &refined)) {
    g_configured_quality_refinement.store(refined,
                                          std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalQualityRefinement",
                              refined ? 1 : 0);
  }
  HelpMarker(
      "Earlier smooth-confidence experiment retained for A/B comparisons. Requires restart.");

  bool intermediate =
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Intermediate scatter retention", &intermediate)) {
    g_configured_thin_geometry_intermediate_scatter.store(
        intermediate, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryIntermediateScatter",
                              intermediate ? 1 : 0);
  }
  HelpMarker(
      "Legacy unconditional retention for fences, foliage and other thin geometry. It can increase trails or flicker and is superseded by Adaptive Quality. Requires restart.");

  int boundary_mode = static_cast<int>(
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed));
  constexpr const char* kBoundaryModes[] = {"Off", "Balanced", "Aggressive"};
  SetNextItemWidthWithHelp("Boundary mitigation A/B");
  if (ImGui::Combo("Boundary mitigation A/B", &boundary_mode,
                   kBoundaryModes,
                   static_cast<int>(std::size(kBoundaryModes)))) {
    g_configured_silhouette_guard_mode.store(
        static_cast<unsigned int>(boundary_mode),
        std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "BoundaryArtifactMitigationMode",
                              boundary_mode);
  }
  HelpMarker(
      "Standalone legacy boundary experiment. Adaptive Quality always uses its integrated Balanced asymmetric path instead. Requires restart.");

  bool validated =
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Validated warp blend A/B", &validated)) {
    g_configured_thin_geometry_validated_warp_blend.store(
        validated, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryValidatedWarpBlend",
                              validated ? 1 : 0);
  }
  HelpMarker(
      "Standalone legacy warp validation. Adaptive Quality uses an integrated version with smooth confidence, border safety and candidate arbitration. Requires restart.");

  bool geometry_v2 =
      g_configured_geometry_confidence_v2.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Geometry confidence V2 A/B", &geometry_v2)) {
    g_configured_geometry_confidence_v2.store(geometry_v2,
                                              std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalGeometryConfidenceV2",
                              geometry_v2 ? 1 : 0);
  }
  HelpMarker(
      "Developer comparison for the prior symmetric geometry-confidence path. Requires the legacy refinement and Balanced boundary settings. Requires restart.");

  bool border =
      g_configured_border_confidence.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Border confidence A/B", &border)) {
    g_configured_border_confidence.store(border, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalBorderConfidence",
                              border ? 1 : 0);
  }
  HelpMarker(
      "Developer comparison for the previous standalone two-pixel screen-border taper. Requires restart.");

  bool previous =
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Previous-to-current scatter (unstable)", &previous)) {
    g_configured_thin_geometry_previous_scatter.store(
        previous, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryPreviousScatter",
                              previous ? 1 : 0);
  }
  HelpMarker(
      "Unstable research path retained only for regression testing. It crashed after restart in earlier testing and is never combined with Adaptive Quality. Requires restart.");

  if (adaptive_quality_managed) ImGui::EndDisabled();
  ImGui::TreePop();
}

void DrawLatencyGuardControl() {
  int mode = static_cast<int>(
      mfgunlock::framecount::g_latency_guard_mode.load(
          std::memory_order_relaxed));
  constexpr const char* kModes[] = {
      "Off", "Monitor Only (Recommended)",
      "Automatic Latency Guard (Advanced)"};

  if (ImGui::BeginTable("##latency_guard_control", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch,
                            0.62f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch,
                            0.38f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Latency Guard",
        mode == static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)
            ? "Tests 6x -> 5x -> 4x -> 3x one step at a time when justified."
            : (mode == static_cast<int>(
                          mfgunlock::pacing::LatencyGuardMode::kMonitor)
                   ? "Read-only Reflex and render-queue monitoring."
                   : "No latency sampling or automatic cap."),
        "Monitor Only is read-only and remains the recommended default. Automatic mode requires healthy, fresh Reflex timing and starts only for sustained queue pressure, output saturation, significant DLSS-G workload or at least 60 ms of marker-to-GPU pipeline time. That 60-ms signal is not end-to-end display latency and only starts a measured trial. With an addon-forced 4x-6x selection it tests one lower multiplier at a time, never below 3x, and keeps only measured improvements. DLSS-G provider reconfiguration can cause a brief hitch whenever a trial or periodic recheck changes the multiplier. A small source-FPS trim remains a queue-only fallback. The saved multiplier, Reflex mode, VSync, G-SYNC, Dynamic MFG and Reflex markers are never rewritten.",
        mode == static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)
            ? "Advanced"
            : nullptr,
        kUiWarning);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##latency_guard", &mode, kModes,
                     static_cast<int>(std::size(kModes)))) {
      mfgunlock::framecount::g_latency_guard_mode.store(
          static_cast<unsigned int>(mode), std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_stable_samples.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_clear_samples.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_candidate_cap_fps.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_active_source_cap_fps.store(
          0, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_multiplier_override.store(
          0, std::memory_order_release);
      mfgunlock::framecount::g_latency_guard_multiplier_trial_accepted.store(
          false, std::memory_order_relaxed);
      mfgunlock::framecount::g_latency_guard_auto_cap_ready.store(
          false, std::memory_order_release);
      mfgunlock::framecount::g_latency_guard_refresh_pending.store(
          true, std::memory_order_release);
      mfgunlock::framecount::g_latency_guard_epoch.fetch_add(
          1, std::memory_order_acq_rel);
      // Show the reset at once, without waiting for the next Present.
      mfgunlock::framecount::PublishLatencyGuardView();
      reshade::set_config_value(nullptr, kConfigSection, "LatencyGuardMode",
                                mode);
    }
    ImGui::EndTable();
  }
}

const char* ReflexWaitableStateText(
    mfgunlock::reflexpacing::WaitableState state) {
  using State = mfgunlock::reflexpacing::WaitableState;
  switch (state) {
    case State::kWaitingForSwapchain: return "Waiting for compatible swapchain";
    case State::kProbing: return "Native-sleep probe";
    case State::kActivationPending: return "Probe passed; activation pending";
    case State::kActive: return "DXGI Waitable active";
    case State::kFallback: return "Native-sleep fallback";
    default: return "Native Reflex sleep";
  }
}

const char* ReflexModeText(uint32_t mode) {
  switch (mode) {
    case static_cast<uint32_t>(sl::ReflexMode::eOff): return "Off";
    case static_cast<uint32_t>(sl::ReflexMode::eLowLatency): return "On";
    case static_cast<uint32_t>(sl::ReflexMode::eLowLatencyWithBoost):
      return "On + Boost";
    default: return "Unknown";
  }
}

void NotifyReflexPacingSettingChanged() {
  mfgunlock::reflexpacing::ResetCadence();
  mfgunlock::framecount::g_latency_guard_epoch.fetch_add(
      1, std::memory_order_acq_rel);
  mfgunlock::framecount::g_latency_guard_refresh_pending.store(
      true, std::memory_order_release);
}

void DrawReflexPacingLabControl(reshade::api::effect_runtime* runtime) {
  using namespace mfgunlock;
  reflexpacing::g_ui_heartbeat_ms.store(GetTickCount64(),
                                        std::memory_order_release);
  constexpr const char* kModeOverrides[] = {
      "Game controlled", "Off (skip Reflex sleep; FG stays on)", "Force On",
      "Force On + Boost"};
  int mode_override = static_cast<int>(pacing::NormalizeReflexModeOverride(
      reflexpacing::g_mode_override.load(std::memory_order_relaxed)));
  SetNextItemWidthWithHelp("Reflex mode override");
  if (ImGui::Combo("Reflex mode override", &mode_override, kModeOverrides,
                   static_cast<int>(std::size(kModeOverrides)))) {
    reflexpacing::g_mode_override.store(
        static_cast<uint32_t>(mode_override), std::memory_order_relaxed);
    reflexpacing::g_mode_rejected.store(false, std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection, "ReflexModeOverride",
                              mode_override);
  }
  HelpMarker(
      "Off does not turn Reflex off: DLSS-G needs it, and several games disable Frame Generation when Reflex is off. Streamline and the NVIDIA overlay keep reporting Reflex On; only the Reflex sleep is skipped, while frame tokens and markers are preserved. With DXGI Waitable selected, the waitable pacer replaces that sleep after its safety probe.\n\nWith Frame Generation on, DLSS-G paces its own queue, so Off, On and On + Boost usually measure the same latency (Cyberpunk 2077 at 6x: within 2 ms of each other). Boost only helps when the CPU is the bottleneck and the GPU would otherwise lower its clocks; it costs power and temperature.");
  if (mode_override == static_cast<int>(pacing::ReflexModeOverride::kOff) ||
      mode_override == static_cast<int>(pacing::ReflexModeOverride::kOnBoost)) {
    ImGui::TextDisabled(
        "With Frame Generation on, expect little or no latency change.");
  }

  constexpr const char* kPacingMethods[] = {
      "Native Reflex Sleep", "DXGI Waitable (Experimental)"};
  int pacing_method = static_cast<int>(pacing::NormalizeReflexPacingMethod(
      reflexpacing::g_pacing_method.load(std::memory_order_relaxed)));
  SetNextItemWidthWithHelp("Pacing method");
  if (ImGui::Combo("Pacing method", &pacing_method, kPacingMethods,
                   static_cast<int>(std::size(kPacingMethods)))) {
    reflexpacing::g_pacing_method.store(
        static_cast<uint32_t>(pacing_method), std::memory_order_release);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection, "ReflexPacingMethod",
                              pacing_method);
  }
  HelpMarker(
      "Waitable mode activates only after a 120-frame native probe and only when the game already created a D3D11/D3D12 waitable swapchain. It never injects the flag or recreates the swapchain.");

  bool headroom =
      reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("VRR headroom cap", &headroom)) {
    reflexpacing::g_headroom_enabled.store(headroom,
                                           std::memory_order_relaxed);
    if (!headroom)
      reflexpacing::g_headroom_limit_us.store(0,
                                              std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "ReflexVrrHeadroomEnabled", headroom ? 1 : 0);
  }
  int basis_points = static_cast<int>(pacing::NormalizeHeadroomBasisPoints(
      reflexpacing::g_headroom_basis_points.load(std::memory_order_relaxed)));
  float headroom_percent = static_cast<float>(basis_points) / 100.0f;
  if (!headroom) ImGui::BeginDisabled();
  SetNextItemWidthWithHelp("VRR headroom");
  if (ImGui::SliderFloat("VRR headroom", &headroom_percent, 0.5f, 3.0f,
                         "%.1f%%")) {
    basis_points = std::clamp(
        static_cast<int>(std::lround(headroom_percent * 100.0f)), 50, 300);
    reflexpacing::g_headroom_basis_points.store(
        static_cast<uint32_t>(basis_points), std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "ReflexVrrHeadroomBasisPoints", basis_points);
  }
  if (!headroom) ImGui::EndDisabled();
  HelpMarker(
      "Uses Reflex's final/output limiter and preserves every stricter game, user or Latency Guard cap. It activates only while NVAPI reports VRR/G-SYNC.");

  const auto state = reflexpacing::g_waitable_state.load(
      std::memory_order_acquire);
  const auto reason = reflexpacing::g_fallback_reason.load(
      std::memory_order_relaxed);
  const auto sleep_intervals = reflexpacing::g_sleep_intervals.Stats();
  const auto sleep_durations = reflexpacing::g_sleep_durations.Stats();
  const auto present_intervals = reflexpacing::g_present_intervals.Stats();
  ImGui::Spacing();
  if (ImGui::BeginTable("##reflex_pacing_lab_status", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("Effective", ImGuiTableColumnFlags_WidthStretch,
                            0.55f);
    const bool fg_safe_off =
        pacing::NormalizeReflexModeOverride(
            reflexpacing::g_mode_override.load(std::memory_order_relaxed)) ==
        pacing::ReflexModeOverride::kOff;
    const auto sleep_dispatch = reflexpacing::g_sleep_dispatch.load(
        std::memory_order_relaxed);
    std::string pacing_status = ReflexWaitableStateText(state);
    if (sleep_dispatch == reflexpacing::SleepDispatch::kBypass) {
      pacing_status = "FG-safe Reflex sleep bypass active";
    } else if (sleep_dispatch == reflexpacing::SleepDispatch::kWaitable &&
               fg_safe_off &&
               (state == reflexpacing::WaitableState::kNative ||
                state == reflexpacing::WaitableState::kWaitingForSwapchain ||
                state == reflexpacing::WaitableState::kFallback)) {
      pacing_status += "; FG-safe bypass while waitable unavailable";
    }
    StatusRow("Pacing", pacing_status.c_str(),
              state == reflexpacing::WaitableState::kActive
                  ? kUiPositive
                  : (sleep_dispatch == reflexpacing::SleepDispatch::kBypass
                         ? kUiPositive
                         : (state == reflexpacing::WaitableState::kFallback
                         ? kUiWarning
                         : kUiMuted)));
    const uint32_t native_mode = reflexpacing::g_native_mode.load(
        std::memory_order_relaxed);
    const uint32_t forwarded_mode = reflexpacing::g_forwarded_mode.load(
        std::memory_order_relaxed);
    const std::string reflex_mode = fg_safe_off
        ? std::string(ReflexModeText(native_mode)) + " -> " +
              ReflexModeText(forwarded_mode) +
              " internally; Reflex sleep bypass requested"
        : std::string(ReflexModeText(native_mode)) + " -> " +
              ReflexModeText(forwarded_mode) +
              (reflexpacing::g_mode_rejected.load(std::memory_order_relaxed)
                   ? " (override rejected)"
                   : "");
    StatusRow("Reflex game -> effective", reflex_mode.c_str(),
              reflexpacing::g_mode_rejected.load(std::memory_order_relaxed)
                  ? kUiWarning
                  : (reflexpacing::g_mode_applied.load(
                         std::memory_order_relaxed)
                         ? kUiPositive
                         : kUiMuted));
    // The driver's own view (NvAPI_D3D_GetSleepStatus). It reports low
    // latency mode and whether the game sleeps; it has no Boost field.
    const auto& nvapi_status = ObserveNvapiUiStatus(runtime);
    if (nvapi_status.sleep_status == mfgunlock::nvapistatus::kOk) {
      const std::string driver_reflex =
          std::string(nvapi_status.sleep.low_latency_mode != 0
                          ? "Low latency On"
                          : "Low latency Off") +
          (nvapi_status.sleep.game_sleep != 0 ? "; game sleeps"
                                              : "; no game sleep");
      StatusRow("Driver Reflex", driver_reflex.c_str(), kUiMuted);
    } else {
      StatusRow("Driver Reflex", "Not reported", kUiMuted);
    }
    if (state == reflexpacing::WaitableState::kFallback)
      StatusRow("Fallback reason", ReflexWaitableFallbackText(reason),
                kUiWarning);
    const std::string probe = std::to_string(
        reflexpacing::g_probe_samples.load(std::memory_order_relaxed)) +
        "/120";
    StatusRow("Waitable probe", probe.c_str(), kUiMuted);
    const std::string dxgi_latency = std::to_string(
        reflexpacing::g_native_maximum_latency.load(
            std::memory_order_relaxed)) +
        " -> " + std::to_string(
            reflexpacing::g_effective_maximum_latency.load(
                std::memory_order_relaxed));
    StatusRow("DXGI MaximumFrameLatency", dxgi_latency.c_str(), kUiMuted);
    const std::string wait_health =
        std::string(reflexpacing::g_waitable_handle.load(
                        std::memory_order_acquire) != nullptr
                        ? "Handle ready"
                        : "No active handle") +
        "; timeouts=" +
        std::to_string(reflexpacing::g_timeout_count.load(
            std::memory_order_relaxed)) +
        "; failures=" +
        std::to_string(reflexpacing::g_wait_failure_count.load(
            std::memory_order_relaxed));
    StatusRow("Waitable health", wait_health.c_str(), kUiMuted);
    const uint32_t refresh = reflexpacing::g_refresh_millihz.load(
        std::memory_order_relaxed);
    std::ostringstream display;
    display << std::fixed << std::setprecision(2)
            << static_cast<double>(refresh) / 1000.0 << " Hz; VRR "
            << (reflexpacing::g_vrr_active.load(std::memory_order_relaxed)
                    ? "active"
                    : "inactive/unavailable");
    StatusRow("Display", display.str().c_str(), kUiMuted);
    const uint32_t headroom_limit = reflexpacing::g_headroom_limit_us.load(
        std::memory_order_relaxed);
    const std::string cap = headroom_limit == 0
        ? "Inactive"
        : std::to_string(headroom_limit) + " us (" +
              std::to_string(pacing::FrameLimitUsToFps(headroom_limit)) +
              " FPS approx.)";
    StatusRow("VRR headroom", cap.c_str(),
              headroom_limit != 0 ? kUiPositive : kUiMuted);
    auto cadence_text = [](const reflexpacing::SampleStats& stats) {
      if (stats.count == 0) return std::string("No samples");
      std::ostringstream text;
      text << stats.median_us / 1000.0 << "/" << stats.p95_us / 1000.0
           << "/" << stats.p99_us / 1000.0 << " ms; delta "
           << stats.mean_delta_us / 1000.0 << " ms; n=" << stats.count;
      return text.str();
    };
    const auto sleep_interval_text = cadence_text(sleep_intervals);
    const auto sleep_duration_text = cadence_text(sleep_durations);
    const auto present_interval_text = cadence_text(present_intervals);
    StatusRow("Sleep interval med/p95/p99", sleep_interval_text.c_str(),
              kUiMuted);
    StatusRow("Sleep duration med/p95/p99", sleep_duration_text.c_str(),
              kUiMuted);
    StatusRow("App Present med/p95/p99", present_interval_text.c_str(),
              kUiMuted);
    ImGui::EndTable();
  }
  ImGui::TextDisabled(
      "These are application-cadence proxies, not generated/displayed frames. Validate final output with PresentMon or FrameView.");
}

const char* MarkerHealthText(mfgunlock::pacing::MarkerHealth health) {
  using mfgunlock::pacing::MarkerHealth;
  switch (health) {
    case MarkerHealth::kWaiting:
      return "Waiting for latency frames";
    case MarkerHealth::kHealthy:
      return "Healthy";
    case MarkerHealth::kMissingSleep:
      return "Game is not calling Reflex sleep";
    case MarkerHealth::kIncomplete:
      return "Incomplete Reflex markers";
    case MarkerHealth::kInvalidOrder:
      return "Invalid marker order";
    case MarkerHealth::kUnstableTiming:
      return "Source timing validation failed";
    default:
      return "Unavailable";
  }
}

const char* TimingValidationText(uint32_t issues) {
  using namespace mfgunlock::latency;
  if (issues == kTimingOk) return "Verified from simulation + Present markers";
  if ((issues & kTimingUnitsUnknown) != 0)
    return "Timestamp scale could not be validated";
  if ((issues & kTimingSimulationCadenceInvalid) != 0)
    return "Simulation cadence is missing or outside the valid range";
  if ((issues & kTimingPresentCadenceMissing) != 0)
    return "Not enough consecutive Present markers";
  if ((issues & kTimingPresentCadenceMismatch) != 0)
    return "Present cadence does not match simulation cadence";
  if ((issues & kTimingSimulationCadenceUnstable) != 0)
    return "Simulation cadence is too unstable for automatic control";
  if ((issues & kTimingPresentCadenceUnstable) != 0)
    return "Present cadence is too unstable for automatic control";
  if ((issues & kTimingInsufficientConsecutiveFrames) != 0)
    return "Waiting for 49 consecutive Reflex frames";
  if ((issues & kTimingNotFresh) != 0)
    return "No sufficiently fresh completed-frame window";
  return "Unclassified timing validation failure";
}

const char* ValidationStageName(mfgunlock::validation::Stage stage) {
  using mfgunlock::validation::Stage;
  switch (stage) {
    case Stage::kNativeRequestObserved:
      return "native request observed";
    case Stage::kRequestAccepted:
      return "request accepted";
    case Stage::kDriverMultiplierActive:
      return "driver multiplier active";
    case Stage::kProviderOutputConfirmed:
      return "provider output confirmed";
    case Stage::kRequestRejected:
      return "request rejected; native fallback preserved";
    case Stage::kRuntimeError:
      return "runtime error";
    case Stage::kWaitingForGameRequest:
    default:
      return "waiting for game request";
  }
}

std::string ValidationStatusText(mfgunlock::validation::Stage stage,
                                 bool dynamic_accepted,
                                 bool effective_seen,
                                 unsigned int effective_multiplier,
                                 unsigned int driver_multiplier,
                                 unsigned int provider_presentations) {
  using mfgunlock::validation::Stage;
  switch (stage) {
    case Stage::kRuntimeError:
      return "DLSS-G runtime reported an error";
    case Stage::kRequestRejected:
      return "Addon request rejected; game fallback preserved";
    case Stage::kProviderOutputConfirmed:
      return std::to_string(provider_presentations) +
             " presentation(s) reported by provider";
    case Stage::kDriverMultiplierActive:
      return std::to_string(driver_multiplier) +
             "x live reported by driver";
    case Stage::kRequestAccepted:
      if (dynamic_accepted)
        return "Dynamic request accepted; output telemetry unavailable";
      if (effective_seen)
        return std::to_string(effective_multiplier) +
               "x request accepted; output telemetry unavailable";
      return "Request accepted; output telemetry unavailable";
    case Stage::kNativeRequestObserved:
      return "Game request observed; waiting for runtime result";
    case Stage::kWaitingForGameRequest:
    default:
      return "Waiting for game Frame Generation request";
  }
}

const char* LatencyBottleneckText(
    mfgunlock::pacing::LatencyBottleneck bottleneck) {
  using mfgunlock::pacing::LatencyBottleneck;
  switch (bottleneck) {
    case LatencyBottleneck::kBalanced:
      return "No dominant avoidable stage observed";
    case LatencyBottleneck::kDisplayOversubscription:
      return "Output oversubscription + render queue";
    case LatencyBottleneck::kRenderQueue:
      return "Render queue";
    case LatencyBottleneck::kFrameGeneration:
      return "DLSS-G workload";
    case LatencyBottleneck::kGpuWork:
      return "Base GPU workload";
    case LatencyBottleneck::kCpuOrSource:
      return "CPU/source-frame interval";
    default:
      return "Insufficient fresh timing data";
  }
}

const char* ResponsiveTrialReasonText(
    mfgunlock::latency::ResponsiveTrialReason reason) {
  using mfgunlock::latency::ResponsiveTrialReason;
  switch (reason) {
    case ResponsiveTrialReason::kDisplayOversubscription:
      return "Output saturation";
    case ResponsiveTrialReason::kRenderQueue:
      return "Sustained render queue";
    case ResponsiveTrialReason::kHighInputLatency:
      return "Input-to-GPU latency at or above 60 ms";
    case ResponsiveTrialReason::kHighPipelineLatency:
      return "Estimated marker-to-GPU pipeline at or above 60 ms";
    case ResponsiveTrialReason::kGpuSaturatedLatencyProxy:
      return "GPU saturation + long marker pipeline proxy";
    case ResponsiveTrialReason::kFrameGenerationWorkload:
      return "Significant DLSS-G workload";
    default:
      return "No responsive trial requested";
  }
}

const char* ResponsiveTrialBlockerText(
    mfgunlock::latency::ResponsiveTrialBlocker blocker) {
  using mfgunlock::latency::ResponsiveTrialBlocker;
  switch (blocker) {
    case ResponsiveTrialBlocker::kNone:
      return "Eligible; collecting a stable trigger";
    case ResponsiveTrialBlocker::kMonitorOnly:
      return "Automatic mode is not enabled";
    case ResponsiveTrialBlocker::kReflexUnhealthy:
      return "Waiting for 48 healthy Reflex timing frames";
    case ResponsiveTrialBlocker::kSourceTimingUnverified:
      return "Source timing is stale or unverified";
    case ResponsiveTrialBlocker::kDynamicActive:
      return "Dynamic MFG is active or driver-controlled";
    case ResponsiveTrialBlocker::kFixedMultiplierRequired:
      return "Select a fixed 4x, 5x or 6x multiplier";
    case ResponsiveTrialBlocker::kLiveMultiplierUnconfirmed:
      return "Waiting for the driver to confirm the fixed multiplier";
    case ResponsiveTrialBlocker::kReflexOptionsUnavailable:
      return "The game has not exposed slReflexSetOptions";
    case ResponsiveTrialBlocker::kUserCapPending:
      return "Waiting for the Reflex output cap to be applied once";
    case ResponsiveTrialBlocker::kUserCapRejected:
      return "Output-FPS cap was rejected; native Reflex settings restored";
    case ResponsiveTrialBlocker::kNoTrigger:
      return "No sustained latency, saturation, queue or workload trigger";
    default:
      return "Eligibility unavailable";
  }
}

const char* MultiplierTrialPhaseText(
    mfgunlock::latency::MultiplierTrialPhase phase) {
  using mfgunlock::latency::MultiplierTrialPhase;
  switch (phase) {
    case MultiplierTrialPhase::kCollectingBaseline:
      return "Collecting current-multiplier baseline";
    case MultiplierTrialPhase::kWaitingForCandidate:
      return "Waiting for the next lower multiplier";
    case MultiplierTrialPhase::kMeasuringCandidate:
      return "Measuring the next lower multiplier";
    case MultiplierTrialPhase::kAccepted:
      return "Last lower multiplier measured beneficial";
    case MultiplierTrialPhase::kWaitingForOriginal:
      return "Restoring saved multiplier for periodic check";
    case MultiplierTrialPhase::kMeasuringOriginal:
      return "Rechecking the saved multiplier";
    case MultiplierTrialPhase::kCooldown:
      return "No benefit measured; cooldown active";
    default:
      return "Monitoring; no responsive trial active";
  }
}

ThinGeometryModulePatch SelectRelevantThinGeometryResult(
    const std::vector<ThinGeometryModulePatch>& modules) {
  if (modules.empty()) return {};
  const auto applied = std::find_if(
      modules.rbegin(), modules.rend(), [](const ThinGeometryModulePatch& item) {
        return item.result.validated_warp_blend.applied ||
               item.result.previous_scatter.applied ||
               item.intermediate_scatter.applied ||
               item.silhouette_boundary_guard.applied;
      });
  return applied != modules.rend() ? *applied : modules.back();
}

void DrawLegacyOverlay(reshade::api::effect_runtime* /*runtime*/) {
  bool gate_patched = false;
  bool midpoint_patched = false;
  bool blackwell_patched = false;
  bool thin_geometry_patched = false;
  size_t gate_provider_count = 0;
  size_t gate_site_count = 0;
  size_t midpoint_provider_count = 0;
  size_t blackwell_provider_count = 0;
  size_t thin_geometry_provider_count = 0;
  std::string midpoint_detail;
  std::string blackwell_detail;
  std::string blackwell_applied_detail;
  mfgunlock::blackwell::Result blackwell_active_result{};
  ThinGeometryModulePatch thin_geometry_last;
  bool ceiling_patched = false;
  unsigned int ceiling_compiled = 0;
  unsigned int ceiling_effective = 0;
  size_t flip_meter_site_count = 0;
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    ImGui::TextDisabled("Diagnostics updating; retry next frame.");
    return;
  }
  gate_patched = g_gate_patched.load(std::memory_order_relaxed);
  midpoint_patched = g_midpoint_patched.load(std::memory_order_relaxed);
  blackwell_patched = g_blackwell_patched.load(std::memory_order_relaxed);
  thin_geometry_patched = g_thin_geometry_patched.load(std::memory_order_relaxed);
  gate_provider_count = g_gate_modules.size();
  gate_site_count = g_gate_sites.size();
  midpoint_provider_count = g_midpoint_modules.size();
  blackwell_provider_count = g_blackwell_modules.size();
  thin_geometry_provider_count = g_thin_geometry_modules.size();
  midpoint_detail = g_midpoint_detail;
  blackwell_detail = g_blackwell_detail;
  blackwell_applied_detail = g_blackwell_applied_detail;
  blackwell_active_result = AggregateBlackwellResults(g_blackwell_modules);
  thin_geometry_last =
      SelectRelevantThinGeometryResult(g_thin_geometry_modules);
  ReleaseSRWLockShared(&g_provider_maintenance_lock);
  if (!TryAcquireSRWLockShared(&g_streamline_maintenance_lock)) {
    ImGui::TextDisabled("Diagnostics updating; retry next frame.");
    return;
  }
  ceiling_patched = g_ceiling_patched.load(std::memory_order_relaxed);
  ceiling_compiled = g_ceiling_compiled;
  ceiling_effective = g_ceiling_effective;
  flip_meter_site_count = g_flip_meter_sites.size();
  ReleaseSRWLockShared(&g_streamline_maintenance_lock);

  bool configured_enabled = g_configured_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Enable addon on next restart", &configured_enabled)) {
    g_configured_enabled.store(configured_enabled, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "Enabled",
                              configured_enabled ? 1 : 0);
  }
  const bool enabled = g_enabled.load(std::memory_order_relaxed);
  if (configured_enabled != enabled) {
    ImGui::TextDisabled("Restart required; this session remains %s.",
                        enabled ? "enabled" : "disabled");
  }

  int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
  if (ImGui::SliderInt("Reported MultiFrameCountMax", &count,
                       static_cast<int>(kMinCount), static_cast<int>(kMaxCount))) {
    if (count < static_cast<int>(kMinCount)) count = static_cast<int>(kMinCount);
    if (count > static_cast<int>(kMaxCount)) count = static_cast<int>(kMaxCount);
    g_max_count.store(static_cast<unsigned int>(count), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
  }
  ImGui::TextDisabled(
      "Takes effect when DLSS-G next queries the runtime -- toggle frame\n"
      "generation off and on in the game if the option does not appear.");

  const DetectedRenderApi render_api = g_render_api.load(std::memory_order_relaxed);
  ImGui::Text("Renderer detected by ReShade: %s%s", RenderApiName(render_api),
              render_api == DetectedRenderApi::kVulkan ? " (experimental)" : "");

  constexpr const char* kRuntimeModes[] = {
      "Game default (recommended)",
      "Prefer local runtime - disable OTA",
      "Force NVIDIA OTA runtime"};
  int runtime_mode = static_cast<int>(
      g_configured_runtime_selection_mode.load(std::memory_order_relaxed));
  if (ImGui::Combo("Streamline runtime selection", &runtime_mode, kRuntimeModes,
                   static_cast<int>(std::size(kRuntimeModes)))) {
    g_configured_runtime_selection_mode.store(
        static_cast<unsigned int>(runtime_mode), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "RuntimeSelectionMode",
                              runtime_mode);
  }
  ImGui::TextDisabled(
      "Restart required. Local mode clears Streamline's OTA flags; OTA mode sets them.\n"
      "Use a complete, version-matched Streamline package rather than mixing DLL versions.");
  const int active_runtime_mode = static_cast<int>(
      mfgunlock::framecount::g_runtime_selection_mode.load(
          std::memory_order_relaxed));
  if (runtime_mode != active_runtime_mode) {
    ImGui::TextDisabled("Runtime-selection change is saved for the next restart.");
  }
  if (active_runtime_mode != static_cast<int>(
                                 mfgunlock::framecount::RuntimeSelectionMode::kGameDefault)) {
    if (mfgunlock::framecount::g_runtime_selection_observed.load(
            std::memory_order_acquire)) {
      ImGui::Text("slInit policy: flags 0x%llx -> 0x%llx; result %u.",
                  mfgunlock::framecount::g_runtime_flags_before.load(
                      std::memory_order_relaxed),
                  mfgunlock::framecount::g_runtime_flags_after.load(
                      std::memory_order_relaxed),
                  mfgunlock::framecount::g_runtime_selection_result.load(
                      std::memory_order_relaxed));
    } else {
      ImGui::TextDisabled(
          "The active runtime policy did not reach slInit during this launch.");
      if (!HasEarlyLoadEntry()) {
        ImGui::TextWrapped(
            "This game initialized Streamline before normal ReShade addon loading. "
            "Early loading is required for the selected local/OTA policy.");
        if (ImGui::Button("Enable early addon loading for next restart")) {
          if (EnsureEarlyLoadEntry()) {
            reshade::log::message(
                reshade::log::level::info,
                "mfgunlock: added this addon to ADDON.LoadFromDllMain; restart required for the Streamline runtime-selection hook.");
          } else {
            reshade::log::message(
                reshade::log::level::error,
                "mfgunlock: could not update ADDON.LoadFromDllMain; add renodx-mfgunlock.addon64 manually and restart.");
          }
        }
      } else {
        ImGui::TextDisabled(
            "Early loading is configured, but slInit was not observed; verify that the "
            "configured addon filename matches the loaded file, then restart fully.");
      }
    }
  }

  bool flip_off =
      g_configured_force_flip_meter_off.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Force legacy software flip pacing on next restart (compatibility)",
                      &flip_off)) {
    g_configured_force_flip_meter_off.store(flip_off,
                                             std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", flip_off ? 1 : 0);
  }
  ImGui::TextDisabled(
      "Leave off with current Streamline builds. Enable only if 3x/4x freezes;\n"
      "applied once per session, so restart the game after changing it.");

  ImGui::Separator();
  const bool active_flip_off =
      g_force_flip_meter_off.load(std::memory_order_relaxed);
  if (flip_off != active_flip_off) {
    ImGui::TextDisabled("Legacy pacing change is saved for the next restart.");
  }
  if (!active_flip_off) {
    ImGui::TextDisabled("Native Streamline pacing active; no legacy metering patch requested.");
  } else if (g_flip_meter_patched.load(std::memory_order_acquire)) {
    ImGui::Text("Flip-metering forced off: +0x%x pinned to %u, %zu site(s).",
                g_flip_meter_offset.load(std::memory_order_relaxed),
                g_flip_meter_value.load(std::memory_order_relaxed),
                flip_meter_site_count);
  } else if (g_flip_meter_attempts.load(std::memory_order_relaxed) >=
             kMaxFlipMeterAttempts) {
    // The counter only advances once the plugin HAS been found, and both give-up
    // paths slam it to the maximum -- so this state is "found it, could not patch
    // it", which is the opposite of what this line used to say.
    ImGui::TextDisabled("DLSS-G plugin found, but flip metering could not be patched.");
    ImGui::TextDisabled("See the ReShade log for the module and the step that failed.");
  } else if (g_flip_meter_attempts.load(std::memory_order_relaxed) > 0) {
    ImGui::TextDisabled("DLSS-G plugin found; flip-metering patch pending (%d).",
                        g_flip_meter_attempts.load(std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("DLSS-G plugin not located yet -- turn frame generation on.");
  }
  if (ceiling_patched) {
    ImGui::Text("Streamline device-limit bypassed: compiled %ux, effective %ux.",
                ceiling_compiled + 1, ceiling_effective + 1);
  }
  if (mfgunlock::framecount::g_capacity_advertised.load(std::memory_order_relaxed)) {
    ImGui::Text("Native menu maximum: runtime %ux, advertised %ux.",
                mfgunlock::framecount::g_runtime_max_generated.load(std::memory_order_relaxed) + 1,
                mfgunlock::framecount::g_advertised_max_generated.load(
                    std::memory_order_relaxed) + 1);
  }
  if (mfgunlock::framecount::g_state_seen.load(std::memory_order_relaxed)) {
    const unsigned int status =
        mfgunlock::framecount::g_dlssg_status.load(std::memory_order_relaxed);
    ImGui::Text("Actual presentations since last state query: %u.",
                mfgunlock::framecount::g_actual_frames_presented.load(std::memory_order_relaxed));
    if (status != 0) {
      ImGui::Text("DLSS-G runtime status: failure flags 0x%x.", status);
    } else {
      const unsigned int observed =
          mfgunlock::framecount::g_max_actual_frames_presented.load(std::memory_order_relaxed);
      if (observed > 1) {
        ImGui::Text("MFG validation: active; observed up to %u actual presentations.", observed);
      } else {
        ImGui::TextDisabled("DLSS-G status is OK; generated output has not been confirmed yet.");
      }
    }
  } else {
    ImGui::TextDisabled("No successful slDLSSGGetState telemetry sample yet (last result: %u).",
                        mfgunlock::framecount::g_state_result.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  constexpr const char* kHdrModes[] = {
      "Native (Default â€” recommended for most games)",
      "Force UI Composition (Advanced)",
      "Automatic Guard + UI Composition (HDR compatibility)",
      "Final Color Fallback (Troubleshooting)"};
  int hdr_mode = static_cast<int>(
      mfgunlock::framecount::g_hdr_compatibility_mode.load(std::memory_order_relaxed));
  if (ImGui::Combo("Frame-generation input quality", &hdr_mode, kHdrModes,
                   static_cast<int>(std::size(kHdrModes)))) {
    mfgunlock::framecount::g_hdr_compatibility_mode.store(
        static_cast<unsigned int>(hdr_mode), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyQualityModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "HDRCompatibilityMode", hdr_mode);
  }
  if (hdr_mode == static_cast<int>(
                      mfgunlock::framecount::HdrCompatibilityMode::kNative)) {
    ImGui::TextDisabled(
        "Passes the game's optional HUD-less and UI tags through unchanged.\n"
        "Recommended for most games; use this if HUD elements show artifacts.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
    ImGui::TextDisabled(
        "Forces Streamline to process the scene and UI separately. This requires\n"
        "the game to provide correctly matched buffers and color spaces.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
    ImGui::TextDisabled(
        "Rejects optional HUD/UI inputs when their metadata is invalid and uses\n"
        "final color as the safe fallback. It also resets temporal history once\n"
        "after HDR, swapchain, resolution, option or multiplier transitions.");
  } else {
    ImGui::TextDisabled(
        "Use for HDR artifacts in games such as Hogwarts Legacy, Jusant, and\n"
        "Mafia: The Old Country. If HUD elements show artifacts, use Native.\n"
        "Matched SDR inputs may use UI Composition; unsafe inputs fall back safely.");
  }
  ImGui::TextDisabled(
      "Color, depth, motion vectors, temporal kernel and frame pacing are not rewritten.");
  const bool hdr_active =
      mfgunlock::framecount::g_hdr_active.load(std::memory_order_relaxed);
  const bool hdr_seen =
      mfgunlock::framecount::g_hdr_state_seen.load(std::memory_order_acquire);
  ImGui::Text("HDR output detected: %s.",
              hdr_seen ? (hdr_active ? "yes" : "no") : "not observed yet");
  if (mfgunlock::framecount::g_ui_recomposition_applied.load(std::memory_order_relaxed)) {
    ImGui::Text("Streamline accepted the UI-capable path (source options v%u).",
                mfgunlock::framecount::g_ui_recomposition_source_version.load(
                    std::memory_order_relaxed));
    if (hdr_mode == static_cast<int>(
                        mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
      ImGui::TextDisabled(
          "Quality Guard still controls whether optional HUD-less/UI tags are used.");
    }
  } else if (mfgunlock::framecount::g_ui_recomposition_fell_back.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("UI Composition rejected (result %u); safe fallback is active.",
                mfgunlock::framecount::g_ui_recomposition_result.load(
                    std::memory_order_relaxed));
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid) &&
             hdr_active) {
    ImGui::TextDisabled(
        "Automatic HDR final-color fallback active; UI Composition is intentionally not submitted.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
    ImGui::TextDisabled(
        hdr_seen
            ? "Automatic SDR path is waiting for SetOptions and a valid HUD-less/UI pair."
            : "Automatic mode is waiting for the primary swapchain color space.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
    ImGui::TextDisabled("UI Composition has not been submitted yet; toggle FG after changing modes.");
  } else if (hdr_mode == static_cast<int>(
                             mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
    ImGui::TextDisabled(
        "Conservative final-color fallback active; UI Composition is intentionally not submitted.");
  }
  if (mfgunlock::framecount::g_hud_inputs_suppressed.load(std::memory_order_relaxed)) {
    ImGui::Text("Quality Guard filtered incompatible optional HUD/UI input.");
    ImGui::TextDisabled("Observed issue mask: 0x%X.",
                        mfgunlock::framecount::g_quality_issue_mask.load(
                            std::memory_order_relaxed));
  }
  if (mfgunlock::framecount::g_quality_mode_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextDisabled(
        "Quality-mode option change pending; toggle Frame Generation off/on to apply it.");
  }
  if (mfgunlock::framecount::g_quality_tag_batch_too_large.load(
          std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "Quality Guard saw a tag batch larger than 64; that batch was forwarded unchanged.");
  }
  if (mfgunlock::framecount::g_quality_viewport_capacity_exhausted.load(
          std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "More than %zu Streamline viewports were seen; extra viewports use conservative fallback.",
        mfgunlock::framecount::kMaxQualityViewports);
  }
  const auto reset_count = mfgunlock::framecount::g_quality_resets_injected.load(
      std::memory_order_relaxed);
  if (reset_count != 0) {
    ImGui::Text("Quality Guard synchronized temporal history %llu time(s).", reset_count);
  }

  constexpr const char* kDepthEdgeModes[] = {
      "Off - game value",
      "Mild (20.0)",
      "Balanced (10.0)",
      "Strong (4.0)",
      "Aggressive (1.0)"};
  int depth_edge_level = static_cast<int>(
      mfgunlock::framecount::g_depth_edge_guard_level.load(std::memory_order_relaxed));
  if (ImGui::Combo("Optional depth-edge guard", &depth_edge_level,
                   kDepthEdgeModes,
                   static_cast<int>(std::size(kDepthEdgeModes)))) {
    mfgunlock::framecount::g_depth_edge_guard_level.store(
        static_cast<unsigned int>(depth_edge_level), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDepthEdgeTuningChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DepthEdgeGuardLevel",
                              depth_edge_level);
  }
  ImGui::TextDisabled(
      "Lower values can improve nearby object/lower-screen edge separation.\n"
      "This does not perform camera-turn resets or modify frame pacing.");
  if (mfgunlock::framecount::g_depth_edge_override_applied.load(
          std::memory_order_relaxed)) {
    ImGui::Text("Depth-edge override active; game supplied %.3f.",
                mfgunlock::framecount::g_last_native_depth_separation.load(
                    std::memory_order_relaxed));
  }

  ImGui::Separator();
  bool dynamic_mfg =
      mfgunlock::framecount::g_dynamic_mfg_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Use NVIDIA Dynamic MFG", &dynamic_mfg)) {
    mfgunlock::framecount::g_dynamic_mfg_enabled.store(dynamic_mfg,
                                                        std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG",
                              dynamic_mfg ? 1 : 0);
  }
  int dynamic_target = static_cast<int>(
      mfgunlock::framecount::g_dynamic_target_fps.load(std::memory_order_relaxed));
  if (ImGui::InputInt("Dynamic output target FPS", &dynamic_target, 1, 10)) {
    if (dynamic_target < 0) dynamic_target = 0;
    if (dynamic_target > 1000) dynamic_target = 1000;
    mfgunlock::framecount::g_dynamic_target_fps.store(
        static_cast<unsigned int>(dynamic_target), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicTargetFPS",
                              dynamic_target);
  }
  ImGui::TextDisabled(
      "Requires D3D12, driver 595.41+, DLSS-G 310.9.1/310.9.16 and Streamline 2.14.1/2.14.16.\n"
      "0 = display refresh. Uses Streamline's native eDynamic scheduler; no Present hook.\n"
      "When VSync is active, Streamline ignores this number and targets display refresh.\n"
      "Toggle frame generation off/on after changing Dynamic settings.");
  if (mfgunlock::framecount::g_dynamic_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextDisabled(
        "Dynamic setting change pending; waiting for a successful game-side SetOptions call.");
  }
  const uint64_t streamline_version =
      mfgunlock::framecount::g_active_streamline_version.load(std::memory_order_relaxed);
  const uint64_t dlssg_version =
      mfgunlock::framecount::g_last_dlssg_version.load(std::memory_order_relaxed);
  if (mfgunlock::framecount::g_streamline_version_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Observed Streamline DLSS-G: %u.%u.%u.%u%s.",
                static_cast<unsigned int>((streamline_version >> 48u) & 0xffffu),
                static_cast<unsigned int>((streamline_version >> 32u) & 0xffffu),
                static_cast<unsigned int>((streamline_version >> 16u) & 0xffffu),
                static_cast<unsigned int>(streamline_version & 0xffffu),
                mfgunlock::framecount::g_streamline_dynamic_version_active.load(
                    std::memory_order_relaxed) ? " (supported)" : " (not Dynamic-supported)");
  }
  if (mfgunlock::framecount::g_dlssg_version_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Observed DLSS-G provider candidate: %u.%u.%u.%u%s.",
                static_cast<unsigned int>((dlssg_version >> 48u) & 0xffffu),
                static_cast<unsigned int>((dlssg_version >> 32u) & 0xffffu),
                static_cast<unsigned int>((dlssg_version >> 16u) & 0xffffu),
                static_cast<unsigned int>(dlssg_version & 0xffffu),
                mfgunlock::framecount::g_dlssg_dynamic_version_seen.load(
                    std::memory_order_relaxed) ? " (supported candidate seen)"
                                               : " (not Dynamic-supported)");
  }
  if (render_api != DetectedRenderApi::kD3D12) {
    ImGui::TextDisabled("Dynamic MFG unavailable: the current renderer is not D3D12.");
  } else if (!mfgunlock::framecount::g_streamline_version_seen.load(
                 std::memory_order_acquire) ||
             !mfgunlock::framecount::g_dlssg_version_seen.load(
                 std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG waiting for the loaded Streamline/DLSS-G versions.");
  } else if (!mfgunlock::framecount::g_streamline_dynamic_version_active.load(
                 std::memory_order_acquire) ||
             !mfgunlock::framecount::g_dlssg_dynamic_version_seen.load(
                 std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG unavailable: requires DLSS-G 310.9.1/310.9.16 and Streamline 2.14.1/2.14.16.");
  } else if (!mfgunlock::framecount::g_dynamic_support_seen.load(
                 std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG support has not been reported by DLSS-G yet.");
  } else if (!mfgunlock::framecount::g_dynamic_supported.load(
                 std::memory_order_relaxed)) {
    ImGui::TextDisabled("Dynamic MFG unavailable: the active provider/driver reports unsupported.");
  } else if (mfgunlock::framecount::g_dynamic_applied.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("Dynamic MFG active; requested target: %s.",
                dynamic_target == 0 ? "display refresh" :
                                      (std::to_string(dynamic_target) + " FPS").c_str());
  } else if (mfgunlock::framecount::g_dynamic_fell_back.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("Dynamic MFG rejected (result %u); fixed MFG fallback is active.",
                mfgunlock::framecount::g_dynamic_result.load(
                    std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("Dynamic MFG supported; waiting for the next SetOptions call.");
  }
  if (!mfgunlock::framecount::g_vsync_support_seen.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("DLSS-G has not reported its VSync capability yet.");
  } else if (mfgunlock::framecount::g_vsync_supported.load(
                 std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "The active DLSS-G runtime reports VSync support. Streamline 2.14.1 adds\n"
        "VSync and frame-limiter support to Dynamic MFG on compatible D3D12 systems.");
  } else {
    ImGui::TextDisabled(
        "The active DLSS-G runtime reports VSync unavailable; this can indicate\n"
        "an older/mismatched runtime or an unsupported presentation mode.");
  }
  int source_cap = static_cast<int>(
      mfgunlock::framecount::g_reflex_source_fps_cap.load(
          std::memory_order_relaxed));
  if (ImGui::InputInt("Output FPS Cap (Reflex)", &source_cap, 1, 10)) {
    source_cap = source_cap <= 0 ? 0 : std::clamp(source_cap, 10, 1000);
    mfgunlock::framecount::g_reflex_source_fps_cap.store(
        static_cast<unsigned int>(source_cap), std::memory_order_relaxed);
    g_source_cap_config_origin.store(
        static_cast<unsigned int>(
            mfgunlock::pacing::SourceCapConfigOrigin::kConfigured),
        std::memory_order_relaxed);
    mfgunlock::framecount::NotifySourceFpsCapChanged();
    reshade::set_config_value(nullptr, kConfigSection,
                              "ReflexSourceFpsCap", source_cap);
  }
  const auto source_cap_state =
      mfgunlock::framecount::internal::ResolveUserSourceCapState();
  ImGui::TextDisabled("%s", UserSourceCapSummary(source_cap_state).c_str());
  ImGui::TextDisabled("%s", UserSourceCapStatusText(source_cap_state.status));

  int force = static_cast<int>(
      mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed));
  // 6x == numFramesToGenerate 5, which is the Streamline plugin's own hard
  // ceiling (its wrapper clamps the count to 5). Whether the runtime accepts it
  // is up to that plugin -- a refusal is logged and falls back to the game's
  // own request, so asking costs nothing.
  if (ImGui::SliderInt("Force frame multiplier", &force, 0, 6,
                       force == 0 ? "off (game decides)" : "%dx")) {
    if (force != 0 && force < 2) force = 2;
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(force),
                                                    std::memory_order_relaxed);
    mfgunlock::framecount::NotifyFixedMultiplierChanged(
        static_cast<unsigned int>(force));
    reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", force);
  }
  ImGui::TextDisabled(
      "Leave off for games with their own 2x/3x/4x selector -- forcing would\n"
      "override your in-game choice. Dynamic MFG takes priority when active.");
  const bool game_request_seen =
      mfgunlock::framecount::g_game_request_seen.load(std::memory_order_acquire);
  if (game_request_seen) {
    ImGui::Text("Game request: %ux.",
                mfgunlock::framecount::g_last_requested.load(
                    std::memory_order_relaxed) + 1);
  } else {
    ImGui::TextDisabled("Game request: not observed yet.");
  }
  if (mfgunlock::forcepolicy::IsFixedMultiplier(
          static_cast<unsigned int>(force))) {
    ImGui::Text("Addon fixed request: %dx.", force);
  } else {
    ImGui::TextDisabled("Addon fixed request: off (game decides).");
  }

  const auto fixed_status =
      static_cast<mfgunlock::forcepolicy::FixedOverrideStatus>(
          mfgunlock::framecount::g_fixed_override_status.load(
              std::memory_order_acquire));
  const bool effective_seen =
      mfgunlock::framecount::g_effective_request_seen.load(
          std::memory_order_acquire);
  const unsigned int effective_multiplier =
      mfgunlock::framecount::g_last_effective_generated.load(
          std::memory_order_relaxed) + 1;
  switch (fixed_status) {
    case mfgunlock::forcepolicy::FixedOverrideStatus::kPending:
      ImGui::TextDisabled(
          "Effective request: pending the next enabled slDLSSGSetOptions call.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kApplied:
      ImGui::Text("Effective downstream request: %ux (addon override accepted).",
                  effective_multiplier);
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kRejected:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game fallback).",
                    effective_multiplier);
      }
      ImGui::TextDisabled("The runtime rejected the addon's fixed request.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kBlockedByPacing:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game fallback).",
                    effective_multiplier);
      }
      ImGui::TextDisabled(
          "Addon override blocked: legacy flip pacing was not verified.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kUnsupportedAbi:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game fallback).",
                    effective_multiplier);
      }
      ImGui::TextDisabled(
          "Addon override unsupported by the game's DLSSGOptions ABI.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kDynamicPriority:
      ImGui::TextDisabled(
          "Effective multiplier: Dynamic MFG/provider-controlled; fixed request is not applied.");
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kMatchedGameRequest:
      ImGui::Text("Effective downstream request: %ux (already matched addon request).",
                  effective_multiplier);
      break;
    case mfgunlock::forcepolicy::FixedOverrideStatus::kNative:
    default:
      if (effective_seen) {
        ImGui::Text("Effective downstream request: %ux (game controlled).",
                    effective_multiplier);
      } else if (mfgunlock::framecount::g_hooked.load(
                     std::memory_order_acquire)) {
        ImGui::TextDisabled(
            "Effective request: waiting for an enabled slDLSSGSetOptions call.");
      }
      break;
  }
  if (!mfgunlock::framecount::g_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("sl.interposer.dll not hooked (no Streamline in this game?).");
  }

  ImGui::Separator();
  bool temporal =
      g_configured_temporal_fix.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Temporal fix on next restart (stops midpoint compaction)",
                      &temporal)) {
    g_configured_temporal_fix.store(temporal, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "TemporalFix", temporal ? 1 : 0);
  }
  ImGui::TextDisabled("Applied once at load; restart the game to change it.");
  if (temporal != g_temporal_fix.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Temporal-fix change is saved for the next restart.");
  }

  bool blackwell = g_configured_blackwell_framework_kernels.load(
      std::memory_order_relaxed);
  if (ImGui::Checkbox("Prefer full Blackwell framework kernels on next restart (experimental)",
                      &blackwell)) {
    g_configured_blackwell_framework_kernels.store(
        blackwell, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "BlackwellFrameworkKernels",
                              blackwell ? 1 : 0);
  }
  ImGui::TextDisabled(
      "On: full motion-vector/inpaint path when validated. Off: 0.7 midpoint correction.\n"
      "Restart the game after changing this option.");
  if (blackwell !=
      g_blackwell_framework_kernels.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Kernel-path change is saved for the next restart.");
  }

  ImGui::Separator();
  ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.22f, 1.0f),
                     "Enhanced thin-geometry interpolation (EXPERIMENTAL)");
  ImGui::TextDisabled(
      "Complementary DLSS-G kernel experiments for thin objects and reprojection.\n"
      "Both are enabled by default but remain independently selectable. Restart\n"
      "the game after changing either option; results can vary by game.");

  bool intermediate_scatter =
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(
          "Intermediate scatter retention (Experimental - Recommended)##thin_intermediate",
          &intermediate_scatter)) {
    g_configured_thin_geometry_intermediate_scatter.store(
        intermediate_scatter, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryIntermediateScatter",
                              intermediate_scatter ? 1 : 0);
  }
  ImGui::TextWrapped(
      "Enabled by default. Relaxes one motion-consistency rejection while DLSS-G\n"
      "builds motion vectors for intermediate generated frames. The separate depth\n"
      "test stays active. May preserve fences, foliage and moving edges; disable it\n"
      "if a game shows added trails, ghosting or disocclusion artifacts.");

  int silhouette_mode = static_cast<int>(
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed));
  const char* silhouette_modes[] = {
      "Off - released 0.9 behavior",
      "Balanced - preserve supported thin silhouettes",
      "Aggressive - prefer cleaner motion/depth boundaries"};
  if (ImGui::Combo("Boundary artifact mitigation (restart required)",
                   &silhouette_mode, silhouette_modes,
                   static_cast<int>(std::size(silhouette_modes)))) {
    g_configured_silhouette_guard_mode.store(
        static_cast<unsigned int>(silhouette_mode),
        std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "BoundaryArtifactMitigationMode",
                              silhouette_mode);
  }
  if (silhouette_mode == static_cast<int>(
                             mfgunlock::blackwell::SilhouetteGuardMode::Balanced)) {
    ImGui::TextWrapped(
        "Balanced conditions extra intermediate retention on one same-depth,\n"
        "motion-coherent neighbor. It targets occlusion boundaries, foreground/\n"
        "background bleeding, silhouette stretching, motion boundaries and depth\n"
        "discontinuities while favoring narrow foliage, hair and weapon edges.");
  } else if (silhouette_mode == static_cast<int>(
                                    mfgunlock::blackwell::SilhouetteGuardMode::Aggressive)) {
    ImGui::TextWrapped(
        "Aggressive requires more than one neighbor-equivalent of coherent support,\n"
        "uses a tighter depth boundary and nonlinear confidence, and permits at\n"
        "most half of Balanced's extra relaxation. It may clean silhouettes more,\n"
        "but can reduce persistence or add flicker to very thin geometry.");
  } else {
    ImGui::TextDisabled(
        "Off keeps the released 0.9 Intermediate Scatter Retention behavior.");
  }
  if (silhouette_mode != static_cast<int>(
                             mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
    ImGui::TextDisabled(
        "The selected guard supersedes unconditional Intermediate retention.\n"
        "It cannot reconstruct genuinely hidden pixels or repair invalid game motion vectors.");
    if (intermediate_scatter) {
      ImGui::TextDisabled(
          "Intermediate retention remains enabled only as a compatibility fallback.");
    }
  }

  bool validated_warp =
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(
          "Validated warp blend (Experimental - Recommended)##thin_validated_warp",
          &validated_warp)) {
    g_configured_thin_geometry_validated_warp_blend.store(
        validated_warp, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryValidatedWarpBlend",
                              validated_warp ? 1 : 0);
  }
  ImGui::TextWrapped(
      "Enabled by default. Later-stage blend experiment inspired by Tony Joaca's\n"
      "DLSSG-Transfusion qualityValidWarp work and independently implemented here.\n"
      "It validates candidate bounds, finite color and mutual agreement before\n"
      "gradually trusting accepted warped color more. It may reduce flicker, but can\n"
      "also increase persistence or ghosting in some scenes.");

  bool previous_scatter =
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(
          "Previous-to-current scatter retention (Experimental/Unstable)##thin_previous",
          &previous_scatter)) {
    g_configured_thin_geometry_previous_scatter.store(
        previous_scatter, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryPreviousScatter",
                              previous_scatter ? 1 : 0);
  }
  ImGui::TextDisabled(
      "Advanced research control; disabled by default. It changes motion rejection\n"
      "between real frames and was unstable in initial game testing. Not recommended\n"
      "for normal use; bounds logic remains unchanged.");

  if (validated_warp !=
          g_thin_geometry_validated_warp_blend.load(std::memory_order_relaxed) ||
      previous_scatter !=
          g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
      intermediate_scatter !=
          g_thin_geometry_intermediate_scatter.load(std::memory_order_relaxed) ||
      static_cast<unsigned int>(silhouette_mode) !=
          g_silhouette_guard_mode.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "Thin-geometry selection is saved for the next restart.");
  }

  const auto show_thin_result = [](const char* label,
                                   const mfgunlock::thingeometry::MechanismResult& result) {
    if (!result.requested) return;
    if (result.applied) {
      ImGui::TextWrapped("%s: applied (%s).", label, result.detail.c_str());
    } else {
      ImGui::TextDisabled("%s: not applied (%s).", label,
                          result.detail.empty() ? "waiting for a supported provider"
                                                : result.detail.c_str());
    }
  };
  if (thin_geometry_provider_count != 0) {
    ImGui::TextDisabled("Validated provider result: %s; processed provider(s): %zu.",
                        thin_geometry_last.provider_version.empty()
                            ? "unsupported/unknown"
                            : thin_geometry_last.provider_version.c_str(),
                        thin_geometry_provider_count);
    show_thin_result("Validated warp blend",
                     thin_geometry_last.result.validated_warp_blend);
    show_thin_result("Previous scatter retention",
                     thin_geometry_last.result.previous_scatter);
    show_thin_result("Intermediate scatter retention",
                     thin_geometry_last.intermediate_scatter);
    show_thin_result("Silhouette disocclusion guard",
                     thin_geometry_last.silhouette_boundary_guard);
  } else if (g_thin_geometry_validated_warp_blend.load(
                 std::memory_order_relaxed) ||
             g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
             g_thin_geometry_intermediate_scatter.load(
                 std::memory_order_relaxed) ||
             g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                 static_cast<unsigned int>(
                     mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
    ImGui::TextDisabled("Waiting for a supported DLSS-G provider (attempt %d).",
                        g_thin_geometry_attempts.load(std::memory_order_relaxed));
  }
  if (thin_geometry_patched) {
    ImGui::TextDisabled(
        "At least one thin-geometry mechanism passed exact validation this session.");
  }

  ImGui::Separator();
  if (blackwell_patched) {
    ImGui::Text("Blackwell framework kernels active: %zu provider(s).",
                blackwell_provider_count);
    ImGui::TextDisabled(
        "Patched roles: motion-vector %s, inpaint %s, decision %s (%zu slots).",
        blackwell_active_result.motion_vector ? "yes" : "no",
        blackwell_active_result.inpaint ? "yes" : "no",
        blackwell_active_result.inpaint_decision ? "yes" : "no",
        blackwell_active_result.kernels);
    if (!blackwell_applied_detail.empty()) {
      ImGui::TextDisabled("Applied provider result: %s.",
                          blackwell_applied_detail.c_str());
    }
    if (!blackwell_detail.empty() &&
        blackwell_detail != blackwell_applied_detail) {
      ImGui::TextDisabled(
          "Separate rejected candidate (does not replace the result above): %s.",
          blackwell_detail.c_str());
    }
  } else if (midpoint_patched) {
    ImGui::TextWrapped("Temporal fix: %zu provider(s); last result: %s.",
                       midpoint_provider_count, midpoint_detail.c_str());
    if (g_blackwell_framework_kernels.load(std::memory_order_relaxed) &&
        !blackwell_detail.empty()) {
      ImGui::TextDisabled("Full Blackwell path fell back safely: %s.",
                          blackwell_detail.c_str());
    }
  } else {
    ImGui::TextDisabled("Temporal fix not applied yet (attempt %d).",
                        g_midpoint_attempts.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  if (mfgunlock::loadhook::g_hooked.load(std::memory_order_acquire)) {
    ImGui::Text("Load-time trigger armed (%u snippet load(s) caught).",
                mfgunlock::loadhook::g_catches.load(std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("Load-time trigger not installed.");
  }
  if (g_discovery_worker_running.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("Bounded fallback discovery running (%u/%u passes; no Present polling).",
                        g_discovery_worker_passes.load(std::memory_order_relaxed),
                        kDiscoveryRetryLimit);
  } else if (g_discovery_worker_finished.load(std::memory_order_acquire)) {
    if (DiscoveryRequirementsMet()) {
      ImGui::TextDisabled("Bounded fallback discovery complete; no Present polling is active.");
    } else {
      ImGui::TextDisabled("Bounded fallback discovery finished; waiting on load-time triggers.");
    }
  }

  ImGui::Separator();
  if (gate_patched) {
    ImGui::Text("DLSS-G arch gates rewritten: %zu provider(s), %zu site(s).",
                gate_provider_count, gate_site_count);
  } else {
    ImGui::TextDisabled("DLSS-G snippet not located yet (attempt %d) -- enable frame generation.",
                        g_gate_attempts.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  ImGui::TextDisabled("Legacy NGX parameter-vtable override disabled; using verified code gates.");
}

void OnRegisterOverlay(reshade::api::effect_runtime* runtime) {
  mfgunlock::ui::Theme panel_theme;
  if (mfgunlock::backend::active != mfgunlock::backend::Kind::DlssG) {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::xemfg::native::Draw();
    return;
#else
    ImGui::TextUnformatted("XeMFG uses OptiScaler's Frame Generation controls and XeLL.");
    ImGui::TextUnformatted("Change backend with Switch-MFGBackend.ps1 while the game is closed.");
    ImGui::TextUnformatted("DLSS-G patches and Reflex controller are inactive in this mode.");
    if (auto module = GetModuleHandleW(L"MFGAdaUnlock.XeMFG.asi")) {
      auto get_status = reinterpret_cast<mfgunlock::xemfg::GetStatus>(
          GetProcAddress(module, "MFGAXeGetStatus"));
      mfgunlock::xemfg::Status status;
      if (get_status && get_status(&status, sizeof(status))) {
        ImGui::TextWrapped("%s", status.detail);
        ImGui::Text("Model: %s | Ready: %s | Enabled: %s",
                    status.model ? "Intel MFG port" : "Native XeFG",
                    status.initialized ? "yes" : "no", status.enabled ? "yes" : "no");
        ImGui::Text("Requested output: %ux | Last presented: %u | Result: %d",
                    status.generated + 1, status.presented, status.result);
        ImGui::Text("Tensor Core launches: %llu", status.tensor_launches);
      }
    } else {
      ImGui::TextUnformatted("XeMFG companion is not loaded. Check the OptiScaler plugins folder.");
    }
#endif
    return;
  }
  mfgunlock::framecount::g_latency_guard_ui_heartbeat_ms.store(
      GetTickCount64(), std::memory_order_release);
  const bool enabled = g_enabled.load(std::memory_order_relaxed);
  bool configured_enabled =
      g_configured_enabled.load(std::memory_order_relaxed);
  const auto render_api = g_render_api.load(std::memory_order_relaxed);

  const bool streamline_seen =
      mfgunlock::framecount::g_streamline_version_seen.load(
          std::memory_order_acquire);
  const bool dlssg_seen =
      mfgunlock::framecount::g_dlssg_version_seen.load(
          std::memory_order_acquire);
  const uint64_t streamline_version =
      mfgunlock::framecount::g_active_streamline_version.load(
          std::memory_order_relaxed);
  const uint64_t dlssg_version =
      mfgunlock::framecount::g_last_dlssg_version.load(
          std::memory_order_relaxed);
  const std::string streamline_text =
      PackedVersionString(streamline_version, streamline_seen);
  const std::string dlssg_text = PackedVersionString(dlssg_version, dlssg_seen);

  const bool dynamic_versions_ready =
      mfgunlock::framecount::g_streamline_dynamic_version_active.load(
          std::memory_order_acquire) &&
      mfgunlock::framecount::g_dlssg_dynamic_version_seen.load(
          std::memory_order_acquire);
  const bool dynamic_support_seen =
      mfgunlock::framecount::g_dynamic_support_seen.load(
          std::memory_order_acquire);
  const bool dynamic_supported =
      mfgunlock::framecount::g_dynamic_supported.load(
          std::memory_order_relaxed);
  const bool dynamic_renderer_blocked =
      render_api != DetectedRenderApi::kUnknown &&
      render_api != DetectedRenderApi::kD3D12;
  const bool dynamic_version_blocked =
      streamline_seen && dlssg_seen && !dynamic_versions_ready;
  const bool dynamic_provider_blocked =
      dynamic_support_seen && !dynamic_supported;
  const bool dynamic_game_blocked =
      mfgunlock::framecount::g_dynamic_game_compat_blocked.load(
          std::memory_order_relaxed);
  const bool dynamic_hard_unavailable =
      dynamic_renderer_blocked || dynamic_version_blocked ||
      dynamic_provider_blocked || dynamic_game_blocked;
  const bool dynamic_enabled =
      mfgunlock::framecount::g_dynamic_mfg_enabled.load(
          std::memory_order_relaxed);
  const bool dynamic_available =
      render_api == DetectedRenderApi::kD3D12 && dynamic_versions_ready &&
      dynamic_support_seen && dynamic_supported && !dynamic_game_blocked;

  const bool state_seen =
      mfgunlock::framecount::g_state_seen.load(std::memory_order_acquire);
  const unsigned int dlssg_status =
      mfgunlock::framecount::g_dlssg_status.load(std::memory_order_relaxed);
  const unsigned int observed_presentations =
      mfgunlock::framecount::g_max_actual_frames_presented.load(
          std::memory_order_relaxed);
  const bool effective_seen =
      mfgunlock::framecount::g_effective_request_seen.load(
          std::memory_order_acquire);
  const unsigned int effective_multiplier =
      mfgunlock::framecount::g_last_effective_generated.load(
          std::memory_order_relaxed) +
      1;
  const bool game_request_seen =
      mfgunlock::framecount::g_game_request_seen.load(
          std::memory_order_acquire);
  const unsigned int game_multiplier =
      mfgunlock::framecount::g_last_requested.load(std::memory_order_relaxed) +
      1;
  const bool hdr_seen =
      mfgunlock::framecount::g_hdr_state_seen.load(std::memory_order_acquire);
  const bool hdr_active =
      mfgunlock::framecount::g_hdr_active.load(std::memory_order_relaxed);
  const auto& nvapi_status = ObserveNvapiUiStatus(runtime);
  const bool sleep_status_available =
      nvapi_status.sleep_status == mfgunlock::nvapistatus::kOk;
  const unsigned int driver_multiplier =
      sleep_status_available
          ? nvapi_status.sleep.frame_generation_multiplier
          : 0;
  const bool driver_vsync_active =
      sleep_status_available && nvapi_status.sleep.control_panel_vsync != 0;
  const bool gsync_active =
      sleep_status_available && nvapi_status.sleep.fullscreen_vrr != 0;
  const std::string frame_generation_preset =
      FrameGenerationPresetText(nvapi_status, dlssg_seen);
  const std::string super_resolution_preset =
      ReconstructionPresetText(nvapi_status, false);
  const std::string ray_reconstruction_preset =
      ReconstructionPresetText(nvapi_status, true);
  const bool super_resolution_active =
      mfgunlock::nvapistatus::NgxFeatureActive(
          nvapi_status.ngx.feedback_super_resolution);
  const bool super_resolution_configured =
      mfgunlock::nvapistatus::NgxFeatureConfigured(
          nvapi_status.ngx.feedback_super_resolution);
  const bool ray_reconstruction_active =
      mfgunlock::nvapistatus::NgxFeatureActive(
          nvapi_status.ngx.feedback_ray_reconstruction);
  const bool ray_reconstruction_configured =
      mfgunlock::nvapistatus::NgxFeatureConfigured(
          nvapi_status.ngx.feedback_ray_reconstruction);
  // The ceiling this addon advertises when it patched the plugin; otherwise
  // the runtime's own numFramesToGenerateMax (OTA plugins are not patched).
  const unsigned int advertised_max_generated =
      mfgunlock::framecount::g_advertised_max_generated.load(
          std::memory_order_relaxed);
  const unsigned int dynamic_max_generated =
      advertised_max_generated != 0
          ? advertised_max_generated
          : mfgunlock::framecount::g_runtime_max_generated.load(
                std::memory_order_relaxed);

  const bool restart_pending =
      configured_enabled != enabled ||
      g_configured_force_flip_meter_off.load(std::memory_order_relaxed) !=
          g_force_flip_meter_off.load(std::memory_order_relaxed) ||
      g_configured_temporal_fix.load(std::memory_order_relaxed) !=
          g_temporal_fix.load(std::memory_order_relaxed) ||
      g_configured_blackwell_framework_kernels.load(
          std::memory_order_relaxed) !=
          g_blackwell_framework_kernels.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality.load(std::memory_order_relaxed) !=
          g_adaptive_quality.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality_profile.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_profile.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_photometric.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_photometric.load(std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_directional_border.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_directional_border.load(
              std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_oriented_geometry.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_oriented_geometry.load(
              std::memory_order_relaxed) ||
      g_configured_adaptive_quality_v3_stability_mode.load(
          std::memory_order_relaxed) !=
          g_adaptive_quality_v3_stability_mode.load(
              std::memory_order_relaxed) ||
      g_configured_quality_refinement.load(std::memory_order_relaxed) !=
          g_quality_refinement.load(std::memory_order_relaxed) ||
      g_configured_geometry_confidence_v2.load(std::memory_order_relaxed) !=
          g_geometry_confidence_v2.load(std::memory_order_relaxed) ||
      g_configured_border_confidence.load(std::memory_order_relaxed) !=
          g_border_confidence.load(std::memory_order_relaxed) ||
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed) !=
          g_thin_geometry_validated_warp_blend.load(
              std::memory_order_relaxed) ||
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed) !=
          g_thin_geometry_previous_scatter.load(std::memory_order_relaxed) ||
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed) !=
          g_thin_geometry_intermediate_scatter.load(
              std::memory_order_relaxed) ||
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed) !=
          g_silhouette_guard_mode.load(std::memory_order_relaxed) ||
      g_configured_runtime_selection_mode.load(std::memory_order_relaxed) !=
          mfgunlock::framecount::g_runtime_selection_mode.load(
              std::memory_order_relaxed);

  const auto fixed_status =
      static_cast<mfgunlock::forcepolicy::FixedOverrideStatus>(
          mfgunlock::framecount::g_fixed_override_status.load(
              std::memory_order_acquire));
  const bool dynamic_accepted =
      mfgunlock::framecount::g_dynamic_applied.load(
          std::memory_order_acquire);
  const bool request_rejected =
      fixed_status ==
          mfgunlock::forcepolicy::FixedOverrideStatus::kRejected ||
      mfgunlock::framecount::g_dynamic_runtime_declined.load(
          std::memory_order_acquire);
  const bool runtime_error = state_seen && dlssg_status != 0;
  const auto validation_stage = mfgunlock::validation::Classify({
      game_request_seen,
      effective_seen || dynamic_accepted,
      request_rejected,
      runtime_error,
      sleep_status_available,
      driver_multiplier,
      state_seen && dlssg_status == 0,
      observed_presentations});
  const std::string validation_text = ValidationStatusText(
      validation_stage, dynamic_accepted, effective_seen,
      effective_multiplier, driver_multiplier, observed_presentations);
  const bool mfg_confirmed =
      validation_stage ==
      mfgunlock::validation::Stage::kProviderOutputConfirmed;
  const bool mfg_live =
      mfgunlock::validation::HasLiveRuntimeEvidence(validation_stage);
  const bool mfg_request_accepted =
      mfgunlock::validation::HasAcceptedRequest(validation_stage);
  const bool provider_ready =
      g_gate_patched.load(std::memory_order_acquire) &&
      mfgunlock::framecount::g_hooked.load(std::memory_order_acquire);

  if (g_is_monster_hunter_wilds &&
      g_monster_hunter_restart_notice_pending.load(
          std::memory_order_acquire)) {
    constexpr const char* kPopup =
        "Monster Hunter Wilds first-launch notice";
    if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(kUiWarning, "Restart once after shader compilation");
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
      ImGui::TextWrapped(
          "Monster Hunter Wilds may build shader.cache2 on the first launch "
          "after installing or updating the addon. Let the game's shader "
          "compilation finish, close the game completely, and launch it again "
          "before evaluating frame pacing or image quality.");
      ImGui::Spacing();
      ImGui::TextDisabled(
          "Do not restart while the shader compilation is still running. The "
          "addon does not delete or modify the game's cache.");
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      if (ImGui::Button("I understand", ImVec2(150.0f, 0.0f))) {
        reshade::set_config_value(nullptr, kConfigSection,
                                  "MonsterHunterRestartNoticeRevision",
                                  kMonsterHunterRestartNoticeRevision);
        g_monster_hunter_restart_notice_pending.store(
            false, std::memory_order_release);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  const bool dynamic_applied=mfgunlock::framecount::g_dynamic_applied.load();
  const std::string multiplier_text=dynamic_applied?DynamicMultiplierText(driver_multiplier,dynamic_max_generated):
      effective_seen?std::to_string(effective_multiplier)+"x":game_request_seen?std::to_string(game_multiplier)+"x":"Waiting";
  const char* player_state=runtime_error?"Compatibility issue":!enabled?"Disabled this session":mfg_confirmed?"Generating frames":
      mfg_live?"Driver reports MFG active":mfg_request_accepted?"Multiplier requested":provider_ready?"Ready - enable FG in the game":"Waiting for frame generation";
  mfgunlock::ui::Hero(multiplier_text.c_str(),"DLSS-G",RenderApiName(render_api),player_state,mfg_live);
  if(dynamic_applied) mfgunlock::ui::Description("Dynamic MFG is choosing the multiplier automatically.");
  if(sleep_status_available && (driver_vsync_active || gsync_active))
    mfgunlock::ui::Description(driver_vsync_active && gsync_active?"Driver VSync + G-SYNC active":driver_vsync_active?"Driver VSync active":"G-SYNC active");
  if (restart_pending) {
    ImGui::Spacing();
    ImGui::TextColored(kUiWarning, "Restart required");
    ImGui::TextWrapped(
        "One or more saved changes will take effect after restarting the game.");
  }
  if(dynamic_enabled && dynamic_hard_unavailable)
    mfgunlock::ui::Notice("Dynamic MFG is unavailable for this game or runtime. Fixed multipliers remain available.");
  if (mfgunlock::framecount::g_dynamic_change_pending.load(
          std::memory_order_acquire) ||
      mfgunlock::framecount::g_quality_mode_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextColored(kUiWarning, "Apply pending");
    ImGui::TextWrapped(
        "Toggle Frame Generation off and on in the game to apply the latest runtime change.");
  }

  enum class UiPage { General, Quality, DlssInfo, Latency, Support };
  static UiPage page=UiPage::General;
  static bool show_runtime_info=false;
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    int selected=page==UiPage::General?0:page==UiPage::Quality?1:page==UiPage::Latency?2:3;
    ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##dlssg_page",&selected,"General\0Quality\0Latency\0Diagnostics\0"))
      page=selected==0?UiPage::General:selected==1?UiPage::Quality:selected==2?UiPage::Latency:UiPage::Support;
    if(selected==3) {
      if(ImGui::RadioButton("Status",!show_runtime_info)) show_runtime_info=false;
      ImGui::SameLine(); if(ImGui::RadioButton("Runtime information",show_runtime_info)) show_runtime_info=true;
      page=show_runtime_info?UiPage::DlssInfo:UiPage::Support;
    }
  } else if(const float tab=mfgunlock::ui::TabWidth(4); ImGui::BeginTabBar("##mfg_unlock_pages")) {
    if(mfgunlock::ui::TabItem("General",tab)) { page=UiPage::General; ImGui::EndTabItem(); }
    if(mfgunlock::ui::TabItem("Quality",tab)) { page=UiPage::Quality; ImGui::EndTabItem(); }
    if(mfgunlock::ui::TabItem("Latency",tab)) { page=UiPage::Latency; ImGui::EndTabItem(); }
    if(mfgunlock::ui::TabItem("Diagnostics",tab)) {
      if(ImGui::RadioButton("Status",!show_runtime_info)) show_runtime_info=false;
      ImGui::SameLine(); if(ImGui::RadioButton("Runtime information",show_runtime_info)) show_runtime_info=true;
      page=show_runtime_info?UiPage::DlssInfo:UiPage::Support;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (page == UiPage::DlssInfo) {
    g_vram_ui_heartbeat_ms.store(GetTickCount64(),
                                 std::memory_order_release);
    if (!mfgunlock::inputdiag::g_enabled.load(std::memory_order_acquire)) {
      mfgunlock::inputdiag::Clear();
      mfgunlock::inputdiag::g_enabled.store(true,
                                            std::memory_order_release);
      g_vram_capture_owned.store(true, std::memory_order_release);
    }
    QueueProviderVramEstimateFromCapture();
    ImGui::Spacing();
    ImGui::TextDisabled("DLSS RUNTIME INFORMATION");
    ImGui::Separator();
    if (ImGui::BeginTable("##dlss_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn("Active mode", ImGuiTableColumnFlags_WidthStretch,
                              0.58f);
      StatusRow("Super Resolution", super_resolution_preset.c_str(),
                super_resolution_active
                    ? kUiPositive
                    : (super_resolution_configured ? kUiWarning : kUiMuted));
      StatusRow("Frame Generation", frame_generation_preset.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow("Ray Reconstruction", ray_reconstruction_preset.c_str(),
                ray_reconstruction_active
                    ? kUiPositive
                    : (ray_reconstruction_configured ? kUiWarning : kUiMuted));
      StatusRow("DLSS-G runtime", dlssg_text.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow("Streamline runtime", streamline_text.c_str(),
                streamline_seen ? kUiPositive : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Read-only information reported by NVIDIA. Game-controlled modes may not expose a preset name.");
    HelpMarker(
        "NVIDIA exposes one shared preset/mode field for Super Resolution and Ray Reconstruction. The addon displays it only when the corresponding feature reports active or configured feedback, and never changes the selected model or render resolution.");

    ImGui::Spacing();
    ImGui::TextDisabled("VRAM / RESOURCE HEALTH");
    ImGui::Separator();
    if (ImGui::BeginTable("##vram_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn("Observation",
                              ImGuiTableColumnFlags_WidthStretch, 0.58f);
      if (g_vram_dxgi_seen.load(std::memory_order_acquire)) {
        const uint64_t usage =
            g_vram_local_usage.load(std::memory_order_relaxed);
        const uint64_t budget =
            g_vram_local_budget.load(std::memory_order_relaxed);
        const std::string usage_text =
            FormatMemoryBytes(usage) + " / " + FormatMemoryBytes(budget);
        const uint64_t headroom = budget > usage ? budget - usage : 0;
        const std::string headroom_text = FormatMemoryBytes(headroom);
        StatusRow("Process local VRAM", usage_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        StatusRow("Budget headroom", headroom_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        const std::string reservation_text = FormatMemoryBytes(
            g_vram_local_available.load(std::memory_order_relaxed));
        StatusRow("Available for reservation", reservation_text.c_str(),
                  kUiMuted);
        const uint64_t shared_usage =
            g_vram_nonlocal_usage.load(std::memory_order_relaxed);
        if (shared_usage != 0) {
          const std::string shared_text =
              FormatMemoryBytes(shared_usage) + " / " +
              FormatMemoryBytes(
                  g_vram_nonlocal_budget.load(std::memory_order_relaxed));
          StatusRow("Process shared GPU memory", shared_text.c_str(),
                    kUiMuted);
        }
      } else {
        StatusRow(
            "Process local VRAM",
            g_render_api.load(std::memory_order_relaxed) ==
                    DetectedRenderApi::kVulkan
                ? "DXGI budget unavailable on Vulkan"
                : "Waiting for DXGI budget",
            kUiMuted);
      }

      const auto estimate_status =
          static_cast<mfgunlock::framecount::VramEstimateStatus>(
              mfgunlock::framecount::g_vram_estimate_status.load(
                  std::memory_order_acquire));
      std::string estimate_text;
      ImVec4 estimate_color = kUiMuted;
      switch (estimate_status) {
        case mfgunlock::framecount::VramEstimateStatus::kReady:
          estimate_text = FormatMemoryBytes(
              mfgunlock::framecount::g_vram_estimate_bytes.load(
                  std::memory_order_relaxed));
          break;
        case mfgunlock::framecount::VramEstimateStatus::kPending:
          estimate_text = "Waiting for next DLSS-G state query";
          break;
        case mfgunlock::framecount::VramEstimateStatus::kUnsupported:
          estimate_text = "Unavailable on this runtime/game";
          break;
        case mfgunlock::framecount::VramEstimateStatus::kFailed:
          estimate_text = "Provider did not return an estimate";
          estimate_color = kUiWarning;
          break;
        default:
          estimate_text = VramReadinessText(
              static_cast<mfgunlock::memorypolicy::EstimateReadiness>(
                  mfgunlock::framecount::g_vram_estimate_readiness.load(
                      std::memory_order_relaxed)));
          break;
      }
      StatusRow("DLSS-G estimated VRAM", estimate_text.c_str(),
                estimate_color);

      const bool native_ready =
          mfgunlock::framecount::g_vram_native_estimate_ready.load(
              std::memory_order_acquire);
      const bool ui_ready =
          mfgunlock::framecount::g_vram_ui_estimate_ready.load(
              std::memory_order_acquire);
      if (native_ready && ui_ready) {
        const uint64_t native_bytes =
            mfgunlock::framecount::g_vram_native_estimate_bytes.load(
                std::memory_order_relaxed);
        const uint64_t ui_bytes =
            mfgunlock::framecount::g_vram_ui_estimate_bytes.load(
                std::memory_order_relaxed);
        const int64_t delta = static_cast<int64_t>(ui_bytes) -
                              static_cast<int64_t>(native_bytes);
        const uint64_t delta_bytes =
            static_cast<uint64_t>(delta >= 0 ? delta : -delta);
        const std::string delta_text =
            std::string(delta >= 0 ? "+" : "-") +
            (delta_bytes == 0 ? "0 MiB" : FormatMemoryBytes(delta_bytes)) +
            " with UI composition";
        StatusRow("Measured UI-composition delta", delta_text.c_str(),
                  kUiMuted);
      }

      const unsigned int volatile_inputs =
          mfgunlock::framecount::g_vram_volatile_input_count.load(
              std::memory_order_relaxed);
      const std::string lifecycle_text = volatile_inputs == 0
          ? "Optimized: relevant inputs valid until Present"
          : std::to_string(volatile_inputs) +
                " input(s) may require transient copies";
      StatusRow("Input lifetimes", lifecycle_text.c_str(),
                volatile_inputs == 0 ? kUiPositive : kUiWarning);
      const unsigned int flags =
          mfgunlock::framecount::g_vram_estimate_flags.load(
              std::memory_order_relaxed);
      const bool retained_while_off =
          (flags & static_cast<unsigned int>(
                       sl::DLSSGFlags::eRetainResourcesWhenOff)) != 0;
      StatusRow("Resources while FG is off",
                retained_while_off ? "Retained by game request"
                                   : "Not retained by game request",
                retained_while_off ? kUiWarning : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Read-only diagnostics. Process usage includes the whole game; the provider estimate is approximate.");
    HelpMarker(
        "This page temporarily captures resource dimensions, formats and lifetimes only; it never reads pixels or stores resource addresses. The addon does not reserve VRAM, rewrite resource lifetimes, or disable the game's retain-resources flag. Close this page to stop metadata capture.");
  }

  if (page == UiPage::General) {
    ImGui::Spacing();
    mfgunlock::ui::Section("Frame generation","Enable Frame Generation in the game, then choose how many frames to generate.");
    mfgunlock::ui::Card generation_card("##dlssg_general_card");
    mfgunlock::ui::BackendSelector();
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::dlssgdisplay::Draw();
#endif
    if (ImGui::BeginTable("##frame_generation_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.38f);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Enable MFG Unlock", "Applied on the next game launch.",
        "Changes the master addon state. If you change this option, a restart-required notice will appear at the top of the panel.");
    ImGui::TableNextColumn();
    if (ImGui::Checkbox("##mfg_enabled", &configured_enabled)) {
      g_configured_enabled.store(configured_enabled,
                                 std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "Enabled",
                                configured_enabled ? 1 : 0);
    }

    int force = static_cast<int>(
        mfgunlock::framecount::g_force_multiplier.load(
            std::memory_order_relaxed));
    int force_choice = force == 0 ? 0 : force - 1;
    constexpr const char* kMultiplierModes[] = {
        "Game controlled", "2x", "3x", "4x", "5x", "6x"};
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Frame Multiplier", "Use the game's selector when possible.",
        "Game controlled preserves the multiplier selected in the game's menu. A fixed value overrides that request when supported. Dynamic MFG takes priority while active.");
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##frame_multiplier", &force_choice,
                     kMultiplierModes,
                     static_cast<int>(std::size(kMultiplierModes)))) {
      force = force_choice == 0 ? 0 : force_choice + 1;
      mfgunlock::framecount::g_force_multiplier.store(
          static_cast<unsigned int>(force), std::memory_order_relaxed);
      mfgunlock::framecount::NotifyFixedMultiplierChanged(
          static_cast<unsigned int>(force));
      reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier",
                                force);
    }

    bool dynamic_mfg = dynamic_enabled && !dynamic_game_blocked;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "NVIDIA Dynamic MFG",
        dynamic_game_blocked
            ? "Disabled for this game's compatibility."
            : (dynamic_available ? "Runtime support detected."
                                 : (dynamic_hard_unavailable
                                        ? "Required runtime is unavailable."
                                        : "Waiting for runtime detection.")),
        "Lets NVIDIA's native scheduler choose the generated-frame count. Requires D3D12, driver 595.41 or newer, DLSS-G 310.9.1/310.9.16 and Streamline 2.14.1/2.14.16. Star Wars Outlaws is kept on fixed/game-controlled MFG because its integration breaks on the Dynamic path. Toggle Frame Generation off/on after changing it.");
    ImGui::TableNextColumn();
    const bool block_dynamic_enable =
        dynamic_game_blocked || (dynamic_hard_unavailable && !dynamic_mfg);
    if (block_dynamic_enable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("##dynamic_mfg", &dynamic_mfg)) {
      mfgunlock::framecount::g_dynamic_mfg_enabled.store(
          dynamic_mfg, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyDynamicModeChanged();
      reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG",
                                dynamic_mfg ? 1 : 0);
    }
    if (block_dynamic_enable) ImGui::EndDisabled();

    if (dynamic_mfg) {
      int dynamic_target = static_cast<int>(
          mfgunlock::framecount::g_dynamic_target_fps.load(
              std::memory_order_relaxed));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "Dynamic Output Target", "0 follows the display refresh rate.",
          "Sets Dynamic MFG's requested final/output target. With VSync active, Streamline follows the display refresh rate and may ignore a non-zero target. This scheduler target is separate from Output FPS Cap (Reflex).");
      ImGui::TableNextColumn();
      if (dynamic_hard_unavailable) ImGui::BeginDisabled();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::InputInt("##dynamic_target", &dynamic_target, 1, 10)) {
        dynamic_target = std::clamp(dynamic_target, 0, 1000);
        mfgunlock::framecount::g_dynamic_target_fps.store(
            static_cast<unsigned int>(dynamic_target),
            std::memory_order_relaxed);
        mfgunlock::framecount::NotifyDynamicModeChanged();
        reshade::set_config_value(nullptr, kConfigSection,
                                  "DynamicTargetFPS", dynamic_target);
      }
      if (dynamic_hard_unavailable) ImGui::EndDisabled();

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "VSync / G-SYNC behavior",
          driver_vsync_active && gsync_active
              ? "Detected: driver VSync + G-SYNC active."
              : (driver_vsync_active
                     ? "Detected: driver VSync active."
                     : (gsync_active
                            ? "Detected: G-SYNC active; driver VSync is off."
                            : "Check the rule before choosing a target.")),
          "With VSync active, Streamline ignores Dynamic Output Target and follows the active display refresh rate. G-SYNC does not change that VSync rule. Output FPS Cap (Reflex) remains a separate final-output ceiling.");
      ImGui::TableNextColumn();
      if (driver_vsync_active) {
        ImGui::TextColored(kUiWarning, "Output target ignored; follows refresh");
      } else {
        ImGui::TextWrapped("Custom output target available when VSync is off");
      }
    }

    ImGui::EndTable();
  }

    ImGui::Spacing();
    ImGui::TextDisabled("FRAME RATE CONTROLS");
    ImGui::Separator();
    if (ImGui::BeginTable("##frame_rate_controls", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch,
                              0.62f);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch,
                              0.38f);
      const auto source_cap_state =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const std::string source_cap_summary =
          UserSourceCapSummary(source_cap_state);
      int source_cap = static_cast<int>(source_cap_state.configured_fps);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "Output FPS Cap (Reflex)", source_cap_summary.c_str(),
          "Sets the final/output FPS ceiling handled by native Reflex sleep and the DLSS-G pacer. Entering 120 targets approximately 120 displayed FPS. It applies with fixed 2x-6x, active Dynamic MFG, or fresh driver-confirmed MFG in Game controlled. A stricter native cap wins. FG-safe Off and active DXGI Waitable bypass/replace this sleep, so this cap is not guaranteed there. Use FrameView Displayed FPS to validate final output.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::InputInt("##rendered_fps_cap", &source_cap, 1, 10)) {
        source_cap = source_cap <= 0 ? 0
                                     : std::clamp(source_cap, 10, 1000);
        mfgunlock::framecount::g_reflex_source_fps_cap.store(
            static_cast<unsigned int>(source_cap),
            std::memory_order_relaxed);
        g_source_cap_config_origin.store(
            static_cast<unsigned int>(
                mfgunlock::pacing::SourceCapConfigOrigin::kConfigured),
            std::memory_order_relaxed);
        mfgunlock::framecount::NotifySourceFpsCapChanged();
        reshade::set_config_value(nullptr, kConfigSection,
                                  "ReflexSourceFpsCap", source_cap);
      }
      ImGui::EndTable();
      if (source_cap_state.configured_fps != 0) {
        ImGui::TextColored(UserSourceCapColor(source_cap_state.status), "%s",
                           UserSourceCapStatusText(source_cap_state.status));
        ImGui::TextDisabled(
            "This is the final FPS ceiling; actual displayed FPS may be lower.");
      }
    }
  }

  if (page == UiPage::Latency) {
    ImGui::Spacing();
    mfgunlock::ui::Section("Responsiveness","NVIDIA Reflex controls the timing of real frames. Generated FPS does not replace a healthy base frame rate.");
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::dlssglatency::Draw();
#endif
    if(ImGui::CollapsingHeader("Advanced pacing (testing)")) DrawReflexPacingLabControl(runtime);
    ImGui::Spacing();
    ImGui::TextDisabled("LATENCY GUARD");
    ImGui::Separator();
    DrawLatencyGuardControl();
    if(ImGui::CollapsingHeader("Latency details")) {
    const auto latency_view = mfgunlock::framecount::LatencyGuardSnapshot();
    const auto latency_guard_mode =
        static_cast<mfgunlock::pacing::LatencyGuardMode>(
            mfgunlock::framecount::g_latency_guard_mode.load(
                std::memory_order_relaxed));
    if (latency_guard_mode ==
        mfgunlock::pacing::LatencyGuardMode::kAutomatic) {
      ImGui::TextColored(
          kUiWarning,
          "Changing the DLSS-G multiplier may cause a brief hitch during trials and periodic rechecks.");
    }
    if (latency_guard_mode == mfgunlock::pacing::LatencyGuardMode::kOff) {
      ImGui::TextDisabled(
          "Latency monitoring is off. Select Monitor Only for read-only information.");
    } else {
    ImGui::Spacing();
    ImGui::TextDisabled("CURRENT LATENCY STATUS");
    const bool guard_sample_seen =
        latency_view.sample_seen;
    if (!guard_sample_seen) {
      ImGui::TextDisabled("Waiting for the first bounded NVAPI sample.");
    } else if (ImGui::BeginTable("##latency_guard_status", 2,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch,
                              0.48f);
      ImGui::TableSetupColumn("Observation",
                              ImGuiTableColumnFlags_WidthStretch, 0.52f);
      StatusSectionRow("INTEGRATION");
      const auto marker_health =
          static_cast<mfgunlock::pacing::MarkerHealth>(
              latency_view.marker_health);
      StatusRow("Reflex integration", MarkerHealthText(marker_health),
                marker_health == mfgunlock::pacing::MarkerHealth::kHealthy
                    ? kUiPositive
                    : (marker_health ==
                               mfgunlock::pacing::MarkerHealth::kWaiting ||
                           marker_health ==
                               mfgunlock::pacing::MarkerHealth::kUnavailable
                       ? kUiMuted
                       : kUiWarning));
      const uint32_t timing_issues =
          latency_view.timing_issue_mask;
      const bool source_timing_confident =
          latency_view.timing_confident;
      const bool queue_timing_confident =
          latency_view.queue_timing_confident;
      StatusRow("Source timing validation",
                TimingValidationText(timing_issues),
                source_timing_confident ? kUiPositive : kUiWarning);
      StatusRow("Queue timing validation",
                !source_timing_confident
                    ? "Waiting for valid source timing"
                    : (queue_timing_confident
                           ? "Verified from at least 48 queue markers"
                           : "Queue-only actions unavailable; other latency signals remain active"),
                queue_timing_confident
                    ? kUiPositive
                    : (source_timing_confident ? kUiWarning : kUiMuted));
      const unsigned int refresh =
          latency_view.display_refresh_fps;
      const unsigned int live_multiplier =
          latency_view.live_multiplier;
      const unsigned int source_cap =
          latency_view.recommended_source_cap_fps;
      const unsigned int active_source_cap =
          latency_view.active_source_cap_fps;
      const unsigned int source_fps =
          latency_view.estimated_source_fps;
      const unsigned int projected_output =
          latency_view.projected_output_fps;
      const unsigned int queue_wait =
          latency_view.queue_wait_us;
      const unsigned int queue_p95 =
          latency_view.queue_p95_us;
      const unsigned int input_to_gpu =
          latency_view.input_to_gpu_end_us;
      const unsigned int gpu_active =
          latency_view.gpu_active_us;
      const unsigned int pipeline =
          latency_view.pipeline_latency_us;
      const unsigned int pipeline_p95 =
          latency_view.pipeline_p95_us;
      const unsigned int ai_frame_time =
          latency_view.ai_frame_time_us;
      const std::string refresh_text =
          refresh == 0 ? "Not detected" : std::to_string(refresh) + " Hz";
      const std::string multiplier_text =
          live_multiplier < 2 ? "Not reported"
                              : std::to_string(live_multiplier) + "x live";
      const std::string source_cap_text =
          source_cap == 0
              ? "No queue trim justified"
              : std::to_string(source_cap) + " source FPS (small trim)";
      const std::string source_text =
          source_fps == 0 ? "Not available"
                          : std::to_string(source_fps) + " real FPS estimated";
      const std::string projected_text =
          projected_output == 0
              ? "Not available"
              : std::to_string(projected_output) + " FPS estimated";
      const std::string queue_text =
          queue_wait == 0
              ? "Not reported"
              : std::to_string(queue_wait / 1000.0f).substr(0, 4) +
                    " ms median / " +
                    std::to_string(queue_p95 / 1000.0f).substr(0, 4) +
                    " ms p95";
      const std::string input_text =
          input_to_gpu == 0
              ? "Input marker not reported"
              : std::to_string(input_to_gpu / 1000.0f).substr(0, 5) +
                    " ms (input sample to GPU end)";
      const std::string ai_text =
          ai_frame_time == 0
              ? "Not reported"
              : std::to_string(ai_frame_time / 1000.0f).substr(0, 5) +
                    " ms";
      const std::string pipeline_text =
          pipeline == 0
              ? "Not reported"
              : std::to_string(pipeline / 1000.0f).substr(0, 5) +
                    " ms median / " +
                    std::to_string(pipeline_p95 / 1000.0f).substr(0, 5) +
                    " ms p95";
      const std::string gpu_active_text =
          gpu_active == 0
              ? "Not reported"
              : std::to_string(gpu_active / 1000.0f).substr(0, 5) +
                    " ms active GPU";
      StatusSectionRow("PERFORMANCE");
      StatusRow("Display refresh", refresh_text.c_str(), kUiMuted);
      StatusRow("Active FG multiplier", multiplier_text.c_str(), kUiMuted);
      const bool iflip_known =
          latency_view.iflip_known;
      const bool iflip_active =
          latency_view.iflip_active;
      StatusRow("Independent flip",
                !iflip_known ? "Not reported"
                             : (iflip_active ? "Active" : "Inactive"),
                !iflip_known || iflip_active ? kUiMuted : kUiWarning);
      StatusRow("Rendered/source FPS", source_text.c_str(), kUiMuted);
      StatusRow("Estimated displayed FPS", projected_text.c_str(),
                latency_view.oversubscribed
                    ? kUiWarning
                    : kUiMuted);
      const uint32_t observed_source_interval = source_fps == 0
          ? 0
          : mfgunlock::pacing::TargetFpsToFrameLimitUs(source_fps);
      const bool meaningful_queue =
          queue_timing_confident && queue_wait >= 1500 &&
          observed_source_interval != 0 &&
          static_cast<uint64_t>(queue_wait) * 4ull >=
              observed_source_interval;
      StatusRow("Median render-queue wait", queue_text.c_str(),
                meaningful_queue ? kUiWarning : kUiMuted);
      StatusRow("Estimated pipeline latency", pipeline_text.c_str(), kUiMuted);
      HelpMarker(
          "This is a marker-to-GPU estimate from game/Reflex timestamps, not NVIDIA overlay end-to-end system latency.");
      StatusRow("GPU active work", gpu_active_text.c_str(), kUiMuted);
      StatusRow("Input-to-GPU latency", input_text.c_str(), kUiMuted);
      StatusRow("DLSS-G workload", ai_text.c_str(), kUiMuted);
      const auto bottleneck =
          static_cast<mfgunlock::pacing::LatencyBottleneck>(
              latency_view.bottleneck);
      StatusRow("Dominant observed stage",
                LatencyBottleneckText(bottleneck),
                bottleneck == mfgunlock::pacing::LatencyBottleneck::kBalanced
                    ? kUiPositive
                    : (bottleneck ==
                               mfgunlock::pacing::LatencyBottleneck::kInsufficientData
                           ? kUiMuted : kUiWarning));
      const unsigned int sample_cost =
          latency_view.sample_cost_us;
      const std::string sample_cost_text =
          sample_cost == 0 ? "Below timer resolution"
                           : std::to_string(sample_cost) + " us every 500 ms";
      StatusRow("Monitoring overhead", sample_cost_text.c_str(), kUiMuted);
      const unsigned int limit_source =
          mfgunlock::framecount::g_reflex_limit_source.load(
              std::memory_order_relaxed);
      const auto user_source_cap =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const bool explicit_source_cap =
          limit_source == static_cast<unsigned int>(
              mfgunlock::framecount::internal::ReflexTargetSource::
                  kUserSourceCap);
      const unsigned int multiplier_override =
          latency_view.multiplier_override;
      const bool multiplier_accepted =
          latency_view.multiplier_trial_accepted;
      const auto multiplier_phase =
          static_cast<mfgunlock::latency::MultiplierTrialPhase>(
              latency_view.multiplier_trial_phase);
      const auto multiplier_reason =
          static_cast<mfgunlock::latency::ResponsiveTrialReason>(
              latency_view.multiplier_trial_reason);
      const unsigned int saved_multiplier =
          mfgunlock::framecount::g_force_multiplier.load(
              std::memory_order_relaxed);
      const unsigned int approved_multiplier =
          latency_view.multiplier_approved;
      const unsigned int candidate_multiplier =
          latency_view.multiplier_candidate;
      StatusSectionRow("GUARD DECISION");
      const char* action =
          explicit_source_cap
              ? "Explicit Reflex output cap has priority"
              : (multiplier_phase !=
                         mfgunlock::latency::MultiplierTrialPhase::kIdle
                     ? MultiplierTrialPhaseText(multiplier_phase)
                     : (limit_source == 2
                     ? "Queue trim active"
                     : (latency_guard_mode ==
                                mfgunlock::pacing::LatencyGuardMode::kAutomatic
                            ? "Monitoring; no safe cap change"
                            : "Read-only monitoring")));
      StatusRow("Guard action", action,
                limit_source == 2 || explicit_source_cap ||
                        multiplier_override != 0
                    ? kUiPositive : kUiMuted);
      if (user_source_cap.configured_fps != 0) {
        const std::string cap_text =
            UserSourceCapSummary(user_source_cap) + "; " +
            UserSourceCapStatusText(user_source_cap.status);
        StatusRow("Output FPS cap", cap_text.c_str(),
                  UserSourceCapColor(user_source_cap.status));
      }
      if (latency_guard_mode ==
              mfgunlock::pacing::LatencyGuardMode::kAutomatic &&
          multiplier_phase ==
              mfgunlock::latency::MultiplierTrialPhase::kIdle) {
        const auto blocker =
            static_cast<mfgunlock::latency::ResponsiveTrialBlocker>(
                latency_view.trial_blocker);
        StatusRow("Responsive eligibility",
                  ResponsiveTrialBlockerText(blocker),
                  blocker ==
                          mfgunlock::latency::ResponsiveTrialBlocker::kNone
                      ? kUiPositive
                      : kUiMuted);
      }
      if (saved_multiplier >= 2) {
        const std::string saved_text =
            std::to_string(saved_multiplier) + "x (never overwritten)";
        StatusRow("Saved multiplier", saved_text.c_str(), kUiMuted);
      }
      if (multiplier_phase !=
          mfgunlock::latency::MultiplierTrialPhase::kIdle) {
        StatusRow("Responsive trial", MultiplierTrialPhaseText(multiplier_phase),
                  multiplier_accepted ? kUiPositive : kUiWarning);
        StatusRow("Trial reason", ResponsiveTrialReasonText(multiplier_reason),
                  kUiMuted);
        if (approved_multiplier >= 3 &&
            approved_multiplier < saved_multiplier) {
          const std::string approved_text =
              std::to_string(approved_multiplier) +
              "x is the last measured-beneficial step";
          StatusRow("Last approved multiplier", approved_text.c_str(),
                    kUiPositive);
        }
        if (candidate_multiplier >= 3 &&
            candidate_multiplier != saved_multiplier) {
          const std::string candidate_text =
              std::to_string(approved_multiplier) + "x -> " +
              std::to_string(candidate_multiplier) + "x";
          StatusRow("Current trial step", candidate_text.c_str(),
                    kUiWarning);
        }
        const unsigned int baseline_samples =
            latency_view.multiplier_baseline_samples;
        const unsigned int trial_samples =
            latency_view.multiplier_trial_samples;
        const std::string window_text =
            "baseline " + std::to_string(baseline_samples) +
            "/8; current " + std::to_string(trial_samples) + "/8";
        StatusRow("Measurement windows", window_text.c_str(), kUiMuted);
        const unsigned int baseline_pipeline =
            latency_view.multiplier_baseline_pipeline_us;
        const unsigned int baseline_p95 =
            latency_view.multiplier_baseline_p95_us;
        const unsigned int trial_pipeline =
            latency_view.multiplier_trial_pipeline_us;
        const unsigned int trial_p95 =
            latency_view.multiplier_trial_p95_us;
        if (baseline_pipeline != 0 && trial_pipeline != 0) {
          const std::string comparison =
              std::to_string(baseline_pipeline / 1000.0f).substr(0, 5) +
              "/" + std::to_string(baseline_p95 / 1000.0f).substr(0, 5) +
              " ms baseline -> " +
              std::to_string(trial_pipeline / 1000.0f).substr(0, 5) +
              "/" + std::to_string(trial_p95 / 1000.0f).substr(0, 5) +
              " ms measured (median/p95)";
          StatusRow("Pipeline comparison", comparison.c_str(), kUiMuted);
        }
      }
      if (source_cap != 0 && limit_source != 2 &&
          !user_source_cap.requested)
        StatusRow("Potential automatic trim", source_cap_text.c_str(),
                  kUiWarning);
      if (active_source_cap != 0) {
        const std::string active_text =
            std::to_string(active_source_cap) + " source FPS";
        StatusRow("Active queue-trim cap", active_text.c_str(), kUiPositive);
      }
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Automatic tests fixed multipliers one step at a time (6x -> 5x -> 4x -> 3x)\n"
        "when input/pipeline latency, a GPU-bound latency proxy, queueing, output saturation or DLSS-G workload justifies a trial.\n"
        "Each step uses two eight-sample windows and is kept only after lower measured latency.\n"
        "It periodically restores the saved multiplier for a new baseline and never goes below 3x.\n"
        "An explicit Reflex output-FPS cap remains unchanged across multiplier tests; queue trim never replaces it.\n"
        "A small source-rate trim is used only as a verified queue-pressure fallback when no explicit cap is set.\n"
        "It does not replace Reflex or change Dynamic MFG, VSync or G-SYNC behavior.");
    ImGui::TextDisabled(
        "Real/source FPS comes from game markers. Estimated output FPS is source FPS x the driver-reported multiplier; it is not a displayed-frame measurement.");
    HelpMarker(
        "ReShade and NVIDIA overlays may expose rendered/source FPS depending on their selected metric. Confirm actual displayed output with FrameView or PresentMon Displayed FPS.");
    }
  }

  } // Latency details
  if (page == UiPage::Quality) {
    mfgunlock::ui::Section("Image quality","Choose a stable quality profile. Changes marked for restart are saved for the next launch.");
    mfgunlock::ui::Card quality_card("##dlssg_quality_card");
    if (ImGui::BeginTable("##image_quality_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.38f);

    // 1.6 (plan B): DLSS Super Resolution preset, applied during play.
    {
      namespace preset = mfgunlock::dlsspreset;
      constexpr const char* kPresets[] = {"Game default", "Preset J", "Preset K", "Preset L", "Preset M"};
      constexpr uint32_t kValues[] = {0, static_cast<uint32_t>(sl::DLSSPreset::ePresetJ),
          static_cast<uint32_t>(sl::DLSSPreset::ePresetK), static_cast<uint32_t>(sl::DLSSPreset::ePresetL),
          static_cast<uint32_t>(sl::DLSSPreset::ePresetM)};
      const uint32_t selected = preset::g_selected.load(std::memory_order_relaxed);
      int index = 0;
      for (int i = 0; i < static_cast<int>(std::size(kValues)); ++i)
        if (kValues[i] == selected) index = i;
      const auto status = static_cast<preset::Status>(preset::g_status.load(std::memory_order_relaxed));
      const bool driver_override =
          (nvapi_status.ngx.feedback_super_resolution & mfgunlock::nvapistatus::kNgxOverridePreset) != 0;
      const uint32_t game_preset = preset::g_game_preset.load(std::memory_order_relaxed);
      std::string summary;
      if (!preset::g_wrapped.load(std::memory_order_relaxed) && preset::g_ngx_seen.load(std::memory_order_relaxed))
        summary = selected == preset::g_ngx_preset.load(std::memory_order_relaxed)
                      ? std::string("This game creates DLSS itself: ") + kPresets[index] + " is in use."
                      : "This game creates DLSS itself: the change applies when it recreates DLSS. Many games do that only when the output resolution changes or at the next launch.";
      else if (!preset::g_wrapped.load(std::memory_order_relaxed))
        summary = "Waiting for the game's DLSS options.";
      else if (status == preset::Status::kUnknownAbi)
        summary = "This game's DLSS options are newer than this build; its own presets are kept.";
      else if (status == preset::Status::kRejected)
        summary = "Streamline refused the preset; the game's presets are kept.";
      else if (status == preset::Status::kApplied)
        summary = std::string("Applied: ") + kPresets[index] + ".";
      else
        summary = game_preset ? "The game's preset (" + std::string(1, static_cast<char>('A' + game_preset - 1)) + ")."
                              : "The game's preset (DLSS default).";
      if (driver_override) summary += " An NVIDIA App override is active and takes precedence.";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "DLSS Super Resolution preset", summary.c_str(),
          "Replaces the game's DLSS Super Resolution preset while you play. K is NVIDIA's default for Quality, Balanced and DLAA; M for Performance; L for Ultra Performance; J is similar to K with slightly less ghosting and more flicker. Streamline rebuilds DLSS when the preset changes, so the image resets for one frame. Games that create DLSS through NGX without Streamline use it when they recreate DLSS, which many do only when the output resolution changes or at the next launch. A preset set in the NVIDIA App overrides this one.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::Combo("##dlss_sr_preset", &index, kPresets, static_cast<int>(std::size(kPresets)))) {
        preset::g_selected.store(kValues[index], std::memory_order_relaxed);
        TryInstallPresetHook();
        reshade::set_config_value(nullptr, kConfigSection, "DlssSuperResolutionPreset",
                                  static_cast<int>(kValues[index]));
      }
    }
    // 1.6 (plan B.6): DLSS Ray Reconstruction preset, applied during play.
    {
      namespace preset = mfgunlock::dlsspreset;
      constexpr const char* kPresets[] = {"Game default", "Preset D", "Preset E", "Preset F (DLSS 4.5)"};
      constexpr uint32_t kValues[] = {0, static_cast<uint32_t>(sl::DLSSDPreset::ePresetD),
          static_cast<uint32_t>(sl::DLSSDPreset::ePresetE), static_cast<uint32_t>(sl::DLSSDPreset::ePresetF)};
      const uint32_t selected = preset::g_rr_selected.load(std::memory_order_relaxed);
      int index = 0;
      for (int i = 0; i < static_cast<int>(std::size(kValues)); ++i)
        if (kValues[i] == selected) index = i;
      const auto status = static_cast<preset::Status>(preset::g_rr_status.load(std::memory_order_relaxed));
      const bool driver_override =
          (nvapi_status.ngx.feedback_ray_reconstruction & mfgunlock::nvapistatus::kNgxOverridePreset) != 0;
      const uint32_t game_preset = preset::g_rr_game_preset.load(std::memory_order_relaxed);
      std::string summary;
      if (!preset::g_rr_wrapped.load(std::memory_order_relaxed))
        summary = "Waiting for the game's Ray Reconstruction options (games with DLSS Ray Reconstruction only).";
      else if (status == preset::Status::kUnknownAbi)
        summary = "This game's Ray Reconstruction options have no preset that can be changed; its own is kept.";
      else if (status == preset::Status::kRejected)
        summary = "Streamline refused the preset; the game's preset is kept.";
      else if (status == preset::Status::kApplied)
        summary = std::string("Applied: ") + kPresets[index] + ".";
      else
        summary = game_preset ? "The game's preset (" + std::string(1, static_cast<char>('A' + game_preset - 1)) + ")."
                              : "The game's preset (DLSS default).";
      if (driver_override) summary += " An NVIDIA App override is active and takes precedence.";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "DLSS Ray Reconstruction preset", summary.c_str(),
          "Replaces the game's DLSS Ray Reconstruction preset while you play. D is NVIDIA's default transformer model and E a later transformer model; F is the DLSS 4.5 model and needs a recent Ray Reconstruction runtime (an older one uses its default instead). Streamline rebuilds Ray Reconstruction when the preset changes, so the image resets for one frame. Only games that pass Ray Reconstruction presets to Streamline (options version 3) can be changed. A preset set in the NVIDIA App overrides this one.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::Combo("##dlss_rr_preset", &index, kPresets, static_cast<int>(std::size(kPresets)))) {
        preset::g_rr_selected.store(kValues[index], std::memory_order_relaxed);
        reshade::set_config_value(nullptr, kConfigSection, "DlssRayReconstructionPreset",
                                  static_cast<int>(kValues[index]));
      }
    }

    constexpr const char* kHdrModes[] = {
        "Native (Most Games)", "Force UI Composition (Advanced)",
        "Automatic Guard + UI (HDR Compatibility)",
        "Final Color Fallback (Troubleshooting)"};
    int hdr_mode = static_cast<int>(
        mfgunlock::framecount::g_hdr_compatibility_mode.load(
            std::memory_order_relaxed));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const char* hdr_summary = "Native game inputs; recommended for most games.";
    if (hdr_mode == static_cast<int>(
                        mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
      hdr_summary = "Advanced UI separation; requires correctly matched game buffers.";
    } else if (hdr_mode == static_cast<int>(
                               mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
      hdr_summary =
          "HDR compatibility for Hogwarts Legacy, Jusant and Mafia: The Old Country.";
    } else if (hdr_mode == static_cast<int>(
                               mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
      hdr_summary = "Conservative HDR/UI troubleshooting fallback.";
    }
    SettingLabel(
        "Frame-generation Inputs", hdr_summary,
        "Native passes the game's tags unchanged and is recommended for most games. Automatic Guard + UI is intended for HDR-related Frame Generation artifacts in games such as Hogwarts Legacy, Jusant and Mafia: The Old Country. It validates HUD-less/UI inputs and falls back safely when their formats or color spaces do not match. If HUD elements show artifacts, return to Native.");
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##hdr_compatibility", &hdr_mode, kHdrModes,
                     static_cast<int>(std::size(kHdrModes)))) {
      mfgunlock::framecount::g_hdr_compatibility_mode.store(
          static_cast<unsigned int>(hdr_mode), std::memory_order_relaxed);
      mfgunlock::framecount::NotifyQualityModeChanged();
      reshade::set_config_value(nullptr, kConfigSection,
                                "HDRCompatibilityMode", hdr_mode);
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Adaptive Quality Suite",
        "Coordinates the existing improvements for edges and fine detail.",
        "Coordinates silhouette confidence, motion-direction scatter coverage, warp validation, candidate arbitration and inpaint decisions as one quality path. Standalone legacy experiments remain saved but do not stack on top of it. Exact kernel/provider validation is mandatory and unavailable components fall back independently. Requires a full game restart.",
        "Recommended", kUiPositive);
    ImGui::TableNextColumn();
    bool adaptive_quality =
        g_configured_adaptive_quality.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("##adaptive_quality", &adaptive_quality)) {
      g_configured_adaptive_quality.store(adaptive_quality,
                                          std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "ExperimentalAdaptiveQuality",
                                adaptive_quality ? 1 : 0);
    }
    if (adaptive_quality !=
        g_adaptive_quality.load(std::memory_order_relaxed))
      ImGui::TextDisabled("Saved for next launch; restart the game.");

    if (adaptive_quality) {
      constexpr const char* kAdaptiveProfiles[] = {
          "Stable (compatibility)",
          "Smooth transitions",
          "Balanced (recommended)"};
      int adaptive_profile = static_cast<int>(
          mfgunlock::adaptivequality::NormalizeProfile(
              g_configured_adaptive_quality_profile.load(
                  std::memory_order_relaxed))) - 1;
      const char* profile_summary =
          adaptive_profile == 2
              ? "Balanced handling of moving edges and lighting differences."
              : adaptive_profile == 1
                    ? "Smoother confidence transitions for fine detail and silhouettes."
                    : "Original compatibility profile.";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          "Adaptive Quality Profile", profile_summary,
          "V3 normalizes luma/chroma disagreement by local luminance and adds motion-oriented border and geometry confidence. V3.2 Local Stable uses only the existing 3x3 tile; optional Temporal Stable stores confidence bytes only after a strict CUDA probe and uses the low-overhead launch fast path. V2 retains absolute RGB confidence; Stable V1 preserves the 1.1.5 behavior. Components fall back independently. Requires restart.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::Combo("##adaptive_quality_profile", &adaptive_profile,
                       kAdaptiveProfiles,
                       static_cast<int>(std::size(kAdaptiveProfiles)))) {
        const auto selected = mfgunlock::adaptivequality::NormalizeProfile(
            static_cast<unsigned int>(adaptive_profile + 1));
        g_configured_adaptive_quality_profile.store(
            static_cast<unsigned int>(selected),
            std::memory_order_relaxed);
        reshade::set_config_value(
            nullptr, kConfigSection, "AdaptiveQualityProfile",
            static_cast<int>(selected));
      }
      if (g_configured_adaptive_quality_profile.load(
              std::memory_order_relaxed) !=
          g_adaptive_quality_profile.load(std::memory_order_relaxed)) {
        ImGui::TextDisabled("Saved for next launch; restart the game.");
      }
    }
    ImGui::EndTable();
  }
  if (page == UiPage::Quality &&
      g_configured_adaptive_quality.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        "Adaptive Quality manages geometry, boundary handling, warp validation and inpaint as one profile.");
  }

  if (page == UiPage::Support && ImGui::CollapsingHeader("Advanced compatibility controls")) {
    constexpr const char* kRuntimeModes[] = {
        "Game default", "Prefer local runtime - disable OTA",
        "Force NVIDIA OTA runtime"};
    int runtime_mode = static_cast<int>(
        g_configured_runtime_selection_mode.load(std::memory_order_relaxed));
    SetNextItemWidthWithHelp("Streamline runtime selection");
    if (ImGui::Combo("Streamline runtime selection", &runtime_mode,
                     kRuntimeModes,
                     static_cast<int>(std::size(kRuntimeModes)))) {
      g_configured_runtime_selection_mode.store(
          static_cast<unsigned int>(runtime_mode),
          std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "RuntimeSelectionMode", runtime_mode);
    }
    HelpMarker(
        "Selects which complete Streamline runtime package initializes on the next launch. Do not mix DLL versions. Game default is recommended. Requires restart.");

    int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
    SetNextItemWidthWithHelp("Reported MultiFrameCountMax");
    if (ImGui::SliderInt("Reported MultiFrameCountMax", &count,
                         static_cast<int>(kMinCount),
                         static_cast<int>(kMaxCount))) {
      count = std::clamp(count, static_cast<int>(kMinCount),
                         static_cast<int>(kMaxCount));
      g_max_count.store(static_cast<unsigned int>(count),
                        std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
    }
    HelpMarker(
        "Controls the maximum generated-frame count advertised to the game. Toggle Frame Generation off/on if the game's selector does not refresh.");

    bool temporal =
        g_configured_temporal_fix.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Temporal midpoint fix", &temporal)) {
      g_configured_temporal_fix.store(temporal,
                                      std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "TemporalFix",
                                temporal ? 1 : 0);
    }
    HelpMarker(
        "Corrects Ada midpoint compaction at higher multipliers. Applied during provider loading and requires restart.");

    bool blackwell = g_configured_blackwell_framework_kernels.load(
        std::memory_order_relaxed);
    if (ImGui::Checkbox("Prefer validated Blackwell framework kernels",
                        &blackwell)) {
      g_configured_blackwell_framework_kernels.store(
          blackwell, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "BlackwellFrameworkKernels",
                                blackwell ? 1 : 0);
    }
    HelpMarker(
        "Uses the full validated motion-vector/inpaint framework path when an exact provider match exists. Otherwise the addon fails closed to its compatible fallback. Requires restart.");

    bool flip_off =
        g_configured_force_flip_meter_off.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Legacy software flip pacing", &flip_off)) {
      g_configured_force_flip_meter_off.store(flip_off,
                                               std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "ForceFlipMeteringOff", flip_off ? 1 : 0);
    }
    HelpMarker(
        "Compatibility fallback for older Streamline integrations where 3x/4x freezes. Leave disabled with current runtimes. Requires restart.");

    const int active_runtime_mode = static_cast<int>(
        mfgunlock::framecount::g_runtime_selection_mode.load(
            std::memory_order_relaxed));
    if (active_runtime_mode != static_cast<int>(
                                   mfgunlock::framecount::RuntimeSelectionMode::kGameDefault) &&
        !mfgunlock::framecount::g_runtime_selection_observed.load(
            std::memory_order_acquire) &&
        !HasEarlyLoadEntry()) {
      ImGui::TextColored(kUiWarning,
                         "Streamline initialized before normal addon loading.");
      if (ImGui::Button("Enable early addon loading")) {
        EnsureEarlyLoadEntry();
      }
      HelpMarker(
          "Adds this addon's current filename to ReShade's ADDON.LoadFromDllMain list. Required only when a selected runtime policy must intercept slInit before normal addon loading. Requires restart.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("IMAGE QUALITY COMPATIBILITY");
    DrawAdvancedQualityControls(
        g_configured_adaptive_quality.load(std::memory_order_relaxed));
  }
  }

  if (page == UiPage::Support) {
    ImGui::Spacing();
    ImGui::TextDisabled("SUPPORT DIAGNOSTICS");
    ImGui::Separator();
    bool gate_patched = false;
    bool midpoint_patched = false;
    bool blackwell_patched = false;
    bool thin_geometry_patched = false;
    size_t gate_provider_count = 0;
    size_t gate_site_count = 0;
    size_t midpoint_provider_count = 0;
    size_t blackwell_provider_count = 0;
    size_t thin_geometry_provider_count = 0;
    std::string midpoint_detail;
    std::string blackwell_detail;
    std::string blackwell_applied_detail;
    mfgunlock::blackwell::Result blackwell_active_result{};
    ThinGeometryModulePatch thin_geometry_last;
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    ImGui::TextDisabled("Diagnostics updating; retry next frame.");
    return;
  }
    gate_patched = g_gate_patched.load(std::memory_order_relaxed);
    midpoint_patched = g_midpoint_patched.load(std::memory_order_relaxed);
    blackwell_patched = g_blackwell_patched.load(std::memory_order_relaxed);
    thin_geometry_patched =
        g_thin_geometry_patched.load(std::memory_order_relaxed);
    gate_provider_count = g_gate_modules.size();
    gate_site_count = g_gate_sites.size();
    midpoint_provider_count = g_midpoint_modules.size();
    blackwell_provider_count = g_blackwell_modules.size();
    thin_geometry_provider_count = g_thin_geometry_modules.size();
    midpoint_detail = g_midpoint_detail;
    blackwell_detail = g_blackwell_detail;
    blackwell_applied_detail = g_blackwell_applied_detail;
    blackwell_active_result = AggregateBlackwellResults(g_blackwell_modules);
    thin_geometry_last =
        SelectRelevantThinGeometryResult(g_thin_geometry_modules);
    ReleaseSRWLockShared(&g_provider_maintenance_lock);

    ImGui::TextDisabled("RUNTIME");
    if (ImGui::BeginTable("##runtime_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      StatusRow("Renderer", RenderApiName(render_api), kUiMuted);
      StatusRow("Streamline", streamline_text.c_str(), kUiMuted);
      StatusRow("DLSS-G", dlssg_text.c_str(), kUiMuted);
      StatusRow("DLSS Super Resolution", super_resolution_preset.c_str(),
                kUiMuted);
      StatusRow("DLSS Frame Generation", frame_generation_preset.c_str(),
                kUiMuted);
      StatusRow("DLSS Ray Reconstruction",
                ray_reconstruction_preset.c_str(), kUiMuted);
      StatusRow("HDR", hdr_seen ? (hdr_active ? "Yes" : "No") : "Unknown",
                kUiMuted);
      const std::string diagnostic_sync =
          !sleep_status_available
              ? "Not reported"
              : (driver_vsync_active && gsync_active
                     ? "Driver VSync + G-SYNC active"
                     : (driver_vsync_active
                            ? "Driver VSync active"
                            : (gsync_active ? "G-SYNC active"
                                            : "Inactive")));
      StatusRow("Driver sync", diagnostic_sync.c_str(), kUiMuted);
      StatusRow("Dynamic MFG VSync API",
                !mfgunlock::framecount::g_vsync_support_seen.load(
                     std::memory_order_acquire)
                    ? "Not reported"
                    : (mfgunlock::framecount::g_vsync_supported.load(
                           std::memory_order_relaxed)
                           ? "Supported by active runtime"
                           : "Not supported by active runtime"),
                kUiMuted);
      ImGui::EndTable();
    }
    TextDisabledWrapped(
        "This reports the Streamline/DLSS-G Dynamic interface, not whether driver VSync is enabled.");

    ImGui::Spacing();
    ImGui::TextDisabled("MFG");
    if (ImGui::BeginTable("##mfg_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      const std::string game_request_text =
          game_request_seen ? std::to_string(game_multiplier) + "x"
                            : "Not observed";
      const std::string effective_text =
          effective_seen ? std::to_string(effective_multiplier) + "x"
                         : "Not observed";
      StatusRow("Game request", game_request_text.c_str(), kUiMuted);
      StatusRow("Effective request", effective_text.c_str(), kUiMuted);
      const std::string actual_text =
          state_seen
              ? std::to_string(mfgunlock::framecount::g_actual_frames_presented.load(
                    std::memory_order_relaxed))
              : "No sample";
      const std::string live_multiplier =
          driver_multiplier >= 2
              ? std::to_string(driver_multiplier) + "x live"
              : "Not reported";
      StatusRow("Driver live multiplier", live_multiplier.c_str(),
                driver_multiplier >= 2 ? kUiPositive : kUiMuted);
      StatusRow("Provider presentations", actual_text.c_str(),
                mfg_confirmed ? kUiPositive : kUiMuted);
      StatusRow("Validation evidence", validation_text.c_str(),
                runtime_error
                    ? kUiError
                    : (request_rejected
                           ? kUiWarning
                           : (mfg_request_accepted ? kUiPositive : kUiMuted)));
      StatusRow("Dynamic MFG",
                mfgunlock::framecount::g_dynamic_applied.load(
                    std::memory_order_relaxed)
                    ? "Active"
                    : (dynamic_available ? "Available" : "Inactive"),
                kUiMuted);
      const auto source_cap_state =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const std::string source_cap_value =
          std::to_string(source_cap_state.configured_fps) +
          " final/output FPS; " +
          UserSourceCapStatusText(source_cap_state.status);
      StatusRow("Output FPS cap", source_cap_value.c_str(),
                UserSourceCapColor(source_cap_state.status));
      std::ostringstream source_cap_intervals;
      source_cap_intervals << "requested "
                           << source_cap_state.requested_limit_us
                           << " us; native "
                           << source_cap_state.native_limit_us
                           << " us; effective "
                           << source_cap_state.effective_limit_us << " us";
      StatusRow("Reflex cap intervals", source_cap_intervals.str().c_str(),
                kUiMuted);
      StatusRow(
          "Cap configuration source",
          SourceCapConfigOriginText(
              static_cast<mfgunlock::pacing::SourceCapConfigOrigin>(
                  g_source_cap_config_origin.load(
                      std::memory_order_relaxed))),
          kUiMuted);
      ImGui::EndTable();
    }
    TextDisabledWrapped(
        "Evidence is layered: an accepted request, a driver-live multiplier and provider output are separate observations.");
    HelpMarker(
        "ReShade's FPS counter and many overlays can report game/source Presents rather than frames scanned out after DLSS-G. Estimated output FPS is never treated as a measured result here. Use FrameView or PresentMon Displayed FPS for external confirmation.");

    ImGui::Spacing();
    ImGui::TextDisabled("PATCHES");
    if (ImGui::BeginTable("##patch_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      StatusRow("Architecture gates", gate_patched ? "Active" : "Pending",
                gate_patched ? kUiPositive : kUiWarning);
      StatusRow("Temporal fix",
                blackwell_patched
                    ? "Blackwell framework active"
                    : (midpoint_patched ? "Midpoint fallback active"
                                        : "Pending"),
                blackwell_patched || midpoint_patched ? kUiPositive
                                                      : kUiWarning);
      StatusRow("Thin-geometry patches",
                thin_geometry_patched ? "At least one active" : "Pending",
                thin_geometry_patched ? kUiPositive : kUiWarning);
      if (g_adaptive_quality.load(std::memory_order_relaxed)) {
        const auto requested_profile =
            mfgunlock::adaptivequality::NormalizeProfile(
                g_adaptive_quality_profile.load(std::memory_order_relaxed));
        const bool adaptive_blend =
            thin_geometry_last.result.validated_warp_blend.applied;
        const bool adaptive_complete =
            blackwell_active_result.adaptive_geometry && adaptive_blend &&
            blackwell_active_result.adaptive_inpaint_decision;
        const auto warp_version =
            thin_geometry_last.result.validated_warp_blend.adaptive_version;
        const auto geometry_version =
            blackwell_active_result.adaptive_geometry_version;
        const auto inpaint_version =
            blackwell_active_result.adaptive_inpaint_version;
        const auto expected_warp =
            mfgunlock::adaptivequality::ExpectedComponentVersion(
                requested_profile,
                mfgunlock::adaptivequality::Component::kWarp);
        const auto expected_geometry =
            requested_profile ==
                        mfgunlock::adaptivequality::Profile::
                            kLuminanceDirectionalV3 &&
                    !g_adaptive_quality_v3_oriented_geometry.load(
                        std::memory_order_relaxed)
                ? mfgunlock::adaptivequality::ComponentVersion::kV2
                : mfgunlock::adaptivequality::ExpectedComponentVersion(
                      requested_profile,
                      mfgunlock::adaptivequality::Component::kGeometry);
        const auto requested_inpaint_mode =
            mfgunlock::cudatemporal::NormalizeInpaintMode(
                g_adaptive_quality_v3_inpaint_mode.load(
                    std::memory_order_relaxed));
        const auto expected_inpaint =
            requested_profile ==
                    mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
                requested_inpaint_mode !=
                    mfgunlock::cudatemporal::InpaintMode::kV2Compatibility
                ? mfgunlock::adaptivequality::ComponentVersion::kV3
                : mfgunlock::adaptivequality::ExpectedComponentVersion(
                      requested_profile,
                      mfgunlock::adaptivequality::Component::kInpaint);
        const bool all_requested_versions =
            warp_version == expected_warp &&
            geometry_version == expected_geometry &&
            inpaint_version == expected_inpaint;
        const std::string profile_status =
            std::string(mfgunlock::adaptivequality::ProfileName(
                requested_profile)) +
            (adaptive_complete && all_requested_versions
                 ? "; core components applied"
                 : "; partial/fallback");
        StatusRow("Adaptive Quality Suite",
                  profile_status.c_str(),
                  adaptive_complete && all_requested_versions
                      ? kUiPositive
                      : kUiWarning);
        const std::string warp_status = AdaptiveComponentStatus(
            expected_warp, warp_version, adaptive_blend);
        StatusRow("Warp confidence", warp_status.c_str(),
                  AdaptiveComponentColor(expected_warp, warp_version,
                                         adaptive_blend));
        const std::string geometry_status = AdaptiveComponentStatus(
            expected_geometry, geometry_version,
            blackwell_active_result.adaptive_geometry);
        StatusRow("Geometry confidence", geometry_status.c_str(),
                  AdaptiveComponentColor(
                      expected_geometry, geometry_version,
                      blackwell_active_result.adaptive_geometry));
        if (geometry_version ==
            mfgunlock::adaptivequality::ComponentVersion::kV3) {
          StatusRow(
              "Geometry V3 install",
              blackwell_active_result.adaptive_geometry_redirected
                  ? "Full ptxas cubin; exact fatbin redirect"
                  : "In-place cubin",
              blackwell_active_result.adaptive_geometry_redirected
                  ? kUiPositive
                  : kUiMuted);
          StatusRow("Geometry V3.2 cubin",
                    mfgunlock::blackwell::AdaptiveGeometryVariantName(
                        blackwell_active_result.adaptive_geometry_variant),
                    blackwell_active_result.adaptive_geometry_variant ==
                            mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal
                        ? kUiPositive
                        : kUiMuted);
          StatusRow("Geometry V3.2 ptxas",
                    "40 registers; 0 spills/local; 7,776 B shared",
                    kUiPositive);
        }
        const std::string inpaint_status = AdaptiveComponentStatus(
            expected_inpaint, inpaint_version,
            blackwell_active_result.adaptive_inpaint_decision);
        StatusRow("Inpaint decision", inpaint_status.c_str(),
                  AdaptiveComponentColor(
                      expected_inpaint, inpaint_version,
                      blackwell_active_result.adaptive_inpaint_decision));
        if (requested_profile ==
            mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3) {
          const char* requested_inpaint =
              requested_inpaint_mode ==
                      mfgunlock::cudatemporal::InpaintMode::kTemporal
                  ? "Temporal V3"
                  : requested_inpaint_mode ==
                            mfgunlock::cudatemporal::InpaintMode::kLocal
                        ? "Local V3"
                        : "V2 Compatibility";
          StatusRow("Inpaint V3.4 requested", requested_inpaint, kUiMuted);
          StatusRow(
              "Inpaint V3.4 cubin",
              mfgunlock::blackwell::AdaptiveInpaintVariantName(
                  blackwell_active_result.adaptive_inpaint_variant),
              blackwell_active_result.adaptive_inpaint_variant ==
                      mfgunlock::blackwell::AdaptiveInpaintVariant::kTemporal
                  ? kUiPositive
                  : kUiMuted);
          const std::string inpaint_temporal_detail =
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
              "V3 inpaint removed; V2 Compatibility requested (see installed cubin above)";
#else
              mfgunlock::cudatemporal::InpaintDetail();
#endif
          StatusRow(
              "Inpaint V3.4 effective", inpaint_temporal_detail.c_str(),
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
              kUiMuted);
#else
              mfgunlock::cudatemporal::g_inpaint_temporal_active.load(
                      std::memory_order_acquire)
                  ? kUiPositive
                  : (mfgunlock::cudatemporal::g_inpaint_fallback.load(
                             std::memory_order_acquire)
                         ? kUiWarning
                         : kUiMuted));
#endif
        }
        if (requested_profile ==
            mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3) {
          StatusRow(
              "V3 photometric A/B",
              g_adaptive_quality_v3_photometric.load(
                      std::memory_order_relaxed)
                  ? "Relative luma/chroma enabled"
                  : "Disabled; using V2 absolute RGB",
              kUiMuted);
          StatusRow(
              "V3 border A/B",
              g_adaptive_quality_v3_directional_border.load(
                      std::memory_order_relaxed)
                  ? "Motion-directional taper enabled"
                  : "Disabled; using V2 symmetric taper",
              kUiMuted);
          StatusRow(
              "V3 geometry A/B",
              g_adaptive_quality_v3_oriented_geometry.load(
                      std::memory_order_relaxed)
                  ? "Motion-oriented diagonals enabled"
                  : "Disabled; requesting geometry V2",
              kUiMuted);
        }
        StatusRow("Directional scatter",
                  blackwell_active_result.adaptive_directional_scatter
                      ? "Native motion-adaptive coverage retained"
                      : "Baseline",
                  kUiMuted);
        const auto stability_mode = mfgunlock::cudatemporal::NormalizeMode(
            g_adaptive_quality_v3_stability_mode.load(
                std::memory_order_relaxed));
        StatusRow(
            "V3.2 stability requested",
            stability_mode ==
                    mfgunlock::cudatemporal::StabilityMode::kTemporal
                ? "Temporal Stable"
                : "Local Stable",
            kUiMuted);
        const std::string temporal_detail =
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
            "History disabled; dedicated Local cubin requested (see installed variant above)";
#else
            mfgunlock::cudatemporal::Detail();
#endif
        StatusRow(
            "V3.2 stability effective", temporal_detail.c_str(),
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
            kUiMuted);
#if defined(MFGUNLOCK_LOCAL_STABILITY)
#if defined(MFGUNLOCK_RELEASE_1_4_5)
        StatusRow("Build", "Release 1.4.5 Engineering; gameplay validation pending", kUiWarning);
#elif defined(MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
        StatusRow("Build", "Experimental 1.4.4 Engineering; gameplay validation pending", kUiWarning);
#elif defined(MFGUNLOCK_RELEASE_1_4_4)
        StatusRow("Build", "Release 1.4.4; DLSS-G 310.9.16 + Streamline 2.14.16 hotfix", kUiPositive);
#elif defined(MFGUNLOCK_RELEASE_1_4_3)
        StatusRow("Build", "Release 1.4.3; continuous border axes + paired diagonals", kUiPositive);
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
        StatusRow("Build", "1.4.2-base; continuous-axis border candidate; in-game validation pending", kUiMuted);
#else
        StatusRow("Build", "Release 1.4.2; continuous borders + paired diagonals", kUiPositive);
#endif
#else
        StatusRow("Build", "Local low-overhead; no V3 inpaint or confidence history", kUiPositive);
#endif
        StatusRow("Temporal backend", "Disabled by build; 0 B history; no self-test or barriers", kUiMuted);
        StatusRow("CUDA hook fast path", "Not installed; no CUDA/NVAPI launch interception", kUiMuted);
#else
            mfgunlock::cudatemporal::g_temporal_active.load(
                    std::memory_order_acquire)
                ? kUiPositive
                : (mfgunlock::cudatemporal::g_fallback.load(
                           std::memory_order_acquire)
                       ? kUiWarning
                       : kUiMuted));
        StatusRow(
            "CUDA hook fast path",
            mfgunlock::cudatemporal::FastPathReady()
                ? "Active; atomic CUfunction compare only for non-target kernels"
                : (mfgunlock::cudatemporal::g_hooked.load(
                           std::memory_order_acquire)
                       ? "Hook installed; target association pending"
                       : "Inactive"),
            mfgunlock::cudatemporal::FastPathReady() ? kUiPositive
                                                     : kUiMuted);
#endif
        if (mfgunlock::cudatemporal::g_history_bytes.load(
                std::memory_order_relaxed) != 0) {
          const uint64_t history_bytes =
              mfgunlock::cudatemporal::g_history_bytes.load(
                  std::memory_order_relaxed);
          std::ostringstream history_status;
          history_status << history_bytes / (1024.0 * 1024.0) << " MiB; "
                         << mfgunlock::cudatemporal::g_history_width.load(
                                std::memory_order_relaxed)
                         << 'x'
                         << mfgunlock::cudatemporal::g_history_height.load(
                                std::memory_order_relaxed)
                         << "; "
                         << mfgunlock::cudatemporal::g_history_multiplier.load(
                                std::memory_order_relaxed)
                         << 'x';
          StatusRow("V3.2 confidence history",
                    history_status.str().c_str(), kUiMuted);
        }
        if (mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                std::memory_order_relaxed) != 0) {
          std::ostringstream inpaint_history;
          inpaint_history
              << mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                     std::memory_order_relaxed) /
                     (1024.0 * 1024.0)
              << " MiB; total arena used "
              << (mfgunlock::cudatemporal::g_history_bytes.load(
                      std::memory_order_relaxed) +
                  mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                      std::memory_order_relaxed)) /
                     (1024.0 * 1024.0)
              << " / 96 MiB; resets "
              << mfgunlock::cudatemporal::g_history_resets.load(
                     std::memory_order_relaxed);
          StatusRow("V3.4 inpaint history",
                    inpaint_history.str().c_str(), kUiMuted);
        }
      }
      if (!g_adaptive_quality.load(std::memory_order_relaxed)) {
        StatusRow("Geometry confidence V2",
                  !g_geometry_confidence_v2.load(std::memory_order_relaxed)
                      ? "Off"
                      : (!g_quality_refinement.load(std::memory_order_relaxed)
                             ? "Requires Quality Refinement"
                             : (g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                                        static_cast<unsigned int>(mfgunlock::blackwell::SilhouetteGuardMode::Balanced)
                                    ? "Requires Balanced boundary mode"
                                    : (thin_geometry_last.geometry_confidence_v2_applied
                                           ? "Patch applied; visual test pending"
                                           : "Requested; V1/fallback or no patch"))),
                  kUiMuted);
        StatusRow("Border confidence trial",
                  !g_border_confidence.load(std::memory_order_relaxed)
                      ? "Off"
                      : (!g_quality_refinement.load(std::memory_order_relaxed)
                             ? "Requires Quality Refinement"
                             : (!thin_geometry_last.result.validated_warp_blend.applied
                                    ? "Requested; blend not applied"
                                    : "Patch applied; visual test pending")),
                  kUiMuted);
      }
      if (!g_adaptive_quality.load(std::memory_order_relaxed) &&
          thin_geometry_provider_count != 0) {
        const auto mechanism_state = [](const auto& result) {
          return !result.requested ? "Not requested"
                                   : (result.applied ? "Active" : "Not applied");
        };
        const bool intermediate_superseded =
            g_thin_geometry_intermediate_scatter.load(
                std::memory_order_relaxed) &&
            (g_adaptive_quality.load(std::memory_order_relaxed) ||
             g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                static_cast<unsigned int>(
                    mfgunlock::blackwell::SilhouetteGuardMode::Off));
        StatusRow("Intermediate retention",
                  intermediate_superseded
                      ? "Superseded by Boundary mitigation"
                      : mechanism_state(
                            thin_geometry_last.intermediate_scatter),
                  thin_geometry_last.intermediate_scatter.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow("Boundary mitigation",
                  mechanism_state(
                      thin_geometry_last.silhouette_boundary_guard),
                  thin_geometry_last.silhouette_boundary_guard.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow("Validated warp blend",
                  mechanism_state(
                      thin_geometry_last.result.validated_warp_blend),
                  thin_geometry_last.result.validated_warp_blend.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow("Previous scatter",
                  mechanism_state(thin_geometry_last.result.previous_scatter),
                  thin_geometry_last.result.previous_scatter.applied
                      ? kUiPositive
                      : kUiMuted);
      }
      StatusRow("Input Quality Guard",
                mfgunlock::framecount::g_hud_inputs_suppressed.load(
                    std::memory_order_relaxed)
                    ? "Filtered incompatible input"
                    : "No filtering reported",
                kUiMuted);
      const unsigned int tracked_viewports =
          mfgunlock::framecount::g_quality_viewport_count.load(
              std::memory_order_relaxed);
      const bool viewport_capacity_exhausted =
          mfgunlock::framecount::g_quality_viewport_capacity_exhausted.load(
              std::memory_order_acquire);
      const std::string viewport_text =
          std::to_string(tracked_viewports) + " / " +
          std::to_string(mfgunlock::framecount::kMaxQualityViewports) +
          (viewport_capacity_exhausted ? "; capacity exhausted" : " tracked");
      StatusRow("Quality Guard viewports", viewport_text.c_str(),
                viewport_capacity_exhausted ? kUiWarning : kUiMuted);
      const auto tag_lock_contentions =
          mfgunlock::framecount::g_quality_tag_lock_contentions.load(
              std::memory_order_relaxed);
      const std::string contention_text =
          std::to_string(tag_lock_contentions) +
          " non-blocking fallback(s)";
      StatusRow("Quality Guard contention", contention_text.c_str(),
                tag_lock_contentions == 0 ? kUiMuted : kUiWarning);
      ImGui::EndTable();
    }

    std::string intermediate_report = "Disabled";
    if (g_adaptive_quality.load(std::memory_order_relaxed)) {
      intermediate_report = "Managed by Adaptive Quality Suite";
    } else if (g_thin_geometry_intermediate_scatter.load(
            std::memory_order_relaxed)) {
      if (g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
          static_cast<unsigned int>(
              mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
        intermediate_report = "Superseded by Boundary mitigation";
      } else if (thin_geometry_last.intermediate_scatter.applied) {
        intermediate_report = "Active";
      } else {
        intermediate_report = "Requested; not applied";
      }
    }
    const auto adaptive_profile =
        mfgunlock::adaptivequality::NormalizeProfile(
            g_adaptive_quality_profile.load(std::memory_order_relaxed));
    const auto expected_warp =
        mfgunlock::adaptivequality::ExpectedComponentVersion(
            adaptive_profile, mfgunlock::adaptivequality::Component::kWarp);
    const auto expected_geometry =
        adaptive_profile ==
                    mfgunlock::adaptivequality::Profile::
                        kLuminanceDirectionalV3 &&
                !g_adaptive_quality_v3_oriented_geometry.load(
                    std::memory_order_relaxed)
            ? mfgunlock::adaptivequality::ComponentVersion::kV2
            : mfgunlock::adaptivequality::ExpectedComponentVersion(
                  adaptive_profile,
                  mfgunlock::adaptivequality::Component::kGeometry);
    const auto requested_inpaint_mode =
        mfgunlock::cudatemporal::NormalizeInpaintMode(
            g_adaptive_quality_v3_inpaint_mode.load(
                std::memory_order_relaxed));
    const auto expected_inpaint =
        adaptive_profile ==
                mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
            requested_inpaint_mode !=
                mfgunlock::cudatemporal::InpaintMode::kV2Compatibility
            ? mfgunlock::adaptivequality::ComponentVersion::kV3
            : mfgunlock::adaptivequality::ExpectedComponentVersion(
                  adaptive_profile,
                  mfgunlock::adaptivequality::Component::kInpaint);
    const std::string adaptive_warp_report = AdaptiveComponentStatus(
        expected_warp,
        thin_geometry_last.result.validated_warp_blend.adaptive_version,
        thin_geometry_last.result.validated_warp_blend.applied);
    const std::string adaptive_geometry_report = AdaptiveComponentStatus(
        expected_geometry, blackwell_active_result.adaptive_geometry_version,
        blackwell_active_result.adaptive_geometry);
    const std::string adaptive_inpaint_report = AdaptiveComponentStatus(
        expected_inpaint, blackwell_active_result.adaptive_inpaint_version,
        blackwell_active_result.adaptive_inpaint_decision);
    const auto source_cap_state =
        mfgunlock::framecount::internal::ResolveUserSourceCapState();
    const auto source_cap_origin =
        static_cast<mfgunlock::pacing::SourceCapConfigOrigin>(
            g_source_cap_config_origin.load(std::memory_order_relaxed));
    const auto latency_view = mfgunlock::framecount::LatencyGuardSnapshot();
    std::ostringstream report;
    report << "MFG Unlock Diagnostics\n"
           << "Renderer: " << RenderApiName(render_api) << '\n'
           << "Streamline: " << streamline_text << '\n'
           << "DLSS-G: " << dlssg_text << '\n'
           << "DLSS Super Resolution: " << super_resolution_preset << '\n'
           << "DLSS Frame Generation: " << frame_generation_preset << '\n'
           << "DLSS Ray Reconstruction: " << ray_reconstruction_preset
           << '\n'
           << "HDR: " << (hdr_seen ? (hdr_active ? "Yes" : "No") : "Unknown")
           << '\n'
           << "Game request: "
           << (game_request_seen ? std::to_string(game_multiplier) + "x"
                                 : "Not observed")
           << '\n'
           << "Effective request: "
           << (effective_seen ? std::to_string(effective_multiplier) + "x"
                              : "Not observed")
           << '\n'
           << "Driver live multiplier: "
           << (driver_multiplier >= 2
                   ? std::to_string(driver_multiplier) + "x"
                   : "Not reported")
           << '\n'
           << "Provider presentations: "
           << (state_seen ? std::to_string(observed_presentations)
                          : "Not sampled")
           << '\n'
           << "Dynamic MFG: "
           << (mfgunlock::framecount::g_dynamic_applied.load(
                   std::memory_order_relaxed)
                   ? "Active"
                   : (dynamic_available ? "Available" : "Unavailable/Pending"))
           << '\n'
           << "Dynamic target FPS: "
           << mfgunlock::framecount::g_dynamic_target_fps.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Output FPS cap (Reflex): "
           << source_cap_state.configured_fps << " final/output FPS; state "
           << UserSourceCapStatusText(source_cap_state.status)
           << "; estimated multiplier "
           << source_cap_state.estimated_multiplier
           << "x; requested/native/effective interval "
           << source_cap_state.requested_limit_us << '/'
           << source_cap_state.native_limit_us << '/'
           << source_cap_state.effective_limit_us << " us; config "
           << SourceCapConfigOriginText(source_cap_origin)
           << '\n'
           << "Latency Guard mode: "
           << mfgunlock::framecount::g_latency_guard_mode.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Timestamp units (0 unknown / 1 us / 2 QPC): "
           << latency_view.units << '\n'
           << "Fresh completed frame count: "
           << latency_view.new_frames << '\n'
           << "Adaptive Quality Suite: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? mfgunlock::adaptivequality::ProfileName(adaptive_profile)
                   : "Off") << '\n'
           << "V3 photometric A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_photometric.load(
                          std::memory_order_relaxed)
                          ? "relative luma/chroma"
                          : "disabled; V2 absolute RGB")
                   : "inactive outside V3") << '\n'
           << "V3 border A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_directional_border.load(
                          std::memory_order_relaxed)
                          ? "motion-directional"
                          : "disabled; V2 symmetric")
                   : "inactive outside V3") << '\n'
           << "V3 geometry A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_oriented_geometry.load(
                          std::memory_order_relaxed)
                          ? "motion-oriented diagonals"
                          : "disabled; V2 geometry")
                   : "inactive outside V3") << '\n'
           << "Adaptive warp confidence: "
           << adaptive_warp_report << '\n'
           << "Adaptive geometry confidence: "
           << adaptive_geometry_report << '\n'
           << "Geometry V3 install: "
           << (blackwell_active_result.adaptive_geometry_redirected
                   ? "Full ptxas cubin; exact fatbin descriptor redirect"
                   : "No oversized redirect active") << '\n'
           << "Geometry V3.2 cubin: "
           << mfgunlock::blackwell::AdaptiveGeometryVariantName(
                  blackwell_active_result.adaptive_geometry_variant)
           << (blackwell_active_result.adaptive_geometry_variant ==
                       mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal
                   ? "; 40 registers; 7,776 B shared; zero stack/spill/local; .text=41,216 B; 8 global loads (2 u8) + 2 u8 stores"
                   : blackwell_active_result.adaptive_geometry_variant ==
                             mfgunlock::blackwell::AdaptiveGeometryVariant::kLocal
                         ? "; 40 registers; 7,776 B shared; zero stack/spill/local; .text="
#if defined(MFGUNLOCK_LOCAL_STABILITY)
                           "39,680 B; paired diagonal consensus; zero global loads/stores"
#else
                           "39,552 B; zero global loads/stores"
#endif
                         : "; resource audit unavailable for fallback/native")
           << '\n'
           << "Adaptive inpaint decision: "
           << adaptive_inpaint_report << '\n'
           << "Inpaint V3.4 requested: "
           << (requested_inpaint_mode ==
                       mfgunlock::cudatemporal::InpaintMode::kTemporal
                   ? "Temporal V3"
                   : requested_inpaint_mode ==
                             mfgunlock::cudatemporal::InpaintMode::kLocal
                         ? "Local V3"
                         : "V2 Compatibility")
           << '\n'
           << "Inpaint V3.4 cubin: "
           << mfgunlock::blackwell::AdaptiveInpaintVariantName(
                  blackwell_active_result.adaptive_inpaint_variant)
           << (blackwell_active_result.adaptive_inpaint_variant ==
                       mfgunlock::blackwell::AdaptiveInpaintVariant::kTemporal
                   ? "; ABI 152 bytes; 48 registers; 784 B shared; zero stack/spill/local; .text=10,368 B; one u8 history load + one u8 store"
                   : blackwell_active_result.adaptive_inpaint_variant ==
                             mfgunlock::blackwell::AdaptiveInpaintVariant::kLocal
                         ? "; 48 registers; 784 B shared; zero stack/spill/local; .text=9,344 B; zero global history access"
                         : "; compatibility/fallback resources")
           << '\n'
           << "Inpaint V3.4 effective: "
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
           << "V3 inpaint removed; V2 Compatibility requested (see installed cubin above)" << '\n'
           << "Confidence history: 0 bytes; disabled by local low-overhead build" << '\n'
#else
           << mfgunlock::cudatemporal::InpaintDetail() << '\n'
           << "Inpaint V3.4 history: "
           << mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                  std::memory_order_relaxed)
           << " bytes; combined used="
           << (mfgunlock::cudatemporal::g_history_bytes.load(
                   std::memory_order_relaxed) +
               mfgunlock::cudatemporal::g_inpaint_history_bytes.load(
                   std::memory_order_relaxed))
           << "/" << mfgunlock::cudatemporal::kArenaLimit
           << "; resets="
           << mfgunlock::cudatemporal::g_history_resets.load(
                  std::memory_order_relaxed)
           << '\n'
#endif
           << "Directional scatter: "
           << (blackwell_active_result.adaptive_directional_scatter
                   ? "Provider-native signed coverage retained"
                   : "Baseline") << '\n'
           << "V3.2 Stability requested: "
           << (mfgunlock::cudatemporal::NormalizeMode(
                       g_adaptive_quality_v3_stability_mode.load(
                           std::memory_order_relaxed)) ==
                       mfgunlock::cudatemporal::StabilityMode::kTemporal
                   ? "Temporal Stable"
                   : "Local Stable") << '\n'
           << "V3.2 Stability effective: "
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
           << "History disabled; dedicated Local cubin requested (see installed variant above)" << '\n'
#if defined(MFGUNLOCK_RELEASE_1_4_5)
           << "Build: Release 1.4.5 Engineering; gameplay validation pending; restart required" << '\n'
#elif defined(MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
           << "Build: Experimental 1.4.4 Engineering; gameplay validation pending; restart required" << '\n'
#elif defined(MFGUNLOCK_RELEASE_1_4_4)
           << "Build: Release 1.4.4; DLSS-G 310.9.16 + Streamline 2.14.16 hotfix; Local Stable; V2 inpaint" << '\n'
#elif defined(MFGUNLOCK_RELEASE_1_4_3)
           << "Build: Release 1.4.3; continuous border axes; paired diagonal consensus; V2 inpaint" << '\n'
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
           << "Build: 1.4.2-base; continuous-axis border candidate; in-game validation pending" << '\n'
#elif defined(MFGUNLOCK_LOCAL_STABILITY)
           << "Build: Release 1.4.2; continuous border transition; paired diagonal consensus; V2 inpaint" << '\n'
#else
           << "Build: Local low-overhead; no V3 inpaint or confidence history" << '\n'
#endif
           << "Temporal backend: Disabled by build; no self-test, allocations or barriers" << '\n'
           << "CUDA hook fast path: Not installed; no CUDA/NVAPI launch interception" << '\n'
#else
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? mfgunlock::cudatemporal::Detail()
                   : "Off") << '\n'
           << "V3.2 CUDA probe: launches="
           << mfgunlock::cudatemporal::g_probe_launches.load(
                  std::memory_order_relaxed)
           << ", phase_mask=0x" << std::hex
           << mfgunlock::cudatemporal::g_probe_phase_mask.load(
                  std::memory_order_relaxed)
           << std::dec << ", history_bytes="
           << mfgunlock::cudatemporal::g_history_bytes.load(
                  std::memory_order_relaxed) << '\n'
           << "V3.2 CUDA hook fast path: "
           << (mfgunlock::cudatemporal::FastPathReady()
                   ? "active; one immutable dispatch-table atomic load for non-target kernels"
                   : "inactive/target association pending")
           << '\n'
#endif
           << "Quality refinement requested this session: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Legacy setting ignored; managed by Adaptive Quality"
                   : (g_quality_refinement.load(std::memory_order_relaxed)
                          ? "Yes (check patch details)"
                          : "No")) << '\n'
           << "Geometry confidence V2: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Integrated asymmetric successor managed by Adaptive Quality"
                   : (g_geometry_confidence_v2.load(std::memory_order_relaxed)
                   ? (thin_geometry_last.geometry_confidence_v2_applied
                          ? "Patch applied; execution unverified"
                          : "Requested; V1/fallback or no patch")
                   : "Off")) << '\n'
           << "Geometry patch detail: "
           << thin_geometry_last.silhouette_boundary_guard.detail << '\n'
           << "Border confidence trial: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Integrated and managed by Adaptive Quality"
                   : (g_border_confidence.load(std::memory_order_relaxed)
                   ? (!g_quality_refinement.load(std::memory_order_relaxed)
                          ? "Requires Quality Refinement"
                          : (thin_geometry_last.result.validated_warp_blend.applied
                                 ? "Patch applied; execution unverified"
                                 : "Requested; blend not applied"))
                   : "Off")) << '\n'
           << "Reflex marker health: "
           << MarkerHealthText(static_cast<mfgunlock::pacing::MarkerHealth>(
                  latency_view.marker_health))
           << '\n'
           << "Source timing validation: "
           << TimingValidationText(
                  latency_view.timing_issue_mask)
           << "; queue timing "
           << (latency_view.queue_timing_confident ? "verified" : "unverified")
           << '\n'
           << "Estimated rendered/source FPS: "
           << latency_view.estimated_source_fps
           << '\n'
           << "Estimated output FPS (not measured): "
           << latency_view.projected_output_fps
           << '\n'
           << "Simulation marker FPS: "
           << latency_view.simulation_fps
           << '\n'
           << "Median render-queue wait (us): "
           << latency_view.queue_wait_us
           << '\n'
           << "P95 render-queue wait (us): "
           << latency_view.queue_p95_us
           << '\n'
           << "Estimated marker-to-GPU pipeline median/p95 (us; not end-to-end): "
           << latency_view.pipeline_latency_us
           << '/'
           << latency_view.pipeline_p95_us
           << '\n'
           << "Median input-to-GPU latency (us): "
           << latency_view.input_to_gpu_end_us
           << '\n'
           << "Median GPU active work (us): "
           << latency_view.gpu_active_us
           << '\n'
           << "Dominant observed latency stage: "
           << LatencyBottleneckText(
                  static_cast<mfgunlock::pacing::LatencyBottleneck>(
                      latency_view.bottleneck))
           << '\n'
           << "Independent flip: "
           << (!latency_view.iflip_known
                   ? "Not reported"
                   : (latency_view.iflip_active
                          ? "Active" : "Inactive"))
           << '\n'
           << "Latency monitor sample cost (us): "
           << latency_view.sample_cost_us
           << '\n'
           << "Automatic multiplier trial: "
           << latency_view.multiplier_override
           << "x (0 = inactive; saved selection unchanged)\n"
           << "Responsive multiplier phase: "
           << MultiplierTrialPhaseText(
                  static_cast<mfgunlock::latency::MultiplierTrialPhase>(
                      latency_view.multiplier_trial_phase))
           << '\n'
           << "Responsive trial reason: "
           << ResponsiveTrialReasonText(
                  static_cast<mfgunlock::latency::ResponsiveTrialReason>(
                      latency_view.multiplier_trial_reason))
           << '\n'
           << "Responsive trial eligibility: "
           << ResponsiveTrialBlockerText(
                  static_cast<mfgunlock::latency::ResponsiveTrialBlocker>(
                      latency_view.trial_blocker))
           << '\n'
           << "Responsive multiplier saved/approved/candidate: "
           << mfgunlock::framecount::g_force_multiplier.load(
                  std::memory_order_relaxed)
           << 'x' << '/'
           << latency_view.multiplier_approved
           << 'x' << '/'
           << latency_view.multiplier_candidate
           << "x\n"
           << "Responsive baseline/trial samples: "
           << latency_view.multiplier_baseline_samples
           << "/8; "
           << latency_view.multiplier_trial_samples
           << "/8\n"
           << "Responsive baseline pipeline median/p95 (us): "
           << latency_view.multiplier_baseline_pipeline_us
           << '/'
           << latency_view.multiplier_baseline_p95_us
           << '\n'
           << "Responsive latest trial pipeline median/p95 (us): "
           << latency_view.multiplier_trial_pipeline_us
           << '/'
           << latency_view.multiplier_trial_p95_us
           << '\n'
           << "Automatic queue-trim cap: "
           << latency_view.active_source_cap_fps
           << '\n'
           << "DXGI frame-latency policy: "
           << (render_api == DetectedRenderApi::kVulkan
                   ? "unavailable on Vulkan"
                   : (g_latency_guard_dxgi_observed.load(std::memory_order_acquire)
                   ? (g_latency_guard_waitable_swapchain.load(
                              std::memory_order_relaxed)
                          ? "waitable"
                          : "No waitable object observed")
                   : "not observed"))
           << '\n'
           << "Process local VRAM usage/budget: "
           << (render_api == DetectedRenderApi::kVulkan
                   ? std::string("DXGI budget unavailable on Vulkan")
                   : std::to_string(
                         g_vram_local_usage.load(std::memory_order_relaxed)) +
                         "/" +
                         std::to_string(
                             g_vram_local_budget.load(
                                 std::memory_order_relaxed)) +
                         " bytes")
           << '\n'
           << "DLSS-G estimated VRAM: "
           << mfgunlock::framecount::g_vram_estimate_bytes.load(
                  std::memory_order_relaxed)
           << " bytes; status "
           << mfgunlock::framecount::g_vram_estimate_status.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Potentially volatile DLSS-G inputs: "
           << mfgunlock::framecount::g_vram_volatile_input_count.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Input quality mode: "
           << mfgunlock::framecount::g_hdr_compatibility_mode.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Quality issue mask: 0x" << std::hex
           << mfgunlock::framecount::g_quality_issue_mask.load(
                  std::memory_order_relaxed)
           << std::dec << '\n'
           << "Quality Guard viewports: "
           << mfgunlock::framecount::g_quality_viewport_count.load(
                  std::memory_order_relaxed)
           << '/' << mfgunlock::framecount::kMaxQualityViewports
           << (mfgunlock::framecount::g_quality_viewport_capacity_exhausted.load(
                       std::memory_order_acquire)
                   ? " (capacity exhausted)" : "")
           << '\n'
           << "Quality Guard tag-lock contention fallbacks: "
           << mfgunlock::framecount::g_quality_tag_lock_contentions.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Validation evidence: " << ValidationStageName(validation_stage)
           << " -- " << validation_text << '\n'
           << "Architecture gates: " << (gate_patched ? "Active" : "Pending")
           << " (providers " << gate_provider_count << ", sites "
           << gate_site_count << ")\n"
           << "Blackwell kernels: " << (blackwell_patched ? "Active" : "Inactive")
           << "\nBlackwell targeted roles: motion-vector "
           << (blackwell_active_result.motion_vector ? "yes" : "no")
           << ", inpaint "
           << (blackwell_active_result.inpaint ? "yes" : "no")
           << ", decision "
           << (blackwell_active_result.inpaint_decision ? "yes" : "no")
           << "; patched slots " << blackwell_active_result.kernels
           << "\nTemporal fallback: " << (midpoint_patched ? "Active" : "Inactive")
           << "\nThin-geometry patches: "
           << (thin_geometry_patched ? "Active" : "Inactive/Pending") << '\n'
           << "Intermediate retention: " << intermediate_report
           << '\n'
           << "Boundary mitigation: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "balanced (selected by Adaptive Quality Suite)"
                   : mfgunlock::blackwell::SilhouetteGuardName(
                         static_cast<mfgunlock::blackwell::SilhouetteGuardMode>(
                             g_silhouette_guard_mode.load(
                                 std::memory_order_relaxed))))
           << '\n'
           << "Validated warp blend: "
           << ((g_adaptive_quality.load(std::memory_order_relaxed) ||
                g_thin_geometry_validated_warp_blend.load(
                    std::memory_order_relaxed))
                   ? "Enabled"
                   : "Disabled")
           << " (" << thin_geometry_last.result.validated_warp_blend.detail
           << ")"
           << '\n'
           << "Previous scatter retention: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? "Disabled by Adaptive Quality Suite; saved preference preserved"
                   : (g_thin_geometry_previous_scatter.load(
                          std::memory_order_relaxed)
                          ? "Enabled"
                          : "Disabled"))
           << '\n';
    if (ImGui::Button("Copy diagnostics")) {
      ImGui::SetClipboardText(report.str().c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("No local paths or credentials are included.");


    if (blackwell_patched) {
      std::ostringstream framework_status;
      framework_status << "Blackwell framework kernels are active on "
                       << blackwell_provider_count << " provider(s).";
      ImGui::TextWrapped("%s", framework_status.str().c_str());
      std::ostringstream patched_roles;
      patched_roles << "Patched roles: motion-vector "
                    << (blackwell_active_result.motion_vector ? "yes" : "no")
                    << ", inpaint "
                    << (blackwell_active_result.inpaint ? "yes" : "no")
                    << ", decision "
                    << (blackwell_active_result.inpaint_decision ? "yes" : "no")
                    << " (" << blackwell_active_result.kernels << " slots).";
      TextDisabledWrapped(patched_roles.str().c_str());
      if (!blackwell_applied_detail.empty()) {
        const std::string applied =
            "Applied provider result: " + blackwell_applied_detail;
        TextDisabledWrapped(applied.c_str());
      }
      if (!blackwell_detail.empty() &&
          blackwell_detail != blackwell_applied_detail) {
        const std::string rejected =
            "Separate rejected candidate (does not replace the result above): " +
            blackwell_detail;
        TextDisabledWrapped(rejected.c_str());
      }
    } else if (!blackwell_detail.empty()) {
      ImGui::TextWrapped("Blackwell: %s", blackwell_detail.c_str());
    }
    if (!midpoint_detail.empty())
      ImGui::TextWrapped("Temporal: %s", midpoint_detail.c_str());
    ImGui::TextDisabled(
        "Providers: gates %zu, Blackwell %zu, temporal %zu, quality %zu.",
        gate_provider_count, blackwell_provider_count,
        midpoint_provider_count, thin_geometry_provider_count);
    ImGui::TextDisabled("Load trigger: %s; caught %u provider load(s).",
                        mfgunlock::loadhook::g_hooked.load(
                            std::memory_order_acquire)
                            ? "armed"
                            : "not installed",
                        mfgunlock::loadhook::g_catches.load(
                            std::memory_order_relaxed));

  }

  mfgunlock::ui::Footer("MFG Unlock | 1.7.1");
  ImGui::TextDisabled(
      "Changes marked for restart keep the current session unchanged.");
}
