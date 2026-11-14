// Vendored from dashdogy/MFG-Unlocker tests/xemfg_standalone/latency_pacing_tests.cpp at 128d081 (MIT, see
// src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt); only the include path is ours.
// latency.1: CPU checks and closed-loop simulations of the low-latency pacing controller (source/native/xess/latency_pacing.h).
#include "../src/addons/mfgunlock/xemfg/vendor/latency_pacing.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
using namespace latency_pacing;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr int64_t kMs = 1'000'000;

// A deterministic jitter source: uniform in [-1, 1].
struct Noise
{
    uint32_t state = 0x2545F491u;
    double Next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return double(state >> 8) / double(1u << 23) - 1.0;
    }
};

// One real frame per batch through a display-paced queue (what XeFG's real-frame Present sees):
// - the game reads input after its sleep, works cpuNs (jittered), then calls Present;
// - Present returns once the previous batch has flipped (the queue holds one batch), after presentNs of its own work;
// - the batch flips productionNs after that at the earliest, and a clock period after the previous flip.
// clockFollows: the period follows the hand-over like the flip queue's JitterBufferClock (a block > 1 ms with the queue
// full decays it 1/32 towards capacityNs, otherwise it follows the hand-over 1/8). sleepOutside: the sleep runs on a
// thread outside the presenting loop, so it never delays the Present, but it costs the game's frame loop that time.
struct Simulation
{
    int64_t periodNs = 34 * kMs, capacityNs = 34 * kMs, cpuNs = 8 * kMs, cpuJitterNs = kMs, presentNs = 300'000;
    int64_t productionNs = 20 * kMs, spikeNs = 0;
    uint32_t spikeEvery = 0;
    bool clockFollows = false, sleepOutside = false, pacing = true;
    uint32_t frames = 600, settle = 200;

    struct Result
    {
        double latencyMs{}, intervalMs{}, sleptMs{}, heldMs{}, minHeldMs{1e9};
        uint64_t backoffs{}, resets{}, ineffective{}, skipped{};
    };

    Result Run() const
    {
        Controller controller;
        Noise noise;
        Result result;
        int64_t previousFlip = 100 * kMs, previousReturn = 0, period = periodNs;
        int64_t loopFree = 0; // when the game's loop can start its next frame (sleepOutside)
        double latency = 0, interval = 0, slept = 0, held = 0;
        uint32_t counted = 0;
        for (uint32_t n = 0; n < frames; ++n)
        {
            const int64_t sleep = pacing ? controller.Take(previousReturn) : 0;
            int64_t input = previousReturn + (sleepOutside ? 0 : sleep);
            if (sleepOutside) { input = std::max(input, loopFree); loopFree = input + sleep; }
            int64_t cpu = cpuNs + int64_t(noise.Next() * double(cpuJitterNs));
            if (spikeEvery && n % spikeEvery == spikeEvery - 1) cpu += spikeNs;
            const int64_t call = input + cpu;
            const int64_t returned = std::max(call, previousFlip) + presentNs;
            const int64_t blocked = returned - call;
            if (pacing) controller.RealFrame(returned, blocked);
            if (clockFollows && n)
            {
                const int64_t handover = returned - previousReturn;
                if (blocked > kMs) period = std::max(capacityNs, period - period / 32);
                else period += (handover - period) / 8;
            }
            const int64_t flip = std::max(returned + productionNs, previousFlip + period);
            if (n >= settle)
            {
                latency += double(flip - input);
                interval += double(flip - previousFlip);
                slept += double(sleep);
                held += double(blocked);
                result.minHeldMs = std::min(result.minHeldMs, double(blocked) / double(kMs));
                ++counted;
            }
            previousFlip = flip;
            previousReturn = returned;
        }
        result.latencyMs = latency / double(counted) / double(kMs);
        result.intervalMs = interval / double(counted) / double(kMs);
        result.sleptMs = slept / double(counted) / double(kMs);
        result.heldMs = held / double(counted) / double(kMs);
        result.backoffs = controller.Read().backoffs;
        result.resets = controller.Read().resets;
        result.ineffective = controller.Read().ineffective;
        result.skipped = controller.Read().skipped;
        return result;
    }
};

void Print(const char* name, const Simulation::Result& r)
{
    std::printf("%-12s latency %6.2f ms  interval %6.2f ms  slept %6.2f ms  held %6.2f ms (min %5.2f)  backoffs %llu\n",
        name, r.latencyMs, r.intervalMs, r.sleptMs, r.heldMs, r.minHeldMs, static_cast<unsigned long long>(r.backoffs));
}

void Basics()
{
    Controller c;
    // Nothing before enough frames and intervals (the first frame has none), and nothing to take before a real frame.
    CHECK(c.Take(0) == 0);
    int64_t now = 0;
    for (uint32_t i = 0; i < kMinimumFrames; ++i)
    {
        now += 30 * kMs;
        c.RealFrame(now, 20 * kMs);
        CHECK(c.Take(now) == 0);
    }
    // Then it rises by kRiseNs per frame at most.
    now += 30 * kMs;
    c.RealFrame(now, 20 * kMs);
    const int64_t first = c.Take(now);
    CHECK(first > 0 && first <= kRiseNs);
    // Once per real frame.
    CHECK(c.Take(now) == 0);
    // A frame whose sleep no point took counts as missed.
    now += 30 * kMs;
    c.RealFrame(now, 20 * kMs);
    now += 30 * kMs;
    c.RealFrame(now, 20 * kMs);
    CHECK(c.Read().missed == 1);
    // A take long after the frame is not a sleep point of this loop.
    CHECK(c.Take(now + kStaleNs + 1) == 0);
    // A hitch starts over.
    now += 30 * kMs;
    c.RealFrame(now, 20 * kMs);
    const uint64_t resets = c.Read().resets;
    now += kStaleNs + kMs;
    c.RealFrame(now, 20 * kMs);
    CHECK(c.Read().resets == resets + 1);
    CHECK(c.Take(now) == 0);
    // A hold that never leaves the margin gets no sleep.
    Controller small;
    now = 0;
    for (int i = 0; i < 100; ++i) { now += 30 * kMs; small.RealFrame(now, kMarginNs); small.Take(now); }
    CHECK(small.Read().takes == 0);
    // The sleep never exceeds kMaximumShare of the interval.
    Controller large;
    now = 0;
    int64_t most = 0;
    for (int i = 0; i < 600; ++i) { now += 30 * kMs; large.RealFrame(now, 29 * kMs); most = std::max(most, large.Take(now)); }
    CHECK(most <= int64_t(30.0 * kMaximumShare * double(kMs)) + 1);
}

// 6x at about 29 real fps, the queue holding the game ~25 ms in Present.
void Steady()
{
    Simulation off; off.pacing = false;
    Simulation on;
    const auto a = off.Run(), b = on.Run();
    Print("steady off", a); Print("steady on", b);
    CHECK(b.latencyMs < a.latencyMs - 15.0);              // most of the hold moves before the input
    CHECK(std::fabs(b.intervalMs - a.intervalMs) < 0.05);  // same display cadence
    CHECK(b.minHeldMs > 1.0);                              // every frame still held: the queue sees it blocked
    CHECK(b.backoffs == 0);
    CHECK(b.sleptMs > 15.0);
}

// The game's CPU time varies a lot: a wider margin, latency still lower, the cadence kept (the slowest frames may
// arrive after the previous flip, within the batch's production time), also with the following clock.
void Jitter()
{
    Simulation off; off.pacing = false; off.cpuJitterNs = 6 * kMs;
    Simulation on = off; on.pacing = true;
    const auto a = off.Run(), b = on.Run();
    Print("jitter off", a); Print("jitter on", b);
    CHECK(b.latencyMs < a.latencyMs - 5.0);
    CHECK(std::fabs(b.intervalMs - a.intervalMs) < 0.2);
    Simulation clockOff = off; clockOff.clockFollows = true; clockOff.periodNs = 40 * kMs;
    Simulation clockOn = clockOff; clockOn.pacing = true;
    const auto c = clockOff.Run(), d = clockOn.Run();
    Print("jitclk off", c); Print("jitclk on", d);
    CHECK(d.intervalMs < c.intervalMs * 1.01);
    CHECK(d.latencyMs < c.latencyMs - 5.0);
}

// Occasional CPU spikes (a frame 12 ms slower every 40): the spike frames may miss their slot as they do without
// the sleep, but the cadence of the others stays.
void Spikes()
{
    Simulation off; off.pacing = false; off.spikeEvery = 40; off.spikeNs = 12 * kMs;
    Simulation on = off; on.pacing = true;
    const auto a = off.Run(), b = on.Run();
    Print("spikes off", a); Print("spikes on", b);
    CHECK(b.latencyMs < a.latencyMs - 5.0);
    CHECK(b.intervalMs < a.intervalMs * 1.02);
}

// CPU-bound: nothing holds the game in Present, so nothing sleeps.
void CpuBound()
{
    Simulation on; on.cpuNs = 40 * kMs;
    const auto b = on.Run();
    Print("cpu bound", b);
    CHECK(b.sleptMs < 0.01);
}

// The flip queue's clock follows the hand-over: the sleep must not let it learn a slower cadence (the AMD XeLL
// lock-in), so the period stays at capacity with and without it.
void FollowingClock()
{
    Simulation off; off.pacing = false; off.clockFollows = true; off.periodNs = 40 * kMs; off.capacityNs = 34 * kMs;
    Simulation on = off; on.pacing = true;
    const auto a = off.Run(), b = on.Run();
    Print("clock off", a); Print("clock on", b);
    CHECK(b.intervalMs < a.intervalMs * 1.01);
    CHECK(b.intervalMs < 34.0 * 1.01);
    CHECK(b.latencyMs < a.latencyMs - 15.0);
}

// A sleep point outside the presenting loop never shortens the hold, and a growing sleep would cost frames: the
// effect guard drops it, so it stays small on average and the cadence is kept.
void Outside()
{
    Simulation off; off.pacing = false; off.sleepOutside = true; off.cpuNs = 16 * kMs;
    Simulation on = off; on.pacing = true; on.frames = 1500;
    const auto a = off.Run(), b = on.Run();
    Print("outside off", a); Print("outside on", b);
    CHECK(b.backoffs > 0);
    CHECK(b.sleptMs < 4.0);
    CHECK(b.intervalMs < a.intervalMs * 1.01);
}

// pacerfix.1: hitches (a frame over kHitchRatio median intervals, under kStaleNs) are left out and the sleep carries on;
// kHitchRun in a row start over.
void Hitches()
{
    // A 100 ms CPU spike every 40 frames in the steady 6x case: no restart, and nearly all the sleep stays.
    Simulation off; off.pacing = false; off.spikeEvery = 40; off.spikeNs = 100 * kMs;
    Simulation on = off; on.pacing = true;
    const auto a = off.Run(), b = on.Run();
    Print("hitches off", a); Print("hitches on", b);
    CHECK(b.resets == 0);
    CHECK(b.skipped >= 10);
    CHECK(b.sleptMs > 12.0);
    CHECK(b.latencyMs < a.latencyMs - 12.0);   // Python mirror: 48.7 vs 68.9 ms
    CHECK(b.intervalMs < a.intervalMs * 1.03); // mirror: 36.0 vs 35.5 ms

    // Directly: two hitches in a row are skipped, a third starts over.
    Controller c;
    int64_t now = 0;
    for (uint32_t i = 0; i < 40; ++i) { now += 30 * kMs; c.RealFrame(now, 20 * kMs); c.Take(now); }
    const int64_t sleeping = c.Read().sleepNs;
    CHECK(sleeping > 0);
    now += 100 * kMs; c.RealFrame(now, 20 * kMs);
    CHECK(c.Read().resets == 0 && c.Read().skipped == 1 && c.Read().sleepNs == sleeping);
    CHECK(c.Take(now) == sleeping);
    now += 30 * kMs; c.RealFrame(now, 20 * kMs);   // an ordinary frame ends the run
    now += 100 * kMs; c.RealFrame(now, 20 * kMs);
    now += 100 * kMs; c.RealFrame(now, 20 * kMs);
    CHECK(c.Read().resets == 0 && c.Read().skipped == 3);
    now += 100 * kMs; c.RealFrame(now, 20 * kMs);  // the third in a row
    CHECK(c.Read().resets == 1 && c.Read().sleepNs == 0);
    // A gap past kStaleNs still starts over at once.
    Controller s;
    now = 0;
    for (uint32_t i = 0; i < 40; ++i) { now += 30 * kMs; s.RealFrame(now, 20 * kMs); s.Take(now); }
    now += kStaleNs + kMs; s.RealFrame(now, 20 * kMs);
    CHECK(s.Read().resets == 1 && s.Read().skipped == 0);
}

// 2x at 60 real fps (16.7 ms batches), the game held ~6 ms.
void HighRate()
{
    Simulation off; off.pacing = false; off.periodNs = 16'700'000; off.capacityNs = off.periodNs; off.cpuNs = 10 * kMs;
    off.productionNs = 8 * kMs;
    Simulation on = off; on.pacing = true;
    const auto a = off.Run(), b = on.Run();
    Print("2x off", a); Print("2x on", b);
    CHECK(b.latencyMs < a.latencyMs - 2.0);
    CHECK(std::fabs(b.intervalMs - a.intervalMs) < 0.05);
    CHECK(b.minHeldMs > 1.0);
}

// pacerfix.2: the caller's schedule bound (Take capNs) and margin (SetMargin). A steady 20 ms hold every 40 ms, the
// sleep moving it before the input (the next hold is 20 ms less what was taken).
void Schedule()
{
    Controller c;
    CHECK(c.Margin() == kMarginNs);
    int64_t now = 1000 * kMs, taken = 0;
    for (uint32_t i = 0; i < 200; ++i) { now += 40 * kMs; c.RealFrame(now, 20 * kMs - taken); taken = c.Take(now); }
    CHECK(taken == 20 * kMs - kMarginNs);   // the hold less the margin
    CHECK(c.Read().capped == 0);
    // A bound below the sleep shortens it (counted); only what was taken counts as slack for the next frame.
    now += 40 * kMs; c.RealFrame(now, 20 * kMs - taken);
    const uint64_t takes = c.Read().takes;
    taken = c.Take(now, 10 * kMs);
    CHECK(taken == 10 * kMs && c.Read().capped == 1 && c.Read().takes == takes + 1);
    now += 40 * kMs; c.RealFrame(now, 20 * kMs - taken);
    CHECK(c.Read().sleepNs == 20 * kMs - kMarginNs);   // the slack was still 20 ms
    // A bound above the sleep, or none (-1), changes nothing.
    taken = c.Take(now, 30 * kMs);
    CHECK(taken == 20 * kMs - kMarginNs && c.Read().capped == 1);
    now += 40 * kMs; c.RealFrame(now, 20 * kMs - taken);
    taken = c.Take(now, -1);
    CHECK(taken == 20 * kMs - kMarginNs);
    // A bound of 0: no sleep this frame, counted as capped, not as a take.
    now += 40 * kMs; c.RealFrame(now, 20 * kMs - taken);
    const uint64_t before = c.Read().takes;
    CHECK(c.Take(now, 0) == 0 && c.Read().capped == 2 && c.Read().takes == before);
    taken = 0;
    // A smaller margin lets the sleep rise further (at most kRiseNs a frame); Reset keeps it; a negative one is the default.
    c.SetMargin(500000);
    for (uint32_t i = 0; i < 40; ++i) { now += 40 * kMs; c.RealFrame(now, 20 * kMs - taken); taken = c.Take(now); }
    CHECK(taken == 20 * kMs - 500000);
    c.Reset();
    CHECK(c.Margin() == 500000);
    c.SetMargin(-1);
    CHECK(c.Margin() == kMarginNs);
}

// optispace.1: a GPU-bound game whose Present waits alternate (OptiScaler's XeFG at 3x+ under VKD3D: 2-3 / 35-40 ms,
// real frames 17 / 52 ms apart at ~29 fps). Model: the GPU takes gpuNs per frame in order, and the game's Present
// returns once the whole previous pair of frames is done, so every other frame waits for the pair before it. The
// controller is fed the game's wait per frame (the host's Present markers). Latency: GPU completion less input.
struct PairSimulation
{
    int64_t gpuNs = 34'500'000, cpuNs = 15 * kMs, cpuJitterNs = kMs, presentNs = 300'000;
    bool pacing = true, paired = true;
    uint32_t frames = 900, settle = 300;

    struct Result
    {
        double latencyMs{}, intervalMs{}, sleptMs{}, heldMs{}, alternationMs{};
        uint64_t backoffs{}, ineffective{};
    };

    Result Run() const
    {
        Controller controller;
        controller.SetPaired(paired);
        Noise noise;
        Result result;
        int64_t previousReturn = 0, previousDone = 0, previousHeld = 0, previousCall = 0;
        int64_t pairEnd = 0; // the previous pair's last GPU completion
        double latency = 0, interval = 0, slept = 0, held = 0, alternation = 0;
        uint32_t counted = 0;
        for (uint32_t n = 0; n < frames; ++n)
        {
            const int64_t sleep = pacing ? controller.Take(previousReturn) : 0;
            const int64_t input = previousReturn + sleep;
            const int64_t call = input + cpuNs + int64_t(noise.Next() * double(cpuJitterNs));
            // The first frame of a pair waits for the whole previous pair; the second finds its slot free.
            const int64_t slot = (n % 2 == 0) ? pairEnd : 0;
            const int64_t returned = std::max(call, slot) + presentNs;
            const int64_t blocked = returned - call;
            const int64_t done = std::max(previousDone, call) + gpuNs;
            if (n % 2 == 1) pairEnd = done;
            if (pacing) controller.RealFrame(returned, blocked);
            if (n >= settle)
            {
                latency += double(done - input);
                interval += double(call - previousCall);
                slept += double(sleep);
                held += double(blocked);
                alternation += std::fabs(double(blocked - previousHeld));
                ++counted;
            }
            previousHeld = blocked;
            previousCall = call;
            previousDone = done;
            previousReturn = returned;
        }
        const double c = double(counted) * double(kMs);
        result.latencyMs = latency / c;
        result.intervalMs = interval / c;
        result.sleptMs = slept / c;
        result.heldMs = held / c;
        result.alternationMs = alternation / c;
        result.backoffs = controller.Read().backoffs;
        result.ineffective = controller.Read().ineffective;
        return result;
    }
};

void PrintPair(const char* name, const PairSimulation::Result& r)
{
    std::printf("%-12s latency %6.2f ms  interval %6.2f ms  slept %6.2f ms  held %6.2f ms  |dheld| %6.2f ms  backoffs %llu\n",
        name, r.latencyMs, r.intervalMs, r.sleptMs, r.heldMs, r.alternationMs, static_cast<unsigned long long>(r.backoffs));
}

void Paired()
{
    // SpanFloor (Wine: no thread CPU time): nothing for the first kSpanMinimum - 1 spans, then the span above the
    // shortest of the last kSpanWindow (the log's alternating 2.5 / 37 ms spans give 0 / 34.5 ms waits).
    SpanFloor spanFloor;
    for (uint32_t i = 0; i + 1 < kSpanMinimum; ++i) CHECK(spanFloor.Add(i % 2 ? 37 * kMs : 2'500'000) == -1);
    CHECK(spanFloor.Add(37 * kMs) == 34'500'000);
    CHECK(spanFloor.Add(2'500'000) == 0);
    CHECK(spanFloor.Add(-5) == 0);                     // a negative span counts as 0 and is the new floor
    CHECK(spanFloor.Add(37 * kMs) == 37 * kMs);
    // The floor is the window's: the 0 leaves after kSpanWindow more spans.
    for (uint32_t i = 0; i < kSpanWindow; ++i) spanFloor.Add(i % 2 ? 37 * kMs : 2'500'000);
    CHECK(spanFloor.Add(37 * kMs) == 34'500'000);

    // PairMeans: consecutive pairs, oldest first, across the ring's wrap.
    std::array<int64_t, kWindow> ring{}, out{};
    ring[0] = 2; ring[1] = 4; ring[2] = 10;
    CHECK(Controller::PairMeans(ring, 3, 3, out) == 2 && out[0] == 3 && out[1] == 7);
    CHECK(Controller::PairMeans(ring, 1, 1, out) == 0);
    for (uint32_t i = 0; i < kWindow; ++i) ring[i] = int64_t(i);
    // Full ring written next at 5: oldest is 5, then 6 ... 31, 0 ... 4.
    CHECK(Controller::PairMeans(ring, kWindow, 5, out) == kWindow - 1);
    CHECK(out[0] == (5 + 6) / 2 && out[26] == (31 + 0) / 2 && out[kWindow - 2] == (3 + 4) / 2);

    // Unpaired: the p10 is the short frames' wait, so almost nothing sleeps and the waits keep alternating.
    PairSimulation off; off.pacing = false;
    PairSimulation single; single.paired = false;
    PairSimulation pairs;
    const auto a = off.Run(), b = single.Run(), c = pairs.Run();
    PrintPair("alt off", a); PrintPair("alt single", b); PrintPair("alt paired", c);
    CHECK(a.alternationMs > 25.0);                       // the model alternates like the log
    CHECK(b.sleptMs < 2.0);                              // the p10 rule sees the short frames
    CHECK(c.sleptMs > 10.0);                             // paired: the wait every pair had moves before the input
    CHECK(c.latencyMs < a.latencyMs - 8.0);
    CHECK(c.alternationMs < a.alternationMs * 0.5);      // and the waits stop jumping
    CHECK(c.intervalMs < a.intervalMs * 1.02);           // at the same frame rate
    CHECK(c.backoffs == 0);

    // Paired on frames that do not alternate (the steady 6x case): as unpaired, give or take the smoother p10.
    Simulation steady;
    Simulation::Result s{};
    {
        // Simulation has no paired switch: run its loop with a paired controller through the same model.
        Simulation on;
        s = on.Run();
    }
    Controller pc;
    pc.SetPaired(true);
    CHECK(pc.Paired());
    Noise noise;
    int64_t previousFlip = 100 * kMs, previousReturn = 0;
    double latency = 0, interval = 0; uint32_t counted = 0;
    uint64_t backoffs = 0;
    for (uint32_t n = 0; n < steady.frames; ++n)
    {
        const int64_t sleep = pc.Take(previousReturn);
        const int64_t input = previousReturn + sleep;
        const int64_t call = input + steady.cpuNs + int64_t(noise.Next() * double(steady.cpuJitterNs));
        const int64_t returned = std::max(call, previousFlip) + steady.presentNs;
        pc.RealFrame(returned, returned - call);
        const int64_t flip = std::max(returned + steady.productionNs, previousFlip + steady.periodNs);
        if (n >= steady.settle) { latency += double(flip - input); interval += double(flip - previousFlip); ++counted; }
        previousFlip = flip;
        previousReturn = returned;
    }
    backoffs = pc.Read().backoffs;
    const double pairedLatency = latency / double(counted) / double(kMs), pairedInterval = interval / double(counted) / double(kMs);
    std::printf("%-12s latency %6.2f ms  interval %6.2f ms (unpaired %6.2f / %6.2f)\n", "steady pair", pairedLatency,
        pairedInterval, s.latencyMs, s.intervalMs);
    CHECK(std::fabs(pairedLatency - s.latencyMs) < 2.0);
    CHECK(std::fabs(pairedInterval - s.intervalMs) < 0.05);
    CHECK(backoffs == 0);
}
}

int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : nullptr;
    const auto run = [&](const char* name, void (*test)()) { if (!only || std::strcmp(only, name) == 0) test(); };
    run("basics", Basics);
    run("steady", Steady);
    run("jitter", Jitter);
    run("spikes", Spikes);
    run("cpu", CpuBound);
    run("clock", FollowingClock);
    run("outside", Outside);
    run("hitches", Hitches);
    run("highrate", HighRate);
    run("schedule", Schedule);
    run("paired", Paired);
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
