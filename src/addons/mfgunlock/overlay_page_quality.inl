// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the Quality page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

  if (page == UiPage::Quality) {
    mfgunlock::ui::Section("Image quality","Choose a stable quality profile. Changes marked for restart are saved for the next launch.");
    mfgunlock::ui::Card quality_card("##dlssg_quality_card");
    if (ImGui::BeginTable("##image_quality_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.5f);

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
      constexpr const char* kShort[] = {"Game", "J", "K", "L", "M"};
      static_assert(std::size(kShort) == std::size(kPresets));
      if (mfgunlock::ui::Segmented("##dlss_sr_preset", index, kShort, static_cast<int>(std::size(kShort)))) {
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
      constexpr const char* kShort[] = {"Game", "D", "E", "F"};
      static_assert(std::size(kShort) == std::size(kPresets));
      if (mfgunlock::ui::Segmented("##dlss_rr_preset", index, kShort, static_cast<int>(std::size(kShort)))) {
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

    ImGui::BeginDisabled(xmx_selected);
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
    ImGui::EndDisabled();
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

    ImGui::BeginDisabled(xmx_selected);
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

    ImGui::EndDisabled();
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
    ImGui::BeginDisabled(xmx_selected);
    DrawAdvancedQualityControls(
        g_configured_adaptive_quality.load(std::memory_order_relaxed));
    ImGui::EndDisabled();
  }
  }
