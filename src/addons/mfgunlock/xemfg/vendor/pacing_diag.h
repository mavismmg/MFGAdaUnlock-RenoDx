#pragma once
// pacediag.1: the flip queue's display-side diagnostics, opt-in (OptiScaler.ini [XeFG] XeFGPacingDiagnostics = on, or
// RTX_MFG_PACING_DIAG=1 in the game's environment). The periodic logs only say when the queue meant each output to flip;
// this records what the screen got, and whether a gap appears between our gate and the flip (driver meter, composition,
// the display).
//
// One event stream per flip queue, written as CSV chunks beside the game's log:
//   Q (meter thread, each output): what the clock was given and decided, the gate's signal and release;
//   P (presenting threads, each gated Present): the gate it waited on, the native chain's present id, how long the native
//     Present held the thread, its sync interval and flags;
//   D (display thread): each new displayed present the native chain's frame statistics report (present id, its refresh
//     counts and sync time), polled about every 0.5 ms while a Present is in flight;
//   A (presenting thread, about once a second): AMD's flip meter configuration as the driver reads it back;
//   G (display thread, about ten times a second while frame generation runs): each GPU engine's running time (the
//     adapter's and this process's), D3DKMTQueryStatistics.
// tests/analyze_pacing_diag.py joins them (Q.gate = P.gate, P.present_id = D.present_count).
//
// Producers never allocate or touch a file: events go into one of two preallocated chunks under a mutex; a full chunk is
// handed to a writer (a thread-pool callback in sdk_pacing.cpp) and comes back empty. While the writer still holds the
// other chunk, events are dropped and counted. At most kMaxChunks chunks are written per queue.
// No Windows calls here.
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace pacing_diag
{
enum Kind : uint8_t
{
    kQueue = 'Q',
    kPresent = 'P',
    kDisplay = 'D',
    kAmd = 'A',
    kGpu = 'G',
};

constexpr size_t kValues = 16;
struct Event
{
    int64_t t{};                         // NowNs() when it was recorded (QPC in nanoseconds)
    std::array<int64_t, kValues> v{};    // per kind, see Columns()
    uint8_t kind{};
};

// The CSV columns after "kind,t_ns" for each kind (the header lines of every chunk file name them).
inline const char* Columns(uint8_t kind) noexcept
{
    switch (kind)
    {
    case kQueue: return "gate,index,outputs,flags,submit_ns,ready_ns,slot_ns,signal_ns,release_ns,interval_ns,pass_ns,"
        "game_wait_ns,game_slept_ns,host_slept_ns,idle_ns,target_ns";
    case kPresent: return "gate,present_id,entered_ns,returned_ns,sync,flags,generated,frame,site,thread,result";
    case kDisplay: return "present_count,present_refresh,sync_refresh,sync_ns,advanced,result";
    case kAmd: return "mode,count,flags,status,output,result,applied,applied_count";
    case kGpu: return "node,global_running,process_running,wall_ns,global_status,process_status";
    default: return "";
    }
}

inline size_t ColumnCount(uint8_t kind) noexcept
{
    size_t count = 1;
    for (const char* text = Columns(kind); *text; ++text) count += *text == ',';
    return *Columns(kind) ? count : 0;
}

// One CSV line (no newline): kind, t, then the kind's columns.
inline std::string Format(const Event& event)
{
    std::string line;
    line.reserve(160);
    line += char(event.kind);
    line += ',';
    line += std::to_string(event.t);
    const size_t count = ColumnCount(event.kind);
    for (size_t i = 0; i < count && i < kValues; ++i)
    {
        line += ',';
        line += std::to_string(event.v[i]);
    }
    return line;
}

// The header of a chunk file: a comment per kind with its columns, then the column line every row uses.
inline std::string Header(uint64_t generation, uint32_t chunk, uint64_t dropped)
{
    std::string text = "# pacediag.1 generation=" + std::to_string(generation) + " chunk=" + std::to_string(chunk) +
        " dropped_before=" + std::to_string(dropped) + "\n";
    for (const uint8_t kind : {kQueue, kPresent, kDisplay, kAmd, kGpu})
        text += std::string("# ") + char(kind) + ": kind,t_ns," + Columns(kind) + "\n";
    return text;
}

class Recorder
{
public:
    // About 20 s of a 2X queue at 180 output fps (Q + P + D rows) plus GPU and meter rows; 144 bytes each, two chunks.
    static constexpr size_t kChunkEvents = 24576;
    static constexpr uint32_t kMaxChunks = 6;

    Recorder() { chunks_[0].reserve(kChunkEvents); chunks_[1].reserve(kChunkEvents); }

    // Records one event. When the current chunk fills, it is handed out through `full` (the caller writes it and gives it
    // back with Return) and recording continues in the other one, if that one is back; otherwise the event is dropped.
    // Returns true when `full` was set.
    bool Push(const Event& event, std::vector<Event>*& full) noexcept
    {
        full = nullptr;
        std::lock_guard lock(mutex_);
        if (closed_ || handedOut_ >= kMaxChunks) { ++dropped_; return false; }
        auto& current = chunks_[current_];
        if (writing_[current_]) { ++dropped_; return false; }
        current.push_back(event);
        if (current.size() < kChunkEvents) return false;
        writing_[current_] = true;
        full = &current;
        ++handedOut_;
        current_ ^= 1;
        return true;
    }

    // The writer is done with `chunk`: it is emptied (capacity kept) and can record again.
    void Return(std::vector<Event>* chunk) noexcept
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < 2; ++i)
            if (&chunks_[i] == chunk) { chunks_[i].clear(); writing_[i] = false; }
    }

    // At the queue's stop: no more events; the partly filled chunk, if any and not being written, is handed out.
    std::vector<Event>* Close() noexcept
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        auto& current = chunks_[current_];
        if (writing_[current_] || current.empty() || handedOut_ >= kMaxChunks) return nullptr;
        writing_[current_] = true;
        ++handedOut_;
        return &current;
    }

    uint64_t Dropped() noexcept { std::lock_guard lock(mutex_); return dropped_; }
    uint32_t HandedOut() noexcept { std::lock_guard lock(mutex_); return handedOut_; }

private:
    std::mutex mutex_;
    std::array<std::vector<Event>, 2> chunks_;
    std::array<bool, 2> writing_{};
    size_t current_{};
    uint32_t handedOut_{};
    uint64_t dropped_{};
    bool closed_{};
};

// Writes a chunk to `file` (header first). False on a short write.
inline bool Write(std::FILE* file, const std::vector<Event>& events, uint64_t generation, uint32_t chunk, uint64_t dropped)
{
    if (!file) return false;
    const auto header = Header(generation, chunk, dropped);
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) return false;
    for (const auto& event : events)
    {
        auto line = Format(event);
        line += '\n';
        if (std::fwrite(line.data(), 1, line.size(), file) != line.size()) return false;
    }
    return true;
}
}
