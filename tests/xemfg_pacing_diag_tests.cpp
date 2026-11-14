// Vendored from dashdogy/MFG-Unlocker tests/xemfg_standalone/pacing_diag_tests.cpp at 128d081 (MIT, see
// src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt); only the include path is ours.
// pacediag.1: CPU checks of the flip queue's display-side diagnostics recorder and CSV rows
// (source/native/xess/pacing_diag.h).
#include "../src/addons/mfgunlock/xemfg/vendor/pacing_diag.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
using namespace pacing_diag;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

Event Make(uint8_t kind, int64_t t) noexcept
{
    Event event{};
    event.kind = kind;
    event.t = t;
    for (size_t i = 0; i < kValues; ++i) event.v[i] = t*100+int64_t(i);
    return event;
}

size_t Commas(const std::string& text)
{
    size_t count{};
    for (const char ch : text) count += ch == ',';
    return count;
}

// Each kind's row has kind, t and exactly its named columns; the header names every kind.
void Rows()
{
    CHECK(ColumnCount(kQueue) == 16);
    CHECK(ColumnCount(kPresent) == 11);
    CHECK(ColumnCount(kDisplay) == 6);
    CHECK(ColumnCount(kAmd) == 8);
    CHECK(ColumnCount(kGpu) == 6);
    CHECK(ColumnCount(0) == 0);
    for (const uint8_t kind : {kQueue, kPresent, kDisplay, kAmd, kGpu})
    {
        const auto line = Format(Make(kind, 7));
        CHECK(line[0] == char(kind));
        CHECK(Commas(line) == ColumnCount(kind)+1);
        CHECK(line.rfind(std::string(",") + std::to_string(700+int64_t(ColumnCount(kind))-1)) == line.size()-4);
    }
    const auto display = Format(Make(kDisplay, 3));
    CHECK(display == "D,3,300,301,302,303,304,305");
    const auto header = Header(2, 1, 5);
    CHECK(header.find("generation=2 chunk=1 dropped_before=5") != std::string::npos);
    for (const char kind : {'Q', 'P', 'D', 'A', 'G'})
        CHECK(header.find(std::string("# ") + kind + ": kind,t_ns,") != std::string::npos);
}

// A chunk is handed out when full; recording goes on in the other one; both out drops (counted); a returned chunk records
// again from empty; at most kMaxChunks are handed out; Close hands out the partial chunk once.
void Chunks()
{
    Recorder recorder;
    std::vector<Event>* full{};
    std::vector<Event>* first{};
    for (size_t i = 0; i < Recorder::kChunkEvents; ++i)
        if (recorder.Push(Make(kQueue, int64_t(i)), full)) first = full;
    CHECK(first && first->size() == Recorder::kChunkEvents && first->front().t == 0);
    CHECK(recorder.HandedOut() == 1);
    std::vector<Event>* second{};
    for (size_t i = 0; i < Recorder::kChunkEvents; ++i)
        if (recorder.Push(Make(kPresent, int64_t(i)), full)) second = full;
    CHECK(second && second != first && second->front().kind == kPresent);
    CHECK(!recorder.Push(Make(kDisplay, 1), full) && !full);
    CHECK(recorder.Dropped() == 1);
    recorder.Return(first);
    CHECK(first->empty());
    CHECK(!recorder.Push(Make(kAmd, 5), full) && !full);
    CHECK(first->size() == 1 && first->front().kind == kAmd);
    recorder.Return(second);
    uint32_t handed = recorder.HandedOut();
    // Fill and return until the cap: then everything drops.
    for (int round = 0; round < 10; ++round)
    {
        std::vector<Event>* out{};
        for (size_t i = 0; i < Recorder::kChunkEvents && !out; ++i)
            if (recorder.Push(Make(kGpu, int64_t(i)), full)) out = full;
        if (out) { ++handed; recorder.Return(out); }
    }
    CHECK(handed == Recorder::kMaxChunks);
    CHECK(recorder.HandedOut() == Recorder::kMaxChunks);
    const uint64_t dropped = recorder.Dropped();
    CHECK(!recorder.Push(Make(kQueue, 1), full) && recorder.Dropped() == dropped+1);
    CHECK(recorder.Close() == nullptr); // the cap is spent

    Recorder partial;
    CHECK(partial.Close() == nullptr); // nothing recorded
    Recorder some;
    for (int i = 0; i < 10; ++i) some.Push(Make(kDisplay, i), full);
    auto* rest = some.Close();
    CHECK(rest && rest->size() == 10);
    CHECK(some.Close() == nullptr);
    CHECK(!some.Push(Make(kDisplay, 11), full) && some.Dropped() == 1);
}

// Producers on several threads: every event is either in a handed-out chunk or counted as dropped.
void Threads()
{
    Recorder recorder;
    std::atomic<uint64_t> recorded{};
    std::vector<std::thread> threads;
    for (int producer = 0; producer < 4; ++producer)
        threads.emplace_back([&recorder, &recorded, producer] {
            std::vector<Event>* full{};
            for (int i = 0; i < 20000; ++i)
                if (recorder.Push(Make(kQueue, producer*100000+i), full))
                {
                    recorded += full->size();
                    recorder.Return(full);
                }
        });
    for (auto& thread : threads) thread.join();
    std::vector<Event>* rest = recorder.Close();
    const uint64_t tail = rest ? rest->size() : 0;
    CHECK(recorded+tail+recorder.Dropped() == 80000);
}

// The file form: header then one line per event.
void File()
{
    std::vector<Event> events{Make(kQueue, 1), Make(kDisplay, 2)};
    const auto path = (std::filesystem::temp_directory_path()/"xemfg-pacing-diag-test.csv").wstring();
    std::FILE* file{};
    CHECK(_wfopen_s(&file, path.c_str(), L"w+b") == 0 && file);
    if (!file) return;
    CHECK(Write(file, events, 4, 2, 0));
    std::rewind(file);
    std::string text;
    char buffer[512]{};
    size_t read{};
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    CHECK(text.rfind("# pacediag.1 generation=4 chunk=2", 0) == 0);
    CHECK(text.find("\nQ,1,100,") != std::string::npos);
    CHECK(text.find("\nD,2,200,201,202,203,204,205\n") != std::string::npos);
}
}

int main(int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "rows") Rows();
    if (only.empty() || only == "chunks") Chunks();
    if (only.empty() || only == "threads") Threads();
    if (only.empty() || only == "file") File();
    std::printf("%s: %d checks, %d failures\n", gFailures ? "FAIL" : "PASS", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
