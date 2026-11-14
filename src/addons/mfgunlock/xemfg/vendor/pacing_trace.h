#pragma once
// pacerfix.2: the flip queue's per-output trace, opt-in (RTX_MFG_PACING_TRACE=1 in the game's environment). The meter
// thread records, for every output it releases, what the clock was given and what it decided (Record), in a fixed ring
// of the last kCapacity outputs (no allocation after it is made); when the meter stops, the ring is written beside the
// game's log as <log name>.pacing.trace (sdk_pacing.cpp DumpTrace). A fresh JitterBufferClock replays each decision
// (tests/xemfg_standalone/trace_replay_tests.cpp), so a user's run becomes a CPU test case. No Windows calls here.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pacing_trace
{
enum Flag : uint32_t
{
    kFirst = 1u << 0,         // a batch's first output
    kHeld = 1u << 1,          // the clock released it after its readiness
    kLate = 1u << 2,          // a first output ready after its slot
    kCoupled = 1u << 3,       // the item's coupled evidence (presenter backlog or a full queue)
    kFull = 1u << 4,          // the item's full evidence
    kVsync = 1u << 5,         // its batch presented with V-Sync
    kMetered = 1u << 6,       // NVIDIA's driver metered the queue at the time
    kAhead = 1u << 7,         // gate-ahead: the gate was signalled before the copy was ready
    kMeteredBatch = 1u << 8,  // a metered batch gated at its first output only (AdvanceUngated after it)
    kHitch = 1u << 9,         // the clock left the hand-over out as a hitch
    kReady = 1u << 10,        // the copy was seen ready (else the clock was reset after it)
    kReset = 1u << 11,        // the clock was reset before this output (an accepted transition)
    kCompensated = 1u << 12,  // the hand-over was learned less the game's wait
    // sleepsight.1: the host's measured low-latency sleep since the last batch was under kEvidenceMinimum (Plan's
    // hostSleptNs 0; without the flag -1), all a replay needs of it. Absent in older traces.
    kHostQuiet = 1u << 13,
};

// Everything JitterBufferClock::Plan was given for one output, the clock settings in effect, and what it decided.
struct Record
{
    uint64_t value{};                       // the gate value
    int64_t submitNs{}, readyNs{};          // hand-over (first outputs), readiness as seen
    int64_t slotNs{};                       // the committed slot (gate-ahead; 0: released at readiness)
    int64_t signalNs{}, gateOpenNs{};       // the gate's signal; the previous output's gate opening (Plan's gateSignal)
    int64_t releaseNs{}, intervalNs{};      // Plan's release and interval
    int64_t gameWaitNs{-1};                 // the item's game-wait evidence (-1: none)
    int64_t floorNs{};                      // the V-Sync floor in effect
    uint32_t outputs{}, index{}, flags{};
    uint32_t passNs{};                      // SetPassLatency
    uint32_t capacity{};                    // SetCapacity
    uint32_t sleptNs{};                     // pacerfix.3: the item's low-latency sleep (Plan's sleptNs; 0 in older traces)
};
static_assert(sizeof(Record) == 104, "the trace format's record");

struct Header
{
    char magic[8]{'X', 'M', 'F', 'G', 'T', 'R', 'C', '1'};
    uint32_t version{1}, recordSize{uint32_t(sizeof(Record))};
    uint64_t generation{}, count{}, dropped{};
};
static_assert(sizeof(Header) == 40, "the trace format's header");

// The last kCapacity records, oldest first through At().
class Ring
{
public:
    static constexpr size_t kCapacity = 4096;
    void Push(const Record& record) noexcept
    {
        records_[next_] = record;
        next_ = (next_+1)%kCapacity;
        if (count_ < kCapacity) ++count_;
        else ++dropped_;
    }
    size_t Count() const noexcept { return count_; }
    uint64_t Dropped() const noexcept { return dropped_; }
    const Record& At(size_t i) const noexcept { return records_[(next_+kCapacity-count_+i)%kCapacity]; }
    void Clear() noexcept { next_ = count_ = 0; dropped_ = 0; }

private:
    std::array<Record, kCapacity> records_{};
    size_t next_{}, count_{};
    uint64_t dropped_{};
};

// The ring, oldest first, after a Header. False on any short write.
inline bool Write(std::FILE* file, const Ring& ring, uint64_t generation) noexcept
{
    if (!file) return false;
    Header header{};
    header.generation = generation;
    header.count = ring.Count();
    header.dropped = ring.Dropped();
    if (std::fwrite(&header, sizeof(header), 1, file) != 1) return false;
    for (size_t i = 0; i < ring.Count(); ++i)
        if (std::fwrite(&ring.At(i), sizeof(Record), 1, file) != 1) return false;
    return true;
}

// A trace Write made. False for another magic, version or record size, too many records, or a short file.
inline bool Read(std::FILE* file, Header& header, std::vector<Record>& records)
{
    records.clear();
    if (!file || std::fread(&header, sizeof(header), 1, file) != 1) return false;
    const Header expected{};
    if (std::memcmp(header.magic, expected.magic, sizeof(header.magic)) != 0 || header.version != expected.version
        || header.recordSize != expected.recordSize || header.count > Ring::kCapacity) return false;
    records.resize(size_t(header.count));
    return records.empty() || std::fread(records.data(), sizeof(Record), records.size(), file) == records.size();
}
}
