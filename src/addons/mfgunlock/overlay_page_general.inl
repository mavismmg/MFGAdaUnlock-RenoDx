// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the General page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

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
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.5f);

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
        "Game", "2x", "3x", "4x", "5x", "6x"};
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "Frame Multiplier", "Use the game's selector when possible.",
        "Game controlled preserves the multiplier selected in the game's menu. A fixed value overrides that request when supported. Dynamic MFG takes priority while active.");
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
          "This game's Streamline is older than 2.7: 3x to 6x are not applied, to avoid a crash.");
      ImGui::PopTextWrapPos();
    }

    bool dynamic_mfg = dynamic_enabled && !dynamic_game_blocked;

    // 1.8.5: name the missing piece instead of a generic "waiting".
    std::string dynamic_status;
    if (dynamic_game_blocked) {
      dynamic_status = "Disabled for this game's compatibility.";
    } else if (dynamic_available) {
      dynamic_status = "Runtime support detected.";
    } else if (dynamic_renderer_blocked) {
      dynamic_status = "Unavailable: NVIDIA Dynamic MFG needs Direct3D 12.";
    } else if (dynamic_version_blocked &&
               !mfgunlock::runtimeversion::IsDynamicStreamlineVersion(streamline_version)) {
      dynamic_status = "Unavailable: this game loads Streamline " + streamline_text + "; Dynamic needs 2.14 or newer.";
    } else if (dynamic_version_blocked) {
      dynamic_status = "Unavailable: this game loads DLSS-G " + dlssg_text + "; Dynamic needs 310.9.1 or newer.";
    } else if (dynamic_provider_blocked) {
      dynamic_status = "Unavailable: NVIDIA's runtime reports Dynamic MFG unsupported on this system (driver or GPU).";
    } else if (!streamline_seen) {
      dynamic_status = "Waiting: Streamline has not been detected in this game yet.";
    } else if (!dlssg_seen) {
      dynamic_status = "Waiting: Streamline " + streamline_text + " found; NVIDIA's frame generation file has not loaded yet.";
    } else {
      dynamic_status = "Waiting: Streamline " + streamline_text + " and DLSS-G " + dlssg_text +
                       " found; the runtime has not answered the Dynamic support query yet. Turn Frame Generation on in the game.";
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    SettingLabel(
        "NVIDIA Dynamic MFG",
        dynamic_status.c_str(),
        "Lets NVIDIA's native scheduler choose the generated-frame count. Requires D3D12, driver 595.41 or newer, DLSS-G 310.9.1 or newer and Streamline 2.14 or newer; NVIDIA's runtime has the final say. Star Wars Outlaws is kept on fixed/game-controlled MFG because its integration breaks on the Dynamic path. Toggle Frame Generation off/on after changing it.");
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
      ImGui::TextColored(kUiWarning, "Experimental with XMX: NVIDIA's runtime picks the count and XMX generates it.");
      ImGui::PopTextWrapPos();
    }

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

      // 1.8.5: the ceiling the NVIDIA app calls "Multiplier: Up to Nx".
      {
        static const char* const kCeilings[] = {"Auto (driver / NVIDIA app)", "Up to 2x", "Up to 3x", "Up to 4x",
                                                "Up to 5x", "Up to 6x"};
        const uint32_t configured = mfgunlock::drsoverride::g_max_multiplier.load(std::memory_order_relaxed);
        int ceiling = configured >= 2 ? static_cast<int>(configured) - 1 : 0;
        std::string ceiling_status;
        const uint32_t answered = mfgunlock::drsoverride::g_answered_value.load(std::memory_order_relaxed);
        if (answered != UINT32_MAX && answered == mfgunlock::drsoverride::DrsValueFor(configured)) {
          ceiling_status = "Applied: NVIDIA's runtime read this ceiling.";
        } else if (answered != UINT32_MAX) {
          ceiling_status = "Restart the game to apply. This session uses up to " + std::to_string(answered + 1) + "x.";
        } else if (configured != 0 && mfgunlock::drsoverride::g_read_seen.load(std::memory_order_acquire)) {
          ceiling_status = "Restart the game to apply. This session uses the driver's ceiling.";
        } else if (configured != 0) {
          ceiling_status = "Restart the game to apply: NVIDIA's runtime reads it when it starts.";
        } else {
          const uint32_t driver = mfgunlock::drsoverride::g_driver_value.load(std::memory_order_relaxed);
          ceiling_status = driver != UINT32_MAX && driver != 0
                               ? "Driver profile: up to " + std::to_string(driver + 1) + "x."
                               : "NVIDIA's runtime picks up to the highest multiplier offered.";
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SettingLabel("Dynamic multiplier ceiling", ceiling_status.c_str(),
                     "The highest multiplier NVIDIA's Dynamic MFG may pick, as the NVIDIA app's \"Multiplier: Up to\" setting. "
                     "The addon gives this value to NVIDIA's runtime in this game only; the driver profile is not changed. "
                     "Auto keeps the driver's or NVIDIA app's value. Applies after a game restart.");
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
                              0.5f);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch,
                              0.5f);
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

  if (page == UiPage::General) {
    mfgunlock::ui::SystemCard(!sleep_status_available ? nullptr : driver_vsync_active && gsync_active ? "Driver VSync + G-SYNC" : driver_vsync_active ? "Driver VSync" : gsync_active ? "G-SYNC" : "Off", xmx_selected);
    DrawFrameGenChainCard();
    DrawSystemCheckCard();
  }
