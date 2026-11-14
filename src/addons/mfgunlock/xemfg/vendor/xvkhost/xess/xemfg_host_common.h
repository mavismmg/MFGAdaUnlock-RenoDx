#pragma once
// What XeMFG's two standalone hosts share (d3d12_host.cpp, vulkan/vk_host.cpp): the log and clock helpers and the
// menu's Dynamic MFG settings. No graphics API types: each host passes its own settings and log names.
#include "xess_mod.h"
#include "xemfg_dynamic.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

namespace xemfg_host
{
inline std::string Escape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

inline std::string Narrow(const std::wstring& text)
{
    std::string out;
    for (wchar_t c : text) out += c < 0x80 ? char(c) : '?';
    return out;
}

inline void Log(const char* stage, const std::string& fields) noexcept
{
    xess_mod::WriteLog(stage, fields);
}

inline int64_t Now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline double Milliseconds(int64_t ticks) noexcept
{
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    return double(ticks) * 1000.0 / frequency;
}

// The menu's Dynamic MFG settings into the host's `settings` (under `mutex`), logged under `stage` when they change.
inline void ConfigureDynamic(std::mutex& mutex, xemfg_standalone::DynamicSettings& settings, const char* stage, bool enabled,
                             uint32_t targetFps, int rounding, bool allow1x, bool targetAsCap) noexcept
{
    try {
        std::lock_guard lock(mutex);
        auto next = settings;
        next.enabled = enabled;
        next.targetFps = std::min<uint32_t>(targetFps, 1000);
        next.rounding = std::clamp(rounding, 0, 2);
        next.allow1x = allow1x;
        next.targetAsCap = targetAsCap;
        if (next == settings) return;
        settings = next;
        Log(stage, std::string("\"enabled\":") + (enabled ? "true" : "false") + ",\"target\":" +
            std::to_string(next.targetFps) + ",\"rounding\":" + std::to_string(next.rounding) + ",\"allow_1x\":" +
            (allow1x ? "true" : "false") + ",\"target_as_cap\":" + (targetAsCap ? "true" : "false"));
    } catch (...) {}
}
} // namespace xemfg_host
