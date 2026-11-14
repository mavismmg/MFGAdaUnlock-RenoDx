#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace sdk_pacing::cadence
{
// The flip queue's display clock: a jitter buffer that times every output's flip.
//
// Why: DLSS-G submits a whole batch early into a flip queue about one batch deep and lets the driver time every flip,
// so its display passes only about half of the game's batch jitter on. CPU pacing that releases each output just in
// time (XeFG's present -> GPU completion is ~9 ms in a GPU-bound game) passes every bit of production and copy jitter
// to the screen; low base frame rates suffer most.
//
// Each output's Present waits on the GPU for a gate the meter thread signals at Plan()'s release time (the copy is
// queued before the gate, so it is complete when the flip is released). Like a media jitter buffer:
// - rate: per-output interval = smoothed batch hand-over delta / outputs (the game's own cadence, where it hands the
//   batch over, not at our flips). A delta over kHitchNumerator/kHitchDenominator of the running batch period is a
//   hitch and is left out only when isolated (no long one in the kHitchSpacing batches before it). Recurring long ones
//   (a bimodal cadence, as XeLL's sleep makes) are averaged at most kCadenceClamp times the rate; kHitchRun in a row
//   restart the rate;
// - phase: each batch's first flip follows the previous flip by the interval, nudged (at most kMaximumCorrection)
//   toward a target hand-over -> first-flip latency, so early or late game frames change the buffer depth instead of
//   the display spacing. With SetFirstAtReadiness the first output is not buffered: it flips once ready, at least
//   kFirstGap of an interval after the previous flip, and the correction is measured but not applied. A buffer about
//   one batch deep lets the game's own latency logic (XeLL's sleep, an engine's frame sync) slow the game to match,
//   and the rate would learn that cadence. kFirstGap 0.9 rather than 0.5 holds an early first close to its slot, so a
//   free game's jitter reaches the display at slope ~0.33 instead of ~0.9 for <1% of the rate;
// - a first output that misses its slot flips at readiness and the clock continues from there (no catch-up burst);
// - target: the buffer must cover how long a batch's first output takes to be ready after the hand-over (its GPU work
//   queues behind the game's rendering). When that production latency comes within kProductionMargin of the target,
//   the target jumps to cover it; otherwise it relaxes. It stays between a fraction of a batch and Ceiling(). A first
//   whose copy only waited for our own previous gate (ready within kGateBoundSlack of its signal) never raises it: the
//   target would follow the display.
// Lock-in: a meter slower than the game fills the queue; the game, held by it, hands its batches over at our own flip
// cadence, which the rate would learn. A batch proven held is therefore not learned:
// - coupled (a gated Present of the previous batch blocked in the native Present), its first output still on time and
//   its hand-over within kFollowTolerance of the cadence we flipped at: the interval decays by 1/kCoupledDecay, never
//   below 1/kCoupledFloor of the last learned rate or kMinimumInterval. A late first proves the queue was empty, and a
//   slower hand-over means the game, GPU or display sets the cadence; both are ordinary samples;
// - deep: `full` (a Present blocked behind more than a whole batch of gates) or a buffer half a batch above the target
//   (correction >= kDeepCorrection) kDeepRun batches in a row, as the output ring's wait on the game queue holds the
//   game without a blocked Present. Unless the hand-over is clearly faster than our cadence, it takes the same decay,
//   but only if our clock held the previous batch's real frame kDeepHoldSlack or more after it was ready; otherwise the
//   GPU or the game's own queue is behind, not ours (DeepRefused());
// - unmetered (SetUnmetered), coupled, first on time and gate-bound: a hand-over slower than the rate is not learned
//   and the rate is kept (Kept()), not decayed: a decay could run the clock to its floor, where a late inner output,
//   not our queue, sets the batch. A game that really slows drains the queue and is learned again.
// An interval pinned at kMinimumInterval for kFloorRun batches (1000 outputs a second) is a runaway: the rate restarts
// from the next hand-over (FloorResets()).
// pacerfix.2: evidence instead of inference. Where the host measures what the game waited on the pacing (gameWaitNs >= 0),
// the coupled, deep and kept rules are not used: the batch learns its hand-over less that wait, at most half of it
// (Compensated()), so a clock slower than the game speeds up by what it held the game. The wait is the Present wait above
// its baseline (what the game waits there in batches our queue did not hold; kBaselineGain) plus the low-latency sleep
// (sleptNs, ours, whole). It is taken when the first output was on time or, late or not, when the game waited
// kHeldNumerator/kHeldDenominator of its hand-over or more (before the hitch rules see it); otherwise a late first (the
// GPU or the game set the cadence) learns the hand-over as it is. Without evidence (-1) the inference rules stay.
// pacerfix.2 (gate-ahead): the queue signals each gate at Slot() before the copy is ready and passes that slot to Plan as
// `committed`, so the release is the later of readiness and the slot. A first output's slot comes from the interval
// before its hand-over is learned; the interval learned is the same (tests/xemfg_standalone flip_queue_tests Ahead).
// pacerfix.3: a floor reset also needs the hand-over itself slower than kFloorEscape times the floor (a game that really
// hands over at the floor is not a runaway), and a restarted rate takes the smaller of its first two hand-overs (one
// stall is not the cadence).
// heldprobe.1: the measured wait is not all that holds a game. On OptiScaler's route it is the real frame's own hold, and
// the game's XeLL / Anti-Lag 2 sleep before its input, which our queue drives, is not seen. kHeldRun first outputs in a
// row, each ready kHeldSlack or more before its slot, prove the batches were ready before we flipped them: they take the
// deep decay after the hand-over less the measured wait is learned, never below the floor (kMinimumInterval or the
// V-Sync floor). Once the clock passes the game's rate its first outputs turn late and the hand-over is learned as usual.
// sleepsight.1: where the host measures the game's low-latency sleep (hostSleptNs >= 0) and the game did not sleep, the
// probe does not run: nothing unseen holds the game. The sleep is otherwise only logged: a GPU-bound game sleeps too
// (XeLL starts its CPU work just in time), so its length cannot tell the lock from the GPU. kHeldRun and kHeldSlack keep
// a GPU-bound game whose batches vary +-2 ms from decaying often, while a held game still escapes the lock quickly.
class JitterBufferClock
{
public:
    static constexpr int64_t kMinimumDelta = 1000000, kMaximumDelta = 250000000, kMaximumHold = 100000000;
    // pacerfix.2: a game wait below this is no evidence of a hold (timer and scheduling noise).
    static constexpr int64_t kEvidenceMinimum = 500000;
    static constexpr double kFirstGap = 0.9;
    static constexpr double kRateGain = 0.125, kPhaseGain = 0.05, kMaximumCorrection = 0.04, kCoupledDecay = 32.0;
    static constexpr double kCoupledFloor = 4.0, kMinimumInterval = 1000000.0, kFollowTolerance = 0.02;
    static constexpr double kDeepTolerance = 0.1, kDeepCorrection = 0.025;
    static constexpr uint32_t kDeepRun = 3;
    static constexpr int64_t kProductionMargin = 2000000, kRelaxStep = 100000, kTargetFollow = 8;
    static constexpr size_t kProductionWindow = 64; // diagnostics only
    static constexpr int64_t kGateBoundSlack = 1500000;
    static constexpr int64_t kHitchNumerator = 3, kHitchDenominator = 2;
    static constexpr uint32_t kHitchRun = 3, kHitchSpacing = 8;
    static constexpr double kCadenceClamp = 2.0;
    static constexpr int64_t kDeepHoldSlack = 250000;
    // sleepsight.1: the held probe's run and each first output's least hold (see above).
    static constexpr uint32_t kHeldRun = 5;
    static constexpr int64_t kHeldSlack = 500000;
    static constexpr uint32_t kFloorRun = 60;
    // pacerfix.3: a game wait of this share of the hand-over or more is the pacing's hold, late first output or not (held
    // games measured 60-100%, free-running ones under 25%); the baseline's gain; and how much slower than the floor the
    // hand-over must be for an interval at the floor to be a runaway.
    static constexpr int64_t kHeldNumerator = 2, kHeldDenominator = 5;
    static constexpr double kBaselineGain = 0.0625, kFloorEscape = 1.5;
    // deepRefused: this batch met the deep conditions, but our clock had not held the previous real frame; floorReset:
    // the rate restarted after kFloorRun batches at kMinimumInterval.
    struct Release { int64_t ns{}, interval{}, latency{}, target{}; bool held{}, batchStart{}, late{}, hitch{}, coupled{}, deep{},
        deepRefused{}, floorReset{}; };

    void Reset() noexcept
    {
        const auto floor = floor_; const auto fraction = fraction_; const auto buffers = buffers_;
        const auto late = lateFirsts_; const auto hitches = hitches_; const auto coupled = coupled_; const auto deep = deep_;
        const auto kept = kept_; const auto unmetered = unmetered_;
        const auto firstReady = firstReady_; const auto gapHolds = gapHolds_;
        const auto deepRefused = deepRefused_; const auto floorResets = floorResets_;
        const auto pass = pass_; const auto compensated = compensated_; const auto baseline = baseline_;
        const auto heldSuppressed = heldSuppressed_;
        *this = {}; floor_ = floor; fraction_ = fraction; buffers_ = buffers; lateFirsts_ = late; hitches_ = hitches;
        coupled_ = coupled; deep_ = deep; kept_ = kept; unmetered_ = unmetered; firstReady_ = firstReady; gapHolds_ = gapHolds;
        deepRefused_ = deepRefused; floorResets_ = floorResets; pass_ = pass; compensated_ = compensated;
        baseline_ = baseline; // the host's, not the rate's
        heldSuppressed_ = heldSuppressed;
    }
    // pacerfix.2 (gate-ahead): the earliest release of the next output from the clock as it stands, before its copy is
    // ready (a batch's first output: kFirstGap of the interval after the last flip, never under the floor); 0 when the
    // clock has no interval yet (the output is released at readiness).
    int64_t Slot(bool first) const noexcept
    {
        if (!released_ || effective_ <= 0) return 0;
        const int64_t spacing = first && firstReady_ ? (std::max)(int64_t(kFirstGap*double(effective_)), floor_) : effective_;
        return last_+spacing;
    }
    // pacerfix.2: `count` outputs after the last one flip without gates, one interval apart (a batch NVIDIA's driver
    // meters, gated at its first output only): the next slot follows the last of them.
    void AdvanceUngated(uint32_t count) noexcept
    {
        if (released_ && effective_ > 0 && count) last_ += int64_t(count)*effective_;
    }
    // pacerfix.2: first outputs whose hand-over was learned less the game's measured wait.
    uint64_t Compensated() const noexcept { return compensated_; }
    // sleepsight.1: held probes not run because the host measured the game's low-latency sleep and it did not sleep.
    uint64_t HeldSuppressed() const noexcept { return heldSuppressed_; }
    // pacerfix.3: the game's Present wait while our queue held nothing (ns; 0 until learned).
    int64_t WaitBaseline() const noexcept { return int64_t(baseline_); }
    // Native swap chain back buffers (0 = unknown: the target may reach 1.5 batches).
    void SetCapacity(uint32_t buffers) noexcept { buffers_ = buffers; }
    uint32_t Capacity() const noexcept { return buffers_; }
    // pacerfix.1: how long after its gate is signalled an output flips when nothing holds it (the queue's measured gate
    // pass latency; 0 where the driver meters the batch or nothing is measured), at most kMaximumPass. An output the
    // clock does not hold is signalled once it is ready and flips that much later, while a held one is signalled that
    // much early and flips at its release: the next slot follows the flip, not the signal.
    static constexpr int64_t kMaximumPass = 2000000;
    void SetPassLatency(int64_t ns) noexcept { pass_ = ns > 0 ? (std::min)(ns, kMaximumPass) : 0; }
    int64_t PassLatency() const noexcept { return pass_; }
    // No driver flip metering times this queue's batches (NVIDIA's metered batches keep the follow rule only).
    void SetUnmetered(bool unmetered) noexcept { unmetered_ = unmetered; }
    // A batch's first output flips at readiness (kFirstGap after the previous flip at the earliest), no buffer.
    void SetFirstAtReadiness(bool ready) noexcept { firstReady_ = ready; }
    bool FirstAtReadiness() const noexcept { return firstReady_; }
    // First outputs ready before kFirstGap after the previous flip, held to it.
    uint64_t GapHolds() const noexcept { return gapHolds_; }
    // Production latency (first output ready - batch hand-over) quantile; 0 until known.
    int64_t ProductionQuantile(double q) const noexcept
    {
        if (!productionCount_) return 0;
        std::array<int64_t, kProductionWindow> sorted{};
        for (size_t i = 0; i < productionCount_; ++i) sorted[i] = production_[(productionStart_+i)%kProductionWindow];
        const size_t k = size_t(q*double(productionCount_-1)+0.5);
        std::nth_element(sorted.begin(), sorted.begin()+k, sorted.begin()+productionCount_);
        return sorted[k];
    }
    int64_t Cap() const noexcept { return cap_; }
    void SetFloor(int64_t interval) noexcept { floor_ = interval > 0 && interval <= kMaximumHold ? interval : 0; }
    // Base buffer as a fraction of one batch period (0.5 = half a batch).
    void SetBufferFraction(double fraction) noexcept { fraction_ = fraction >= 0.0 && fraction <= 1.0 ? fraction : 0.5; }
    int64_t Interval() const noexcept { return effective_; }
    int64_t Target() const noexcept { return target_; }
    int64_t Latency() const noexcept { return latency_; }
    uint64_t LateFirsts() const noexcept { return lateFirsts_; }
    uint64_t Hitches() const noexcept { return hitches_; }
    uint64_t Coupled() const noexcept { return coupled_; }
    // Coupled decays taken because the meter was proven behind (full, or a deep buffer; in Coupled()). heldprobe.1: with
    // evidence, the decays taken after kDeepRun first outputs in a row were held before their slots (not in Coupled()).
    uint64_t Deep() const noexcept { return deep_; }
    // Slower hand-overs not learned because an unmetered queue held the batch.
    uint64_t Kept() const noexcept { return kept_; }
    // Deep decays refused (our clock held nothing: the GPU or the game's queue was behind), and rate restarts
    // after the interval sat at the floor; LastHold: how long our clock held the last output after it was ready.
    uint64_t DeepRefused() const noexcept { return deepRefused_; }
    uint64_t FloorResets() const noexcept { return floorResets_; }
    int64_t LastHold() const noexcept { return lastHold_; }
    // Phase correction of the last batch in parts per million (positive = faster).
    int32_t CorrectionPpm() const noexcept { return int32_t(correction_*1e6); }

    // ready: this output's copy completion; submit: when the game handed this batch over (first outputs only);
    // outputs: generated outputs + the real frame; gateSignal: when the previous output's gate was signalled (0 =
    // unknown); coupled (first outputs): the game handed this batch over after being held by the full presentation
    // queue (a gated Present of the previous batch blocked in the native Present); full: that block waited
    // behind more than a whole batch of gates. First outputs since the previous batch: gameWaitNs, the game's measured
    // wait in its Present (-1: no evidence); sleptNs, the low-latency sleep the pacing caused; hostSleptNs, the host's
    // measured Reflex / XeLL / Anti-Lag 2 sleep (-1: not measured). committed: the slot the gate was signalled for before
    // the copy was ready (gate-ahead; 0: released at readiness or by the clock's own rule).
    Release Plan(int64_t ready, int64_t submit, bool first, uint32_t outputs, int64_t gateSignal = 0,
        bool coupled = false, bool full = false, int64_t gameWaitNs = -1, int64_t committed = 0,
        int64_t sleptNs = 0, int64_t hostSleptNs = -1) noexcept
    {
        Release result{};
        if (ready <= 0 || outputs < 2) { Reset(); result.ns = ready; return result; }
        if (first)
        {
            result.batchStart = true;
            const bool evidence = gameWaitNs >= 0;
            const int64_t delta = submit-lastSubmit_;
            // A live count change (Dynamic MFG) keeps the batch period: the per-output interval is rescaled, so the
            // switch batch is not flipped as a burst.
            if (outputs != outputs_ && outputs_ && interval_ > 0.0) interval_ = interval_*double(outputs_)/double(outputs);
            if (outputs != outputs_ && outputs_ && pending_ > 0.0) pending_ = pending_*double(outputs_)/double(outputs);
            // The first output is still waiting for its slot on the running clock (the queue held it).
            const bool onTime = released_ && effective_ > 0 && ready <= last_+effective_;
            // The last batches' buffers stayed half a batch or more above the target.
            deepRun_ = released_ && correction_ >= kDeepCorrection ? deepRun_+1 : 0;
            const double cadence = double(effective_)*double(outputs_);
            const bool follows = coupled && double(delta) <= cadence*(1.0+kFollowTolerance);
            const bool held = unmetered_ && coupled && onTime && gateSignal > 0 && ready <= gateSignal+kGateBoundSlack;
            const bool deepWanted = (full || deepRun_ >= kDeepRun) && double(delta) >= cadence*(1.0-kDeepTolerance);
            // And our clock held the previous batch's real frame (the output planned before this one).
            const bool deep = deepWanted && lastHold_ >= kDeepHoldSlack;
            // heldprobe.1: this first output was ready kHeldSlack or more before its slot on the running clock (the
            // committed one under gate-ahead): the batch was ready before we flipped it.
            const int64_t firstSlot = released_ && effective_ > 0 ? (committed > 0 ? committed : Slot(true)) : 0;
            heldRun_ = firstSlot > 0 && ready+kHeldSlack <= firstSlot ? heldRun_+1 : 0;
            if (!evidence && deepWanted && !deep && !follows && onTime && interval_ > 0.0)
            { result.deepRefused = true; ++deepRefused_; }
            if (submit <= 0 || !lastSubmit_ || delta < kMinimumDelta || delta > kMaximumDelta)
            { interval_ = 0.0; effective_ = 0; hitchRun_ = 0; deepRun_ = 0; pending_ = 0.0; }
            else if (evidence)
            {
                // pacerfix.2: the game's own measured wait decides, not the inference rules below (see the class comment).
                const int64_t slept = sleptNs > 0 ? sleptNs : 0;
                const bool heldShare = (gameWaitNs+slept)*kHeldDenominator >= delta*kHeldNumerator;
                const int64_t baseline = int64_t(baseline_);
                const int64_t excess = (gameWaitNs > baseline ? gameWaitNs-baseline : 0)+slept;
                const bool admitted = heldShare || (onTime && interval_ > 0.0);
                const int64_t wait = admitted && excess >= kEvidenceMinimum ? (std::min)(excess, delta/2) : 0;
                if (wait) { result.coupled = true; ++compensated_; }
                // The baseline learns from batches our queue held nothing of: a late first output, the game not held for
                // that share.
                if (released_ && effective_ > 0 && !onTime && !heldShare)
                    baseline_ += kBaselineGain*(double(gameWaitNs)-baseline_);
                Learn(delta-wait, outputs, result);
                // heldprobe.1: kHeldRun first outputs in a row held before their slots: something unmeasured holds the game
                // at our cadence, so the deep decay, never below the floor (not where the game measurably did not sleep).
                const double bound = (std::max)(kMinimumInterval, double(floor_));
                const bool unseenHold = hostSleptNs < 0 || hostSleptNs >= kEvidenceMinimum;
                if (heldRun_ >= kHeldRun && interval_ > bound && double(delta) >= cadence*(1.0-kDeepTolerance))
                {
                    if (!unseenHold) ++heldSuppressed_;
                    else
                    {
                        interval_ = (std::max)(interval_-interval_/kCoupledDecay, bound);
                        result.coupled = true; result.deep = true; ++deep_;
                    }
                }
            }
            else if (onTime && interval_ > 0.0 && (follows || deep))
            {
                // The hand-over followed our own flips: probe faster instead of learning them, within bounds. A deep
                // queue's decay is bounded by its own conditions (its first output still on time, the buffer deep) instead
                // of the learned rate, which the lock-in itself inflated.
                const double floor = follows ? (std::max)(learned_/kCoupledFloor, kMinimumInterval) : kMinimumInterval;
                interval_ = (std::max)(interval_-interval_/kCoupledDecay, (std::min)(floor, interval_));
                result.coupled = true; ++coupled_; hitchRun_ = 0;
                if (!follows) { result.deep = true; ++deep_; }
            }
            else if (held && interval_ > 0.0 && double(delta) > interval_*double(outputs))
            { ++kept_; hitchRun_ = 0; } // the rate stays
            else Learn(delta, outputs, result);
            // An interval pinned at the floor restarts the rate, but only while the game hands its batches over
            // kFloorEscape times slower than that: one as fast as the floor is not a runaway.
            const bool floorRunaway = interval_ > 0.0 && interval_ <= kMinimumInterval*1.01 && submit > 0
                && double(delta) > kFloorEscape*kMinimumInterval*double(outputs);
            floorRun_ = floorRunaway ? floorRun_+1 : 0;
            if (floorRun_ >= kFloorRun)
            {
                interval_ = 0.0; learned_ = 0.0; effective_ = 0; hitchRun_ = 0; deepRun_ = 0; floorRun_ = 0; pending_ = 0.0;
                result.floorReset = true; ++floorResets_;
            }
            lastSubmit_ = submit > 0 ? submit : 0; outputs_ = outputs;
            correction_ = 0.0;
            if (interval_ > 0.0)
            {
                const double batch = interval_*outputs;
                base_ = int64_t(fraction_*batch);
                cap_ = buffers_ ? int64_t(double(buffers_ > outputs+3 ? buffers_-outputs-2 : 1)*interval_) : int64_t(1.5*batch);
                if (base_ > cap_) base_ = cap_;
                if (!target_ || target_ < base_) target_ = base_;
                if (target_ > Ceiling()) target_ = Ceiling();
                // Buffer this batch would get if it flipped on the running clock.
                const int64_t slot = released_ ? last_+effective_ : 0;
                if (slot && submit > 0 && effective_ > 0)
                {
                    latency_ = slot-submit;
                    double error = double(latency_-target_)/batch;
                    error = error > 1.0 ? 1.0 : error < -1.0 ? -1.0 : error;
                    double correction = kPhaseGain*error;
                    correction = correction > kMaximumCorrection ? kMaximumCorrection
                        : correction < -kMaximumCorrection ? -kMaximumCorrection : correction;
                    correction_ = correction;
                    // With first at readiness the correction is still measured (the deep run reads it) but not applied.
                    effective_ = firstReady_ ? int64_t(interval_) : int64_t(interval_*(1.0-correction));
                }
                else effective_ = int64_t(interval_);
                if (effective_ < floor_) effective_ = floor_;
            }
        }
        int64_t release = ready;
        if (committed > 0)
        {
            // pacerfix.2 (gate-ahead): the gate went at the slot Slot() gave; the flip is the later of it and readiness.
            if (committed > ready) release = committed;
        }
        else
        {
            // With first at readiness a first output waits only for the minimum gap after the previous flip (never under
            // the V-Sync floor), not for its slot.
            const int64_t spacing = first && firstReady_ ? (std::max)(int64_t(kFirstGap*double(effective_)), floor_)
                : effective_;
            if (released_ && effective_ > 0 && last_+spacing > ready) release = last_+spacing;
        }
        if (release-ready > kMaximumHold) release = ready+kMaximumHold;
        result.held = release > ready;
        if (first && firstReady_ && result.held) ++gapHolds_;
        if (first && released_ && effective_ > 0 && ready > last_+effective_) { result.late = true; ++lateFirsts_; }
        if (first && target_)
        {
            const bool sample = !result.coupled && submit > 0 && ready > submit && ready-submit < kMaximumHold
                && (gateSignal <= 0 || ready > gateSignal+kGateBoundSlack);
            const int64_t production = sample ? ready-submit : 0;
            if (sample)
            {
                production_[(productionStart_+productionCount_)%kProductionWindow] = production;
                if (productionCount_ < kProductionWindow) ++productionCount_;
                else productionStart_ = (productionStart_+1)%kProductionWindow;
            }
            // Next batch's target (this batch already flipped on its own).
            if (sample && production+kProductionMargin > target_)
                target_ = production+kProductionMargin < Ceiling() ? production+kProductionMargin : Ceiling();
            else
            {
                // Above what the production latency needs (a base set during a slow stretch: half of a 140 ms batch
                // stayed ~70 ms for hundreds of batches) the target follows 1/kTargetFollow of the excess.
                const int64_t need = (std::max)(base_, productionCount_ ? ProductionQuantile(0.9)+kProductionMargin : base_);
                const int64_t step = target_ > need ? (std::max)(kRelaxStep, (target_-need)/kTargetFollow) : kRelaxStep;
                target_ = target_-step > base_ ? target_-step : base_;
            }
        }
        // pacerfix.1: the flip, not the signal: an output not held is signalled at readiness and flips pass_ later.
        released_ = true; last_ = result.held ? release : release+pass_; lastHold_ = release-ready;
        result.ns = release; result.interval = effective_; result.latency = latency_; result.target = target_;
        return result;
    }

private:
    // The highest target: the capacity is queue time after readiness, on top of the production latency (p90).
    int64_t Ceiling() const noexcept { return cap_+ProductionQuantile(0.9); }
    // One hand-over delta learned into the rate (the hitch rules: see the class comment's rate).
    void Learn(int64_t delta, uint32_t outputs, Release& result) noexcept
    {
        const double batch = interval_*double(outputs);
        const bool hitch = interval_ > 0.0 && double(delta)*kHitchDenominator > batch*kHitchNumerator;
        const bool isolated = sinceLong_ > kHitchSpacing;
        sinceLong_ = hitch ? 0 : (std::min)(sinceLong_+1, kHitchSpacing+1);
        if (hitch) ++hitchRun_;
        if (hitch && isolated && hitchRun_ < kHitchRun) { result.hitch = true; ++hitches_; return; }
        const bool lasting = hitch && hitchRun_ >= kHitchRun;
        double sample = double(delta)/double(outputs);
        if (hitch && !lasting) sample = (std::min)(sample, interval_*kCadenceClamp);
        // pacerfix.3: a restarted rate takes the smaller of its first two hand-overs (one stall is not the cadence).
        if (interval_ <= 0.0)
        {
            if (pending_ <= 0.0) { pending_ = sample; return; }
            sample = (std::min)(sample, pending_);
            pending_ = 0.0;
        }
        interval_ = interval_ > 0.0 && !lasting ? interval_+kRateGain*(sample-interval_) : sample;
        if (interval_ < kMinimumInterval) interval_ = kMinimumInterval;
        learned_ = interval_;
        if (!hitch || lasting) hitchRun_ = 0;
    }

    double interval_{}, fraction_{0.5}, correction_{}, learned_{};
    int64_t lastSubmit_{}, last_{}, effective_{}, target_{}, latency_{}, floor_{}, cap_{}, base_{};
    uint64_t lateFirsts_{}, hitches_{}, coupled_{}, deep_{}, kept_{}, gapHolds_{};
    std::array<int64_t, kProductionWindow> production_{};
    size_t productionStart_{}, productionCount_{};
    uint32_t outputs_{}, buffers_{}, hitchRun_{}, deepRun_{};
    // heldprobe.1: first outputs in a row ready kHeldSlack or more before their slots.
    uint32_t heldRun_{};
    // First outputs since the last long hand-over (above kHitchSpacing: none recently).
    uint32_t sinceLong_{kHitchSpacing+1};
    // The last output's hold (release - ready, >= 0), batches at the floor, and the two counters.
    int64_t lastHold_{};
    uint32_t floorRun_{};
    uint64_t deepRefused_{}, floorResets_{};
    bool released_{}, unmetered_{}, firstReady_{};
    int64_t pass_{}; // SetPassLatency
    uint64_t compensated_{};
    uint64_t heldSuppressed_{}; // sleepsight.1 (kept by Reset)
    // pacerfix.3: the game's Present wait while our queue held nothing (kept by Reset), and a restarted rate's first
    // hand-over per output (0: none yet).
    double baseline_{}, pending_{};
};

// The gate lead where nothing meters the flips (AMD, Intel; NVIDIA batches without flip metering). There each flip
// happens when the presentation queue gets past its gate, and the meter signals the gate this lead early. A fixed lead
// fits one GPU only (queue-wake latency varies by GPU and vendor), so the meter samples how long gates take to pass after
// their Signal; once kMinimumSamples are in, the lead is the median of the last kWindow, within the bounds. The lower
// bound is low because gates have been measured passing in ~22 us; a higher floor flips every held output early.
class GatePassLead
{
public:
    static constexpr int64_t kMinimumLead = 50000, kMaximumLead = 2000000;
    static constexpr size_t kWindow = 64;
    static constexpr uint64_t kMinimumSamples = 32;

    // A gate's pass latency (pass - signal), > 0.
    void Add(int64_t passNs) noexcept
    {
        if (passNs <= 0) return;
        window_[(start_+count_)%kWindow] = passNs;
        if (count_ < kWindow) ++count_;
        else start_ = (start_+1)%kWindow;
        ++samples_;
        std::array<int64_t, kWindow> sorted{};
        for (size_t i = 0; i < count_; ++i) sorted[i] = window_[(start_+i)%kWindow];
        std::nth_element(sorted.begin(), sorted.begin()+count_/2, sorted.begin()+count_);
        median_ = sorted[count_/2];
    }
    uint64_t Samples() const noexcept { return samples_; }
    // Median of the window; 0 before the first sample.
    int64_t Median() const noexcept { return median_; }
    // fallback until kMinimumSamples are in.
    int64_t Lead(int64_t fallback) const noexcept
    {
        return samples_ < kMinimumSamples ? fallback : (std::clamp)(median_, kMinimumLead, kMaximumLead);
    }
    // pacerfix.1: the measured pass latency for the clock (JitterBufferClock::SetPassLatency): the median, at most
    // kMaximumLead, once kMinimumSamples are in; 0 before.
    int64_t Pass() const noexcept { return samples_ < kMinimumSamples ? 0 : (std::min)(median_, kMaximumLead); }

private:
    std::array<int64_t, kWindow> window_{};
    size_t start_{}, count_{};
    uint64_t samples_{};
    int64_t median_{};
};

// pacerfix.1: the gated Presents XeFG's native swap chain can hold: its maximum frame latency once raised to buffers - 1,
// which StartQueue does when the chain has more than baseLatency + 1 back buffers (NativeBuffersScope); otherwise XeFG's
// own latency (baseLatency, at most 3).
constexpr uint32_t ChainCapacity(uint32_t buffers, uint32_t baseLatency) noexcept
{
    return buffers > baseLatency+1 ? buffers-1 : baseLatency;
}

// The small-chain rule (sdk_pacing.cpp GateBeforePresent): without driver flip metering a batch is not gated when the
// chain's capacity (not the maximum latency in effect, which only gated batches adjust) cannot hold its generated outputs.
constexpr bool SmallChain(bool metered, uint32_t capacity, uint32_t generated) noexcept
{
    return !metered && capacity && capacity < generated;
}

// pacerfix.1: how long before its target a precise wait stops sleeping on the high-resolution timer and spins (the meter
// thread's gate releases, the low-latency pacing's sleep). The timer's late wake-ups are learned per waiter: the margin
// is their mean plus two mean deviations, within bounds, so only that margin is spun.
class WaitMargin
{
public:
    static constexpr int64_t kMinimum = 100000, kMaximum = 1500000, kStartLate = 500000, kStartDeviation = 250000;
    static constexpr int64_t kSampleLimit = 5000000; // a wake-up later than this (preemption) counts as this
    static constexpr int64_t kGain = 16;            // 1/kGain of each new sample
    // A timer wake-up `lateNs` after its due time (negative: early).
    void Add(int64_t lateNs) noexcept
    {
        lateNs = (std::clamp)(lateNs, int64_t(0), kSampleLimit);
        late_ += (lateNs-late_)/kGain;
        const int64_t deviation = lateNs > late_ ? lateNs-late_ : late_-lateNs;
        deviation_ += (deviation-deviation_)/kGain;
        ++samples_;
    }
    int64_t Margin() const noexcept { return (std::clamp)(late_+2*deviation_, kMinimum, kMaximum); }
    int64_t Late() const noexcept { return late_; }
    uint64_t Samples() const noexcept { return samples_; }

private:
    int64_t late_{kStartLate}, deviation_{kStartDeviation};
    uint64_t samples_{};
};
}
