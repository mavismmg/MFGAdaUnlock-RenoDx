#include "host_unlock.h"
#include "runtime_layout.h"
#include <cstring>
#include <stdexcept>

namespace xess_host_unlock
{
namespace
{
void Write(uint8_t* address, const std::vector<uint8_t>& from, const std::vector<uint8_t>& to, DWORD protection,
    const char* name)
{
    if (memcmp(address, from.data(), from.size()) != 0) throw std::runtime_error(std::string(name)+"_unexpected_bytes");
    DWORD prior{};
    if (!VirtualProtect(address, to.size(), PAGE_EXECUTE_READWRITE, &prior))
        throw std::runtime_error(std::string(name)+"_make_writable");
    memcpy(address, to.data(), to.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, to.size()) != FALSE;
    DWORD temporary{};
    const bool restored = VirtualProtect(address, to.size(), prior, &temporary) != FALSE;
    MEMORY_BASIC_INFORMATION now{};
    const bool queried = VirtualQuery(address, &now, sizeof(now)) == sizeof(now);
    if (!flushed || !restored || !queried || now.Protect != protection || memcmp(address, to.data(), to.size()) != 0)
        throw std::runtime_error(std::string(name)+"_write_verification");
}
}

std::vector<Site> Sites(uint8_t n, bool keep2xModel, bool llGate, const xess_layout::Fields& fields)
{
    const auto low = uint8_t(fields.overrideCount), high = uint8_t(fields.overrideCount >> 8);
    const auto skip = fields.keep2xSkip;
    std::vector<Site> all{
        {"fg_override_clamp", true, 0x1A45C2, {0xC7,0x87,low,high,0,0,0x01,0,0,0}, {0xC7,0x87,low,high,0,0,n,0,0,0}},
        {"fg_default_ceiling", true, 0x1A517D, {0xBB,0x03,0,0,0}, {0xBB,n,0,0,0}},
        {"fg_reported_maximum", true, 0x20973B, {0xB8,0x01,0,0,0}, {0xB8,n,0,0,0}},
        {"fg_keep_2x_model", true, 0x1A5DE4, {0x74,skip}, {0xEB,uint8_t(skip-3)}},
        // cmp eax,1 / cmp edx,1 -> 0: a live count change to or from 1 no longer re-initializes XeFG.
        {"fg_live_count_no_reinit", true, 0x21B86B, {0x83,0xF8,0x01,0x74,0x0F,0x83,0xFA,0x01,0x74,0x0A},
            {0x83,0xF8,0x00,0x74,0x0F,0x83,0xFA,0x00,0x74,0x0A}},
        {"ll_generated_frames_gate", false, kLlGate, {0x83,0xFB,0x03,0x76,0x07}, {0x83,0xFB,n,0x76,0x07}},
    };
    if (!keep2xModel)
        std::erase_if(all, [](const Site& site) { return std::string(site.name) == "fg_keep_2x_model"; });
    if (!llGate) std::erase_if(all, [](const Site& site) { return !site.fg; });
    return all;
}

Unlock::Unlock(HMODULE fg, HMODULE ll, uint8_t generated, bool keep2xModel) : keeps2xModel(keep2xModel)
{
    if (generated < 2 || generated > 5) throw std::runtime_error("generated_count_range");
    const auto& fgLayout = xess_layout::Get(fg);
    const auto& llLayout = xess_layout::Get(ll, false);
    llGated = llLayout.addresses.contains(kLlGate);
    if (!llGated)
    {
        if (!llLayout.addresses.contains(kLlUngatedSetter)) throw std::runtime_error("ll_count_gate_unknown");
        // The witnessed ungated setter is still the one in the image (no gate added since discovery).
        llLayout.VerifyFunction(ll, kLlUngatedSetter);
    }
    sites = Sites(generated, keep2xModel, llGated, fgLayout.fields);
    for (auto& site : sites) site.rva = xess_layout::Rva(site.fg ? fg : ll, site.rva, site.fg);
    for (const auto& site : sites)
    {
        auto* address = reinterpret_cast<uint8_t*>(site.fg ? fg : ll)+site.rva;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != (site.fg ? fg : ll)
            || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
            || reinterpret_cast<uintptr_t>(address)+site.before.size()
                > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
            throw std::runtime_error(std::string(site.name)+"_ownership");
        if (memcmp(address, site.before.data(), site.before.size()) != 0)
            throw std::runtime_error(std::string(site.name)+"_unexpected_bytes");
        targets.push_back({&site, address, info.Protect});
    }
    size_t next = 0;
    try
    {
        for (; next < targets.size(); ++next)
        {
            const auto& target = targets[next];
            Write(target.address, target.site->before, target.site->after, target.protection, target.site->name);
            applied.push_back(*target.site);
        }
    }
    catch (...)
    {
        // A site whose write failed verification after the bytes changed is put back too.
        if (next < targets.size())
        {
            const auto& failed = targets[next];
            if (memcmp(failed.address, failed.site->after.data(), failed.site->after.size()) == 0)
                try { Write(failed.address, failed.site->after, failed.site->before, failed.protection, failed.site->name); }
                catch (...) {}
        }
        Restore();
        throw;
    }
}

void Unlock::Restore()
{
    for (size_t i = applied.size(); i-- > 0;)
    {
        const auto& target = targets[i];
        Write(target.address, target.site->after, target.site->before, target.protection, target.site->name);
    }
    applied.clear();
}

SceneDetour::SceneDetour(HMODULE fg, SceneDecision replacement)
{
    static constexpr uint8_t kEntry[] = {0x48,0x83,0xEC,0x48, 0xF3,0x0F,0x10,0x6A,0x08, 0x0F,0x57,0xC0};
    static constexpr uint8_t kThreshold[] = {0xF3,0x0F,0x10,0xB9}; // movss xmm7,[rcx+disp32]
    constexpr size_t kThresholdAt = 0x78;
    if (!replacement) throw std::runtime_error("scene_replacement_missing");
    const auto& layout = xess_layout::Get(fg);
    rva = layout.Rva(kSceneDecision);
    layout.VerifyFunction(fg, kSceneDecision); // unchanged since discovery: the family's witness
    const auto& body = layout.witnessed.at(rva);
    if (body.size() < kThresholdAt+8 || memcmp(body.data(), kEntry, sizeof(kEntry)) != 0)
        throw std::runtime_error("scene_entry_bytes");
    if (memcmp(body.data()+kThresholdAt, kThreshold, sizeof(kThreshold)) != 0) throw std::runtime_error("scene_threshold_field");
    memcpy(&thresholdOffset, body.data()+kThresholdAt+sizeof(kThreshold), sizeof(thresholdOffset));
    if (thresholdOffset < 0x40 || thresholdOffset > 0x400 || thresholdOffset % 4) throw std::runtime_error("scene_threshold_field");
    before.assign(std::begin(kEntry), std::end(kEntry));
    const auto target = reinterpret_cast<uint64_t>(replacement);
    after = {0x48,0xB8}; // mov rax,imm64
    for (int i = 0; i < 8; ++i) after.push_back(uint8_t(target >> (8*i)));
    after.insert(after.end(), {0xFF,0xE0}); // jmp rax
    address = reinterpret_cast<uint8_t*>(fg)+rva;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != fg
        || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
        || reinterpret_cast<uintptr_t>(address)+after.size() > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
        throw std::runtime_error("fg_scene_change_off_ownership");
    protection = info.Protect;
    try { Write(address, before, after, protection, "fg_scene_change_off"); }
    catch (...)
    {
        // Written but not verified (protection or readback): put the entry back.
        if (memcmp(address, after.data(), after.size()) == 0)
            try { Write(address, after, before, protection, "fg_scene_change_off"); } catch (...) {}
        throw;
    }
    applied = true;
}

void SceneDetour::Restore()
{
    if (!applied) return;
    Write(address, after, before, protection, "fg_scene_change_off");
    applied = false;
}

namespace
{
struct RingShape
{
    std::vector<uint8_t> bytes; // up to the call's displacement
    uint32_t countOffset;
};
// lea rbx,[r15+108h] / mov qword ptr [rbx+8],1 / mov rcx,rbx / call (fg78, fg68)
const RingShape kRingRbx{{0x49,0x8D,0x9F,0x08,0x01,0x00,0x00, 0x48,0xC7,0x43,0x08,0x01,0x00,0x00,0x00, 0x48,0x8B,0xCB,
    0xE8}, 0x110};
// lea rcx,[rdi+110h] / mov qword ptr [rcx+8],1 / call (fg95)
const RingShape kRingRcx{{0x48,0x8D,0x8F,0x10,0x01,0x00,0x00, 0x48,0xC7,0x41,0x08,0x01,0x00,0x00,0x00, 0xE8}, 0x118};
constexpr size_t kRingMov = 7, kRingImmediate = 11;

struct Image
{
    const uint8_t* base{};
    const IMAGE_NT_HEADERS64* nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
};
// `name` prefixes the reasons (the HDR10 flag shares these with the ring).
Image Headers(HMODULE module, const std::string& name = "fg_output_ring")
{
    Image image{reinterpret_cast<const uint8_t*>(module)};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    if (!image.base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
        throw std::runtime_error(name+"_image");
    image.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base+dos->e_lfanew);
    if (image.nt->Signature != IMAGE_NT_SIGNATURE || image.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || image.nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !image.nt->FileHeader.NumberOfSections
        || image.nt->FileHeader.NumberOfSections > 96)
        throw std::runtime_error(name+"_image");
    const auto* first = IMAGE_FIRST_SECTION(image.nt);
    for (unsigned i = 0; i < image.nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& section = first[i];
        if (section.VirtualAddress >= image.nt->OptionalHeader.SizeOfImage
            || section.Misc.VirtualSize > image.nt->OptionalHeader.SizeOfImage-section.VirtualAddress)
            throw std::runtime_error(name+"_image");
        image.sections.push_back(section);
    }
    return image;
}
// Every offset in the executable sections where `shape` matches; the 4 bytes after it (the call's displacement) are
// free, the call target must lie in an executable section.
std::vector<uint32_t> FindRing(const Image& image, const RingShape& shape)
{
    std::vector<uint32_t> found;
    const auto size = shape.bytes.size();
    for (const auto& section : image.sections)
    {
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.Misc.VirtualSize < size+4) continue;
        const uint8_t* at = image.base+section.VirtualAddress;
        for (uint32_t offset = 0; offset+size+4 <= section.Misc.VirtualSize; ++offset)
        {
            if (at[offset] != shape.bytes[0] || memcmp(at+offset, shape.bytes.data(), size) != 0) continue;
            int32_t displacement{};
            memcpy(&displacement, at+offset+size, 4);
            const int64_t target = int64_t(section.VirtualAddress)+offset+size+4+displacement;
            bool executable{};
            for (const auto& other : image.sections)
                executable |= (other.Characteristics & IMAGE_SCN_MEM_EXECUTE) && target >= other.VirtualAddress
                    && target < int64_t(other.VirtualAddress)+other.Misc.VirtualSize;
            if (executable) found.push_back(section.VirtualAddress+offset);
        }
    }
    return found;
}
// The per-frame entry index: div qword ptr [r13+count] (49 F7 B5 disp32).
bool CountRead(const Image& image, uint32_t countOffset)
{
    uint8_t pattern[7]{0x49,0xF7,0xB5};
    memcpy(pattern+3, &countOffset, 4);
    for (const auto& section : image.sections)
    {
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.Misc.VirtualSize < sizeof(pattern)) continue;
        const uint8_t* at = image.base+section.VirtualAddress;
        for (uint32_t offset = 0; offset+sizeof(pattern) <= section.Misc.VirtualSize; ++offset)
            if (at[offset] == 0x49 && memcmp(at+offset, pattern, sizeof(pattern)) == 0) return true;
    }
    return false;
}
// The mov is an immediate store, never an address: a base relocation inside it means another image layout.
void RejectRelocations(const Image& image, uint32_t start, uint32_t end, const std::string& name = "fg_output_ring")
{
    const auto& directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!directory.VirtualAddress || !directory.Size) return;
    if (directory.VirtualAddress > image.nt->OptionalHeader.SizeOfImage
        || directory.Size > image.nt->OptionalHeader.SizeOfImage-directory.VirtualAddress)
        throw std::runtime_error(name+"_relocations");
    for (size_t offset = 0; offset+sizeof(IMAGE_BASE_RELOCATION) <= directory.Size;)
    {
        const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image.base+directory.VirtualAddress+offset);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > directory.Size-offset)
            throw std::runtime_error(name+"_relocations");
        const auto* entry = reinterpret_cast<const WORD*>(block+1);
        for (size_t i = 0; i < (block->SizeOfBlock-sizeof(IMAGE_BASE_RELOCATION))/sizeof(WORD); ++i)
        {
            if ((entry[i] >> 12) == IMAGE_REL_BASED_ABSOLUTE) continue;
            const uint32_t at = block->VirtualAddress+(entry[i] & 0xfff);
            if (at+8 > start && at < end) throw std::runtime_error(name+"_relocated");
        }
        offset += block->SizeOfBlock;
    }
}

// XeFG's SetColorSpace1 proxy, 0 = wildcard (displacements): mov [rsp+8],rbx / push rdi / sub rsp,20h /
// mov edi,edx / mov rbx,rcx / mov rcx,[rcx+context] / xor edx,edx / cmp edi,0Ch / sete dl / call rel32 /
// mov rcx,[rbx+native] / mov edx,edi / mov rax,[rcx] / mov rax,[rax+130h] / mov rbx,[rsp+30h] / add rsp,20h /
// pop rdi / jmp qword ptr [rip+rel32].
constexpr uint8_t kColorSpaceProxy[] = {0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x20, 0x8B,0xFA, 0x48,0x8B,0xD9,
    0x48,0x8B,0x49,0x00, 0x33,0xD2, 0x83,0xFF,0x0C, 0x0F,0x94,0xC2, 0xE8,0x00,0x00,0x00,0x00, 0x48,0x8B,0x4B,0x00, 0x8B,0xD7,
    0x48,0x8B,0x01, 0x48,0x8B,0x80,0x30,0x01,0x00,0x00, 0x48,0x8B,0x5C,0x24,0x30, 0x48,0x83,0xC4,0x20, 0x5F, 0x48,0xFF,0x25};
constexpr size_t kProxyContext = 18, kProxySete = 24, kProxyCall = 27, kProxyNative = 35;
constexpr bool ProxyWildcard(size_t i) { return i == kProxyContext || i == kProxyNative || (i > kProxyCall && i <= kProxyCall+4); }
// The forwarder the call reaches: mov [rsp+8],rbx / push rdi / sub rsp,20h / mov rdi,rcx / mov ebx,edx / mov ecx,edx /
// call (colour-space check) / mov rcx,[rdi+5F8h] / mov edx,ebx / call (inner context).
constexpr uint8_t kColorSpaceForward[] = {0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xF9, 0x8B,0xDA,
    0x8B,0xCA, 0xE8,0x00,0x00,0x00,0x00, 0x48,0x8B,0x8F,0xF8,0x05,0x00,0x00, 0x8B,0xD3, 0xE8};
constexpr bool ForwardWildcard(size_t i) { return i >= 18 && i <= 21; }
constexpr uint8_t kSete[] = {0x0F,0x94,0xC2}, kNop3[] = {0x0F,0x1F,0x00}; // sete dl / nop dword ptr [rax]

bool Executable(const Image& image, uint64_t rva, size_t size)
{
    for (const auto& section : image.sections)
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress
            && rva+size <= uint64_t(section.VirtualAddress)+section.Misc.VirtualSize)
            return true;
    return false;
}
// The rel32 target of the call or jmp whose displacement starts at `at`.
uint64_t RelativeTarget(const Image& image, uint32_t at)
{
    int32_t displacement{};
    memcpy(&displacement, image.base+at, 4);
    return int64_t(at)+4+displacement;
}
}

OutputRing::OutputRing(HMODULE fg, uint8_t depth) : depth(depth)
{
    if (depth < 2 || depth > 4) throw std::runtime_error("fg_output_ring_depth");
    const auto image = Headers(fg);
    const std::string family = xess_layout::Get(fg).family;
    const auto& shape = family == "fg95" ? kRingRcx : kRingRbx;
    const auto found = FindRing(image, shape);
    if (found.empty()) throw std::runtime_error("fg_output_ring_not_found");
    if (found.size() != 1) throw std::runtime_error("fg_output_ring_ambiguous");
    // The lea's displacement + 8 is the count the mov writes; the per-frame index must read that field.
    uint32_t array{};
    memcpy(&array, shape.bytes.data()+3, 4);
    countOffset = array+8;
    if (countOffset != shape.countOffset || !CountRead(image, countOffset)) throw std::runtime_error("fg_output_ring_count_field");
    rva = found.front()+uint32_t(kRingMov);
    RejectRelocations(image, rva, rva+8);
    immediate = const_cast<uint8_t*>(image.base)+found.front()+kRingImmediate;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(immediate, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != fg
        || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
        || reinterpret_cast<uintptr_t>(immediate)+4 > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
        throw std::runtime_error("fg_output_ring_ownership");
    protection = info.Protect;
    try { Write(immediate, {0x01}, {depth}, protection, "fg_output_ring"); }
    catch (...)
    {
        if (*immediate == depth)
            try { Write(immediate, {depth}, {0x01}, protection, "fg_output_ring"); } catch (...) {}
        throw;
    }
}

Hdr10Flag::Hdr10Flag(HMODULE fg)
{
    const auto image = Headers(fg, "fg_hdr10_flag");
    const auto& layout = xess_layout::Get(fg);
    const std::string family = layout.family;
    if (family != "fg78" && family != "fg95") throw std::runtime_error("fg_hdr10_flag_family");
    std::vector<uint32_t> found;
    for (const auto& section : image.sections)
    {
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.Misc.VirtualSize < sizeof(kColorSpaceProxy)+4) continue;
        const uint8_t* at = image.base+section.VirtualAddress;
        for (uint32_t offset = 0; offset+sizeof(kColorSpaceProxy)+4 <= section.Misc.VirtualSize; ++offset)
        {
            if (at[offset] != kColorSpaceProxy[0]) continue;
            size_t i = 1;
            while (i < sizeof(kColorSpaceProxy) && (ProxyWildcard(i) || at[offset+i] == kColorSpaceProxy[i])) ++i;
            if (i == sizeof(kColorSpaceProxy)) found.push_back(section.VirtualAddress+offset);
        }
    }
    if (found.empty()) throw std::runtime_error("fg_hdr10_flag_not_found");
    if (found.size() != 1) throw std::runtime_error("fg_hdr10_flag_ambiguous");
    rva = found.front();
    const uint8_t* proxy = image.base+rva;
    if (proxy[kProxyNative] != layout.fields.chainNative) throw std::runtime_error("fg_hdr10_flag_native_field");
    contextOffset = proxy[kProxyContext];
    if (contextOffset != (family == "fg95" ? 0x50u : 0x48u)) throw std::runtime_error("fg_hdr10_flag_context_field");
    // The call reaches the forwarder to the inner context, directly or through one jmp thunk.
    uint64_t target = RelativeTarget(image, rva+uint32_t(kProxyCall)+1);
    if (!Executable(image, target, 5)) throw std::runtime_error("fg_hdr10_flag_call_target");
    if (image.base[target] == 0xE9)
    {
        target = RelativeTarget(image, uint32_t(target)+1);
        if (!Executable(image, target, 1) || image.base[target] == 0xE9) throw std::runtime_error("fg_hdr10_flag_call_target");
    }
    if (!Executable(image, target, sizeof(kColorSpaceForward)+4)) throw std::runtime_error("fg_hdr10_flag_forwarder");
    for (size_t i = 0; i < sizeof(kColorSpaceForward); ++i)
        if (!ForwardWildcard(i) && image.base[target+i] != kColorSpaceForward[i]) throw std::runtime_error("fg_hdr10_flag_forwarder");
    if (!Executable(image, RelativeTarget(image, uint32_t(target)+18), 1)
        || !Executable(image, RelativeTarget(image, uint32_t(target)+sizeof(kColorSpaceForward)), 1))
        throw std::runtime_error("fg_hdr10_flag_forwarder");
    RejectRelocations(image, rva+uint32_t(kProxySete), rva+uint32_t(kProxySete+sizeof(kSete)), "fg_hdr10_flag");
    sete = const_cast<uint8_t*>(image.base)+rva+kProxySete;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(sete, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != fg
        || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
        || reinterpret_cast<uintptr_t>(sete)+sizeof(kSete) > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
        throw std::runtime_error("fg_hdr10_flag_ownership");
    protection = info.Protect;
    const std::vector<uint8_t> stock(std::begin(kSete), std::end(kSete)), nop(std::begin(kNop3), std::end(kNop3));
    try { Write(sete, stock, nop, protection, "fg_hdr10_flag"); }
    catch (...)
    {
        // Written but not verified (protection or readback): put the sete back.
        if (memcmp(sete, kNop3, sizeof(kNop3)) == 0)
            try { Write(sete, nop, stock, protection, "fg_hdr10_flag"); } catch (...) {}
        throw;
    }
}
}
