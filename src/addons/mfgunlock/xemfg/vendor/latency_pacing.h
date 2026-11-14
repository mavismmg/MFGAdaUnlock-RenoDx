#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>

// latency.1: low-latency pacing for XeMFG on D3D12 (OptiScaler's XeFG and XeMFG standalone). XeFG presents each batch's
// real frame on the game's thread, and that Present (with the real frame's Pace when the CPU paces) holds the thread
// until the display has room, often far longer than the game's own XeLL sleep. The game reads its input for the next
// frame only after that, so the whole hold is latency. The controller moves the part of the hold that every recent frame
// had to a sleep before the game's next frame (its XeLL / Reflex sleep, sdk_pacing SleepBeforeInput): the frame then
// reaches the same display slot, held for less, with input read later. A margin of the hold stays, so the queue still
// sees a held game (sdk_pacing kQueueBlockedNs); the game's own variation widens it.
// pacerfix.2: where the flip queue publishes its schedule (XeMFG standalone's D3D12 host) the caller bounds each sleep by
// it (Take capNs) and sets a smaller margin (SetMargin): the queue then takes the game's measured wait as evidence.
// optispace.1: a game whose Present wait alternates between short and long frames (the native swap chain's back-pressure
// landing on every other batch, e.g. under VKD3D at 3x+) gives a p10 of the short frames only, so nothing sleeps. In
// paired mode (SetPaired) the window's slack and intervals are the means of consecutive frame pairs: the part of the
// wait every pair had, which the sleep then spreads over both frames.
// No Windows calls here: sdk_pacing.cpp feeds it and sleeps (tests/xemfg_standalone latency_pacing_tests.cpp).
namespace latency_pacing
{
// 0 auto (on wherever sdk_pacing paces XeFG), 1 on, 2 off: the control key latencyPacing.
constexpr uint32_t kAuto = 0, kOn = 1, kOff = 2;
constexpr bool Enabled(uint32_t mode) noexcept { return mode != kOff; }

// The hold every frame keeps, at least: twice sdk_pacing's 1 ms "blocked" test.
inline constexpr int64_t kMarginNs = 2'000'000;
// Recent real frames the target looks at, and how many it needs.
inline constexpr uint32_t kWindow = 32;
inline constexpr uint32_t kMinimumFrames = 16;
// The sleep grows by at most this much per frame and drops at once.
inline constexpr int64_t kRiseNs = 500'000;
// Never more than this, nor more than kMaximumShare of the real frame interval.
inline constexpr int64_t kMaximumSleepNs = 50'000'000;
inline constexpr double kMaximumShare = 0.6;
// A real frame this long after the previous one starts over: a loading screen or frame generation switched off and on.
inline constexpr int64_t kStaleNs = 250'000'000;
// pacerfix.1: a real frame more than kHitchRatio median intervals after the previous one (but within kStaleNs) is a hitch:
// it is left out of the windows and the sleep carries on (a restart costs kMinimumFrames frames without a sleep);
// kHitchRun of them in a row start over (a lasting change).
inline constexpr int64_t kHitchRatio = 3;
inline constexpr uint32_t kHitchRun = 3;
// The sleep must not cost frames: while it runs, a median real frame interval this much longer than when it started
// (a game that turned CPU-bound, or a sleep point outside the presenting loop) drops it for kWindow frames.
inline constexpr double kThroughputGuard = 1.10;
// The sleep must shorten the hold: a window after it started, the mean hold must be down by at least half the mean
// sleep, or the sleep point is outside the presenting loop (the hold stays, the sleep only delays another thread); it
// then stops for kIneffectiveWindows windows.
inline constexpr uint32_t kIneffectiveWindows = 4;

// optispace.1: a Present's wait where the thread's CPU time is unknown (QueryThreadCycleTime is a stub returning FALSE
// under Wine): its span above the shortest of the thread's last kSpanWindow spans, the Present's own work, which no frame
// skips. -1 until kSpanMinimum spans are in.
inline constexpr uint32_t kSpanWindow = 32, kSpanMinimum = 8;
struct SpanFloor
{
    std::array<int64_t, kSpanWindow> spans{};
    uint32_t count{}, next{};
    int64_t Add(int64_t span) noexcept
    {
        if (span < 0) span = 0;
        spans[next] = span;
        next = (next + 1) % kSpanWindow;
        if (count < kSpanWindow) ++count;
        if (count < kSpanMinimum) return -1;
        const int64_t shortest = *std::min_element(spans.begin(), spans.begin() + count);
        return span - shortest;
    }
};

struct Stats
{
    uint64_t frames{};    // real frames measured
    uint64_t takes{};     // sleeps handed out (one per real frame at most)
    uint64_t missed{};    // real frames whose sleep no sleep point took
    uint64_t resets{};
    uint64_t skipped{};   // hitches left out of the windows (pacerfix.1)
    uint64_t backoffs{};  // throughput or effect guard trips
    uint64_t ineffective{}; // of those, the sleep did not shorten the hold
    uint64_t capped{};    // sleeps shortened by the caller's schedule bound (pacerfix.2, Take capNs)
    int64_t sleepNs{};    // the sleep the next frame gets
    int64_t lastFrameNs{}; // the last real frame's Present return
    int64_t heldSumNs{}, slackSumNs{}, sleptSumNs{}; // over `frames`: held in Present, held + slept, slept
};

class Controller
{
public:
    void Reset() noexcept
    {
        count_ = next_ = 0;
        intervalCount_ = intervalNext_ = 0;
        sleepNs_ = takenNs_ = lastNs_ = baseIntervalNs_ = baseHeldNs_ = 0;
        cooldown_ = started_ = hitchRun_ = 0;
        armed_ = false;
        ++stats_.resets;
        stats_.sleepNs = 0;
    }

    // A real frame's Present returned at nowNs, the game's thread held heldNs in it (and its Pace).
    void RealFrame(int64_t nowNs, int64_t heldNs) noexcept
    {
        if (heldNs < 0) heldNs = 0;
        if (armed_) ++stats_.missed;
        const int64_t interval = lastNs_ ? nowNs - lastNs_ : 0;
        const bool hitch = lastNs_ && interval > 0 && interval <= kStaleNs && intervalCount_ >= kMinimumFrames
            && interval > kHitchRatio * MedianInterval();
        if (hitch && hitchRun_ + 1 < kHitchRun)
        {
            // pacerfix.1: a hitch: counted, left out of the windows; the sleep stays armed for the next frame.
            ++hitchRun_;
            ++stats_.skipped;
            ++stats_.frames;
            stats_.heldSumNs += heldNs;
            stats_.slackSumNs += heldNs + takenNs_;
            stats_.sleptSumNs += takenNs_;
            takenNs_ = 0;
            lastNs_ = nowNs;
            stats_.lastFrameNs = nowNs;
            armed_ = true;
            return;
        }
        if (lastNs_ && (interval <= 0 || interval > kStaleNs || hitch))
            Reset();
        else if (interval > 0)
        {
            hitchRun_ = 0;
            intervals_[intervalNext_] = interval;
            intervalNext_ = (intervalNext_ + 1) % kWindow;
            if (intervalCount_ < kWindow) ++intervalCount_;
        }
        lastNs_ = nowNs;
        stats_.lastFrameNs = nowNs;
        // What the frame would have been held without the sleep before its input.
        const int64_t slack = heldNs + takenNs_;
        ++stats_.frames;
        stats_.heldSumNs += heldNs;
        stats_.slackSumNs += slack;
        stats_.sleptSumNs += takenNs_;
        slack_[next_] = slack;
        held_[next_] = heldNs;
        taken_[next_] = takenNs_;
        takenNs_ = 0;
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
        const int64_t sleep = Next();
        if (sleep > 0 && sleepNs_ <= 0)
        {
            // The interval and the hold the sleep is judged against.
            baseIntervalNs_ = MedianInterval();
            baseHeldNs_ = Mean(held_);
            started_ = 0;
        }
        if (sleep > 0) ++started_;
        sleepNs_ = sleep;
        stats_.sleepNs = sleepNs_;
        armed_ = true;
    }

    // At the game's next sleep point: the sleep before its input, once per real frame (0 when none is due). Not
    // shortened by the time since the Present: an engine that presents on its render thread (Unreal) reaches the sleep
    // on its game thread at any point of the frame, and the sleep there still delays the next frame's Present by as
    // much.
    // pacerfix.2: capNs >= 0 bounds it (the flip queue's schedule: the game's next frame must still reach its batch's
    // first slot); a bound below the sleep counts as capped, and only the sleep taken counts for the next frame's slack.
    int64_t Take(int64_t nowNs, int64_t capNs = -1) noexcept
    {
        if (!armed_ || !lastNs_ || nowNs - lastNs_ > kStaleNs) return 0;
        armed_ = false;
        if (sleepNs_ <= 0) return 0;
        int64_t sleep = sleepNs_;
        if (capNs >= 0 && capNs < sleep)
        {
            sleep = capNs;
            ++stats_.capped;
        }
        if (sleep <= 0) return 0;
        ++stats_.takes;
        takenNs_ = sleep;
        return sleep;
    }

    // pacerfix.2: the hold every frame keeps at least (kMarginNs unless the caller sets another; not reset).
    void SetMargin(int64_t marginNs) noexcept { marginNs_ = marginNs >= 0 ? marginNs : kMarginNs; }
    int64_t Margin() const noexcept { return marginNs_; }

    // optispace.1: frame pairs (see the namespace comment); not reset.
    void SetPaired(bool paired) noexcept { paired_ = paired; }
    bool Paired() const noexcept { return paired_; }

    const Stats& Read() const noexcept { return stats_; }

    // optispace.1: the means of consecutive pairs of a ring's last `count` values (written next at `next`), oldest
    // first; returns how many (count - 1, or 0).
    static uint32_t PairMeans(const std::array<int64_t, kWindow>& values, uint32_t count, uint32_t next,
        std::array<int64_t, kWindow>& out) noexcept
    {
        if (count < 2 || count > kWindow) return 0;
        const uint32_t oldest = (next + kWindow - count) % kWindow;
        for (uint32_t k = 1; k < count; ++k)
            out[k - 1] = (values[(oldest + k - 1) % kWindow] + values[(oldest + k) % kWindow]) / 2;
        return count - 1;
    }

private:
    int64_t MedianInterval() const noexcept
    {
        std::array<int64_t, kWindow> sorted{};
        uint32_t n = 0;
        if (paired_) n = PairMeans(intervals_, intervalCount_, intervalNext_, sorted);
        if (!n)
        {
            n = intervalCount_;
            std::copy_n(intervals_.begin(), n, sorted.begin());
        }
        if (!n) return 0;
        std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.begin() + n);
        return sorted[n / 2];
    }

    int64_t Mean(const std::array<int64_t, kWindow>& values) const noexcept
    {
        int64_t sum = 0;
        for (uint32_t i = 0; i < count_; ++i) sum += values[i];
        return count_ ? sum / int64_t(count_) : 0;
    }

    int64_t Next() noexcept
    {
        if (cooldown_) { --cooldown_; return 0; }
        if (count_ < kMinimumFrames || intervalCount_ < kMinimumFrames) return 0;
        if (sleepNs_ > 0 && baseIntervalNs_ > 0 && double(MedianInterval()) > double(baseIntervalNs_) * kThroughputGuard)
        {
            ++stats_.backoffs;
            cooldown_ = kWindow;
            return 0;
        }
        if (sleepNs_ >= 2 * kMarginNs && started_ >= kWindow && Mean(held_) > baseHeldNs_ - Mean(taken_) / 2)
        {
            ++stats_.backoffs;
            ++stats_.ineffective;
            cooldown_ = kIneffectiveWindows * kWindow;
            return 0;
        }
        std::array<int64_t, kWindow> sorted{};
        uint32_t n = 0;
        if (paired_) n = PairMeans(slack_, count_, next_, sorted); // optispace.1: per frame pair
        if (!n)
        {
            n = count_;
            std::copy_n(slack_.begin(), n, sorted.begin());
        }
        std::sort(sorted.begin(), sorted.begin() + n);
        const int64_t low = sorted[n / 10];       // p10: the hold nearly every frame (or pair) had
        const int64_t median = sorted[n / 2];
        // The game's own variation keeps half its spread as extra margin.
        const int64_t margin = std::max(marginNs_, (median - low) / 2);
        int64_t target = std::max<int64_t>(low - margin, 0);
        target = std::min({target, kMaximumSleepNs, int64_t(double(MedianInterval()) * kMaximumShare)});
        return target < sleepNs_ ? target : std::min(target, sleepNs_ + kRiseNs);
    }

    std::array<int64_t, kWindow> slack_{}, held_{}, taken_{}, intervals_{};
    uint32_t count_{}, next_{}, intervalCount_{}, intervalNext_{};
    int64_t sleepNs_{}, takenNs_{}, lastNs_{}, baseIntervalNs_{}, baseHeldNs_{};
    int64_t marginNs_{kMarginNs};
    uint32_t cooldown_{}, started_{}, hitchRun_{};
    bool armed_{}, paired_{};
    Stats stats_{};
};
}
