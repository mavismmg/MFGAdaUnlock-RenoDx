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

// 1.8: a live table whose height never changes. Values refresh a few times per
// second on one line each; wrapped text changing every frame moved the controls
// above it and could toggle the window's scrollbar. Clipped text is in a tooltip.
struct StableStatusRows {
  struct Row { std::string label, value; ImVec4 color; };
  std::vector<Row> rows;
  ULONGLONG next{};
  bool Due() {
    const auto now = GetTickCount64();
    if (now < next) return false;
    next = now + 250;
    rows.clear();
    return true;
  }
  void Add(const char* label, const char* value, const ImVec4& color) {
    rows.push_back({label, value, color});
  }
  void Draw() const {
    for (const auto& row : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", row.label.c_str());
      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, row.color);
      ImGui::TextUnformatted(row.value.c_str());
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered() &&
          ImGui::CalcTextSize(row.value.c_str()).x >
              ImGui::GetColumnWidth())
        ImGui::SetTooltip("%s", row.value.c_str());
    }
  }
};

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
  return version + (actual == expected ? T(Str::kAppliedExecutionUnverified_5496)
                                       : T(Str::kFallbackExecutionUnverified_4acb));
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
         << (bytes >= (1ull << 30) ? T(Str::kGib_a9c3) : T(Str::kMib_ec5d));
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
    append_options(T(Str::kGame_2327), snapshot.game_options);
    append_options(T(Str::kForwarded_ed4b), snapshot.forwarded_options);
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

// 1.9.0: where each link of the frame generation chain came from.
std::string FrameGenChainGenerator(ChainHealth* health = nullptr) {
  ChainHealth ignored;
  ChainHealth& out = health ? *health : ignored;
  if (!mfgunlock::xmx::generator::Enabled()) {
    const int64_t created = mfgunlock::xmx::preada::g_create_result.load();
    if (mfgunlock::xmx::preada::Admitted() && created >= 0 && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(created))) {
      out = ChainHealth::kFail;
      return "NVIDIA DLSS-G could not be created on this GPU";
    }
    out = ChainHealth::kOk;
    return mfgunlock::xmx::preada::Admitted() ? T(Str::kNvidiaDlssGKernelsPrepared_9e45) : T(Str::kNvidiaDlssG_b2c0);
  }
  const auto report = mfgunlock::xmx::generator::Snapshot();
  if (!report.failure.empty() && !report.generating) {
    out = ChainHealth::kFail;
    return "XeMFG DLSS-G selected; not running: " + report.failure;
  }
  out = report.generating ? ChainHealth::kOk : ChainHealth::kWarn;
  return report.generating ? "XeMFG DLSS-G (Intel's model) generating" : report.hooked ? "XeMFG DLSS-G ready" : "XeMFG DLSS-G selected; not started";
}
// 1.10.0 (plan 1.4): the registry, read once per panel session.
mfgunlock::system::Hags SystemCheckHags() {
  static const mfgunlock::system::Hags hags = mfgunlock::system::ReadHags();
  return hags;
}
ImVec4 ChainColor(ChainHealth health) {
  switch (health) {
    case ChainHealth::kOk: return mfgunlock::ui::Accent;  // the panel's colour for a good value
    case ChainHealth::kWarn: return mfgunlock::ui::Warning;
    case ChainHealth::kFail: return mfgunlock::ui::Danger;
    default: return mfgunlock::ui::Muted;
  }
}
std::string FrameGenChainReport() {
  const auto chain = DescribeFrameGenChain();
  if (chain.streamline.empty()) return std::string();
  return "chain_streamline=" + chain.streamline + "\nchain_provider=" + chain.provider + "\nchain_gate=" + chain.gate +
         "\nchain_slinit=" + chain.slinit + "\nchain_dynamic=" + chain.dynamic +
         (chain.ngx_feature.empty() ? std::string() : "\nchain_ngx_feature=" + chain.ngx_feature) +
         [] {  // 1.10.0 (dashdogy A1): only when NGX's requirements query failed
           const std::string failure = mfgunlock::xmx::preada::RequirementsReport();
           return failure.empty() ? std::string() : "\nchain_ngx_requirements=" + failure;
         }() + "\nchain_vram=" + FrameGenerationVramReport() +
#if defined(MFGUNLOCK_NATIVE_XEMFG) && defined(MFGUNLOCK_XEMFG_VULKAN)
         [] {  // 1.10.0 (item 3): XMX on Vulkan, only when wanted
           const std::string text = mfgunlock::xemfg::native::vulkan::XmxProviderReport();
           return text.empty() ? std::string() : "\nchain_xmx_vulkan=" + text;
         }() +
         [] {  // 1.10.0 (plan XMX Vulkan pacing): the support report stays in English.
           namespace vk = mfgunlock::xemfg::native::vulkan;
           if (!vk::XmxProviderWanted()) return std::string();
           const uint32_t limit_us = mfgunlock::reflexpacing::g_xmx_vulkan_limit_us.load(std::memory_order_relaxed);
           std::string report = vk::XmxPacingEnabled() ? (limit_us ? "limit " + std::to_string(int(1000000.0 / limit_us + 0.5)) + " fps" : std::string("no limit")) : std::string("off");
           return "\nchain_xmx_vulkan_pacing=" + report + ", usual real-frame interval " + std::to_string(vk::XmxPacingCostUs() / 1000) + " ms";
         }() +
#endif
         [] {  // 1.10.0 (force_fg.hpp): only when [DLSSG] ForceFrameGeneration=1
           if (!mfgunlock::framecount::g_force_fg_enabled.load(std::memory_order_relaxed)) return std::string();
           const auto decision = static_cast<mfgunlock::forcefg::Decision>(
               mfgunlock::framecount::g_force_fg_decision.load(std::memory_order_acquire));
           namespace fc = mfgunlock::framecount::internal;
           std::string text = fc::g_init_seen.load(std::memory_order_acquire)
                                  ? mfgunlock::forcefg::DecisionText(decision)
                              : fc::g_host_init_seen.load(std::memory_order_acquire)
                                  ? "not used: MFG Unlock's own Streamline runs frame generation (Add frame generation)"
                                  : "slInit not seen by the addon (it needs to load early)";
           if (mfgunlock::forcefg::Added(decision))
             text += std::string("; ") + mfgunlock::forcefg::ApplyText(static_cast<mfgunlock::forcefg::Apply>(
                         mfgunlock::framecount::g_force_fg_apply.load(std::memory_order_acquire)));
           return "\nchain_force_fg=" + text;
         }() + "\nchain_generator=" +
         FrameGenChainGenerator() + "\n";
}
// 1.10.0 (plan 1.4): what a support report needs to say about this PC and
// this install, one verdict per line. Closed by default, like the chain card.
void DrawSystemCheckCard() {
  if (!ImGui::CollapsingHeader(T(Str::kSystemCheck_6bbc))) return;
  mfgunlock::ui::Description(T(Str::kWhatFrameGenerationNeedsFrom_b2d8));
  mfgunlock::ui::Card card("##system_check_card");
  using mfgunlock::ui::Readout;
  const auto& machine = mfgunlock::system::ReadMachine();
  Readout("##sys_gpu", "GPU", machine.gpu.c_str(), nullptr, mfgunlock::ui::Muted);
  Readout("##sys_driver", T(Str::kDriver_52ff), machine.driver.c_str(), nullptr, mfgunlock::ui::Muted);
  Readout("##sys_windows", T(Str::kWindows_028a), machine.windows.c_str(), nullptr, mfgunlock::ui::Muted);
  const auto hags = SystemCheckHags();
  Readout("##sys_hags", "HAGS", mfgunlock::system::HagsName(hags),
          T(Str::kHardwareAcceleratedGpuSchedulingDlss_d65d),
          ChainColor(hags == mfgunlock::system::Hags::kOn ? ChainHealth::kOk : hags == mfgunlock::system::Hags::kOff ? ChainHealth::kFail : ChainHealth::kUnknown));
  static std::string ngx;
  if (ngx.empty()) ngx = mfgunlock::system::NgxCoreVersion();
  Readout("##sys_ngx", T(Str::kNgxCore_a12d), ngx.empty() ? T(Str::kNotLoadedYet_873d) : ngx.c_str(),
          T(Str::kTheDriverSNvngxDll_5c05),
          ChainColor(ngx.empty() ? ChainHealth::kUnknown : ChainHealth::kOk));
  if (mfgunlock::xmx::preada::Admitted()) {
    const bool early = HasEarlyLoadEntry();
    Readout("##sys_early", T(Str::kEarlyLoading_4fb0), early ? T(Str::kInReshadeIniLoadfromdllmain_a484) : T(Str::kNotInReshadeIni_9b07),
            T(Str::kRtx2030NeedThe_402a),
            ChainColor(early ? ChainHealth::kOk : ChainHealth::kWarn));
  }
  const auto copies = CheckAddonCopies();
  const std::string copies_text = copies.others.empty() && copies.stale_early.empty()
      ? T(Str::kOneAddonFile_7d11)
      : std::to_string(copies.others.size()) + " other file(s), " + std::to_string(copies.stale_early.size()) + " stale early-load entr(y/ies)";
  Readout("##sys_copies", T(Str::kAddonFiles_4abe), copies_text.c_str(), T(Str::kOnlyOneMfgUnlockAddon_2b2b),
          ChainColor(copies.others.empty() && copies.stale_early.empty() ? ChainHealth::kOk : ChainHealth::kWarn));
}
void DrawFrameGenChainCard() {
  static FrameGenChain shown;
  if (const auto chain = DescribeFrameGenChain(); !chain.streamline.empty()) shown = chain;
  if (shown.streamline.empty()) return;
  // 1.9.0: closed by default, like "Advanced pacing (testing)".
  if (!ImGui::CollapsingHeader(T(Str::kFrameGenerationChain_660d))) return;
  mfgunlock::ui::Description(T(Str::kWhereEachPartOfFrame_3c7b));
  mfgunlock::ui::Card card("##fg_chain_card");
  using mfgunlock::ui::Readout;
  Readout("##chain_streamline", T(Str::kStreamlineDlssG_ed8f), shown.streamline.c_str(),
          T(Str::kSlDlssGDllThe_8be5),
          ChainColor(shown.streamline_health));
  Readout("##chain_provider", T(Str::kFrameGenerationFile_c732), shown.provider.c_str(),
          T(Str::kNvngxDlssgTheGameS_9e2e),
          ChainColor(shown.provider_health));
  Readout("##chain_gate", T(Str::kMultiFrameGate_68c5), shown.gate.c_str(),
          T(Str::kWhetherTheAddonOpenedNvidia_d63f),
          ChainColor(shown.gate_health));
  Readout("##chain_slinit", T(Str::kStreamlineStart_335f), shown.slinit.c_str(),
          T(Str::kWhetherTheAddonSawThe_2aa4),
          ChainColor(shown.slinit_health));
  Readout("##chain_dynamic", T(Str::kDynamicMfg_4fba), shown.dynamic.c_str(),
          T(Str::kWhetherNvidiaSRuntimeReports_1de9),
          ChainColor(shown.dynamic_health));
  if (!shown.ngx_feature.empty())
    Readout("##chain_ngx_feature", T(Str::kNgxFeature_a82c), shown.ngx_feature.c_str(),
            T(Str::kWhetherNvidiaSRuntimeCreated_edc5),
            ChainColor(shown.ngx_feature_health));
  ChainHealth generator_health = ChainHealth::kUnknown;
  const std::string generator = FrameGenChainGenerator(&generator_health);
  Readout("##chain_generator", T(Str::kGenerator_8b62), generator.c_str(), nullptr, ChainColor(generator_health));
}

void DrawInputDiagnosticsPanel() {
  if (!ImGui::TreeNode(T(Str::kStreamlineInputResourcesReadOnly_d856))) return;
  bool enabled = mfgunlock::inputdiag::g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kCaptureResourceMetadataThisSession_9850), &enabled)) {
    if (enabled) mfgunlock::inputdiag::Clear();
    mfgunlock::inputdiag::g_enabled.store(enabled, std::memory_order_release);
  }
  ImGui::SameLine();
  if (ImGui::Button(T(Str::kClearInputSamples_c55d))) mfgunlock::inputdiag::Clear();
  ImGui::TextDisabled(
      T(Str::kGameSubmissionsBeforeQualityGuard_5404));
  ImGui::TextDisabled(
      T(Str::kRelevantTagTypesMayAlso_ff71));
  ImGui::TextDisabled(
      T(Str::kNativeFormatIsApiSpecific_700f));
  ImGui::TextDisabled(T(Str::kSkippedLockTruncatedBatchesLlu_bc8a),
      mfgunlock::inputdiag::g_dropped_contention.load(std::memory_order_relaxed),
      mfgunlock::inputdiag::g_truncated_batches.load(std::memory_order_relaxed));
  mfgunlock::inputdiag::OutputSnapshot output{};
  if (mfgunlock::inputdiag::SnapshotOutput(output)) {
    ImGui::TextDisabled(
        T(Str::kPrimarySwapchainUxUFormat_2945),
        output.width, output.height, output.format,
        InputColorSpaceName(output.color_space), output.samples,
        output.changes);
  } else {
    ImGui::TextDisabled(T(Str::kPrimarySwapchainWaitingForA_14e1));
  }
  if (mfgunlock::inputdiag::g_viewport_overflow.load(std::memory_order_relaxed))
    ImGui::TextColored(kUiWarning, T(Str::kMoreThanEightViewportsCapture_e7d4));
  std::array<mfgunlock::inputdiag::ViewportSnapshot,
             mfgunlock::inputdiag::kMaxViewports> snapshots{};
  const size_t count = mfgunlock::inputdiag::SnapshotAll(snapshots);
  for (size_t viewport = 0; viewport < count; ++viewport) {
    const auto& snapshot = snapshots[viewport];
    if (snapshot.tag_batches == 0 && snapshot.constants.calls == 0) continue;
    ImGui::PushID(static_cast<int>(snapshot.viewport));
    if (ImGui::TreeNode("viewport", T(Str::kViewportUSSFormats_0e2f),
                        snapshot.viewport,
                        InputNativeApiName(snapshot.native_api),
                        InputFormatApiName(snapshot.format_api),
                        snapshot.tag_batches)) {
      ImGui::TextDisabled(T(Str::kFrameAwareTagBatchesLlu_c089), snapshot.frame_aware_batches);
      const auto draw_options = [](
          const char* label,
          const mfgunlock::inputdiag::OptionsSummary& options) {
        if (options.calls == 0) return;
        if (!options.valid) {
          ImGui::TextDisabled(T(Str::kSOptionsLluCallsNo_f060),
                              label, options.calls,
                              options.invalid_base_metadata);
          return;
        }
        ImGui::Text(
            T(Str::kSOptionsVUS_9fc3),
            label, options.struct_version, InputDlssgModeName(options.mode),
            options.generated_frames, options.generated_frames + 1,
            options.flags);
        ImGui::TextDisabled(
            T(Str::kColorUxUFmtU_3315),
            options.color_width, options.color_height, options.color_format,
            options.mvec_depth_width, options.mvec_depth_height,
            options.mvec_format, options.depth_format,
            options.hudless_format, options.ui_format);
        ImGui::TextDisabled(
            T(Str::kUiRecompositionSCallsLlu_5327),
            InputBooleanName(options.ui_recomposition), options.calls,
            options.changes, options.invalid_base_metadata);
        if (KnownInputFloat(options.dynamic_target_fps))
          ImGui::TextDisabled(T(Str::kDynamicTarget4gFps_74a4),
                              options.dynamic_target_fps);
      };
      draw_options(T(Str::kGame_2327), snapshot.game_options);
      draw_options(T(Str::kForwarded_ed4b), snapshot.forwarded_options);
      for (size_t index = 0; index < mfgunlock::inputdiag::kKindCount; ++index) {
        const auto& tag = snapshot.tags[index];
        if (tag.sets == 0 && tag.clears == 0) continue;
        ImGui::Separator();
        ImGui::Text(T(Str::kSTagUxUStreamline_2afb),
                    mfgunlock::inputdiag::KindName(
                        static_cast<mfgunlock::inputdiag::Kind>(index)),
                    tag.width, tag.height, tag.resource_width,
                    tag.resource_height, tag.native_format);
        if (tag.native_desc_seen) {
          ImGui::TextDisabled(T(Str::kNativeDescUxUFormat_9a92),
                              tag.native_desc_width, tag.native_desc_height,
                              tag.native_desc_format);
        } else {
          ImGui::TextDisabled(T(Str::kNativeDescUnavailableForThis_d0b7));
        }
        ImGui::TextDisabled(
            T(Str::kSetsLluClearsLluLast_12bd),
            tag.sets, tag.clears, tag.has_resource ? "set" : "cleared",
            InputLifecycleName(tag.lifecycle), tag.invalid_base_metadata);
        ImGui::TextDisabled(
            T(Str::kMetadataChangesLluNativeDesc_5c29),
            tag.metadata_changes, tag.native_desc_changes,
            tag.native_identity_changes);
        if (tag.frame_known)
          ImGui::TextDisabled(T(Str::kLastFrameTokenU_1609), tag.last_frame);
      }
      const auto& constants = snapshot.constants;
      if (constants.calls != 0) {
        ImGui::Separator();
        ImGui::Text(T(Str::kConstantsVULluCalls_1cb1),
                    constants.struct_version, constants.calls,
                    constants.game_reset_true);
        ImGui::TextDisabled(
            T(Str::kFrameUDepthInvertedS_97ff),
            constants.last_frame, InputBooleanName(constants.depth_inverted),
            InputBooleanName(constants.camera_motion_included),
            InputBooleanName(constants.motion_vectors_3d),
            InputBooleanName(constants.motion_vectors_dilated),
            InputBooleanName(constants.motion_vectors_jittered));
        if (KnownInputFloat(constants.mvec_scale_x) &&
            KnownInputFloat(constants.mvec_scale_y))
          ImGui::TextDisabled(T(Str::kMvScale4g4g_cad6), constants.mvec_scale_x,
                              constants.mvec_scale_y);
        if (KnownInputFloat(constants.jitter_x) &&
            KnownInputFloat(constants.jitter_y))
          ImGui::TextDisabled(T(Str::kJitter4g4g_a009), constants.jitter_x,
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
    result = T(Str::kPreset_d23a);
    result.push_back(static_cast<char>('A' + preset - 1));
  } else if (preset == 0x00fffffeu) {
    result = T(Str::kDefaultPreset_3bb1);
  } else if (preset == 0x00ffffffu) {
    result = T(Str::kLatestPreset_ea29);
  } else if (preset == 0) {
    result = T(Str::kGameProviderControlled_e835);
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
               ? T(Str::kConfiguredWaitingForFrameGeneration_fbf7)
               : T(Str::kNotDetected_cc90);
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
                             : T(Str::kAuto_efb6);
  if (maximum_generated_frames == 0) return live;
  return live + " / " + std::to_string(maximum_generated_frames + 1) + "x";
}

void DrawAdvancedQualityControls(bool adaptive_quality_managed) {
  const char* kDepthEdgeModes[] = {
      T(Str::kGameDefault_70be), T(Str::kMild_a97b), T(Str::kBalanced_603d), T(Str::kStrong_409a), T(Str::kAggressive_2569)};
  int depth_edge_level = static_cast<int>(
      mfgunlock::framecount::g_depth_edge_guard_level.load(
          std::memory_order_relaxed));
  SetNextItemWidthWithHelp(T(Str::kDepthEdgeCompatibility_94d4));
  if (ImGui::Combo(T(Str::kDepthEdgeCompatibility_94d4), &depth_edge_level,
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
      T(Str::kOptionalCompatibilityOverrideForThe_b5c5));

  const auto configured_profile =
      mfgunlock::adaptivequality::NormalizeProfile(
          g_configured_adaptive_quality_profile.load(
              std::memory_order_relaxed));
  if (adaptive_quality_managed &&
      configured_profile ==
          mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3 &&
      ImGui::TreeNode(T(Str::kAdaptiveQualityV3AB_5703))) {
    ImGui::TextDisabled(
        T(Str::kDeveloperComparisonsInsideTheSingle_b811));
    bool photometric =
        g_configured_adaptive_quality_v3_photometric.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kRelativeLumaChromaConfidence_dd8f), &photometric)) {
      g_configured_adaptive_quality_v3_photometric.store(
          photometric, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3Photometric",
                                photometric ? 1 : 0);
    }
    HelpMarker(
        T(Str::kOffRetainsV2AbsoluteRgb_be4d));

    bool directional_border =
        g_configured_adaptive_quality_v3_directional_border.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kMotionDirectionalBorderTaper_cf26),
                        &directional_border)) {
      g_configured_adaptive_quality_v3_directional_border.store(
          directional_border, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3DirectionalBorder",
                                directional_border ? 1 : 0);
    }
    HelpMarker(
        T(Str::kOffReturnsTheWarpPath_eb9c));

    bool oriented_geometry =
        g_configured_adaptive_quality_v3_oriented_geometry.load(
            std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kMotionOrientedDiagonalGeometry_920b),
                        &oriented_geometry)) {
      g_configured_adaptive_quality_v3_oriented_geometry.store(
          oriented_geometry, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "AdaptiveQualityV3OrientedGeometry",
                                oriented_geometry ? 1 : 0);
    }
    HelpMarker(
        T(Str::kOffDeliberatelyRequestsTheValidated_7c84));

#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
    ImGui::TextWrapped(
        T(Str::kLocalLowOverheadBuildLocal_409f));
#else
    const char* kStabilityModes[] = {
        T(Str::kLocalStable3x3Tile_51e2), T(Str::kTemporalStableConfidenceHistory_a975)};
    int stability_mode = static_cast<int>(
        mfgunlock::cudatemporal::NormalizeMode(
            g_configured_adaptive_quality_v3_stability_mode.load(
                std::memory_order_relaxed))) - 1;
    SetNextItemWidthWithHelp(T(Str::kV32SilhouetteStability_089f));
    if (ImGui::Combo(T(Str::kV32SilhouetteStability_089f), &stability_mode,
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
        T(Str::kTemporalStableStoresOnlyOne_ea57));
    if (g_configured_adaptive_quality_v3_stability_mode.load(
            std::memory_order_relaxed) !=
        g_adaptive_quality_v3_stability_mode.load(
            std::memory_order_relaxed)) {
      ImGui::TextDisabled(T(Str::kSavedForNextLaunchRestart_21d0));
    }

    const char* kInpaintModes[] = {
        T(Str::kV2Compatibility_5ad9), T(Str::kLocalV3_9cfb), T(Str::kTemporalV3_ae9a)};
    int inpaint_mode = static_cast<int>(
        mfgunlock::cudatemporal::NormalizeInpaintMode(
            g_configured_adaptive_quality_v3_inpaint_mode.load(
                std::memory_order_relaxed)));
    SetNextItemWidthWithHelp(T(Str::kV34InpaintStability_541a));
    if (ImGui::Combo(T(Str::kV34InpaintStability_541a), &inpaint_mode,
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
        T(Str::kTemporalV3IsTheDefault_faac));
    if (g_configured_adaptive_quality_v3_inpaint_mode.load(
            std::memory_order_relaxed) !=
        g_adaptive_quality_v3_inpaint_mode.load(
            std::memory_order_relaxed)) {
      ImGui::TextDisabled(T(Str::kInpaintModeSavedForNext_084a));
    }
#endif
    ImGui::TreePop();
  }

  if (!ImGui::TreeNode(T(Str::kLegacyQualityABControls_e4e5))) return;
  ImGui::TextDisabled(
      T(Str::kForRegressionTestingOnlyAdaptive_8b90));
  if (adaptive_quality_managed) {
    ImGui::TextColored(kUiPositive,
                       T(Str::kManagedByAdaptiveQualitySuite_31fe));
    ImGui::BeginDisabled();
  }

  bool refined =
      g_configured_quality_refinement.load(std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kLegacySmoothConfidenceRefinement_f394), &refined)) {
    g_configured_quality_refinement.store(refined,
                                          std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalQualityRefinement",
                              refined ? 1 : 0);
  }
  HelpMarker(
      T(Str::kEarlierSmoothConfidenceExperimentRetained_cb41));

  bool intermediate =
      g_configured_thin_geometry_intermediate_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kIntermediateScatterRetention_6854), &intermediate)) {
    g_configured_thin_geometry_intermediate_scatter.store(
        intermediate, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryIntermediateScatter",
                              intermediate ? 1 : 0);
  }
  HelpMarker(
      T(Str::kLegacyUnconditionalRetentionForFences_5054));

  int boundary_mode = static_cast<int>(
      g_configured_silhouette_guard_mode.load(std::memory_order_relaxed));
  const char* kBoundaryModes[] = {T(Str::kOff_b4aa), T(Str::kBalanced_603d), T(Str::kAggressive_2569)};
  SetNextItemWidthWithHelp(T(Str::kBoundaryMitigationAB_dd24));
  if (ImGui::Combo(T(Str::kBoundaryMitigationAB_dd24), &boundary_mode,
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
      T(Str::kStandaloneLegacyBoundaryExperimentAdaptive_345d));

  bool validated =
      g_configured_thin_geometry_validated_warp_blend.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kValidatedWarpBlendAB_2a4c), &validated)) {
    g_configured_thin_geometry_validated_warp_blend.store(
        validated, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryValidatedWarpBlend",
                              validated ? 1 : 0);
  }
  HelpMarker(
      T(Str::kStandaloneLegacyWarpValidationAdaptive_c325));

  bool geometry_v2 =
      g_configured_geometry_confidence_v2.load(std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kGeometryConfidenceV2AB_a3eb), &geometry_v2)) {
    g_configured_geometry_confidence_v2.store(geometry_v2,
                                              std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalGeometryConfidenceV2",
                              geometry_v2 ? 1 : 0);
  }
  HelpMarker(
      T(Str::kDeveloperComparisonForThePrior_a215));

  bool border =
      g_configured_border_confidence.load(std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kBorderConfidenceAB_debb), &border)) {
    g_configured_border_confidence.store(border, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ExperimentalBorderConfidence",
                              border ? 1 : 0);
  }
  HelpMarker(
      T(Str::kDeveloperComparisonForThePrevious_8223));

  bool previous =
      g_configured_thin_geometry_previous_scatter.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kPreviousToCurrentScatterUnstable_bd7b), &previous)) {
    g_configured_thin_geometry_previous_scatter.store(
        previous, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection,
                              "ThinGeometryPreviousScatter",
                              previous ? 1 : 0);
  }
  HelpMarker(
      T(Str::kUnstableResearchPathRetainedOnly_8fa4));

  if (adaptive_quality_managed) ImGui::EndDisabled();
  ImGui::TreePop();
}

void DrawLatencyGuardControl() {
  int mode = static_cast<int>(
      mfgunlock::framecount::g_latency_guard_mode.load(
          std::memory_order_relaxed));
  const char* kModes[] = {
      T(Str::kOff_b4aa), T(Str::kMonitorOnlyRecommended_f6ff),
      T(Str::kAutomaticLatencyGuardAdvanced_a614)};

  if (ImGui::BeginTable("##latency_guard_control", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T(Str::kSetting_754b), ImGuiTableColumnFlags_WidthStretch,
                            0.5f);
    ImGui::TableSetupColumn(T(Str::kControl_bf3e), ImGuiTableColumnFlags_WidthStretch,
                            0.5f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        T(Str::kLatencyGuard_9c58),
        mode == static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)
            ? T(Str::kTests6x5x4x3x_9745)
            : (mode == static_cast<int>(
                          mfgunlock::pacing::LatencyGuardMode::kMonitor)
                   ? T(Str::kReadOnlyReflexAndRender_cae5)
                   : T(Str::kNoLatencySamplingOrAutomatic_7f0d)),
        T(Str::kMonitorOnlyIsReadOnly_92bf),
        mode == static_cast<int>(
                    mfgunlock::pacing::LatencyGuardMode::kAutomatic)
            ? T(Str::kAdvanced_de7b)
            : nullptr,
        kUiWarning);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    const char* kGuardLabels[] = {T(Str::kOff_b4aa), T(Str::kMonitor_8ab9), T(Str::kAutomatic_6784)};
    if (mfgunlock::ui::Segmented("##latency_guard", mode, kGuardLabels, 3)) {
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
  const char* kModeOverrides[] = {
      T(Str::kGameControlled_1cb7), T(Str::kOffSkipReflexSleepFg_bc7c), T(Str::kForceOn_5275),
      T(Str::kForceOnBoost_8bbf)};
  int mode_override = static_cast<int>(pacing::NormalizeReflexModeOverride(
      reflexpacing::g_mode_override.load(std::memory_order_relaxed)));
  SetNextItemWidthWithHelp(T(Str::kReflexModeOverride_52ae));
  if (ImGui::Combo(T(Str::kReflexModeOverride_52ae), &mode_override, kModeOverrides,
                   static_cast<int>(std::size(kModeOverrides)))) {
    reflexpacing::g_mode_override.store(
        static_cast<uint32_t>(mode_override), std::memory_order_relaxed);
    reflexpacing::g_mode_rejected.store(false, std::memory_order_relaxed);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection, "ReflexModeOverride",
                              mode_override);
  }
  HelpMarker(
      T(Str::kOffDoesNotTurnReflex_87c8));
  if (mode_override == static_cast<int>(pacing::ReflexModeOverride::kOff) ||
      mode_override == static_cast<int>(pacing::ReflexModeOverride::kOnBoost)) {
    ImGui::TextDisabled(
        T(Str::kWithFrameGenerationOnExpect_c23e));
  }

  const char* kPacingMethods[] = {
      T(Str::kNativeReflexSleep_326f), T(Str::kDxgiWaitableExperimental_4aff)};
  int pacing_method = static_cast<int>(pacing::NormalizeReflexPacingMethod(
      reflexpacing::g_pacing_method.load(std::memory_order_relaxed)));
  SetNextItemWidthWithHelp(T(Str::kPacingMethod_643e));
  if (ImGui::Combo(T(Str::kPacingMethod_643e), &pacing_method, kPacingMethods,
                   static_cast<int>(std::size(kPacingMethods)))) {
    reflexpacing::g_pacing_method.store(
        static_cast<uint32_t>(pacing_method), std::memory_order_release);
    NotifyReflexPacingSettingChanged();
    reshade::set_config_value(nullptr, kConfigSection, "ReflexPacingMethod",
                              pacing_method);
  }
  HelpMarker(
      T(Str::kWaitableModeActivatesOnlyAfter_a9e2));

  bool headroom =
      reflexpacing::g_headroom_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox(T(Str::kVrrHeadroomCap_4faa), &headroom)) {
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
  SetNextItemWidthWithHelp(T(Str::kVrrHeadroom_c5ae));
  if (ImGui::SliderFloat(T(Str::kVrrHeadroom_c5ae), &headroom_percent, 0.5f, 3.0f,
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
      T(Str::kUsesReflexSFinalOutput_2b42));

  static StableStatusRows rows;
  if (rows.Due()) {
    const auto state = reflexpacing::g_waitable_state.load(
        std::memory_order_acquire);
    const auto reason = reflexpacing::g_fallback_reason.load(
        std::memory_order_relaxed);
    const auto sleep_intervals = reflexpacing::g_sleep_intervals.Stats();
    const auto sleep_durations = reflexpacing::g_sleep_durations.Stats();
    const auto present_intervals = reflexpacing::g_present_intervals.Stats();
    const bool fg_safe_off =
        pacing::NormalizeReflexModeOverride(
            reflexpacing::g_mode_override.load(std::memory_order_relaxed)) ==
        pacing::ReflexModeOverride::kOff;
    const auto sleep_dispatch = reflexpacing::g_sleep_dispatch.load(
        std::memory_order_relaxed);
    std::string pacing_status = ReflexWaitableStateText(state);
    if (sleep_dispatch == reflexpacing::SleepDispatch::kBypass) {
      pacing_status = T(Str::kFgSafeReflexSleepBypass_b943);
    } else if (sleep_dispatch == reflexpacing::SleepDispatch::kWaitable &&
               fg_safe_off &&
               (state == reflexpacing::WaitableState::kNative ||
                state == reflexpacing::WaitableState::kWaitingForSwapchain ||
                state == reflexpacing::WaitableState::kFallback)) {
      pacing_status += "; FG-safe bypass while waitable unavailable";
    }
    rows.Add(T(Str::kPacing_de2d), pacing_status.c_str(),
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
                   ? T(Str::kOverrideRejected_86b4)
                   : "");
    rows.Add(T(Str::kReflexGameEffective_f587), reflex_mode.c_str(),
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
                          ? T(Str::kLowLatencyOn_98fc)
                          : T(Str::kLowLatencyOff_3bf6)) +
          (nvapi_status.sleep.game_sleep != 0 ? T(Str::kGameSleeps_e2d4)
                                              : T(Str::kNoGameSleep_889a));
      rows.Add(T(Str::kDriverReflex_4c87), driver_reflex.c_str(), kUiMuted);
    } else {
      rows.Add(T(Str::kDriverReflex_4c87), T(Str::kNotReported_055d), kUiMuted);
    }
    rows.Add(T(Str::kFallbackReason_d94d),
             state == reflexpacing::WaitableState::kFallback
                 ? ReflexWaitableFallbackText(reason)
                 : T(Str::kNone_f7fb),
             state == reflexpacing::WaitableState::kFallback ? kUiWarning
                                                             : kUiMuted);
    const std::string probe = std::to_string(
        reflexpacing::g_probe_samples.load(std::memory_order_relaxed)) +
        "/120";
    rows.Add(T(Str::kWaitableProbe_69c2), probe.c_str(), kUiMuted);
    const std::string dxgi_latency = std::to_string(
        reflexpacing::g_native_maximum_latency.load(
            std::memory_order_relaxed)) +
        " -> " + std::to_string(
            reflexpacing::g_effective_maximum_latency.load(
                std::memory_order_relaxed));
    rows.Add(T(Str::kDxgiMaximumframelatency_5ffa), dxgi_latency.c_str(), kUiMuted);
    const std::string wait_health =
        std::string(reflexpacing::g_waitable_handle.load(
                        std::memory_order_acquire) != nullptr
                        ? T(Str::kHandleReady_1bf0)
                        : T(Str::kNoActiveHandle_f7ee)) +
        "; timeouts=" +
        std::to_string(reflexpacing::g_timeout_count.load(
            std::memory_order_relaxed)) +
        "; failures=" +
        std::to_string(reflexpacing::g_wait_failure_count.load(
            std::memory_order_relaxed));
    rows.Add(T(Str::kWaitableHealth_4b2e), wait_health.c_str(), kUiMuted);
    const uint32_t refresh = reflexpacing::g_refresh_millihz.load(
        std::memory_order_relaxed);
    std::ostringstream display;
    display << std::fixed << std::setprecision(2)
            << static_cast<double>(refresh) / 1000.0 << " Hz; VRR "
            << (reflexpacing::g_vrr_active.load(std::memory_order_relaxed)
                    ? "active"
                    : "inactive/unavailable");
    rows.Add(T(Str::kDisplay_d9b5), display.str().c_str(), kUiMuted);
    const uint32_t headroom_limit = reflexpacing::g_headroom_limit_us.load(
        std::memory_order_relaxed);
    const std::string cap = headroom_limit == 0
        ? T(Str::kInactive_ca78)
        : std::to_string(headroom_limit) + " us (" +
              std::to_string(pacing::FrameLimitUsToFps(headroom_limit)) +
              " FPS approx.)";
    rows.Add(T(Str::kVrrHeadroom_c5ae), cap.c_str(),
              headroom_limit != 0 ? kUiPositive : kUiMuted);
    auto cadence_text = [](const reflexpacing::SampleStats& stats) {
      if (stats.count == 0) return std::string(T(Str::kNoSamples_ccbd));
      std::ostringstream text;
      text << stats.median_us / 1000.0 << "/" << stats.p95_us / 1000.0
           << "/" << stats.p99_us / 1000.0 << " ms; delta "
           << stats.mean_delta_us / 1000.0 << " ms; n=" << stats.count;
      return text.str();
    };
    const auto sleep_interval_text = cadence_text(sleep_intervals);
    const auto sleep_duration_text = cadence_text(sleep_durations);
    const auto present_interval_text = cadence_text(present_intervals);
    rows.Add(T(Str::kSleepIntervalMedP95P99_8fe3), sleep_interval_text.c_str(),
              kUiMuted);
    rows.Add(T(Str::kSleepDurationMedP95P99_db56), sleep_duration_text.c_str(),
              kUiMuted);
    rows.Add(T(Str::kAppPresentMedP95P99_7051), present_interval_text.c_str(),
              kUiMuted);
  }
  ImGui::Spacing();
  if (ImGui::BeginTable("##reflex_pacing_lab_status", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn(T(Str::kItem_8006), ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn(T(Str::kEffective_2292), ImGuiTableColumnFlags_WidthStretch,
                            0.55f);
    rows.Draw();
    ImGui::EndTable();
  }
  ImGui::TextDisabled(
      T(Str::kTheseAreApplicationCadenceProxies_82e7));
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

// 1.8.1: the panel's System card. Once a second is plenty for a readout.
void ObserveDisplay(reshade::api::effect_runtime* runtime) {
  static ULONGLONG next_sample = 0;
  const ULONGLONG now = GetTickCount64();
  if (runtime == nullptr || now < next_sample) return;
  next_sample = now + 1000;
  auto& display = mfgunlock::system::g_display;
  const auto desc = runtime->get_device()->get_resource_desc(runtime->get_current_back_buffer());
  display.format = static_cast<int>(desc.texture.format);
  display.windowsHdr = mfgunlock::system::MonitorInHdr(static_cast<HWND>(runtime->get_hwnd()));
  display.width = desc.texture.width;
  display.height = desc.texture.height;
  display.refreshHz = mfgunlock::system::RefreshHz(static_cast<HWND>(runtime->get_hwnd()));
  display.known = true;
}

void OnRegisterOverlay(reshade::api::effect_runtime* runtime) {
  mfgunlock::ui::Theme panel_theme;
  mfgunlock::ui::BeginLanguageFrame();
  ObserveDisplay(runtime);
  if (mfgunlock::backend::active != mfgunlock::backend::Kind::DlssG) {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::xemfg::native::Draw();
    return;
#else
    ImGui::TextUnformatted(T(Str::kXemfgUsesOptiscalerSFrame_7be7));
    ImGui::TextUnformatted(T(Str::kChangeBackendWithSwitchMfgbackend_7af4));
    ImGui::TextUnformatted(T(Str::kDlssGPatchesAndReflex_dd13));
    if (auto module = GetModuleHandleW(L"MFGAdaUnlock.XeMFG.asi")) {
      auto get_status = reinterpret_cast<mfgunlock::xemfg::GetStatus>(
          GetProcAddress(module, "MFGAXeGetStatus"));
      mfgunlock::xemfg::Status status;
      if (get_status && get_status(&status, sizeof(status))) {
        ImGui::TextWrapped("%s", status.detail);
        ImGui::Text(T(Str::kModelSReadySEnabled_9376),
                    status.model ? T(Str::kIntelMfgPort_b136) : T(Str::kNativeXefg_8bea),
                    status.initialized ? "yes" : "no", status.enabled ? "yes" : "no");
        ImGui::Text(T(Str::kRequestedOutputUxLastPresented_cb20),
                    status.generated + 1, status.presented, status.result);
        ImGui::Text(T(Str::kTensorCoreLaunchesLlu_2b9e), status.tensor_launches);
      }
    } else {
      ImGui::TextUnformatted(T(Str::kXemfgCompanionIsNotLoaded_50bb));
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
    // 1.10.0: the title is translated; "###" keeps the popup's ID the same
    // in every language.
    const std::string popup_title =
        std::string(T(Str::kMonsterHunterWildsFirstLaunch_0f42)) + "###mhw_first_launch";
    const char* kPopup = popup_title.c_str();
    if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(kUiWarning, T(Str::kRestartOnceAfterShaderCompilation_3f23));
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
      ImGui::TextWrapped(
          T(Str::kMonsterHunterWildsMayBuild_2fd6));
      ImGui::Spacing();
      ImGui::TextDisabled(
          T(Str::kDoNotRestartWhileThe_7d53));
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      if (ImGui::Button(T(Str::kIUnderstand_b07e), ImVec2(150.0f, 0.0f))) {
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
      effective_seen?std::to_string(effective_multiplier)+"x":game_request_seen?std::to_string(game_multiplier)+"x":T(Str::kWaiting_0e48);
  const char* player_state=runtime_error?T(Str::kCompatibilityIssue_19f4):!enabled?T(Str::kDisabledThisSession_cd7f):mfg_confirmed?T(Str::kGeneratingFrames_c5e5):
      mfg_live?T(Str::kDriverReportsMfgActive_2db6):mfg_request_accepted?T(Str::kMultiplierRequested_fcfe):provider_ready?T(Str::kReadyEnableFgInThe_eb9f):T(Str::kWaitingForFrameGeneration_08ec);
  // 1.8: DLSS-G's presentation, but Intel's model generates; the panel must say so.
  const bool xmx_selected=mfgunlock::xmx::generator::Enabled();
  const auto xmx_report=xmx_selected?mfgunlock::xmx::generator::Snapshot():mfgunlock::xmx::generator::Report{};
  // The hook only covers Direct3D 12; elsewhere NVIDIA's generator keeps running.
  const bool xmx_d3d12=strcmp(RenderApiName(render_api),"Direct3D 12")==0;
  const bool xmx_running=xmx_selected && xmx_report.hooked && xmx_d3d12;
  const bool xmx_failed=xmx_running && !xmx_report.generating && !xmx_report.failure.empty();
  // Not called at all: the game turned frame generation off, or NGX uses a module without the hook.
  const bool xmx_bypassed=xmx_running && mfgunlock::xmx::policy::Bypassed(xmx_report.hooked,xmx_report.everEvaluated,mfg_live);
  // 1.10.0 (plan 1.3): RTX 20/30, frame generation on, and NVIDIA never created it.
  const bool create_missing=mfgunlock::xmx::preada::CreateMissing(GetTickCount64());
  if(xmx_bypassed) {
    static std::atomic_flag logged=ATOMIC_FLAG_INIT;
    if(!logged.test_and_set()) mfgunlock::xmx::Log(T(Str::kNotCalledWhileTheDriver_5b2d));
  }
  const char* xmx_waiting=xmx_bypassed?T(Str::kNvidiaIsGenerating_0bf6):xmx_report.evaluating?T(Str::kWaitingForFrames_9c2f):T(Str::kPausedOrOffInThe_52c7);
  const auto xmx_support_report=[&] {
    std::string report="MFG Unlock 1.10.1\nmethod=";
    report+=xmx_selected?"XeMFG DLSS-G":"DLSS-G";
    report+=std::string("\nrenderer=")+RenderApiName(render_api)+"\nstatus="+player_state;
    if(xmx_selected) {
      report+=std::string("\nxmx_hooked=")+(xmx_report.hooked?"1":"0")+"\nxmx_partial="+(xmx_report.partiallyHooked?"1":"0")
          +"\nxmx_bypassed="+(xmx_bypassed?"1":"0")+"\nxmx_evaluating="+(xmx_report.evaluating?"1":"0")+"\nxmx_generating="+(xmx_report.generating?"1":"0")
          +"\nxmx_idle="+mfgunlock::xmx::policy::IdleText(xmx_report.idle)+"\nxmx_failure="+xmx_report.failure
          +"\nxmx_frames_limit="+std::to_string(xmx_report.framesLimit)+"\nxmx_hud="+xmx_report.hud;
      // The support report stays in English.
      const auto created=g_xmx_handles_created.load();
      report+="\nxmx_nvidia_feature="+(!XmxSaveVramWanted()?std::string("created by NVIDIA (XmxSaveVram=0)"):created>0?"not created, memory saved ("+std::to_string(created)+" creation(s))":std::string("not created yet"));
    }
    report+=std::string("\npre_ada_admitted=")+(mfgunlock::xmx::preada::Admitted()?"1":"0");
    report+="\nstreamline_log="+mfgunlock::streamlinelog::Describe(mfgunlock::streamlinelog::Current());
    // 1.10.0 (sl_host.hpp): Streamline hosted for an NGX-only game.
    if(mfgunlock::slhost::Wanted()) report+="\nstreamline_host="+mfgunlock::slhost::Describe();
    report+=CapabilityReport();
    // 1.9.0: the driver profile, read only, and only now: this runs when
    // the report is saved or copied.
    return report+"\n"+FrameGenChainReport()+mfgunlock::drsreport::Describe()+mfgunlock::system::Report();
  };
  mfgunlock::ui::Hero(multiplier_text.c_str(),xmx_running?mfgunlock::ui::kXemfgDlssgName:"DLSS-G",RenderApiName(render_api),
      xmx_failed?T(Str::kFrameGenerationStopped_cfe5):xmx_running?(xmx_report.generating?T(Str::kGeneratingFrames_c5e5):xmx_waiting):player_state,
      xmx_running?xmx_report.generating:mfg_live);
  // 1.9.0: another MFG Unlock file beside this one, or an early-loading
  // entry that names a file other than this one.
  if(const auto copies=CheckAddonCopies(); !copies.others.empty() || !copies.stale_early.empty()) {
    mfgunlock::ui::ProblemCard problem("##addon_copies",T(Str::kMoreThanOneMfgUnlock_2ac5),mfgunlock::ui::Warning);
    std::string text;
    if(!copies.others.empty()) {
      text=T(Str::kReshadeLoadsEveryAddonFile_ba0a);
      for(size_t i=0;i<copies.others.size();++i) text+=(i?", ":"")+copies.others[i];
      text+=". Keep only "+SelfModuleFileName()+" and delete the others while the game is closed.";
    }
    if(!copies.stale_early.empty()) {
      if(!text.empty()) text+="\n";
      text+="ReShade.ini loads early a file that is not this one: ";
      for(size_t i=0;i<copies.stale_early.size();++i) text+=(i?", ":"")+copies.stale_early[i];
      text+=". Replace it with "+SelfModuleFileName()+" under [ADDON] LoadFromDllMain.";
    }
    ImGui::TextWrapped("%s",text.c_str());
  }
  // 1.10.0: DLSS Frame Generation needs HAGS on every GPU; NVIDIA's plugin
  // goes quiet without it, so the game shows the option and nothing happens.
  if(SystemCheckHags()==mfgunlock::system::Hags::kOff) {
    mfgunlock::ui::ProblemCard problem("##hags_off",T(Str::kHardwareAcceleratedGpuSchedulingIs_ae9a),mfgunlock::ui::Warning);
    ImGui::TextWrapped(T(Str::kDlssFrameGenerationRequiresIt_ac77));
  }
  // 1.8.5: only the driver's built-in provider is in use (2x at most).
  if(const auto driver_only=CheckDriverOnlyProvider(); driver_only.active) {
    mfgunlock::ui::ProblemCard problem("##driver_only_provider",T(Str::kOnly2xNvidiaIsUsing_6a1e),mfgunlock::ui::Warning);
    ImGui::TextWrapped(T(Str::kThisGameIsUsingThe_dbd3),
        driver_only.version.empty()?"":(" ("+driver_only.version+")").c_str());
  }
  // 1.10.0: on Vulkan, dashdogy's provider generates XMX's frames instead of the D3D12 hook.
  bool xmx_vulkan=false;
#if defined(MFGUNLOCK_NATIVE_XEMFG) && defined(MFGUNLOCK_XEMFG_VULKAN)
  if(xmx_selected && !xmx_d3d12 && mfgunlock::xemfg::native::vulkan::XmxProviderWanted())
    xmx_vulkan=mfgunlock::xemfg::native::vulkan::XmxProviderReport().rfind("created",0)==0;
#endif
  if(xmx_selected && xmx_vulkan) {
    mfgunlock::ui::Readout("##xmx_generator",T(Str::kGenerator_8b62),T(Str::kIntelXefgModelInDlss_a80b),
        T(Str::kTheXmxModeReplacesNvidia_8197),mfgunlock::ui::Accent);
  } else if(xmx_selected) {
    mfgunlock::ui::Readout("##xmx_generator",T(Str::kGenerator_8b62),!xmx_running?T(Str::kNvidiaDlssGXmxNot_1c34):xmx_bypassed?T(Str::kNvidiaDlssGXmxNot_cd23):xmx_failed?T(Str::kNoneXmxFailed_a6e0):xmx_report.generating?T(Str::kIntelXefgModelInDlss_a80b):xmx_report.evaluating?T(Str::kIntelXefgModelWaitingFor_e883):create_missing?T(Str::kNotStartedNvidiaSRuntime_c49b):T(Str::kIntelXefgModelNotBeing_da0a),
        T(Str::kTheXmxModeReplacesNvidia_8197),xmx_report.generating?mfgunlock::ui::Accent:mfgunlock::ui::Muted);
    if(mfgunlock::framecount::g_game_dynamic_replaced.load(std::memory_order_relaxed))
      mfgunlock::ui::Description(T(Str::kTheGameAskedForDynamic_e14d));
    if(!xmx_d3d12) mfgunlock::ui::Notice(T(Str::kXmxOnlyRunsInDirect3d_28b9));
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    // 1.8.3: RTX 20 and 30. NVIDIA's runtime is admitted on this GPU so the
    // game offers DLSS Frame Generation; if it still cannot create the
    // feature here, XeMFG is the way to generate on it.
    else if(mfgunlock::xmx::preada::Engaged() && mfgunlock::xmx::preada::g_create_result.load()>=0 &&
            NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(mfgunlock::xmx::preada::g_create_result.load()))) {
      mfgunlock::ui::ProblemCard problem("##xmx_pre_ada",T(Str::kXmxCouldNotStartOn_814f),mfgunlock::ui::Warning);
      ImGui::TextWrapped(T(Str::kOnRtx20And30_b0f7),xmx_report.failure.c_str());
      mfgunlock::ui::SupportActions(xmx_support_report);
      static bool switch_failed=false;
      mfgunlock::ui::ButtonRow actions(1);
      if(actions(T(Str::kUseXemfg_d141),T(Str::kSavesXemfgAsTheFrame_a69a)))
        switch_failed=!mfgunlock::backend::SelectMode(mfgunlock::backend::Mode::XeMfg);
      if(switch_failed) mfgunlock::ui::Description(T(Str::kTheChangeWasNotSaved_67c0));
      else if(mfgunlock::backend::ReadMode()==mfgunlock::backend::Mode::XeMfg) mfgunlock::ui::Description(T(Str::kSavedRestartTheGameTo_268f));
    }
#endif
    else if(!xmx_report.hooked && !xmx_report.failure.empty()) {
      mfgunlock::ui::Notice(("XeMFG DLSS-G could not start, so NVIDIA's generator is producing the frames: "+xmx_report.failure).c_str());
#if defined(MFGUNLOCK_NATIVE_XEMFG)
      mfgunlock::ui::ButtonRow actions(1);
      if(actions(T(Str::kInstallUpdateFiles_73d5),T(Str::kOpensTheExternalHelperWith_d3e3)))
        mfgunlock::dependencies::Repair();
      const auto repair=mfgunlock::dependencies::Snapshot();
      if(repair.helperRunning || !repair.message.empty()) mfgunlock::ui::Description(repair.message.c_str());
#endif
    }
    else if(xmx_failed) {
      mfgunlock::ui::ProblemCard problem("##xmx_failed",T(Str::kXmxStoppedGenerating_5aa9),mfgunlock::ui::Danger);
      ImGui::TextWrapped(T(Str::kNoFramesAreGeneratedUntil_a798),xmx_report.failure.c_str());
      mfgunlock::ui::Description(T(Str::kSelectAnotherFrameGenerationMethod_4fd5));
      mfgunlock::ui::SupportActions(xmx_support_report);
    }
    else if(xmx_bypassed) {
      mfgunlock::ui::ProblemCard problem("##xmx_bypassed",T(Str::kXmxIsNotBeingUsed_ad69),mfgunlock::ui::Warning);
      ImGui::TextWrapped(T(Str::kThisGameRunsACopy_13ab));
      mfgunlock::ui::SupportActions(xmx_support_report);
    }
    else if(xmx_running && !xmx_report.generating) {
      if(!xmx_report.evaluating) mfgunlock::ui::Description(T(Str::kDlssGIsNotCalling_e7af));
      else if(xmx_report.idle!=mfgunlock::xmx::policy::Idle::None) mfgunlock::ui::Description((std::string(T(Str::kNotGenerating_7848))+mfgunlock::xmx::policy::IdleText(xmx_report.idle)+".").c_str());
    }
    if(xmx_report.partiallyHooked) mfgunlock::ui::Notice(T(Str::kXmxCouldNotHookEvery_790f));
    if(xmx_report.framesLimit!=0) mfgunlock::ui::Notice(("This XMX model produces "+std::to_string(xmx_report.framesLimit)+" generated frame(s) per real frame; higher multipliers repeat it.").c_str());
  }
  // 1.9.0: RTX 20/30 with XMX or with NVIDIA's own generator.
  if(mfgunlock::xmx::preada::Admitted()) {
    mfgunlock::ui::Description(xmx_selected
        ?T(Str::kRtx2030ExperimentalNvidia_f6a7)
        :T(Str::kRtx2030ExperimentalNvidia_f754));
    // 1.8.5: Streamline drops DLSS-G inside slInit on these GPUs; the
    // admission reaches slInit only when the addon loads early.
    if(g_auto_early_load.load(std::memory_order_acquire)==static_cast<int>(AutoEarlyLoad::kAdded) &&
       !mfgunlock::xmx::preada::g_init_observed.load(std::memory_order_acquire))
      mfgunlock::ui::Notice(T(Str::kTheAddonAddedItselfTo_b9b5),mfgunlock::ui::Muted);
    if(!mfgunlock::xmx::preada::g_init_observed.load(std::memory_order_acquire) && !HasEarlyLoadEntry()) {
      mfgunlock::ui::Notice(T(Str::kThisGameStartedStreamlineBefore_3623));
      static int early_state=0;  // 0 not tried, 1 saved, 2 failed
      mfgunlock::ui::ButtonRow early(1);
      if(early(T(Str::kEnableEarlyAddonLoading_a017),T(Str::kAddsThisAddonToReshade_09f4))) {
        early_state=EnsureEarlyLoadEntry()?1:2;
        reshade::log::message(early_state==1?reshade::log::level::info:reshade::log::level::error,
            early_state==1?"mfgunlock: added this addon to ADDON.LoadFromDllMain for the pre-Ada admission; restart required."
                          :"mfgunlock: could not update ADDON.LoadFromDllMain; add the addon manually and restart.");
      }
      if(early_state==1) mfgunlock::ui::Description(T(Str::kSavedRestartTheGame_a1f6));
      else if(early_state==2) mfgunlock::ui::Description(T(Str::kReshadeIniCouldNotBe_03d9));
    }
    if(const uint64_t slow=mfgunlock::xmx::preada::g_slowest_prepare_ms.load(std::memory_order_relaxed); slow>=mfgunlock::xmx::preada::kSlowPrepareMs)
      mfgunlock::ui::Description(("Preparing NVIDIA's kernels for this GPU took "+std::to_string((slow+500)/1000)+" s during the game's start. This is expected on RTX 20 and happens on every start.").c_str());
    // 1.9.0: no temporal correction in any provider: 3x and above are not smoother than 2x.
    if(!xmx_selected && mfgunlock::xmx::preada::g_temporal_applied.load(std::memory_order_relaxed)==0 &&
       mfgunlock::xmx::preada::g_temporal_missing.load(std::memory_order_relaxed)!=0)
      mfgunlock::ui::Notice(T(Str::kTheTemporalCorrectionCouldNot_e3e9));
    const int64_t created=mfgunlock::xmx::preada::g_create_result.load();
    if(!xmx_selected && created>=0 && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(created))) {
      mfgunlock::ui::ProblemCard problem("##dlssg_pre_ada",T(Str::kNvidiaSFrameGenerationCould_9278),mfgunlock::ui::Warning);
      char code[16]; std::snprintf(code,sizeof(code),"0x%08x",static_cast<unsigned>(created));
      ImGui::TextWrapped(T(Str::kNvidiaSRuntimeRefusedTo_d636),code);
    }
  }
  mfgunlock::ui::SessionNotice(xmx_support_report);
  mfgunlock::ui::SafeModeNotice();
  if(dynamic_applied) mfgunlock::ui::Description(T(Str::kDynamicMfgIsChoosingThe_bef7));
  if(sleep_status_available && (driver_vsync_active || gsync_active))
    mfgunlock::ui::Description(driver_vsync_active && gsync_active?T(Str::kDriverVsyncGSyncActive_1e98):driver_vsync_active?T(Str::kDriverVsyncActive_d54c):T(Str::kGSyncActive_351c));
  if (restart_pending) {
    ImGui::Spacing();
    ImGui::TextColored(kUiWarning, T(Str::kRestartRequired_15ed));
    ImGui::TextWrapped(
        T(Str::kOneOrMoreSavedChanges_f677));
  }
  if(dynamic_enabled && dynamic_hard_unavailable)
    mfgunlock::ui::Notice(T(Str::kDynamicMfgIsUnavailableFor_e9d5));
  if (mfgunlock::framecount::g_dynamic_change_pending.load(
          std::memory_order_acquire) ||
      mfgunlock::framecount::g_quality_mode_change_pending.load(
          std::memory_order_acquire)) {
    ImGui::TextColored(kUiWarning, T(Str::kApplyPending_9518));
    ImGui::TextWrapped(
        T(Str::kToggleFrameGenerationOffAnd_a32a));
  }

  enum class UiPage { General, Quality, DlssInfo, Latency, Support };
  static UiPage page=UiPage::General;
  static bool show_runtime_info=false;
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    int selected=page==UiPage::General?0:page==UiPage::Quality?1:page==UiPage::Latency?2:3;
    ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##dlssg_page",&selected,T(Str::kGeneralQualityLatencyDiagnostics_b9c8)))
      page=selected==0?UiPage::General:selected==1?UiPage::Quality:selected==2?UiPage::Latency:UiPage::Support;
    if(selected==3) {
      if(ImGui::RadioButton(T(Str::kStatus_f20f),!show_runtime_info)) show_runtime_info=false;
      ImGui::SameLine(); if(ImGui::RadioButton(T(Str::kRuntimeInformation_e591),show_runtime_info)) show_runtime_info=true;
      page=show_runtime_info?UiPage::DlssInfo:UiPage::Support;
    }
  } else if(const float tab=mfgunlock::ui::TabWidth(4); mfgunlock::ui::BeginTabs("##mfg_unlock_pages")) {
    if(mfgunlock::ui::TabItem(T(Str::kGeneral_fd0b),tab)) { page=UiPage::General; ImGui::EndTabItem(); }
    if(mfgunlock::ui::TabItem(T(Str::kQuality_bec6),tab)) { page=UiPage::Quality; ImGui::EndTabItem(); }
    if(mfgunlock::ui::TabItem(T(Str::kLatency_5ef9),tab)) { page=UiPage::Latency; ImGui::EndTabItem(); }
    if(mfgunlock::ui::TabItem(T(Str::kDiagnostics_1397),tab)) {
      if(ImGui::RadioButton(T(Str::kStatus_f20f),!show_runtime_info)) show_runtime_info=false;
      ImGui::SameLine(); if(ImGui::RadioButton(T(Str::kRuntimeInformation_e591),show_runtime_info)) show_runtime_info=true;
      page=show_runtime_info?UiPage::DlssInfo:UiPage::Support;
      ImGui::EndTabItem();
    }
    mfgunlock::ui::EndTabs();
  }
  // 1.10.0 (plan 4.3): this page's blocks, in overlay_page_dlssinfo.inl; textually included so the locals above stay visible.
#include "./overlay_page_dlssinfo.inl"

  // 1.10.0 (plan 4.3): this page's blocks, in overlay_page_general.inl; textually included so the locals above stay visible.
#include "./overlay_page_general.inl"

  // 1.10.0 (plan 4.3): this page's blocks, in overlay_page_latency.inl; textually included so the locals above stay visible.
#include "./overlay_page_latency.inl"
  // 1.8: these change NVIDIA's generator, which does not run under XMX. DLSS
  // presets stay: they belong to the upscaler.
  if (page == UiPage::Quality && xmx_selected)
    mfgunlock::ui::Description(T(Str::kXmxSelectedSettingsForNvidia_3e26));
  // 1.10.0 (plan 4.3): this page's blocks, in overlay_page_quality.inl; textually included so the locals above stay visible.
#include "./overlay_page_quality.inl"

  // 1.10.0 (plan 4.3): this page's blocks, in overlay_page_support.inl; textually included so the locals above stay visible.
#include "./overlay_page_support.inl"

  mfgunlock::ui::Footer(T(Str::kChangesMarkedForRestartKeep_57d2));
}
