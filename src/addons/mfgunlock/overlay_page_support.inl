// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the Support page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

  if (page == UiPage::Support && mfgunlock::ui::Expander(T(Str::kAllSettingsAdvanced_cf72))) {
    mfgunlock::ui::Description(T(Str::kAppliesAfterAGameRestart_e6f3));
    mfgunlock::ui::advanced::Draw(mfgunlock::ui::advanced::kDlssG);
    mfgunlock::ui::advanced::Result();
    bool raise_ceiling=g_raise_ceiling.load(std::memory_order_relaxed);
    if (ImGui::Checkbox(T(Str::kRaiseFrameCeiling_a1b5),&raise_ceiling)) {
      g_raise_ceiling.store(raise_ceiling,std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "RaiseFrameCeiling", raise_ceiling ? 1 : 0);
    }
    mfgunlock::ui::Tooltip(T(Str::kLetsAnOldPluginS_5686));
    bool rtx50=g_rtx50_quality_fixes.load(std::memory_order_relaxed);
    ImGui::BeginDisabled(xmx_selected);
    if (ImGui::Checkbox(T(Str::kRtx50QualityFixes_2204),&rtx50)) {
      g_rtx50_quality_fixes.store(rtx50,std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "Rtx50QualityFixes", rtx50 ? 1 : 0);
    }
    ImGui::EndDisabled();
    mfgunlock::ui::Tooltip(T(Str::kAppliesTheValidatedQualityFixes_7f62));
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
            (state==mfgunlock::streamlinelog::State::kOff?T(Str::kAppliesFromTheNextStart_9da2):"")+".";
        mfgunlock::ui::Notice(text.c_str(),state==mfgunlock::streamlinelog::State::kFailed?mfgunlock::ui::Danger:mfgunlock::ui::Muted);
        mfgunlock::ui::Description(T(Str::kIfTheChainCardSays_e34d));
      }
    }
    ImGui::Spacing();
    ImGui::TextDisabled(T(Str::kSupportDiagnostics_c7dc));
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
    ImGui::TextDisabled(T(Str::kDiagnosticsUpdatingRetryNextFrame_e020));
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
      StatusRow(T(Str::kRenderer_391e), RenderApiName(render_api), kUiMuted);
      StatusRow(T(Str::kStreamline_1ce7), streamline_text.c_str(), kUiMuted);
      StatusRow("DLSS-G", dlssg_text.c_str(), kUiMuted);
      StatusRow(T(Str::kDlssSuperResolution_aa9a), super_resolution_preset.c_str(),
                kUiMuted);
      StatusRow(T(Str::kDlssFrameGeneration_d8a8), frame_generation_preset.c_str(),
                kUiMuted);
      StatusRow(T(Str::kDlssRayReconstruction_7071),
                ray_reconstruction_preset.c_str(), kUiMuted);
      StatusRow("HDR", hdr_seen ? (hdr_active ? T(Str::kYes_3630) : T(Str::kNo_be1a)) : T(Str::kUnknown_0e59),
                kUiMuted);
      const std::string diagnostic_sync =
          !sleep_status_available
              ? T(Str::kNotReported_055d)
              : (driver_vsync_active && gsync_active
                     ? T(Str::kDriverVsyncGSyncActive_1e98)
                     : (driver_vsync_active
                            ? T(Str::kDriverVsyncActive_d54c)
                            : (gsync_active ? T(Str::kGSyncActive_351c)
                                            : T(Str::kInactive_ca78))));
      StatusRow(T(Str::kDriverSync_0c02), diagnostic_sync.c_str(), kUiMuted);
      StatusRow(T(Str::kDynamicMfgVsyncApi_1b73),
                !mfgunlock::framecount::g_vsync_support_seen.load(
                     std::memory_order_acquire)
                    ? T(Str::kNotReported_055d)
                    : (mfgunlock::framecount::g_vsync_supported.load(
                           std::memory_order_relaxed)
                           ? T(Str::kSupportedByActiveRuntime_dba0)
                           : T(Str::kNotSupportedByActiveRuntime_0801)),
                kUiMuted);
      ImGui::EndTable();
    }
    TextDisabledWrapped(
        T(Str::kThisReportsTheStreamlineDlss_5348));

    ImGui::Spacing();
    ImGui::TextDisabled("MFG");
    if (ImGui::BeginTable("##mfg_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      const std::string game_request_text =
          game_request_seen ? std::to_string(game_multiplier) + "x"
                            : T(Str::kNotObserved_db92);
      const std::string effective_text =
          effective_seen ? std::to_string(effective_multiplier) + "x"
                         : T(Str::kNotObserved_db92);
      StatusRow(T(Str::kGameRequest_3cf2), game_request_text.c_str(), kUiMuted);
      StatusRow(T(Str::kEffectiveRequest_2b9b), effective_text.c_str(), kUiMuted);
      const std::string actual_text =
          state_seen
              ? std::to_string(mfgunlock::framecount::g_actual_frames_presented.load(
                    std::memory_order_relaxed))
              : T(Str::kNoSample_2a1c);
      const std::string live_multiplier =
          driver_multiplier >= 2
              ? std::to_string(driver_multiplier) + "x live"
              : T(Str::kNotReported_055d);
      StatusRow(T(Str::kDriverLiveMultiplier_65b6), live_multiplier.c_str(),
                driver_multiplier >= 2 ? kUiPositive : kUiMuted);
      StatusRow(T(Str::kProviderPresentations_eddd), actual_text.c_str(),
                mfg_confirmed ? kUiPositive : kUiMuted);
      StatusRow(T(Str::kValidationEvidence_9bbf), validation_text.c_str(),
                runtime_error
                    ? kUiError
                    : (request_rejected
                           ? kUiWarning
                           : (mfg_request_accepted ? kUiPositive : kUiMuted)));
      StatusRow(T(Str::kDynamicMfg_4fba),
                mfgunlock::framecount::g_dynamic_applied.load(
                    std::memory_order_relaxed)
                    ? T(Str::kActive_134f)
                    : (dynamic_available ? T(Str::kAvailable_2638) : T(Str::kInactive_ca78)),
                kUiMuted);
      const auto source_cap_state =
          mfgunlock::framecount::internal::ResolveUserSourceCapState();
      const std::string source_cap_value =
          std::to_string(source_cap_state.configured_fps) +
          " final/output FPS; " +
          UserSourceCapStatusText(source_cap_state.status);
      StatusRow(T(Str::kOutputFpsCap_dfcf), source_cap_value.c_str(),
                UserSourceCapColor(source_cap_state.status));
      std::ostringstream source_cap_intervals;
      source_cap_intervals << "requested "
                           << source_cap_state.requested_limit_us
                           << " us; native "
                           << source_cap_state.native_limit_us
                           << " us; effective "
                           << source_cap_state.effective_limit_us << " us";
      StatusRow(T(Str::kReflexCapIntervals_2215), source_cap_intervals.str().c_str(),
                kUiMuted);
      StatusRow(
          T(Str::kCapConfigurationSource_3732),
          SourceCapConfigOriginText(
              static_cast<mfgunlock::pacing::SourceCapConfigOrigin>(
                  g_source_cap_config_origin.load(
                      std::memory_order_relaxed))),
          kUiMuted);
      ImGui::EndTable();
    }
    TextDisabledWrapped(
        T(Str::kEvidenceIsLayeredAnAccepted_8cc1));
    HelpMarker(
        T(Str::kReshadeSFpsCounterAnd_8b19));

    ImGui::Spacing();
    ImGui::TextDisabled("PATCHES");
    if (ImGui::BeginTable("##patch_diagnostics", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      StatusRow(T(Str::kArchitectureGates_b93e), gate_patched ? T(Str::kActive_134f) : T(Str::kPending_afdc),
                gate_patched ? kUiPositive : kUiWarning);
      StatusRow(T(Str::kTemporalFix_af0c),
                blackwell_patched
                    ? T(Str::kBlackwellFrameworkActive_a7c6)
                    : (midpoint_patched ? T(Str::kMidpointFallbackActive_6cf5)
                                        : T(Str::kPending_afdc)),
                blackwell_patched || midpoint_patched ? kUiPositive
                                                      : kUiWarning);
      StatusRow(T(Str::kThinGeometryPatches_01c7),
                thin_geometry_patched ? T(Str::kAtLeastOneActive_32f5) : T(Str::kPending_afdc),
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
                 ? T(Str::kCoreComponentsApplied_bc5a)
                 : T(Str::kPartialFallback_b1ce));
        StatusRow(T(Str::kAdaptiveQualitySuite_ddc2),
                  profile_status.c_str(),
                  adaptive_complete && all_requested_versions
                      ? kUiPositive
                      : kUiWarning);
        const std::string warp_status = AdaptiveComponentStatus(
            expected_warp, warp_version, adaptive_blend);
        StatusRow(T(Str::kWarpConfidence_f9e9), warp_status.c_str(),
                  AdaptiveComponentColor(expected_warp, warp_version,
                                         adaptive_blend));
        const std::string geometry_status = AdaptiveComponentStatus(
            expected_geometry, geometry_version,
            blackwell_active_result.adaptive_geometry);
        StatusRow(T(Str::kGeometryConfidence_8c57), geometry_status.c_str(),
                  AdaptiveComponentColor(
                      expected_geometry, geometry_version,
                      blackwell_active_result.adaptive_geometry));
        if (geometry_version ==
            mfgunlock::adaptivequality::ComponentVersion::kV3) {
          StatusRow(
              T(Str::kGeometryV3Install_f8e7),
              blackwell_active_result.adaptive_geometry_redirected
                  ? T(Str::kFullPtxasCubinExactFatbin_f0d1)
                  : T(Str::kInPlaceCubin_dd15),
              blackwell_active_result.adaptive_geometry_redirected
                  ? kUiPositive
                  : kUiMuted);
          StatusRow(T(Str::kGeometryV32Cubin_0f21),
                    mfgunlock::blackwell::AdaptiveGeometryVariantName(
                        blackwell_active_result.adaptive_geometry_variant),
                    blackwell_active_result.adaptive_geometry_variant ==
                            mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal
                        ? kUiPositive
                        : kUiMuted);
          StatusRow(T(Str::kGeometryV32Ptxas_7bb8),
                    T(Str::k40Registers0SpillsLocal_9780),
                    kUiPositive);
        }
        const std::string inpaint_status = AdaptiveComponentStatus(
            expected_inpaint, inpaint_version,
            blackwell_active_result.adaptive_inpaint_decision);
        StatusRow(T(Str::kInpaintDecision_acac), inpaint_status.c_str(),
                  AdaptiveComponentColor(
                      expected_inpaint, inpaint_version,
                      blackwell_active_result.adaptive_inpaint_decision));
        if (requested_profile ==
            mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3) {
          const char* requested_inpaint =
              requested_inpaint_mode ==
                      mfgunlock::cudatemporal::InpaintMode::kTemporal
                  ? T(Str::kTemporalV3_ae9a)
                  : requested_inpaint_mode ==
                            mfgunlock::cudatemporal::InpaintMode::kLocal
                        ? T(Str::kLocalV3_9cfb)
                        : T(Str::kV2Compatibility_5ad9);
          StatusRow(T(Str::kInpaintV34Requested_16df), requested_inpaint, kUiMuted);
          StatusRow(
              T(Str::kInpaintV34Cubin_0d5a),
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
              T(Str::kInpaintV34Effective_f956), inpaint_temporal_detail.c_str(),
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
              T(Str::kV3PhotometricAB_a8a0),
              g_adaptive_quality_v3_photometric.load(
                      std::memory_order_relaxed)
                  ? T(Str::kRelativeLumaChromaEnabled_e1e0)
                  : T(Str::kDisabledUsingV2AbsoluteRgb_d400),
              kUiMuted);
          StatusRow(
              T(Str::kV3BorderAB_be92),
              g_adaptive_quality_v3_directional_border.load(
                      std::memory_order_relaxed)
                  ? T(Str::kMotionDirectionalTaperEnabled_8511)
                  : T(Str::kDisabledUsingV2SymmetricTaper_63f7),
              kUiMuted);
          StatusRow(
              T(Str::kV3GeometryAB_b2c8),
              g_adaptive_quality_v3_oriented_geometry.load(
                      std::memory_order_relaxed)
                  ? T(Str::kMotionOrientedDiagonalsEnabled_675f)
                  : T(Str::kDisabledRequestingGeometryV2_a701),
              kUiMuted);
        }
        StatusRow(T(Str::kDirectionalScatter_3101),
                  blackwell_active_result.adaptive_directional_scatter
                      ? T(Str::kNativeMotionAdaptiveCoverageRetained_830b)
                      : T(Str::kBaseline_9a62),
                  kUiMuted);
        const auto stability_mode = mfgunlock::cudatemporal::NormalizeMode(
            g_adaptive_quality_v3_stability_mode.load(
                std::memory_order_relaxed));
        StatusRow(
            T(Str::kV32StabilityRequested_3d5d),
            stability_mode ==
                    mfgunlock::cudatemporal::StabilityMode::kTemporal
                ? T(Str::kTemporalStable_c082)
                : T(Str::kLocalStable_282b),
            kUiMuted);
        const std::string temporal_detail =
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
            "History disabled; dedicated Local cubin requested (see installed variant above)";
#else
            mfgunlock::cudatemporal::Detail();
#endif
        StatusRow(
            T(Str::kV32StabilityEffective_0eec), temporal_detail.c_str(),
#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
            kUiMuted);
#if defined(MFGUNLOCK_LOCAL_STABILITY)
#if defined(MFGUNLOCK_RELEASE_1_4_5)
        StatusRow(T(Str::kBuild_7543), T(Str::kRelease145Engineering_3ab6), kUiWarning);
#elif defined(MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
        StatusRow(T(Str::kBuild_7543), T(Str::kExperimental144Engineering_2668), kUiWarning);
#elif defined(MFGUNLOCK_RELEASE_1_4_4)
        StatusRow(T(Str::kBuild_7543), T(Str::kRelease144Dlss_4d21), kUiPositive);
#elif defined(MFGUNLOCK_RELEASE_1_4_3)
        StatusRow(T(Str::kBuild_7543), T(Str::kRelease143Continuous_defb), kUiPositive);
#elif defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
        StatusRow(T(Str::kBuild_7543), T(Str::k142BaseContinuous_5eca), kUiMuted);
#else
        StatusRow(T(Str::kBuild_7543), T(Str::kRelease142Continuous_52aa), kUiPositive);
#endif
#else
        StatusRow(T(Str::kBuild_7543), T(Str::kLocalLowOverheadNoV3_d0ee), kUiPositive);
#endif
        StatusRow(T(Str::kTemporalBackend_0243), T(Str::kDisabledByBuild0B_77f3), kUiMuted);
        StatusRow(T(Str::kCudaHookFastPath_b6e4), T(Str::kNotInstalledNoCudaNvapi_a795), kUiMuted);
#else
            mfgunlock::cudatemporal::g_temporal_active.load(
                    std::memory_order_acquire)
                ? kUiPositive
                : (mfgunlock::cudatemporal::g_fallback.load(
                           std::memory_order_acquire)
                       ? kUiWarning
                       : kUiMuted));
        StatusRow(
            T(Str::kCudaHookFastPath_b6e4),
            mfgunlock::cudatemporal::FastPathReady()
                ? T(Str::kActiveAtomicCufunctionCompareOnly_81ae)
                : (mfgunlock::cudatemporal::g_hooked.load(
                           std::memory_order_acquire)
                       ? T(Str::kHookInstalledTargetAssociationPending_b312)
                       : T(Str::kInactive_ca78)),
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
          StatusRow(T(Str::kV32ConfidenceHistory_baf6),
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
          StatusRow(T(Str::kV34InpaintHistory_95bd),
                    inpaint_history.str().c_str(), kUiMuted);
        }
      }
      if (!g_adaptive_quality.load(std::memory_order_relaxed)) {
        StatusRow(T(Str::kGeometryConfidenceV2_ddd1),
                  !g_geometry_confidence_v2.load(std::memory_order_relaxed)
                      ? T(Str::kOff_b4aa)
                      : (!g_quality_refinement.load(std::memory_order_relaxed)
                             ? T(Str::kRequiresQualityRefinement_3cad)
                             : (g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                                        static_cast<unsigned int>(mfgunlock::blackwell::SilhouetteGuardMode::Balanced)
                                    ? T(Str::kRequiresBalancedBoundaryMode_75a6)
                                    : (thin_geometry_last.geometry_confidence_v2_applied
                                           ? T(Str::kPatchAppliedVisualTestPending_48da)
                                           : T(Str::kRequestedV1FallbackOrNo_28fa)))),
                  kUiMuted);
        StatusRow(T(Str::kBorderConfidenceTrial_9239),
                  !g_border_confidence.load(std::memory_order_relaxed)
                      ? T(Str::kOff_b4aa)
                      : (!g_quality_refinement.load(std::memory_order_relaxed)
                             ? T(Str::kRequiresQualityRefinement_3cad)
                             : (!thin_geometry_last.result.validated_warp_blend.applied
                                    ? T(Str::kRequestedBlendNotApplied_8323)
                                    : T(Str::kPatchAppliedVisualTestPending_48da))),
                  kUiMuted);
      }
      if (!g_adaptive_quality.load(std::memory_order_relaxed) &&
          thin_geometry_provider_count != 0) {
        const auto mechanism_state = [](const auto& result) {
          return !result.requested ? T(Str::kNotRequested_77b2)
                                   : (result.applied ? T(Str::kActive_134f) : T(Str::kNotApplied_8bd7));
        };
        const bool intermediate_superseded =
            g_thin_geometry_intermediate_scatter.load(
                std::memory_order_relaxed) &&
            (g_adaptive_quality.load(std::memory_order_relaxed) ||
             g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
                static_cast<unsigned int>(
                    mfgunlock::blackwell::SilhouetteGuardMode::Off));
        StatusRow(T(Str::kIntermediateRetention_d23e),
                  intermediate_superseded
                      ? T(Str::kSupersededByBoundaryMitigation_383f)
                      : mechanism_state(
                            thin_geometry_last.intermediate_scatter),
                  thin_geometry_last.intermediate_scatter.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow(T(Str::kBoundaryMitigation_c312),
                  mechanism_state(
                      thin_geometry_last.silhouette_boundary_guard),
                  thin_geometry_last.silhouette_boundary_guard.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow(T(Str::kValidatedWarpBlend_ea4a),
                  mechanism_state(
                      thin_geometry_last.result.validated_warp_blend),
                  thin_geometry_last.result.validated_warp_blend.applied
                      ? kUiPositive
                      : kUiMuted);
        StatusRow(T(Str::kPreviousScatter_c6f6),
                  mechanism_state(thin_geometry_last.result.previous_scatter),
                  thin_geometry_last.result.previous_scatter.applied
                      ? kUiPositive
                      : kUiMuted);
      }
      StatusRow(T(Str::kInputQualityGuard_57cf),
                mfgunlock::framecount::g_hud_inputs_suppressed.load(
                    std::memory_order_relaxed)
                    ? T(Str::kFilteredIncompatibleInput_edb7)
                    : T(Str::kNoFilteringReported_d5cb),
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
          (viewport_capacity_exhausted ? T(Str::kCapacityExhausted_00b1) : T(Str::kTracked_bdcf));
      StatusRow(T(Str::kQualityGuardViewports_348c), viewport_text.c_str(),
                viewport_capacity_exhausted ? kUiWarning : kUiMuted);
      const auto tag_lock_contentions =
          mfgunlock::framecount::g_quality_tag_lock_contentions.load(
              std::memory_order_relaxed);
      const std::string contention_text =
          std::to_string(tag_lock_contentions) +
          " non-blocking fallback(s)";
      StatusRow(T(Str::kQualityGuardContention_cb88), contention_text.c_str(),
                tag_lock_contentions == 0 ? kUiMuted : kUiWarning);
      ImGui::EndTable();
    }

    std::string intermediate_report = T(Str::kDisabled_9925);
    if (g_adaptive_quality.load(std::memory_order_relaxed)) {
      intermediate_report = T(Str::kManagedByAdaptiveQualitySuite_31fe);
    } else if (g_thin_geometry_intermediate_scatter.load(
            std::memory_order_relaxed)) {
      if (g_silhouette_guard_mode.load(std::memory_order_relaxed) !=
          static_cast<unsigned int>(
              mfgunlock::blackwell::SilhouetteGuardMode::Off)) {
        intermediate_report = T(Str::kSupersededByBoundaryMitigation_383f);
      } else if (thin_geometry_last.intermediate_scatter.applied) {
        intermediate_report = T(Str::kActive_134f);
      } else {
        intermediate_report = T(Str::kRequestedNotApplied_0b94);
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
           << "HDR: " << (hdr_seen ? (hdr_active ? T(Str::kYes_3630) : T(Str::kNo_be1a)) : T(Str::kUnknown_0e59))
           << '\n'
           << "Game request: "
           << (game_request_seen ? std::to_string(game_multiplier) + "x"
                                 : T(Str::kNotObserved_db92))
           << '\n'
           << "Effective request: "
           << (effective_seen ? std::to_string(effective_multiplier) + "x"
                              : T(Str::kNotObserved_db92))
           << '\n'
           << "Driver live multiplier: "
           << (driver_multiplier >= 2
                   ? std::to_string(driver_multiplier) + "x"
                   : T(Str::kNotReported_055d))
           << '\n'
           << "Provider presentations: "
           << (state_seen ? std::to_string(observed_presentations)
                          : T(Str::kNotSampled_9fb8))
           << '\n'
           << "Dynamic MFG: "
           << (mfgunlock::framecount::g_dynamic_applied.load(
                   std::memory_order_relaxed)
                   ? T(Str::kActive_134f)
                   : (dynamic_available ? T(Str::kAvailable_2638) : "Unavailable/Pending"))
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
                   : T(Str::kOff_b4aa)) << '\n'
           << "V3 photometric A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_photometric.load(
                          std::memory_order_relaxed)
                          ? T(Str::kRelativeLumaChroma_b639)
                          : T(Str::kDisabledV2AbsoluteRgb_39c0))
                   : T(Str::kInactiveOutsideV3_eece)) << '\n'
           << "V3 border A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_directional_border.load(
                          std::memory_order_relaxed)
                          ? "motion-directional"
                          : T(Str::kDisabledV2Symmetric_8fe5))
                   : T(Str::kInactiveOutsideV3_eece)) << '\n'
           << "V3 geometry A/B: "
           << (g_adaptive_quality.load(std::memory_order_relaxed) &&
                       adaptive_profile == mfgunlock::adaptivequality::Profile::
                                               kLuminanceDirectionalV3
                   ? (g_adaptive_quality_v3_oriented_geometry.load(
                          std::memory_order_relaxed)
                          ? T(Str::kMotionOrientedDiagonals_4e66)
                          : T(Str::kDisabledV2Geometry_cdce))
                   : T(Str::kInactiveOutsideV3_eece)) << '\n'
           << "Adaptive warp confidence: "
           << adaptive_warp_report << '\n'
           << "Adaptive geometry confidence: "
           << adaptive_geometry_report << '\n'
           << "Geometry V3 install: "
           << (blackwell_active_result.adaptive_geometry_redirected
                   ? T(Str::kFullPtxasCubinExactFatbin_d108)
                   : T(Str::kNoOversizedRedirectActive_2429)) << '\n'
           << "Geometry V3.2 cubin: "
           << mfgunlock::blackwell::AdaptiveGeometryVariantName(
                  blackwell_active_result.adaptive_geometry_variant)
           << (blackwell_active_result.adaptive_geometry_variant ==
                       mfgunlock::blackwell::AdaptiveGeometryVariant::kTemporal
                   ? T(Str::k40Registers7776B_478b)
                   : blackwell_active_result.adaptive_geometry_variant ==
                             mfgunlock::blackwell::AdaptiveGeometryVariant::kLocal
                         ? "; 40 registers; 7,776 B shared; zero stack/spill/local; .text="
#if defined(MFGUNLOCK_LOCAL_STABILITY)
                           "39,680 B; paired diagonal consensus; zero global loads/stores"
#else
                           "39,552 B; zero global loads/stores"
#endif
                         : T(Str::kResourceAuditUnavailableForFallback_0e54))
           << '\n'
           << "Adaptive inpaint decision: "
           << adaptive_inpaint_report << '\n'
           << "Inpaint V3.4 requested: "
           << (requested_inpaint_mode ==
                       mfgunlock::cudatemporal::InpaintMode::kTemporal
                   ? T(Str::kTemporalV3_ae9a)
                   : requested_inpaint_mode ==
                             mfgunlock::cudatemporal::InpaintMode::kLocal
                         ? T(Str::kLocalV3_9cfb)
                         : T(Str::kV2Compatibility_5ad9))
           << '\n'
           << "Inpaint V3.4 cubin: "
           << mfgunlock::blackwell::AdaptiveInpaintVariantName(
                  blackwell_active_result.adaptive_inpaint_variant)
           << (blackwell_active_result.adaptive_inpaint_variant ==
                       mfgunlock::blackwell::AdaptiveInpaintVariant::kTemporal
                   ? T(Str::kAbi152Bytes48Registers_e3cf)
                   : blackwell_active_result.adaptive_inpaint_variant ==
                             mfgunlock::blackwell::AdaptiveInpaintVariant::kLocal
                         ? T(Str::k48Registers784BShared_db2a)
                         : T(Str::kCompatibilityFallbackResources_b94c))
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
                   ? T(Str::kProviderNativeSignedCoverageRetained_f0ea)
                   : T(Str::kBaseline_9a62)) << '\n'
           << "V3.2 Stability requested: "
           << (mfgunlock::cudatemporal::NormalizeMode(
                       g_adaptive_quality_v3_stability_mode.load(
                           std::memory_order_relaxed)) ==
                       mfgunlock::cudatemporal::StabilityMode::kTemporal
                   ? T(Str::kTemporalStable_c082)
                   : T(Str::kLocalStable_282b)) << '\n'
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
                   : T(Str::kOff_b4aa)) << '\n'
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
                   ? T(Str::kActiveOneImmutableDispatchTable_3b58)
                   : T(Str::kInactiveTargetAssociationPending_c1cc))
           << '\n'
#endif
           << "Quality refinement requested this session: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? T(Str::kLegacySettingIgnoredManagedBy_d0b8)
                   : (g_quality_refinement.load(std::memory_order_relaxed)
                          ? T(Str::kYesCheckPatchDetails_6743)
                          : T(Str::kNo_be1a))) << '\n'
           << "Geometry confidence V2: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? T(Str::kIntegratedAsymmetricSuccessorManagedBy_6b45)
                   : (g_geometry_confidence_v2.load(std::memory_order_relaxed)
                   ? (thin_geometry_last.geometry_confidence_v2_applied
                          ? T(Str::kPatchAppliedExecutionUnverified_d57a)
                          : T(Str::kRequestedV1FallbackOrNo_28fa))
                   : T(Str::kOff_b4aa))) << '\n'
           << "Geometry patch detail: "
           << thin_geometry_last.silhouette_boundary_guard.detail << '\n'
           << "Border confidence trial: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? T(Str::kIntegratedAndManagedByAdaptive_fc4c)
                   : (g_border_confidence.load(std::memory_order_relaxed)
                   ? (!g_quality_refinement.load(std::memory_order_relaxed)
                          ? T(Str::kRequiresQualityRefinement_3cad)
                          : (thin_geometry_last.result.validated_warp_blend.applied
                                 ? T(Str::kPatchAppliedExecutionUnverified_d57a)
                                 : T(Str::kRequestedBlendNotApplied_8323)))
                   : T(Str::kOff_b4aa))) << '\n'
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
                   ? T(Str::kNotReported_055d)
                   : (latency_view.iflip_active
                          ? T(Str::kActive_134f) : T(Str::kInactive_ca78)))
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
                   ? T(Str::kUnavailableOnVulkan_345d)
                   : (g_latency_guard_dxgi_observed.load(std::memory_order_acquire)
                   ? (g_latency_guard_waitable_swapchain.load(
                              std::memory_order_relaxed)
                          ? "waitable"
                          : T(Str::kNoWaitableObjectObserved_3c0a))
                   : T(Str::kNotObserved_a072)))
           << '\n'
           << "Process local VRAM usage/budget: "
           << (render_api == DetectedRenderApi::kVulkan
                   ? std::string(T(Str::kDxgiBudgetUnavailableOnVulkan_87a0))
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
                   ? T(Str::kCapacityExhausted_b949) : "")
           << '\n'
           << "Quality Guard tag-lock contention fallbacks: "
           << mfgunlock::framecount::g_quality_tag_lock_contentions.load(
                  std::memory_order_relaxed)
           << '\n'
           << "Validation evidence: " << ValidationStageName(validation_stage)
           << " -- " << validation_text << '\n'
           << "Architecture gates: " << (gate_patched ? T(Str::kActive_134f) : T(Str::kPending_afdc))
           << " (providers " << gate_provider_count << ", sites "
           << gate_site_count << ")\n"
           << "Blackwell kernels: " << (blackwell_patched ? T(Str::kActive_134f) : T(Str::kInactive_ca78))
           << "\nBlackwell targeted roles: motion-vector "
           << (blackwell_active_result.motion_vector ? "yes" : "no")
           << ", inpaint "
           << (blackwell_active_result.inpaint ? "yes" : "no")
           << ", decision "
           << (blackwell_active_result.inpaint_decision ? "yes" : "no")
           << "; patched slots " << blackwell_active_result.kernels
           << "\nTemporal fallback: " << (midpoint_patched ? T(Str::kActive_134f) : T(Str::kInactive_ca78))
           << "\nThin-geometry patches: "
           << (thin_geometry_patched ? T(Str::kActive_134f) : "Inactive/Pending") << '\n'
           << "Intermediate retention: " << intermediate_report
           << '\n'
           << "Boundary mitigation: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? T(Str::kBalancedSelectedByAdaptiveQuality_18b3)
                   : mfgunlock::blackwell::SilhouetteGuardName(
                         static_cast<mfgunlock::blackwell::SilhouetteGuardMode>(
                             g_silhouette_guard_mode.load(
                                 std::memory_order_relaxed))))
           << '\n'
           << "Validated warp blend: "
           << ((g_adaptive_quality.load(std::memory_order_relaxed) ||
                g_thin_geometry_validated_warp_blend.load(
                    std::memory_order_relaxed))
                   ? T(Str::kEnabled_e43e)
                   : T(Str::kDisabled_9925))
           << " (" << thin_geometry_last.result.validated_warp_blend.detail
           << ")"
           << '\n'
           << "Previous scatter retention: "
           << (g_adaptive_quality.load(std::memory_order_relaxed)
                   ? T(Str::kDisabledByAdaptiveQualitySuite_725e)
                   : (g_thin_geometry_previous_scatter.load(
                          std::memory_order_relaxed)
                          ? T(Str::kEnabled_e43e)
                          : T(Str::kDisabled_9925)))
           << '\n';
    if (ImGui::Button(T(Str::kCopyDiagnostics_ef00))) {
      ImGui::SetClipboardText(report.str().c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled(T(Str::kNoLocalPathsOrCredentials_be08));


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
      ImGui::TextWrapped(T(Str::kBlackwellS_338e), blackwell_detail.c_str());
    }
    if (!midpoint_detail.empty())
      ImGui::TextWrapped(T(Str::kTemporalS_65dd), midpoint_detail.c_str());
    ImGui::TextDisabled(
        T(Str::kProvidersGatesZuBlackwellZu_81e3),
        gate_provider_count, blackwell_provider_count,
        midpoint_provider_count, thin_geometry_provider_count);
    ImGui::TextDisabled(T(Str::kLoadTriggerSCaughtU_d8bf),
                        mfgunlock::loadhook::g_hooked.load(
                            std::memory_order_acquire)
                            ? "armed"
                            : T(Str::kNotInstalled_200c),
                        mfgunlock::loadhook::g_catches.load(
                            std::memory_order_relaxed));

  }
