#pragma once
// autofps.1: Dynamic frame generation's presented-rate target, as the frame generation that runs it reports it, for
// DLSS 5 Auto (dlss5_auto.h DynamicBudgetMs): DLSS-G Dynamic (patcher.cpp, on each DLSS-G state sample) and XeMFG
// standalone's Dynamic MFG (xess/d3d12_host.cpp, xess/vulkan/vk_host.cpp, each frame). A report older than kFreshMs
// no longer counts (the game stopped asking, generation went off). Header only, lock-free, one per module.
#include "xess/xemfg_window.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace frame_gen_target
{
enum class Source : uint32_t { eDlssg = 0, eXemfg = 1 };
inline constexpr uint64_t kFreshMs = 2000;

struct Target
{
    float outputFps = 0.0f;   // presented frames a second Dynamic aims at; 0 none
    uint32_t minFrames = 0;   // presented frames per real frame at least (1: generation may go off)
    uint32_t maxFrames = 0;   // and at most (generated + 1)
    bool Active() const noexcept { return outputFps > 0.0f && maxFrames >= 1; }
};

namespace detail
{
// Per source: the target in milli-FPS (0: the display's refresh rate, DLSS-G's own default) in bits 0-31, the most
// frames in bits 32-39, the fewest in 40-47, bit 63 while Dynamic runs; and when it was reported (GetTickCount64).
inline std::atomic<uint64_t> gReport[2]{};
inline std::atomic<uint64_t> gTick[2]{};
inline std::atomic<uint32_t> gRefreshHz{0};
inline std::atomic<uint64_t> gRefreshTick{0};

// The refresh rate of the display this process's window in front is on, looked up at most every 2 s (the last one
// known meanwhile, and while another process's window is in front).
inline uint32_t RefreshHz(uint64_t now) noexcept
{
    const uint64_t last = gRefreshTick.load(std::memory_order_relaxed);
    if (last && now >= last && now - last < 2000) return gRefreshHz.load(std::memory_order_relaxed);
    gRefreshTick.store(now, std::memory_order_relaxed);
    const HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (window) GetWindowThreadProcessId(window, &process);
    if (window && process == GetCurrentProcessId())
        if (const uint32_t hz = xemfg_standalone::WindowRefreshHz(window)) gRefreshHz.store(hz, std::memory_order_relaxed);
    return gRefreshHz.load(std::memory_order_relaxed);
}
}

// Dynamic frame generation's state from `source`: running (`active`) toward `targetFps` (0: the display's refresh
// rate) with `minFrames` to `maxFrames` presented per real frame.
inline void Publish(Source source, bool active, float targetFps, uint32_t minFrames, uint32_t maxFrames) noexcept
{
    const size_t index = static_cast<size_t>(source) & 1;
    const double fps = std::isfinite(targetFps) && targetFps > 0.0f ? (std::min)(double(targetFps), 4.0e6) : 0.0;
    const uint64_t milli = static_cast<uint64_t>(std::llround(fps * 1000.0)) & 0xFFFFFFFFull;
    maxFrames = (std::min<uint32_t>)(maxFrames, 255);
    minFrames = std::clamp<uint32_t>(minFrames, 1, (std::max<uint32_t>)(maxFrames, 1));
    const uint64_t report = active && maxFrames
        ? (1ull << 63) | (uint64_t(minFrames) << 40) | (uint64_t(maxFrames) << 32) | milli : 0;
    detail::gReport[index].store(report, std::memory_order_relaxed);
    detail::gTick[index].store(GetTickCount64(), std::memory_order_release);
}

// The target DLSS 5 Auto follows: the latest running report of the last kFreshMs, a 0 target taken as just under
// the refresh rate (116 at 120 Hz, as xemfg_dynamic.h DynamicTargetFps). None while no Dynamic runs, or while a 0
// target's refresh rate is unknown.
inline Target Current() noexcept
{
    const uint64_t now = GetTickCount64();
    uint64_t best = 0, bestTick = 0;
    for (size_t index = 0; index < 2; ++index)
    {
        const uint64_t tick = detail::gTick[index].load(std::memory_order_acquire);
        const uint64_t report = detail::gReport[index].load(std::memory_order_relaxed);
        if (!(report >> 63) || !tick || (now >= tick && now - tick > kFreshMs) || tick < bestTick) continue;
        best = report;
        bestTick = tick;
    }
    Target target{};
    if (!best) return target;
    double fps = double(best & 0xFFFFFFFFull) / 1000.0;
    if (fps <= 0.0)
    {
        const double refresh = double(detail::RefreshHz(now));
        fps = refresh > 0.0 ? std::floor(refresh - refresh * refresh / 3600.0) : 0.0;
    }
    if (fps <= 0.0) return target;
    target.outputFps = static_cast<float>(fps);
    target.maxFrames = uint32_t((best >> 32) & 0xFF);
    target.minFrames = uint32_t((best >> 40) & 0xFF);
    return target;
}
}
