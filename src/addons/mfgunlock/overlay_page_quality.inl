// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the Quality page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

  if (page == UiPage::Quality) {
    mfgunlock::ui::Section(T(Str::kImageQuality_d36b),T(Str::kChooseAStableQualityProfile_410c));
    mfgunlock::ui::Card quality_card("##dlssg_quality_card");
    if (ImGui::BeginTable("##image_quality_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T(Str::kSetting_754b), ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn(T(Str::kControl_bf3e), ImGuiTableColumnFlags_WidthStretch, 0.5f);

    // 1.6 (plan B): DLSS Super Resolution preset, applied during play.
    {
      namespace preset = mfgunlock::dlsspreset;
      const char* kPresets[] = {T(Str::kGameDefault_d21e), T(Str::kPresetJ_4650), T(Str::kPresetK_47e3), T(Str::kPresetL_4fc2), T(Str::kPresetM_5155)};
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
                      ? std::string(T(Str::kThisGameCreatesDlssItself_c04b)) + kPresets[index] + " is in use."
                      : T(Str::kThisGameCreatesDlssItself_10d5);
      else if (!preset::g_wrapped.load(std::memory_order_relaxed))
        summary = T(Str::kWaitingForTheGameS_c616);
      else if (status == preset::Status::kUnknownAbi)
        summary = T(Str::kThisGameSDlssOptions_40fe);
      else if (status == preset::Status::kRejected)
        summary = T(Str::kStreamlineRefusedThePresetThe_7f75);
      else if (status == preset::Status::kApplied)
        summary = std::string(T(Str::kApplied_4470)) + kPresets[index] + ".";
      else
        summary = game_preset ? "The game's preset (" + std::string(1, static_cast<char>('A' + game_preset - 1)) + ")."
                              : T(Str::kTheGameSPresetDlss_4f47);
      if (driver_override) summary += " An NVIDIA App override is active and takes precedence.";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          T(Str::kDlssSuperResolutionPreset_fa7d), summary.c_str(),
          T(Str::kReplacesTheGameSDlss_c596));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      const char* kShort[] = {T(Str::kGame_2327), "J", "K", "L", "M"};
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
      const char* kPresets[] = {T(Str::kGameDefault_d21e), T(Str::kPresetD_5c5a), T(Str::kPresetE_5ded), T(Str::kPresetFDlss45_6e12)};
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
        summary = T(Str::kWaitingForTheGameS_de5a);
      else if (status == preset::Status::kUnknownAbi)
        summary = T(Str::kThisGameSRayReconstruction_889a);
      else if (status == preset::Status::kRejected)
        summary = T(Str::kStreamlineRefusedThePresetThe_41b6);
      else if (status == preset::Status::kApplied)
        summary = std::string(T(Str::kApplied_4470)) + kPresets[index] + ".";
      else
        summary = game_preset ? "The game's preset (" + std::string(1, static_cast<char>('A' + game_preset - 1)) + ")."
                              : T(Str::kTheGameSPresetDlss_4f47);
      if (driver_override) summary += " An NVIDIA App override is active and takes precedence.";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          T(Str::kDlssRayReconstructionPreset_1004), summary.c_str(),
          T(Str::kReplacesTheGameSDlss_7099));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.0f);
      const char* kShort[] = {T(Str::kGame_2327), "D", "E", "F"};
      static_assert(std::size(kShort) == std::size(kPresets));
      if (mfgunlock::ui::Segmented("##dlss_rr_preset", index, kShort, static_cast<int>(std::size(kShort)))) {
        preset::g_rr_selected.store(kValues[index], std::memory_order_relaxed);
        reshade::set_config_value(nullptr, kConfigSection, "DlssRayReconstructionPreset",
                                  static_cast<int>(kValues[index]));
      }
    }

    const char* kHdrModes[] = {
        T(Str::kNativeMostGames_943d), T(Str::kForceUiCompositionAdvanced_71b1),
        T(Str::kAutomaticGuardUiHdrCompatibility_bf7f),
        T(Str::kFinalColorFallbackTroubleshooting_765b)};
    int hdr_mode = static_cast<int>(
        mfgunlock::framecount::g_hdr_compatibility_mode.load(
            std::memory_order_relaxed));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const char* hdr_summary = T(Str::kNativeGameInputsRecommendedFor_cfe0);
    if (hdr_mode == static_cast<int>(
                        mfgunlock::framecount::HdrCompatibilityMode::kUiRecomposition)) {
      hdr_summary = T(Str::kAdvancedUiSeparationRequiresCorrectly_3c92);
    } else if (hdr_mode == static_cast<int>(
                               mfgunlock::framecount::HdrCompatibilityMode::kAutomaticHybrid)) {
      hdr_summary =
          T(Str::kHdrCompatibilityForHogwartsLegacy_1b1e);
    } else if (hdr_mode == static_cast<int>(
                               mfgunlock::framecount::HdrCompatibilityMode::kFinalColorFallback)) {
      hdr_summary = T(Str::kConservativeHdrUiTroubleshootingFallback_566b);
    }
    SettingLabel(
        T(Str::kFrameGenerationInputs_ac8c), hdr_summary,
        T(Str::kNativePassesTheGameS_4ced));
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
        T(Str::kAdaptiveQualitySuite_ddc2),
        T(Str::kCoordinatesTheExistingImprovementsFor_9e76),
        T(Str::kCoordinatesSilhouetteConfidenceMotionDirection_518f),
        T(Str::kRecommended_024c), kUiPositive);
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
      ImGui::TextDisabled(T(Str::kSavedForNextLaunchRestart_21d0));

    if (adaptive_quality) {
      const char* kAdaptiveProfiles[] = {
          T(Str::kStableCompatibility_1945),
          T(Str::kSmoothTransitions_2d5b),
          T(Str::kBalancedRecommended_5b47)};
      int adaptive_profile = static_cast<int>(
          mfgunlock::adaptivequality::NormalizeProfile(
              g_configured_adaptive_quality_profile.load(
                  std::memory_order_relaxed))) - 1;
      const char* profile_summary =
          adaptive_profile == 2
              ? T(Str::kBalancedHandlingOfMovingEdges_556e)
              : adaptive_profile == 1
                    ? T(Str::kSmootherConfidenceTransitionsForFine_6d22)
                    : T(Str::kOriginalCompatibilityProfile_fdd3);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          T(Str::kAdaptiveQualityProfile_eb7b), profile_summary,
          T(Str::kV3NormalizesLumaChromaDisagreement_1351));
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
        ImGui::TextDisabled(T(Str::kSavedForNextLaunchRestart_21d0));
      }
    }
    ImGui::EndDisabled();
    ImGui::EndTable();
  }
  if (page == UiPage::Quality &&
      g_configured_adaptive_quality.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled(
        T(Str::kAdaptiveQualityManagesGeometryBoundary_f3ef));
  }

  if (page == UiPage::Support && ImGui::CollapsingHeader(T(Str::kAdvancedCompatibilityControls_ef87))) {
    const char* kRuntimeModes[] = {
        T(Str::kGameDefault_d21e), T(Str::kPreferLocalRuntimeDisableOta_b895),
        T(Str::kForceNvidiaOtaRuntime_2fd9)};
    int runtime_mode = static_cast<int>(
        g_configured_runtime_selection_mode.load(std::memory_order_relaxed));
    SetNextItemWidthWithHelp(T(Str::kStreamlineRuntimeSelection_7ca9));
    if (ImGui::Combo(T(Str::kStreamlineRuntimeSelection_7ca9), &runtime_mode,
                     kRuntimeModes,
                     static_cast<int>(std::size(kRuntimeModes)))) {
      g_configured_runtime_selection_mode.store(
          static_cast<unsigned int>(runtime_mode),
          std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "RuntimeSelectionMode", runtime_mode);
    }
    HelpMarker(
        T(Str::kSelectsWhichCompleteStreamlineRuntime_6095));

    int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
    SetNextItemWidthWithHelp(T(Str::kReportedMultiframecountmax_553b));
    if (ImGui::SliderInt(T(Str::kReportedMultiframecountmax_553b), &count,
                         static_cast<int>(kMinCount),
                         static_cast<int>(kMaxCount))) {
      count = std::clamp(count, static_cast<int>(kMinCount),
                         static_cast<int>(kMaxCount));
      g_max_count.store(static_cast<unsigned int>(count),
                        std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
    }
    HelpMarker(
        T(Str::kControlsTheMaximumGeneratedFrame_f51d));

    ImGui::BeginDisabled(xmx_selected);
    bool temporal =
        g_configured_temporal_fix.load(std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kTemporalMidpointFix_2a22), &temporal)) {
      g_configured_temporal_fix.store(temporal,
                                      std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "TemporalFix",
                                temporal ? 1 : 0);
    }
    HelpMarker(
        T(Str::kCorrectsAdaMidpointCompactionAt_3396));

    bool blackwell = g_configured_blackwell_framework_kernels.load(
        std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kPreferValidatedBlackwellFrameworkKernels_e57c),
                        &blackwell)) {
      g_configured_blackwell_framework_kernels.store(
          blackwell, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "BlackwellFrameworkKernels",
                                blackwell ? 1 : 0);
    }
    HelpMarker(
        T(Str::kUsesTheFullValidatedMotion_af84));

    ImGui::EndDisabled();
    bool flip_off =
        g_configured_force_flip_meter_off.load(std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kLegacySoftwareFlipPacing_8d9a), &flip_off)) {
      g_configured_force_flip_meter_off.store(flip_off,
                                               std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection,
                                "ForceFlipMeteringOff", flip_off ? 1 : 0);
    }
    HelpMarker(
        T(Str::kCompatibilityFallbackForOlderStreamline_2186));

    const int active_runtime_mode = static_cast<int>(
        mfgunlock::framecount::g_runtime_selection_mode.load(
            std::memory_order_relaxed));
    if (active_runtime_mode != static_cast<int>(
                                   mfgunlock::framecount::RuntimeSelectionMode::kGameDefault) &&
        !mfgunlock::framecount::g_runtime_selection_observed.load(
            std::memory_order_acquire) &&
        !HasEarlyLoadEntry()) {
      ImGui::TextColored(kUiWarning,
                         T(Str::kStreamlineInitializedBeforeNormalAddon_f5b9));
      if (ImGui::Button(T(Str::kEnableEarlyAddonLoading_a017))) {
        EnsureEarlyLoadEntry();
      }
      HelpMarker(
          T(Str::kAddsThisAddonSCurrent_9025));
    }

    ImGui::Spacing();
    ImGui::TextDisabled(T(Str::kImageQualityCompatibility_4707));
    ImGui::BeginDisabled(xmx_selected);
    DrawAdvancedQualityControls(
        g_configured_adaptive_quality.load(std::memory_order_relaxed));
    ImGui::EndDisabled();
  }
  }
