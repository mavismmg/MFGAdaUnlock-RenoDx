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
    ImGui::TextDisabled("DLSS RUNTIME INFORMATION");
    ImGui::Separator();
    if (ImGui::BeginTable("##dlss_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn("Active mode", ImGuiTableColumnFlags_WidthStretch,
                              0.58f);
      StatusRow("Super Resolution", super_resolution_preset.c_str(),
                super_resolution_active
                    ? kUiPositive
                    : (super_resolution_configured ? kUiWarning : kUiMuted));
      StatusRow("Frame Generation", frame_generation_preset.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow("Ray Reconstruction", ray_reconstruction_preset.c_str(),
                ray_reconstruction_active
                    ? kUiPositive
                    : (ray_reconstruction_configured ? kUiWarning : kUiMuted));
      StatusRow("DLSS-G runtime", dlssg_text.c_str(),
                dlssg_seen ? kUiPositive : kUiMuted);
      StatusRow("Streamline runtime", streamline_text.c_str(),
                streamline_seen ? kUiPositive : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Read-only information reported by NVIDIA. Game-controlled modes may not expose a preset name.");
    HelpMarker(
        "NVIDIA exposes one shared preset/mode field for Super Resolution and Ray Reconstruction. The addon displays it only when the corresponding feature reports active or configured feedback, and never changes the selected model or render resolution.");

    ImGui::Spacing();
    ImGui::TextDisabled("VRAM / RESOURCE HEALTH");
    ImGui::Separator();
    if (ImGui::BeginTable("##vram_information", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch,
                              0.42f);
      ImGui::TableSetupColumn("Observation",
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
        StatusRow("Process local VRAM", usage_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        StatusRow("Budget headroom", headroom_text.c_str(),
                  budget != 0 && usage * 100ull >= budget * 90ull
                      ? kUiWarning : kUiMuted);
        const std::string reservation_text = FormatMemoryBytes(
            g_vram_local_available.load(std::memory_order_relaxed));
        StatusRow("Available for reservation", reservation_text.c_str(),
                  kUiMuted);
        const uint64_t shared_usage =
            g_vram_nonlocal_usage.load(std::memory_order_relaxed);
        if (shared_usage != 0) {
          const std::string shared_text =
              FormatMemoryBytes(shared_usage) + " / " +
              FormatMemoryBytes(
                  g_vram_nonlocal_budget.load(std::memory_order_relaxed));
          StatusRow("Process shared GPU memory", shared_text.c_str(),
                    kUiMuted);
        }
      } else {
        StatusRow(
            "Process local VRAM",
            g_render_api.load(std::memory_order_relaxed) ==
                    DetectedRenderApi::kVulkan
                ? "DXGI budget unavailable on Vulkan"
                : "Waiting for DXGI budget",
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
          estimate_text = "Waiting for next DLSS-G state query";
          break;
        case mfgunlock::framecount::VramEstimateStatus::kUnsupported:
          estimate_text = "Unavailable on this runtime/game";
          break;
        case mfgunlock::framecount::VramEstimateStatus::kFailed:
          estimate_text = "Provider did not return an estimate";
          estimate_color = kUiWarning;
          break;
        default:
          estimate_text = VramReadinessText(
              static_cast<mfgunlock::memorypolicy::EstimateReadiness>(
                  mfgunlock::framecount::g_vram_estimate_readiness.load(
                      std::memory_order_relaxed)));
          break;
      }
      StatusRow("DLSS-G estimated VRAM", estimate_text.c_str(),
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
            (delta_bytes == 0 ? "0 MiB" : FormatMemoryBytes(delta_bytes)) +
            " with UI composition";
        StatusRow("Measured UI-composition delta", delta_text.c_str(),
                  kUiMuted);
      }

      const unsigned int volatile_inputs =
          mfgunlock::framecount::g_vram_volatile_input_count.load(
              std::memory_order_relaxed);
      const std::string lifecycle_text = volatile_inputs == 0
          ? "Optimized: relevant inputs valid until Present"
          : std::to_string(volatile_inputs) +
                " input(s) may require transient copies";
      StatusRow("Input lifetimes", lifecycle_text.c_str(),
                volatile_inputs == 0 ? kUiPositive : kUiWarning);
      const unsigned int flags =
          mfgunlock::framecount::g_vram_estimate_flags.load(
              std::memory_order_relaxed);
      const bool retained_while_off =
          (flags & static_cast<unsigned int>(
                       sl::DLSSGFlags::eRetainResourcesWhenOff)) != 0;
      StatusRow("Resources while FG is off",
                retained_while_off ? "Retained by game request"
                                   : "Not retained by game request",
                retained_while_off ? kUiWarning : kUiMuted);
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Read-only diagnostics. Process usage includes the whole game; the provider estimate is approximate.");
    HelpMarker(
        "This page temporarily captures resource dimensions, formats and lifetimes only; it never reads pixels or stores resource addresses. The addon does not reserve VRAM, rewrite resource lifetimes, or disable the game's retain-resources flag. Close this page to stop metadata capture.");
  }
