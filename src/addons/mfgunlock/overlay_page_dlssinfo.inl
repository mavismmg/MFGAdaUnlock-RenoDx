// SPDX-License-Identifier: MIT
// 1.10.0 (plan 4.3): the DlssInfo page of the MFG Unlock panel. Included inside
// OnRegisterOverlay (addon_overlay.inl) after its locals; not a standalone file.
#ifndef MFGUNLOCK_ADDON_FRAGMENTS
#error "overlay page files are included only by addon_overlay.inl"
#endif

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
    ImGui::TextDisabled(T(Str::kDlssRuntimeInformation_eb65));
    ImGui::Separator();
    if (ImGui::BeginTable("##dlss_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn(T(Str::kFeature_02dd), ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn(T(Str::kActiveMode_ef2a), ImGuiTableColumnFlags_WidthStretch,
                              0.58f);
      StatusRow(T(Str::kSuperResolution_3fe6), super_resolution_preset.c_str(),
                super_resolution_active
                    ? kUiPositive
                    : (super_resolution_configured ? kUiWarning : kUiMuted));
      StatusRow(T(Str::kFrameGeneration_0acc), frame_generation_preset.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow(T(Str::kRayReconstruction_5b85), ray_reconstruction_preset.c_str(),
                ray_reconstruction_active
                    ? kUiPositive
                    : (ray_reconstruction_configured ? kUiWarning : kUiMuted));
      StatusRow(T(Str::kDlssGRuntime_e183), dlssg_text.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow(T(Str::kStreamlineRuntime_4bab), streamline_text.c_str(),
                streamline_seen ? kUiPositive : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        T(Str::kReadOnlyInformationReportedBy_f045));
    HelpMarker(
        T(Str::kNvidiaExposesOneSharedPreset_928e));

    ImGui::Spacing();
    ImGui::TextDisabled(T(Str::kVramResourceHealth_9614));
    ImGui::Separator();
    if (ImGui::BeginTable("##vram_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn(T(Str::kMetric_1321), ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn(T(Str::kObservation_118d),
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
        StatusRow(T(Str::kProcessLocalVram_2d89), usage_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        StatusRow(T(Str::kBudgetHeadroom_2669), headroom_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        const std::string reservation_text = FormatMemoryBytes(
            g_vram_local_available.load(std::memory_order_relaxed));
        StatusRow(T(Str::kAvailableForReservation_74c9), reservation_text.c_str(),
                  kUiMuted);
        const uint64_t shared_usage =
            g_vram_nonlocal_usage.load(std::memory_order_relaxed);
        if (shared_usage != 0) {
          const std::string shared_text =
              FormatMemoryBytes(shared_usage) + " / " +
              FormatMemoryBytes(
                  g_vram_nonlocal_budget.load(std::memory_order_relaxed));
          StatusRow(T(Str::kProcessSharedGpuMemory_039c), shared_text.c_str(),
                    kUiMuted);
        }
      } else {
        StatusRow(
            T(Str::kProcessLocalVram_2d89),
            g_render_api.load(std::memory_order_relaxed) ==
                    DetectedRenderApi::kVulkan
                ? T(Str::kDxgiBudgetUnavailableOnVulkan_87a0)
                : T(Str::kWaitingForDxgiBudget_836e),
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
          estimate_text = T(Str::kWaitingForNextDlssG_c147);
          break;
        case mfgunlock::framecount::VramEstimateStatus::kUnsupported:
          estimate_text = T(Str::kUnavailableOnThisRuntimeGame_31a1);
          break;
        case mfgunlock::framecount::VramEstimateStatus::kFailed:
          estimate_text = T(Str::kProviderDidNotReturnAn_0e8d);
          estimate_color = kUiWarning;
          break;
        default:
          estimate_text = VramReadinessText(
              static_cast<mfgunlock::memorypolicy::EstimateReadiness>(
                  mfgunlock::framecount::g_vram_estimate_readiness.load(
                      std::memory_order_relaxed)));
          break;
      }
      StatusRow(T(Str::kDlssGEstimatedVram_42cd), estimate_text.c_str(),
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
            (delta_bytes == 0 ? T(Str::k0Mib_18db) : FormatMemoryBytes(delta_bytes)) +
            " with UI composition";
        StatusRow(T(Str::kMeasuredUiCompositionDelta_072c), delta_text.c_str(),
                  kUiMuted);
      }

      const unsigned int volatile_inputs =
          mfgunlock::framecount::g_vram_volatile_input_count.load(
              std::memory_order_relaxed);
      const std::string lifecycle_text = volatile_inputs == 0
          ? T(Str::kOptimizedRelevantInputsValidUntil_edf3)
          : std::to_string(volatile_inputs) +
                " input(s) may require transient copies";
      StatusRow(T(Str::kInputLifetimes_656b), lifecycle_text.c_str(),
                volatile_inputs == 0 ? kUiPositive : kUiWarning);
      const unsigned int flags =
          mfgunlock::framecount::g_vram_estimate_flags.load(
              std::memory_order_relaxed);
      const bool retained_while_off =
          (flags & static_cast<unsigned int>(
                       sl::DLSSGFlags::eRetainResourcesWhenOff)) != 0;
      StatusRow(T(Str::kResourcesWhileFgIsOff_9b7f),
                retained_while_off ? T(Str::kRetainedByGameRequest_7afd)
                                   : T(Str::kNotRetainedByGameRequest_3b1a),
                retained_while_off ? kUiWarning : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        T(Str::kReadOnlyDiagnosticsProcessUsage_e98a));
    HelpMarker(
        T(Str::kThisPageTemporarilyCapturesResource_7047));
  }
