// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the General page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif


  if (page == UiPage::General) {
    namespace ui = mfgunlock::ui;
    ImGui::Spacing();
    // 1.10.0 (panel reformulation): one card per job. Statuses go in a chip under their
    // control; explanations live in the tooltips.
    ui::Section(T(Str::kFrameGeneration_83ac),T(Str::kEnableFrameGenerationInThe_1380));
    {
    ui::Card generation_card("##dlssg_general_card");
    ui::BackendSelector();

    int force = static_cast<int>(mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed));
    int force_choice = force == 0 ? 0 : force - 1;
    const char* kMultiplierModes[] = {T(Str::kGame_2327), "2x", "3x", "4x", "5x", "6x"};
    ui::Setting("##frame_multiplier_row", T(Str::kFrameMultiplier_4a05), T(Str::kGameControlledPreservesTheMultiplier_c085), [&] {
      if (ui::Segmented("##frame_multiplier", force_choice, kMultiplierModes, static_cast<int>(std::size(kMultiplierModes)))) {
        force = force_choice == 0 ? 0 : force_choice + 1;
        mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(force), std::memory_order_relaxed);
        mfgunlock::framecount::NotifyFixedMultiplierChanged(static_cast<unsigned int>(force));
        reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", force);
      }
    });
    if (mfgunlock::forcepolicy::StreamlineBlocksMultiFrame(
            mfgunlock::framecount::g_active_streamline_version.load(std::memory_order_acquire), 2))
      ui::StatusChip(T(Str::kThisGameSStreamlineIs_9679), ui::Warning);

    bool dynamic_mfg = dynamic_enabled && !dynamic_game_blocked;
    // 1.8.5: name the missing piece instead of a generic "waiting".
    std::string dynamic_status;
    ImVec4 dynamic_color = ui::Warning;
    if (dynamic_game_blocked) {
      dynamic_status = T(Str::kDisabledForThisGameS_4b28); dynamic_color = ui::Muted;
    } else if (dynamic_available) {
      dynamic_status = T(Str::kRuntimeSupportDetected_b544); dynamic_color = ui::Accent;
    } else if (dynamic_renderer_blocked) {
      dynamic_status = T(Str::kUnavailableNvidiaDynamicMfgNeeds_d275); dynamic_color = ui::Muted;
    } else if (dynamic_version_blocked &&
               !mfgunlock::runtimeversion::IsDynamicStreamlineVersion(streamline_version)) {
      dynamic_status = "Unavailable: this game loads Streamline " + streamline_text + "; Dynamic needs 2.14 or newer."; dynamic_color = ui::Muted;
    } else if (dynamic_version_blocked) {
      dynamic_status = "Unavailable: this game loads DLSS-G " + dlssg_text + "; Dynamic needs 310.9.1 or newer."; dynamic_color = ui::Muted;
    } else if (dynamic_provider_blocked) {
      dynamic_status = T(Str::kUnavailableNvidiaSRuntimeReports_8975); dynamic_color = ui::Muted;
    } else if (!streamline_seen) {
      dynamic_status = T(Str::kWaitingStreamlineHasNotBeen_a456);
    } else if (!dlssg_seen) {
      dynamic_status = "Waiting: Streamline " + streamline_text + " found; NVIDIA's frame generation file has not loaded yet.";
    } else {
      dynamic_status = "Waiting: Streamline " + streamline_text + " and DLSS-G " + dlssg_text +
                       " found; the runtime has not answered the Dynamic support query yet. Turn Frame Generation on in the game.";
    }
    const bool block_dynamic_enable = dynamic_game_blocked || (dynamic_hard_unavailable && !dynamic_mfg);
    ui::Setting("##dynamic_mfg_row", T(Str::kNvidiaDynamicMfg_3547), T(Str::kLetsNvidiaSNativeScheduler_f909), [&] {
      if (block_dynamic_enable) ImGui::BeginDisabled();
      if (ui::Toggle("##dynamic_mfg", dynamic_mfg)) {
        mfgunlock::framecount::g_dynamic_mfg_enabled.store(dynamic_mfg, std::memory_order_relaxed);
        mfgunlock::framecount::NotifyDynamicModeChanged();
        reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG", dynamic_mfg ? 1 : 0);
      }
      if (block_dynamic_enable) ImGui::EndDisabled();
    });
    ui::StatusChip(dynamic_status.c_str(), dynamic_color);
    if (xmx_selected && dynamic_mfg) ui::StatusChip(T(Str::kExperimentalWithXmxNvidiaS_3514), ui::Warning);

    if (dynamic_mfg && ImGui::BeginTable("##frame_generation_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T(Str::kSetting_754b), ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn(T(Str::kControl_bf3e), ImGuiTableColumnFlags_WidthStretch, 0.5f);
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
    ImGui::EndTable();
    }
#if defined(MFGUNLOCK_NATIVE_XEMFG)
    mfgunlock::dlssgdisplay::Draw();
#endif
    }

    // Frame rate.
    ui::Section(T(Str::kFrameRate_p5));
    {
      ui::Card rate_card("##frame_rate_card");
      const auto source_cap_state = mfgunlock::framecount::internal::ResolveUserSourceCapState();
      int source_cap = static_cast<int>(source_cap_state.configured_fps);
      ui::Setting("##rendered_fps_cap_row", T(Str::kOutputFpsCapReflex_01c8), T(Str::kSetsTheFinalOutputFps_c7e4), [&] {
        if (ImGui::InputInt("##rendered_fps_cap", &source_cap, 1, 10)) {
          source_cap = source_cap <= 0 ? 0 : std::clamp(source_cap, 10, 1000);
          mfgunlock::framecount::g_reflex_source_fps_cap.store(static_cast<unsigned int>(source_cap), std::memory_order_relaxed);
          g_source_cap_config_origin.store(
              static_cast<unsigned int>(mfgunlock::pacing::SourceCapConfigOrigin::kConfigured), std::memory_order_relaxed);
          mfgunlock::framecount::NotifySourceFpsCapChanged();
          reshade::set_config_value(nullptr, kConfigSection, "ReflexSourceFpsCap", source_cap);
        }
      });
      if (source_cap_state.configured_fps != 0)
        ui::StatusChip(UserSourceCapStatusText(source_cap_state.status), UserSourceCapColor(source_cap_state.status));
      else
        ui::StatusChip(UserSourceCapSummary(source_cap_state).c_str(), ui::Muted);
    }

    if (mfgunlock::backend::active == mfgunlock::backend::Kind::DlssG) {
      namespace adv = mfgunlock::ui::advanced;
      // An INI switch of advanced_settings.hpp, drawn as a toggle.
      const auto toggle = [&](const adv::Option& option, const char* label) {
        ImGui::PushID(&option);
        bool on = adv::Current(option) == L"1";
        ui::Setting("##toggle_row", label, T(option.help), [&] {
          if (ui::Toggle("##toggle", on)) adv::Store(option, on ? L"1" : L"0");
        });
        ImGui::PopID();
      };
      // XMX options (1.10.0): only with XMX selected.
      if (xmx_selected && ui::SectionHeader(T(Str::kXmxOptions_p1), true)) {
        ui::Card xmx_card("##xmx_options_card");
        toggle(adv::kXmxSaveVram, T(Str::kXmxSaveVram_v1a));
        const bool save_wanted = adv::Current(adv::kXmxSaveVram) == L"1";
        if (save_wanted != XmxSaveVramWanted()) ui::StatusChip(T(Str::kRestartTheGameToApply_r1b), ui::Warning);
        else if (save_wanted && g_xmx_handles_created.load(std::memory_order_relaxed) > 0)
          ui::StatusChip(T(Str::kXmxSaveVramActive_v1c), ui::Accent);
#if defined(MFGUNLOCK_NATIVE_XEMFG) && defined(MFGUNLOCK_XEMFG_VULKAN)
        // On by default, Vulkan games only ([DLSSG] XmxVulkan=0 turns it off): a readout, not a switch.
        namespace vk = mfgunlock::xemfg::native::vulkan;
        const bool vk_selected = adv::Current(adv::kRoutes[1]) == L"1";
        ui::Readout("##xmx_vulkan_row", T(Str::kVulkanGames_p6), vk_selected ? T(Str::kAutomatic_p7) : T(Str::kOff_b4aa),
                    T(Str::kExperimentalXmxInVulkan_x3v2));
        if (!vk::VulkanLayerHost()) {
          ui::StatusChip(T(Str::kXmxVulkanNotVulkan_r1k), ui::Muted);
        } else if (vk_selected != vk::XmxProviderWanted()) {
          ui::StatusChip(T(Str::kRestartTheGameToApply_r1b), ui::Warning);
        } else if (vk::XmxProviderWanted()) {
          const std::string state = vk::XmxProviderReport();
          if (state.rfind("created", 0) == 0) ui::StatusChip(T(Str::kXmxVulkanActive_r1l), ui::Accent);
          else if (state.empty() || state.rfind("waiting", 0) == 0) ui::StatusChip(T(Str::kWaitingForTheGame_r1j), ui::Warning);
          else ui::StatusChip(T(Str::kXmxVulkanRefused_r1m), ui::Warning);
        }
        if (vk::VulkanLayerHost() && vk_selected) {
          toggle(adv::kXmxVulkanPacing, T(Str::kXmxVulkanPacing_q1));
          const bool pacing_wanted = adv::Current(adv::kXmxVulkanPacing) == L"1";
          if (vk::XmxProviderWanted() && pacing_wanted != vk::XmxPacingEnabled()) {
            ui::StatusChip(T(Str::kRestartTheGameToApply_r1b), ui::Warning);
          } else if (vk::XmxPacingEnabled() && vk::XmxProviderReport().rfind("created", 0) == 0) {
            const uint32_t limit_us = mfgunlock::reflexpacing::g_xmx_vulkan_limit_us.load(std::memory_order_relaxed);
            if (limit_us != 0) {
              char text[256]{};
              std::snprintf(text, sizeof(text), T(Str::kXmxVulkanPacingLimit_q3), int(1000000.0 / limit_us + 0.5));
              ui::StatusChip(text, ui::Accent);
            } else {
              ui::StatusChip(T(Str::kXmxVulkanPacingIdle_q4), ui::Accent);
            }
          }
        }
#endif
      }

      // Games without frame generation (1.10.0): one switch for both routes, DLSS-G forced
      // through the game's own Streamline (force_fg.hpp) or Streamline started by the addon
      // for games with DLSS SR only (sl_host.hpp). The status says which one applies.
      const bool add_wanted = adv::Current(adv::kRoutes[3]) == L"1" || adv::Current(adv::kRoutes[0]) == L"1" ||
                              adv::Current(adv::kRoutes[2]) == L"1";
      const bool force_running = mfgunlock::framecount::g_force_fg_enabled.load(std::memory_order_relaxed);
      // The Vulkan counterpart of sl_host.hpp (xemfg/sl_vulkan.hpp): its own state when it runs.
      int vk_host = 0;
#if defined(MFGUNLOCK_NATIVE_XEMFG) && defined(MFGUNLOCK_XEMFG_VULKAN)
      vk_host = mfgunlock::xemfg::native::vulkan::StreamlineHostState();
      if (vk_host == static_cast<int>(mfgunlock::slhost::State::kGameStreamline)) vk_host = 0;
#endif
      const bool add_running = force_running || mfgunlock::slhost::Wanted() || vk_host != 0;
      if (ui::SectionHeader(T(Str::kGamesWithoutFg_p2), add_wanted || add_running, T(Str::kExperimentalTag_p3))) {
        ui::Card add_card("##add_fg_card");
        toggle(adv::kRoutes[3], T(Str::kAddFgToggle_p8));
#if defined(MFGUNLOCK_NATIVE_XEMFG)
        // 1.10.1 (plan item 8): the MFGUnlock-Streamline files, installed by the external helper.
        if (adv::Current(adv::kRoutes[3]) == L"1") {
          static bool inspected = false;
          if (!inspected) {
            inspected = true;
            mfgunlock::dependencies::InspectStreamline();
          }
          const auto files = mfgunlock::dependencies::StreamlineSnapshot();
          ui::Readout("##streamline_files", T(Str::kStreamlineFiles_s1), "MFGUnlock-Streamline", T(Str::kStreamlineFilesDesc_s2));
          if (files.checked && files.missing == 0 && files.different == 0 && !files.helperRunning) {
            ui::StatusChip(T(Str::kStreamlineFilesOk_s3), ui::Accent);
          } else if (files.checked) {
            if (files.missing != 0 || files.different != 0) {
              char text[256];
              std::snprintf(text, sizeof(text), T(Str::kStreamlineFilesMissing_s4), files.missing, files.different);
              ui::StatusChip(text, ui::Warning);
            }
            ui::ButtonRow actions(1);
            if (actions(T(Str::kInstallStreamlineFiles_s5), T(Str::kInstallStreamlineFilesTip_s6)))
              mfgunlock::dependencies::RepairStreamline();
          }
          if (!files.message.empty()) ui::Description(files.message.c_str());
        }
#endif
        using H = mfgunlock::slhost::State;
        const H host = vk_host != 0 ? static_cast<H>(vk_host) : mfgunlock::slhost::Current();
        if (add_wanted != add_running) {
          ui::StatusChip(T(Str::kRestartTheGameToApply_r1b), ui::Warning);
        } else if (add_running && (vk_host != 0 || mfgunlock::slhost::Wanted()) && host != H::kGameStreamline && host != H::kOff) {
          switch (host) {
            case H::kGenerating: ui::StatusChip(T(Str::kHostActive_h1c), ui::Accent); break;
            case H::kSwapChain: ui::StatusChip(T(Str::kHostWaitingDlss_h1d), ui::Warning); break;
            case H::kMissingFiles: ui::StatusChip(T(Str::kHostMissingFiles_h1f), ui::Danger); break;
            case H::kFailed: ui::StatusChip(T(Str::kHostFailed_h1g), ui::Danger); break;
            default: ui::StatusChip(T(Str::kHostWaitingSwapchain_h1h), ui::Warning); break;
          }
          ui::Readout("##add_fg_route", T(Str::kRoute_p9), T(Str::kRouteHostedShort_p10), T(Str::kHostStreamlineDesc_h1b));
          // 1.10.1: DLSS-G does not generate on an FP16 (scRGB) HDR swap chain (KCD2's HDR).
          if (vk_host == 0 && mfgunlock::slhost::Fp16Hdr()) ui::StatusChip(T(Str::kHostFp16Hdr_s7), ui::Danger);
          // The HUD fix: the image before the HUD, for NVIDIA's generator and for XMX (D3D12 only for now).
          if (vk_host == 0) {
          toggle(adv::kHudFix, T(Str::kHudFix_p15));
          const bool fix_wanted = adv::Current(adv::kHudFix) == L"1";
          if (fix_wanted != mfgunlock::hudcapture::g_registered.load()) ui::StatusChip(T(Str::kRestartTheGameToApply_r1b), ui::Warning);
          else if (fix_wanted && mfgunlock::slhost::HudCaptures() > 0) ui::StatusChip(T(Str::kHudFixFound_p19), ui::Accent);
          else if (fix_wanted) ui::StatusChip(T(Str::kHudFixWaiting_p20), ui::Warning);
          // XMX composes the real frame's HUD over its frames; this shows what counts as HUD.
          if (xmx_selected && fix_wanted) {
            ImGui::PushID("hud_debug");
            bool debug = mfgunlock::xmx::generator::HudDebugWanted();
            ui::Setting("##toggle_row", T(Str::kHudDebug_p17), T(Str::kHudDebugDesc_p18), [&] {
              if (ui::Toggle("##toggle", debug)) {
                mfgunlock::xmx::generator::g_hud_debug.store(debug ? 1 : 0, std::memory_order_relaxed);
                adv::Store(adv::kXmxHudDebug, debug ? L"1" : L"0");
              }
            });
            ImGui::PopID();
          }
          }
        } else if (force_running) {
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        GetSystemTimeAsFileTime(&now);
        const auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const uint64_t uptime_ms = ticks(now) > ticks(created) ? (ticks(now) - ticks(created)) / 10000 : 0;
        const auto status = mfgunlock::forcefg::PanelStatus(
            true, GetModuleHandleW(L"sl.interposer.dll") != nullptr,
            mfgunlock::framecount::internal::g_init_seen.load(std::memory_order_acquire), uptime_ms,
            static_cast<mfgunlock::forcefg::Decision>(mfgunlock::framecount::g_force_fg_decision.load(std::memory_order_acquire)),
            static_cast<mfgunlock::forcefg::Apply>(mfgunlock::framecount::g_force_fg_apply.load(std::memory_order_acquire)));
        using S = mfgunlock::forcefg::Status;
        switch (status) {
          case S::kActive: ui::StatusChip(T(Str::kForcedFgActive_r1i), mfgunlock::ui::Accent); break;
          case S::kNoStreamline: ui::StatusChip(T(Str::kNoStreamlineUseXemfg_r1c), ui::Warning); break;
          case S::kInitMissed: {
            ui::StatusChip(T(Str::kSlinitNotSeenEarly_r1d), ui::Warning);
            static int early_state = 0;  // 0 not tried, 1 saved, 2 failed
            mfgunlock::ui::ButtonRow early(1);
            if (early(T(Str::kEnableEarlyAddonLoading_a017), T(Str::kAddsThisAddonToReshade_09f4)))
              early_state = EnsureEarlyLoadEntry() ? 1 : 2;
            if (early_state == 1) ui::StatusChip(T(Str::kSavedRestartTheGame_a1f6));
            else if (early_state == 2) ui::StatusChip(T(Str::kReshadeIniCouldNotBe_03d9));
            break;
          }
          case S::kGameHasFrameGeneration: ui::StatusChip(T(Str::kGameHasOwnFrameGeneration_r1e)); break;
          case S::kAllFeatures: ui::StatusChip(T(Str::kGameLoadsEveryFeature_r1f)); break;
          case S::kNoSuperResolution: ui::StatusChip(T(Str::kNoSuperResolution_r1g), ui::Warning); break;
          case S::kNoReflex: ui::StatusChip(T(Str::kNoReflex_r1h), ui::Warning); break;
          case S::kWaiting: ui::StatusChip(T(Str::kWaitingForTheGame_r1j)); break;
          default: break;
        }
          ui::Readout("##add_fg_route", T(Str::kRoute_p9), T(Str::kRouteGameShort_p11), T(Str::kForStreamlineGamesWith_f4g2));
        }
      }
      adv::Result();
    }

    // The add-on itself: rarely changed, applies on the next launch.
    if (ui::SectionHeader(T(Str::kAddonSection_p4), false)) {
      ui::Card addon_card("##addon_card");
      ui::Setting("##mfg_enabled_row", T(Str::kMfgUnlockEnabled_p13), T(Str::kChangesTheMasterAddonState_fcdf), [&] {
        if (ui::Toggle("##mfg_enabled", configured_enabled)) {
          g_configured_enabled.store(configured_enabled, std::memory_order_relaxed);
          reshade::set_config_value(nullptr, kConfigSection, "Enabled", configured_enabled ? 1 : 0);
        }
      });
      ui::StatusChip(T(Str::kAppliedOnTheNextGame_e392), ui::Muted);
    }
  }

  if (page == UiPage::General) {
    mfgunlock::ui::SystemCard(!sleep_status_available ? nullptr : driver_vsync_active && gsync_active ? T(Str::kDriverVsyncGSync_284a) : driver_vsync_active ? T(Str::kDriverVsync_77be) : gsync_active ? "G-SYNC" : T(Str::kOff_b4aa), xmx_selected);
    DrawFrameGenChainCard();
    DrawSystemCheckCard();
  }
