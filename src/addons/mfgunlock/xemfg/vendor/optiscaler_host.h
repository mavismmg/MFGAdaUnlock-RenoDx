// SPDX-License-Identifier: MIT
#pragma once
// 1.7 (plan J): the only part of MFG Unlocker's optiscaler_host the pacing
// component uses. This build never runs under OptiScaler, so no OptiScaler
// menu draws on XeFG's native swap chain: its buffer cap is the flip queue's
// own maximum.
// 1.10.0 (dashdogy v1.7.1): the Vulkan host's questions too. Never detected;
// the tensor-core route follows upstream's defaults (on, every layer, no full
// barriers), as the D3D12 CUDA route here has always been on.
#include <windows.h>

#include <cstdint>
#include <string>

namespace optiscaler_host
{
constexpr uint32_t kMenuBackBuffers = 16;
inline uint32_t MenuBackBuffers() noexcept { return kMenuBackBuffers; }
inline bool Detected() noexcept { return false; }
inline bool RunningUnderWine() noexcept
{
    static const bool wine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return wine;
}
inline bool ReadXefgTensorCores(std::wstring& setting) noexcept { setting = L"default"; return true; }
inline uint32_t ReadXefgTensorCoresMask(std::wstring& setting) noexcept { setting = L"default"; return (1u << 17) - 1; }
inline bool ReadXefgTensorCoresVulkanBarriers(std::wstring& setting) noexcept { setting = L"default"; return false; }
}
