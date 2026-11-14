#pragma once
#include <Windows.h>
#include "runtime_layout.h"
#include <cstdint>
#include <string>
#include <vector>

namespace xess_host_unlock
{
// The frame-count unlock for OptiScaler's own XeFG output (FGOutput xefg), whose
// contexts RTXMFG leaves untracked. runtime_layout.h discovers the verified SDK 3.x
// instruction families and maps these reference RVAs into the actual loaded pair.
//  - fg 0x1A45C2: the override clamp, mov [rdi+16Ch],1 -> generated;
//  - fg 0x1A517D: the default ceiling, mov ebx,3 -> generated;
//  - fg 0x20973B: the reported maximum, mov eax,1 -> generated;
//  - fg 0x1A5DE4: for more than one generated frame XeFG swaps its 2x model for an MFG model
//    (18 -> 16, absent from this build); the predicate is forced true, so the 2x model stays;
//  - fg 0x21B86B: Dynamic MFG. The live count setter (0x21B860) re-initializes the whole pipeline when the
//    count changes to or from 1 (100-350 ms on the caller's thread, and 1 gets the 2x model); both compares are made
//    to never match, so every live count runs on the pipeline created at capacity. 2x then comes from the model that
//    serves the other counts (the ported MFG model: exactly its t=1/2 frame; with fg_keep_2x_model the 2x model, as
//    stock).
//  - ll 0xD1C9:  xellSetGeneratedFramesCount rejects counts above 3, cmp ebx,3 -> generated.
// The override count's offset and the keep-2x-model branch come from the discovered family's
// fields (XeFG 1.3.4: mov [rdi+18Ch],1 and je +0C -> jmp +09); everything else is the same bytes.
// Every site is checked (decoded layout, committed execute-read image page owned by the module,
// exact bytes) before any is written; writes restore the protection and are read back.
// XeLL 1.2.x (SDK 2.1.0/2.1.1, what OptiScaler master pairs its XeFG with when the game
// ships one) has no count gate. Its layout family witnesses the whole setter and binds only its entry
// (0xD1B0); the ll site is left out, that module is not written, and the setter must still match.
inline constexpr uint32_t kLlGate = 0xD1C9;
inline constexpr uint32_t kLlUngatedSetter = 0xD1B0;
struct Site
{
    const char* name;
    bool fg;
    uint32_t rva;
    std::vector<uint8_t> before, after;
};
// keep2xModel=false leaves fg_keep_2x_model out, so XeFG remaps its 2x model to the MFG model
// for more than one generated frame (the game route publishes the ported model 16 for it).
std::vector<Site> Sites(uint8_t generated, bool keep2xModel = true, bool llGate = true,
    const xess_layout::Fields& fields = {});

class Unlock
{
public:
    // All sites or none; throws std::runtime_error with the failing site and reason.
    Unlock(HMODULE fg, HMODULE ll, uint8_t generated, bool keep2xModel = true);
    bool Keeps2xModel() const noexcept { return keeps2xModel; }
    ~Unlock() = default; // production keeps it for the process lifetime (contexts may use it)
    Unlock(const Unlock&) = delete;
    Unlock& operator=(const Unlock&) = delete;
    // Writes the original bytes back (tests; never while a context exists).
    void Restore();
    const std::vector<Site>& Applied() const noexcept { return applied; }
private:
    struct Target { const Site* site; uint8_t* address; DWORD protection; };
    std::vector<Site> sites;
    std::vector<Site> applied;
    bool keeps2xModel{true};
    bool llGated{true};
    std::vector<Target> targets;
};

// XeFG's scene-change decision, bool(swap chain, previous view matrix, current view matrix). When the
// dot product of both real frames' view-matrix column 2 (elements 2, 6, 10) is below the swap chain's threshold
// (default 0.7: a turn of about 45.6 degrees between two real frames) every generated output of that batch is a
// copy of the real frame (freeze, then jump). Fast mouse turns at a low base frame rate pass that angle. The game's
// resetHistory takes another path and still resets. The witnessed decision (fg78 0x18A300, fg68 0x18A2B0, fg95
// 0x18A6A0) starts with the same 12 bytes in every family (sub rsp,48 / movss xmm5,[rdx+8] / xorps xmm0,xmm0); they
// become mov rax,replacement / jmp rax, and the body is not reached again.
inline constexpr uint32_t kSceneDecision = 0x18A300;
using SceneDecision = bool (*)(void* swapChain, const float* previous, const float* current);
class SceneDetour
{
public:
    // Checks the whole decision is the witnessed one, the committed execute-read image page and the entry bytes,
    // then writes and reads back; throws std::runtime_error with the reason.
    SceneDetour(HMODULE fg, SceneDecision replacement);
    ~SceneDetour() = default; // production keeps it for the process lifetime
    SceneDetour(const SceneDetour&) = delete;
    SceneDetour& operator=(const SceneDetour&) = delete;
    // Writes the original entry back (tests).
    void Restore();
    uint32_t Rva() const noexcept { return rva; }
    // The swap chain's threshold field the decision reads (movss xmm7,[rcx+disp32] at +0x78): +D8, fg95 +E0.
    uint32_t ThresholdOffset() const noexcept { return thresholdOffset; }
    bool Applied() const noexcept { return applied; }
private:
    uint8_t* address{};
    uint32_t rva{}, thresholdOffset{};
    DWORD protection{};
    std::vector<uint8_t> before, after;
    bool applied{};
};

// XeFG's generated-output ring. The chain keeps its output textures in a ring of entries (fg78/fg68: array chain+0x108,
// count +0x110; fg95: +0x110/+0x118), set to ONE entry by the allocator (fg78 0x20792F / fg68 0x2078DF
// `mov qword ptr [rbx+8],1`, fg95 0x203B0D `mov qword ptr [rcx+8],1`, each followed by the call that sizes the array).
// Each frame's generation is recorded on the game's queue and first waits on its entry's release value (fg78 0x20B65E
// frame % count, 0x20B69C Wait). The presenter signals that value only after the batch's paced real frame. With one
// entry the next generation, and all of the game's GPU work queued after it, waits until the previous batch has been
// presented: the GPU idles while the outputs are paced, and the real frame -> next first output gap is generation +
// copies instead of the interval. Two entries let generation k+1 fill entry (k+1)%2 while batch k is presented; each
// entry's own release value still gates its reuse. The array sizing, per-frame indexing, output allocation (fg95: on
// demand per count, over every entry) and teardown all use the stored count. The only byte written is the immediate.
// The instruction is found by exact bytes (the call's displacement aside) in the image's executable sections and must
// be unique; the count field it writes must be the one a per-frame `div qword ptr [r13+count]` reads; no base
// relocation may touch it. Throws std::runtime_error with the reason.
class OutputRing
{
public:
    OutputRing(HMODULE fg, uint8_t depth);
    ~OutputRing() = default; // production keeps it for the process lifetime (chains are sized with it)
    OutputRing(const OutputRing&) = delete;
    OutputRing& operator=(const OutputRing&) = delete;
    uint32_t Rva() const noexcept { return rva; } // the mov instruction
    uint32_t CountOffset() const noexcept { return countOffset; }
    uint8_t Depth() const noexcept { return depth; }
private:
    uint8_t* immediate{};
    uint32_t rva{}, countOffset{};
    uint8_t depth{};
    DWORD protection{};
};

// XeFG's HDR10 (PQ) flag. The proxy swap chain's SetColorSpace1 (fg78 0x20DBC0, fg95 0x20A600) passes
// `space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (0xC) through the context (+48h, fg95 +50h) and its inner
// context (+5F8h) to XefgContext vtable +98h, the only writer of the model's flag (XefgModel +14Ch); the model copies
// it into the shaders' per-frame constants. A re-init builds a new model with the flag 0. Set, bright HDR10 areas
// interpolate badly on high-peak-brightness displays; forcing re-inits would clear it but also undo
// fg_live_count_no_reinit. The `sete dl` becomes a 3-byte nop: the call passes the preceding xor's 0 and the colour
// space still reaches the native swap chain (vtable +130h). The function is found by exact bytes (displacements aside)
// in the executable sections and must be unique; its native-chain field must be the family's, its context field the
// family's, and its call must reach (through at most one jmp thunk) the forwarder to the inner context +5F8h. fg78 and
// fg95 only. Throws std::runtime_error with the reason.
class Hdr10Flag
{
public:
    explicit Hdr10Flag(HMODULE fg);
    ~Hdr10Flag() = default; // production keeps it for the process lifetime (proxy swap chains call it)
    Hdr10Flag(const Hdr10Flag&) = delete;
    Hdr10Flag& operator=(const Hdr10Flag&) = delete;
    uint32_t Rva() const noexcept { return rva; } // the proxy's SetColorSpace1
    uint32_t ContextOffset() const noexcept { return contextOffset; }
private:
    uint8_t* sete{};
    uint32_t rva{}, contextOffset{};
    DWORD protection{};
};
}
