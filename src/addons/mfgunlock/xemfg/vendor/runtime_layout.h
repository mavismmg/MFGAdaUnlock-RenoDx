#pragma once
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace xess_layout {
struct Edge { size_t offset, end; int32_t local; unsigned kind; uint32_t key; };
struct Witness { const unsigned char* bytes; size_t size; const Edge* edges; size_t count; bool unwind; };
struct Binding { uint32_t key; size_t witness, offset, end; };
struct Family { const char* name; const Witness* witnesses; size_t count; const Binding* bindings; size_t bindingCount; };
}
#include "runtime_layout.generated.h"

namespace xess_layout {
// OptiScaler (upstream master 9171bf45) redirects its own libxell's exports with a Detours jump at each
// entry. A registered filter decides whether such an entry jump is the host's; only then a witness ignores the
// entry bytes the jump replaced. Without a filter (no host) witnesses match exactly.
using EntryRedirectFilter = bool (*)(const void* entry) noexcept;
inline std::atomic<EntryRedirectFilter> entryRedirectFilter{nullptr};

class Image {
    struct Section { uint32_t start, size, flags; };
    std::vector<Section> sections;
    uintptr_t base{};
    uint32_t bytes{};
    std::vector<RUNTIME_FUNCTION> functions;
    // Each executable section is copied once per Image and shared by every scanned witness.
    mutable std::map<uint32_t,std::vector<unsigned char>> code;
    const std::vector<unsigned char>& Code(const Section& section) const {
        auto& copy=code[section.start];
        if (copy.empty()) {
            copy.resize(section.size);
            if (!Read(section.start,copy.data(),copy.size())) { code.erase(section.start); throw std::runtime_error("layout_text_read"); }
        }
        return copy;
    }
public:
    explicit Image(HMODULE module) : base(reinterpret_cast<uintptr_t>(module)) {
        IMAGE_DOS_HEADER dos{};
        if (!Read(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x1000)
            throw std::runtime_error("layout_dos_identity");
        IMAGE_NT_HEADERS64 nt{};
        if (!Read(dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE
            || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) || !nt.FileHeader.NumberOfSections
            || nt.FileHeader.NumberOfSections>96 || nt.OptionalHeader.SizeOfImage>0x20000000)
            throw std::runtime_error("layout_pe_identity");
        bytes=nt.OptionalHeader.SizeOfImage;
        if (bytes<0x10000) throw std::runtime_error("layout_image_bounds");
        const uint32_t table=uint32_t(dos.e_lfanew)+sizeof(nt);
        for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
            IMAGE_SECTION_HEADER section{};
            if (!Read(table+i*sizeof(section),&section,sizeof(section)) || section.VirtualAddress>=bytes
                || section.Misc.VirtualSize>bytes-section.VirtualAddress)
                throw std::runtime_error("layout_section_bounds");
            for (const auto& old:sections)
                if (section.VirtualAddress<old.start+old.size && old.start<section.VirtualAddress+section.Misc.VirtualSize)
                    throw std::runtime_error("layout_section_overlap");
            sections.push_back({section.VirtualAddress,section.Misc.VirtualSize,section.Characteristics});
        }
        const auto dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir.Size || dir.Size%sizeof(RUNTIME_FUNCTION) || dir.Size>0x400000 || !Range(dir.VirtualAddress,dir.Size,3))
            throw std::runtime_error("layout_unwind_directory");
        functions.resize(dir.Size/sizeof(RUNTIME_FUNCTION));
        if (!Read(dir.VirtualAddress,functions.data(),dir.Size)) throw std::runtime_error("layout_unwind_read");
        uint32_t prior{};
        for (const auto& f:functions) {
            if (f.BeginAddress<prior || f.EndAddress<=f.BeginAddress || !Range(f.BeginAddress,f.EndAddress-f.BeginAddress,1))
                throw std::runtime_error("layout_unwind_bounds");
            prior=f.EndAddress;
        }
    }
    bool Read(uint32_t rva,void* output,size_t count) const noexcept {
        if (!base || !count || count>0x400000 || (bytes && (rva>=bytes || count>bytes-rva))) return false;
        uintptr_t p=base+rva, end=p+count;
        if (end<p) return false;
        for (auto next=p;next<end;) {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQuery(reinterpret_cast<void*>(next),&m,sizeof(m))!=sizeof(m) || m.AllocationBase!=reinterpret_cast<void*>(base)
                || m.Type!=MEM_IMAGE || m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
            const auto limit=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
            if (limit<=next) return false;
            next=std::min(limit,end);
        }
        SIZE_T got{};
        return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),output,count,&got) && got==count;
    }
    bool Range(uint32_t rva,size_t count,unsigned kind) const noexcept {
        for (const auto& s:sections) {
            if (rva<s.start || rva-s.start>=s.size || count>s.size-(rva-s.start)) continue;
            return kind==0 ? true : kind==1 ? (s.flags&IMAGE_SCN_MEM_EXECUTE)!=0
                : kind==2 ? (s.flags&IMAGE_SCN_MEM_WRITE)!=0 && !(s.flags&IMAGE_SCN_MEM_EXECUTE)
                : !(s.flags&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)) && (s.flags&IMAGE_SCN_MEM_READ);
        }
        return false;
    }
    uint32_t Target(uint32_t start,const Edge& edge) const {
        int32_t displacement{};
        if (!Read(start+uint32_t(edge.offset),&displacement,4)) throw std::runtime_error("layout_edge_read");
        const int64_t target=int64_t(start)+edge.end+displacement;
        if (target<=0 || target>=bytes || !Range(uint32_t(target),1,edge.kind)) throw std::runtime_error("layout_edge_owner");
        if (edge.local>=0 && target!=int64_t(start)+edge.local) throw std::runtime_error("layout_local_branch");
        return uint32_t(target);
    }
    // The entry bytes a host's 5-byte jump replaced (Detours fills the rest of the last replaced
    // instruction with int 3), when the filter attributes the jump to the host; 0 keeps the exact match. No
    // relative edge may lie in them, its displacement would be the jump's.
    size_t HostReplaced(uint32_t start,const std::vector<unsigned char>& data,const Witness& w) const noexcept {
        const auto filter=entryRedirectFilter.load(std::memory_order_acquire);
        if (!filter || w.size<16 || data[0]!=0xe9 || w.bytes[0]==0xe9) return 0;
        size_t replaced=5;
        while (replaced<w.size && data[replaced]==0xcc && w.bytes[replaced]!=0xcc) ++replaced;
        for (size_t i=0;i<w.count;++i) if (w.edges[i].offset<replaced) return 0;
        return filter(reinterpret_cast<const void*>(base+start)) ? replaced : 0;
    }
    std::vector<uint32_t> Find(const Witness& w) const {
        std::vector<uint32_t> result;
        std::vector<unsigned char> mask(w.size,1);
        for (size_t i=0;i<w.count;++i) {
            const auto& edge=w.edges[i];
            if (edge.offset+4>w.size || edge.end>w.size) throw std::runtime_error("layout_bad_witness");
            std::fill(mask.begin()+edge.offset,mask.begin()+edge.offset+4,static_cast<unsigned char>(0));
        }
        auto match=[&](uint32_t start) {
            std::vector<unsigned char> data(w.size);
            if (!Read(start,data.data(),data.size())) return;
            for (size_t i=HostReplaced(start,data,w);i<w.size;++i) if (mask[i] && data[i]!=w.bytes[i]) return;
            try { for (size_t i=0;i<w.count;++i) Target(start,w.edges[i]); }
            catch (...) { return; }
            result.push_back(start);
        };
        if (w.unwind) {
            for (const auto& f:functions) if (f.EndAddress-f.BeginAddress==w.size) match(f.BeginAddress);
        } else {
            const bool redirects=entryRedirectFilter.load(std::memory_order_relaxed)!=nullptr;
            for (const auto& section:sections) {
                if (!(section.flags&IMAGE_SCN_MEM_EXECUTE) || section.size<w.size) continue;
                const auto& code=Code(section);
                // MSVC leaf entry points in these two decoded families are 16-byte aligned.
                for (uint32_t offset=0;offset<=section.size-w.size;offset+=16) {
                    const unsigned char* at=code.data()+offset;
                    const bool redirect=at[0]==0xe9 && redirects;
                    if (at[0]!=w.bytes[0] && !redirect) continue;
                    // The copy rejects what match() would reject, without reading the image again.
                    // A host's entry jump can excuse at most the bytes HostReplaced counts, so the bytes after
                    // them must match either way. Every remaining candidate is still read and verified by match().
                    size_t i=1;
                    if (redirect && at[0]!=w.bytes[0] && w.size>=16) {
                        i=5;
                        while (i<w.size && at[i]==0xcc && w.bytes[i]!=0xcc) ++i;
                    }
                    while (i<w.size && (!mask[i] || at[i]==w.bytes[i])) ++i;
                    if (i==w.size) match(section.start+offset);
                }
            }
        }
        return result;
    }
};

// Object fields the RTXMFG code reads or patches next to the mapped sites. Each value is used by an
// instruction inside the family's witnessed functions, so a matched family proves it. XeFG 1.3.4 (fg95) moved
// the implementation's swap chain (+8 -> +10), the chain's native swap chain (+30 -> +38) and presenter
// (+120 -> +128; queue +28 stays), the override count (+16C -> +18C), and the keep-2x-model predicate's first
// test skips 0C bytes (was 09: its second compare uses a 32-bit displacement).
struct Fields {
    uint32_t implementationChain{8}, chainQueue{0x28}, chainNative{0x30}, chainPresenter{0x120}, overrideCount{0x16c};
    uint8_t keep2xSkip{0x09};
};
inline constexpr Fields kFg95Fields{0x10,0x28,0x38,0x128,0x18c,0x0c};

struct Layout {
    const char* family{};
    Fields fields{};
    std::map<uint32_t,uint32_t> addresses;
    std::map<uint32_t,std::vector<unsigned char>> witnessed;
    uint32_t Rva(uint32_t reference) const {
        const auto found=addresses.find(reference);
        if (found==addresses.end()) throw std::runtime_error("layout_address_unmapped");
        return found->second;
    }
    void VerifyFunction(HMODULE module,uint32_t reference) const {
        const auto rva=Rva(reference);
        const auto found=witnessed.find(rva);
        if (found==witnessed.end()) throw std::runtime_error("layout_function_unmapped");
        const Image image(module);
        std::vector<unsigned char> current(found->second.size());
        if (!image.Read(rva,current.data(),current.size()) || current!=found->second)
            throw std::runtime_error("layout_function_changed");
    }
};

inline Layout Discover(HMODULE module,bool fg) {
    const Image image(module);
    const auto version=GetProcAddress(module,fg ? "xefgSwapChainGetVersion" : "xellGetVersion");
    const auto address=reinterpret_cast<uintptr_t>(version),base=reinterpret_cast<uintptr_t>(module);
    struct Version { uint16_t major,minor,patch,build; } value{};
    if (!version || address<base || address-base>UINT32_MAX || !image.Range(uint32_t(address-base),5,1)
        || reinterpret_cast<int(__cdecl*)(Version*)>(version)(&value)!=0 || value.major!=1)
        throw std::runtime_error(fg ? "fg_feature_identity" : "ll_feature_identity");
    const auto families=fg ? std::vector<const Family*>(std::begin(generated::fg),std::end(generated::fg))
        : std::vector<const Family*>(std::begin(generated::ll),std::end(generated::ll));
    for (const auto* family:families) {
        try {
            Layout result{family->name,family==&generated::fg95 ? kFg95Fields : Fields{},{},{}};
            std::vector<uint32_t> found;
            for (size_t i=0;i<family->count;++i) {
                const auto candidates=image.Find(family->witnesses[i]);
                if (candidates.size()!=1) throw std::runtime_error("layout_witness_not_unique");
                found.push_back(candidates.front());
                std::vector<unsigned char> bytes(family->witnesses[i].size);
                if (!image.Read(candidates.front(),bytes.data(),bytes.size())) throw std::runtime_error("layout_witness_read");
                result.witnessed.emplace(candidates.front(),std::move(bytes));
            }
            for (size_t i=0;i<family->bindingCount;++i) {
                const auto& b=family->bindings[i];
                uint32_t target=found.at(b.witness)+uint32_t(b.offset);
                if (b.end) target=image.Target(found.at(b.witness),{b.offset,b.end,-1,0,0});
                if (!result.addresses.emplace(b.key,target).second) throw std::runtime_error("layout_duplicate_binding");
            }
            for (size_t i=0;i<family->count;++i) for (size_t j=0;j<family->witnesses[i].count;++j) {
                const auto& edge=family->witnesses[i].edges[j];
                if (edge.key && image.Target(found[i],edge)!=result.Rva(edge.key)) throw std::runtime_error("layout_edge_mismatch");
            }
            if (fg) {
                uintptr_t getter{};
                if (!image.Read(result.Rva(0x157c3a0)+0x80,&getter,sizeof(getter)) || getter<uintptr_t(module)
                    || getter-uintptr_t(module)>UINT32_MAX) throw std::runtime_error("layout_getter_pointer");
                const auto thunk=uint32_t(getter-uintptr_t(module));
                unsigned char opcode{};
                if (!image.Read(thunk,&opcode,1) || opcode!=0xe9
                    || image.Target(thunk,{1,5,-1,1,0})!=result.Rva(0x211ae0)) throw std::runtime_error("layout_getter_target");
                result.addresses.emplace(0xf410,thunk);
                const auto decodeThunk=result.Rva(0x11230);
                if (!image.Read(decodeThunk,&opcode,1) || opcode!=0xe9
                    || image.Target(decodeThunk,{1,5,-1,1,0})!=result.Rva(0x19fda0)) throw std::runtime_error("layout_decoder_target");
            }
            return result;
        } catch (const std::exception&) {}
    }
    throw std::runtime_error(fg ? "fg_layout_not_supported" : "ll_layout_not_supported");
}

inline const Layout& Get(HMODULE module,bool fg=true) {
    static std::mutex mutex;
    static std::map<std::pair<HMODULE,bool>,Layout> cache;
    std::lock_guard lock(mutex);
    const auto key=std::make_pair(module,fg);
    if (const auto it=cache.find(key);it!=cache.end()) return it->second;
    auto layout=Discover(module,fg);
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module) throw std::runtime_error("layout_module_pin");
    return cache.emplace(key,std::move(layout)).first->second;
}
inline uint32_t Rva(HMODULE module,uint32_t reference,bool fg=true) { return Get(module,fg).Rva(reference); }
inline bool Pair(HMODULE fg,HMODULE ll) noexcept {
    try { Get(fg); Get(ll,false); return true; } catch (...) { return false; }
}
}
