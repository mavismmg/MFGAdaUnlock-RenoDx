// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the Support page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

  if (page == UiPage::Support && mfgunlock::ui::Expander("All settings (advanced)")) {
    mfgunlock::ui::Description("Applies after a game restart. Change these only to test or to diagnose a problem.");
    mfgunlock::ui::advanced::Draw(mfgunlock::ui::advanced::kDlssG);
    mfgunlock::ui::advanced::Result();
    bool raise_ceiling=g_raise_ceiling.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Raise frame ceiling",&raise_ceiling)) {
      g_raise_ceiling.store(raise_ceiling,std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "RaiseFrameCeiling", raise_ceiling ? 1 : 0);
    }
    mfgunlock::ui::Tooltip("Lets an old plugin's frame limit be raised. Off by default.");
    bool rtx50=g_rtx50_quality_fixes.load(std::memory_order_relaxed);
    ImGui::BeginDisabled(xmx_selected);
    if (ImGui::Checkbox("RTX 50 quality fixes",&rtx50)) {
      g_rtx50_quality_fixes.store(rtx50,std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "Rtx50QualityFixes", rtx50 ? 1 : 0);
    }
    ImGui::EndDisabled();
    mfgunlock::ui::Tooltip("Applies the validated quality fixes on RTX 50 cards too. Off by default.");
  }

  if (page == UiPage::Support) {
    // 1.9.0: always available here, not only on a problem card.
    mfgunlock::ui::SupportActions(xmx_support_report);
    // 1.10.0: Streamline's own log (streamline_log.hpp). The setting itself is
    // in "All settings (advanced)"; this says what happened at this start.
    {
      const auto state=mfgunlock::streamlinelog::Current();
      const bool requested=mfgunlock::streamlinelog::Requested();
      if(requested || state!=mfgunlock::streamlinelog::State::kOff) {
        const std::string text="Streamline log: "+mfgunlock::streamlinelog::Describe(state)+
            (state==mfgunlock::streamlinelog::State::kOff?" (applies from the next start)":"")+".";
        mfgunlock::ui::Notice(text.c_str(),state==mfgunlock::streamlinelog::State::kFailed?mfgunlock::ui::Danger:mfgunlock::ui::Muted);
        mfgunlock::ui::Description("If the chain card says Streamline started before the addon could hook it, the game read the environment too early. "
            "Then set, in the registry under HKCU\\SOFTWARE\\NVIDIA Corporation\\Global\\Streamline, LogLevel (DWORD) = 2 and LogPath (string) = this game's folder, and restart. "
            "Save support files copies sl.log from the game folder.");
      }
    }
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
