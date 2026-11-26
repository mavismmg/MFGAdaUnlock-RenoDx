// SPDX-License-Identifier: MIT
#pragma once
// 1.12.1 (D3): DLSS 5 neural rendering with the XeMFG backend (FusselBob, KCD2, 2026-11-23: the DLSS 5 tab
// was gone with XeMFG). The XeMFG engine's NGX hooks (native_ngx.cpp, "DLSS SR input") tell the add-on's
// DLSS 5 code (addon_dlss5.inl, through nr_runtime.hpp's upscaler table) of each feature and evaluation, so
// NR runs after the game's DLSS SR, before XeFG sees the frame. The native panel shows the DLSS 5 page.
// Each pointer is set by addon.cpp only when DLSS 5 is on at load.
#include <atomic>

struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

namespace mfgunlock::xemfg::native::nrbridge {
inline std::atomic<void (*)(int feature, const NVSDK_NGX_Parameter* params, const void* handle)> g_created{nullptr};
inline std::atomic<void (*)(ID3D12GraphicsCommandList* list, const void* handle, const NVSDK_NGX_Parameter* params)>
    g_evaluated{nullptr};
inline std::atomic<void (*)(const void* handle)> g_released{nullptr};
// The panel's DLSS 5 page (nullptr: no tab).
inline std::atomic<void (*)()> g_page{nullptr};
}  // namespace mfgunlock::xemfg::native::nrbridge
