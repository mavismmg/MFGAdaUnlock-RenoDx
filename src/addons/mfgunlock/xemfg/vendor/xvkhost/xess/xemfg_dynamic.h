#pragma once
// Dynamic MFG for XeMFG standalone, on both hosts (xess/d3d12_host.cpp, xess/vulkan/vk_host.cpp).
// The count follows the real frame rate toward a target, up to the most frames set in the menu, with our OptiScaler
// XeFG output's controllers (xefg_dynamic_count.h, xefg_dynamic_cap.h; the fork's XeFG_DynamicMFG drives them the same
// way there). Each host calls Update once per game frame, before its present, with what the game waited for in the
// host that frame: its XeLL sleep (the game's Reflex sleep, or the host's own) and XeFG's Present. 1X is frame
// generation off: XeFG keeps its count, nothing is rebuilt, and it is back on in microseconds.
//
// The target is the one set, at most the FPS limit (which counts presented frames, as the target does), else the FPS
// limit, else just under the refresh rate (116 at 120 Hz), where a VRR display's frame cap sits. It is not a limit.
// "Use target as cap" holds the presented rate at the target with the host's XeLL limit (CapFps) and picks the fewest
// frames that reach it (no rounding band, no 1X).
//
// Header only and API free: tests/xemfg_standalone checks it on the CPU with the controllers' simulations.
#include "xefg_dynamic_count.h"
#include "xefg_dynamic_cap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace xemfg_standalone
{
// The menu's Dynamic MFG settings (control file xemfgDynamic, xemfgTargetFps, xemfgRounding, xemfgAllow1x,
// xemfgTargetAsCap; xemfgFrames is the most frames) and the FPS limit (reflexFrameLimitFps, applied by XeLL).
struct DynamicSettings
{
    bool enabled = false;
    uint32_t maxFrames = 6;     // presented frames per game frame at most, 2..6
    uint32_t targetFps = 0;     // 0: the FPS limit, else just under the refresh rate
    int rounding = 1;           // 0 at most, 1 nearest, 2 at least (the controller's down / nearest / up)
    bool allow1x = true;        // frame generation may turn off (1X)
    bool targetAsCap = false;
    uint32_t limitFps = 0;      // the menu's FPS limit, 0 none

    bool operator==(const DynamicSettings&) const = default;
};

inline double DynamicTargetFps(const DynamicSettings& s, double refreshFps) noexcept
{
    const double limit = double(s.limitFps);
    if (s.targetFps > 0) return limit > 0.0 ? std::min(double(s.targetFps), limit) : double(s.targetFps);
    if (limit > 0.0) return limit;
    return refreshFps > 0.0 ? std::floor(refreshFps - refreshFps * refreshFps / 3600.0) : 0.0;
}

class DynamicFrames
{
public:
    struct Frame
    {
        double nowMs = 0.0;       // monotonic clock
        double frameMs = 0.0;     // the game's frame interval (present to present)
        double sleepMs = 0.0;     // of it, the game's XeLL sleep (the low latency sleep and the FPS limit)
        double presentMs = 0.0;   // and its last call of XeFG's Present (V-Sync, a full queue)
        double refreshFps = 0.0;  // the window's display, 0 unknown
        int focus = 1;            // the game's window in front 1, in the background 0, no foreground window -1
        uint32_t frames = 2;      // presented frames per game frame now; 1 = generation off
    };

    // The presented frames per game frame for this frame: 1 = generation off (1X), else 2..maxFrames.
    uint32_t Update(const Frame& f, const DynamicSettings& s) noexcept
    {
        changed_ = false;
        reason_ = "";
        const int targetCount = int(std::clamp<uint32_t>(s.maxFrames, 2, 6)) - 1;
        const int current = f.frames <= 1 ? 0 : std::min(int(f.frames) - 1, targetCount);
        const int count1 = current >= 1 ? current : targetCount;   // the count kept while generation is off
        if (!s.enabled) {
            if (active_) Reset();
            return uint32_t(targetCount + 1);
        }
        refresh_ = f.refreshFps;
        target_ = DynamicTargetFps(s, f.refreshFps);
        const bool cap = s.targetAsCap && target_ > 0.0 && targetCount > 1;
        capFps_ = cap ? target_ : 0.0;
        if (!(settings_ == s)) settings_ = s;

        // Alt-Tab: in the background the game is throttled, so the count climbed to the most frames and every step
        // down failed (stuck at 6X in the fork). It stays as it is meanwhile; back in front both controllers start over.
        if (f.focus == 0 || (f.focus < 0 && unfocused_)) {
            if (!unfocused_) { reason_ = "background"; changed_ = true; }
            unfocused_ = true;
            active_ = true;
            return f.frames;
        }
        if (unfocused_) {
            unfocused_ = false;
            count_.Reset();
            cap_.Reset();
            capActive_ = false;
            reason_ = "foreground";
            changed_ = true;
        }

        int count = current;
        if (cap) {
            if (!capActive_) cap_.Reset();
            count = cap_.Update({f.nowMs, f.frameMs, f.sleepMs, target_, targetCount, count1});
            static constexpr const char* kCapReasons[] = {"", "up", "down", "try down", "undo"};
            reason_ = kCapReasons[std::clamp(int(cap_.LastReason()), 0, 4)];
        } else {
            if (capActive_) count_.Restart();
            const int minCount = s.allow1x ? 0 : 1;
            count = count_.Update({f.nowMs, f.frameMs, f.sleepMs + f.presentMs, target_, f.refreshFps, double(s.limitFps),
                                   targetCount, current, std::clamp(s.rounding, 0, 2), minCount});
            static constexpr const char* kReasons[] = {"", "up", "down", "try down", "undo", "no gain, back"};
            reason_ = kReasons[std::clamp(int(count_.LastReason()), 0, 5)];
        }
        capActive_ = cap;
        active_ = true;
        if (count != current) changed_ = true;
        return count <= 0 ? 1u : uint32_t(count + 1);
    }

    // Generation back on (the game's frame generation switch, inputs back): the averages start over, what was
    // learned stays.
    void Restart() noexcept
    {
        count_.Restart();
        cap_.Reset();
        capActive_ = false;
    }
    // Dynamic MFG switched off: everything.
    void Reset() noexcept
    {
        count_.Reset();
        cap_.Reset();
        active_ = capActive_ = unfocused_ = false;
        capFps_ = target_ = 0.0;
    }

    bool Active() const noexcept { return active_; }
    bool Changed() const noexcept { return changed_; }     // a new count or focus state this frame (log it)
    const char* Reason() const noexcept { return reason_; }
    double TargetFps() const noexcept { return target_; }  // the target in use, 0 none
    // "Use target as cap": the presented rate the host's XeLL limit holds, 0 none.
    double CapFps() const noexcept { return capFps_; }
    bool CapActive() const noexcept { return capActive_; }

    // The log's fields after the last Update, for a change and every few seconds.
    std::string Describe(uint32_t from, uint32_t to) const
    {
        char text[512]{};
        if (capActive_)
            snprintf(text, sizeof(text), "\"from\":%u,\"to\":%u,\"reason\":\"%s\",\"mode\":\"cap\",\"presented\":%.1f,"
                "\"target\":%.1f,\"reachable\":%.1f,\"backoff_s\":%.0f", from, to, reason_, cap_.PresentedFps(), target_,
                cap_.ReachableFps(), cap_.BackoffMs() / 1000.0);
        else
            snprintf(text, sizeof(text), "\"from\":%u,\"to\":%u,\"reason\":\"%s\",\"mode\":\"%s\",\"presented\":%.1f,"
                "\"target\":%.1f,\"set\":%u,\"rounding\":%d,\"refresh\":%.0f,\"one_less\":%.1f,\"waits_pct\":%.0f,"
                "\"frame_cost_ms\":%.2f,\"backoff_s\":%.0f", from, to, reason_, count_.Uncapped() ? "no_cap" : "capped",
                count_.PresentedFps(), count_.TargetFps(), settings_.targetFps, settings_.rounding, refresh_,
                count_.LowerFps(), count_.WaitShare() * 100.0, count_.FrameCostMs(), count_.BackoffMs() / 1000.0);
        return text;
    }

private:
    XeFG_DynamicCount count_;
    XeFG_DynamicCap cap_;
    DynamicSettings settings_;
    bool active_ = false, capActive_ = false, unfocused_ = false, changed_ = false;
    double target_ = 0.0, capFps_ = 0.0, refresh_ = 0.0;
    const char* reason_ = "";
};
}
