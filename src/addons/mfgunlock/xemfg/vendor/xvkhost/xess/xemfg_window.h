#pragma once
// What Dynamic MFG asks of the game's window (xemfg_dynamic.h, both standalone hosts), as our OptiScaler fork's
// XeFG_DynamicMFG does: whether it is in front, and its display's refresh rate.
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <thread>

namespace xemfg_standalone
{
// The game's window (or another of its process) in front and not minimised 1, in the background 0, no foreground
// window right now -1 (activation changing hands). A game can present to one top-level window while another of its own
// has the focus (its input window, a launcher it hosts): any window of the process in front counts.
inline int WindowFocus(HWND window) noexcept
{
    // The takeover harness's window runs in the background while its runner works (MFG_HARNESS_XEMFG_FOCUS=1, set
    // only by run-takeover-*.ps1's dynamic mode): it counts as in front.
    static const bool harness = [] {
        wchar_t value[4]{};
        return GetEnvironmentVariableW(L"MFG_HARNESS_XEMFG_FOCUS", value, 4) == 1 && value[0] == L'1';
    }();
    if (harness) return 1;
    if (!window || !IsWindow(window)) return 1;
    if (IsIconic(GetAncestor(window, GA_ROOT))) return 0;
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return -1;
    if (GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT)) return 1;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

// A monitor's refresh rate in Hz, straight from the driver (EnumDisplaySettingsW: slow, tens of ms on some PCs).
inline uint32_t QueryMonitorRefreshHz(HMONITOR monitor) noexcept
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;   // 0 and 1 mean the hardware default
}

// MFG Unlock 1.10.0: the refresh rate of `monitor`, cached. The callers run on the game's Present thread every 2 s, and
// EnumDisplaySettingsW there stalled DOOM: The Dark Ages every 2 s with XMX in Vulkan. Only the first lookup (or a new
// monitor) waits for the driver; a known monitor is looked up again in the background every 10 s.
inline uint32_t CachedMonitorRefreshHz(HMONITOR monitor) noexcept
{
    static std::atomic<HMONITOR> known{nullptr};
    static std::atomic<uint32_t> hz{0};
    static std::atomic<uint64_t> checked{0};
    static std::atomic<bool> busy{false};
    if (!monitor) return 0;
    const uint64_t now = GetTickCount64();
    if (known.load(std::memory_order_acquire) != monitor) {
        const uint32_t value = QueryMonitorRefreshHz(monitor);
        hz.store(value, std::memory_order_relaxed);
        checked.store(now, std::memory_order_relaxed);
        known.store(monitor, std::memory_order_release);
        return value;
    }
    if (now - checked.load(std::memory_order_relaxed) >= 10000 && !busy.exchange(true, std::memory_order_acq_rel)) {
        checked.store(now, std::memory_order_relaxed);
        try {
            std::thread([monitor] {
                const uint32_t value = QueryMonitorRefreshHz(monitor);
                if (known.load(std::memory_order_acquire) == monitor) hz.store(value, std::memory_order_relaxed);
                busy.store(false, std::memory_order_release);
            }).detach();
        } catch (...) {
            busy.store(false, std::memory_order_release);
        }
    }
    return hz.load(std::memory_order_relaxed);
}

// The refresh rate of the display the window is on, 0 unknown. The window can move: callers look again every 2 s.
inline uint32_t WindowRefreshHz(HWND window) noexcept
{
    return CachedMonitorRefreshHz(window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr);
}
}
