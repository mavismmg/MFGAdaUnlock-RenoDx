// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the Latency page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

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
