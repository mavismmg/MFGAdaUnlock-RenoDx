// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the General page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

  if (page == UiPage::General) {
    ImGui::Spacing();
    mfgunlock::ui::Section(T(Str::kFrameGeneration_83ac),T(Str::kEnableFrameGenerationInThe_1380));
    mfgunlock::ui::Card generation_card("##dlssg_general_card");
    mfgunlock::ui::BackendSelector();
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::dlssgdisplay::Draw();
#endif
    if (ImGui::BeginTable("##frame_generation_settings", 2,
                        ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T(Str::kSetting_754b), ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn(T(Str::kControl_bf3e), ImGuiTableColumnFlags_WidthStretch, 0.5f);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        T(Str::kEnableMfgUnlock_67ca), T(Str::kAppliedOnTheNextGame_e392),
        T(Str::kChangesTheMasterAddonState_fcdf));
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
    const char* kMultiplierModes[] = {
        T(Str::kGame_2327), "2x", "3x", "4x", "5x", "6x"};
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        T(Str::kFrameMultiplier_4a05), T(Str::kUseTheGameSSelector_35b5),
        T(Str::kGameControlledPreservesTheMultiplier_c085));
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (mfgunlock::ui::Segmented("##frame_multiplier", force_choice,
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
    if (mfgunlock::forcepolicy::StreamlineBlocksMultiFrame(
            mfgunlock::framecount::g_active_streamline_version.load(std::memory_order_acquire), 2)) {
      ImGui::PushTextWrapPos();
      ImGui::TextColored(kUiWarning,
          T(Str::kThisGameSStreamlineIs_9679));
      ImGui::PopTextWrapPos();
    }

    bool dynamic_mfg = dynamic_enabled && !dynamic_game_blocked;

    // 1.8.5: name the missing piece instead of a generic "waiting".
    std::string dynamic_status;
    if (dynamic_game_blocked) {
      dynamic_status = T(Str::kDisabledForThisGameS_4b28);
    } else if (dynamic_available) {
      dynamic_status = T(Str::kRuntimeSupportDetected_b544);
    } else if (dynamic_renderer_blocked) {
      dynamic_status = T(Str::kUnavailableNvidiaDynamicMfgNeeds_d275);
    } else if (dynamic_version_blocked &&
               !mfgunlock::runtimeversion::IsDynamicStreamlineVersion(streamline_version)) {
      dynamic_status = "Unavailable: this game loads Streamline " + streamline_text + "; Dynamic needs 2.14 or newer.";
    } else if (dynamic_version_blocked) {
      dynamic_status = "Unavailable: this game loads DLSS-G " + dlssg_text + "; Dynamic needs 310.9.1 or newer.";
    } else if (dynamic_provider_blocked) {
      dynamic_status = T(Str::kUnavailableNvidiaSRuntimeReports_8975);
    } else if (!streamline_seen) {
      dynamic_status = T(Str::kWaitingStreamlineHasNotBeen_a456);
    } else if (!dlssg_seen) {
      dynamic_status = "Waiting: Streamline " + streamline_text + " found; NVIDIA's frame generation file has not loaded yet.";
    } else {
      dynamic_status = "Waiting: Streamline " + streamline_text + " and DLSS-G " + dlssg_text +
                       " found; the runtime has not answered the Dynamic support query yet. Turn Frame Generation on in the game.";
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        T(Str::kNvidiaDynamicMfg_3547),
        dynamic_status.c_str(),
        T(Str::kLetsNvidiaSNativeScheduler_f909));
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
    if (xmx_selected && dynamic_mfg) {
      ImGui::PushTextWrapPos();
      ImGui::TextColored(kUiWarning, T(Str::kExperimentalWithXmxNvidiaS_3514));
      ImGui::PopTextWrapPos();
    }

    if (dynamic_mfg) {
      int dynamic_target = static_cast<int>(
          mfgunlock::framecount::g_dynamic_target_fps.load(
              std::memory_order_relaxed));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          T(Str::kDynamicOutputTarget_dc54), T(Str::k0FollowsTheDisplayRefresh_6f3f),
          T(Str::kSetsDynamicMfgSRequested_0d45));
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

      // 1.8.5: the ceiling the NVIDIA app calls "Multiplier: Up to Nx".
      {
        const char* kCeilings[] = {T(Str::kAutoDriverNvidiaApp_586e), T(Str::kUpTo2x_6f99), T(Str::kUpTo3x_2790), T(Str::kUpTo4x_806f),
                                                T(Str::kUpTo5x_3866), T(Str::kUpTo6x_098d)};
        const uint32_t configured = mfgunlock::drsoverride::g_max_multiplier.load(std::memory_order_relaxed);
        int ceiling = configured >= 2 ? static_cast<int>(configured) - 1 : 0;
        std::string ceiling_status;
        const uint32_t answered = mfgunlock::drsoverride::g_answered_value.load(std::memory_order_relaxed);
        if (answered != UINT32_MAX && answered == mfgunlock::drsoverride::DrsValueFor(configured)) {
          ceiling_status = T(Str::kAppliedNvidiaSRuntimeRead_6950);
        } else if (answered != UINT32_MAX) {
          ceiling_status = "Restart the game to apply. This session uses up to " + std::to_string(answered + 1) + "x.";
        } else if (configured != 0 && mfgunlock::drsoverride::g_read_seen.load(std::memory_order_acquire)) {
          ceiling_status = T(Str::kRestartTheGameToApply_00ca);
        } else if (configured != 0) {
          ceiling_status = T(Str::kRestartTheGameToApply_8467);
        } else {
          const uint32_t driver = mfgunlock::drsoverride::g_driver_value.load(std::memory_order_relaxed);
          ceiling_status = driver != UINT32_MAX && driver != 0
                               ? "Driver profile: up to " + std::to_string(driver + 1) + "x."
                               : T(Str::kNvidiaSRuntimePicksUp_4fa5);
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SettingLabel(T(Str::kDynamicMultiplierCeiling_11e2), ceiling_status.c_str(),
                     T(Str::kTheHighestMultiplierNvidiaS_3f4a));
        ImGui::TableNextColumn();
        if (dynamic_hard_unavailable) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##dynamic_ceiling", &ceiling, kCeilings, IM_ARRAYSIZE(kCeilings))) {
          const uint32_t multiplier = ceiling == 0 ? 0u : static_cast<uint32_t>(ceiling) + 1u;
          mfgunlock::drsoverride::g_max_multiplier.store(multiplier, std::memory_order_relaxed);
          reshade::set_config_value(nullptr, kConfigSection, "DynamicMaxMultiplier", static_cast<int>(multiplier));
        }
        if (dynamic_hard_unavailable) ImGui::EndDisabled();
      }

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          T(Str::kVsyncGSyncBehavior_ea0c),
          driver_vsync_active && gsync_active
              ? T(Str::kDetectedDriverVsyncGSync_0046)
              : (driver_vsync_active
                     ? T(Str::kDetectedDriverVsyncActive_255a)
                     : (gsync_active
                            ? T(Str::kDetectedGSyncActiveDriver_cba1)
                            : T(Str::kCheckTheRuleBeforeChoosing_da1f))),
          T(Str::kWithVsyncActiveStreamlineIgnores_7e11));
      ImGui::TableNextColumn();
      if (driver_vsync_active) {
        ImGui::TextColored(kUiWarning, T(Str::kOutputTargetIgnoredFollowsRefresh_c01d));
      } else {
        ImGui::TextWrapped(T(Str::kCustomOutputTargetAvailableWhen_d045));
      }
    }

    ImGui::EndTable();
  }

    ImGui::Spacing();
    ImGui::TextDisabled(T(Str::kFrameRateControls_5b82));
    ImGui::Separator();
    if (ImGui::BeginTable("##frame_rate_controls", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn(T(Str::kSetting_754b), ImGuiTableColumnFlags_WidthStretch,
                              0.5f);
      ImGui::TableSetupColumn(T(Str::kControl_bf3e), ImGuiTableColumnFlags_WidthStretch,
                              0.5f);
      const auto source_cap_state =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const std::string source_cap_summary =
          UserSourceCapSummary(source_cap_state);
      int source_cap = static_cast<int>(source_cap_state.configured_fps);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      SettingLabel(
          T(Str::kOutputFpsCapReflex_01c8), source_cap_summary.c_str(),
          T(Str::kSetsTheFinalOutputFps_c7e4));
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
            T(Str::kThisIsTheFinalFps_1fb6));
      }
    }
  }

  if (page == UiPage::General) {
    mfgunlock::ui::SystemCard(!sleep_status_available ? nullptr : driver_vsync_active && gsync_active ? T(Str::kDriverVsyncGSync_284a) : driver_vsync_active ? T(Str::kDriverVsync_77be) : gsync_active ? "G-SYNC" : T(Str::kOff_b4aa), xmx_selected);
    DrawFrameGenChainCard();
    DrawSystemCheckCard();
  }
