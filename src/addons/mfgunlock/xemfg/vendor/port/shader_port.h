#include "../runtime_payload.h"
// Exact-build, process-local shader additions. Unknown keys use the provider's
// original decoder and retain its native failure behavior; a decoded
// program identical to one RTXMFG replaces is swapped for RTXMFG's own shader.
// Install before any context; retain programs and relay until after exact
// context destruction.
#include <map>
#include <sstream>
#include <atomic>
class ShaderPort {
    using Decode = void (*)(void*,const void*,void*);
    using Resize = void (*)(void*,size_t);
    inline static ShaderPort* instance{};
    uintptr_t base{};
    unsigned char* site{};
    void* relay{};
    LONG original{},replacement{};
    DWORD protection{};
    Decode decode{};
    Resize resize{};
    bool active{};
    std::atomic<unsigned> callers{};
    std::filesystem::path captures;
    std::map<std::string,std::vector<unsigned char>> programs;
    std::vector<xess_payload::Replacement> replacements;
    std::mutex nativeKeyMutex;
    std::map<std::string,bool> nativeKeys;
    static void read(uintptr_t p,void* out,size_t count) {
        SIZE_T got{};
        if (!p || count>1000000 || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,count,&got) || got!=count)
            throw std::runtime_error("shader_port_read");
    }
    template<class T> static T get(uintptr_t p) { T out{}; read(p,&out,sizeof(out)); return out; }
    static std::string string(uintptr_t p) {
        const auto n=get<size_t>(p+16),cap=get<size_t>(p+24);
        if (!n || n>128 || cap<n || cap>65536) throw std::runtime_error("shader_port_string");
        std::string out(n,'\0'); read(cap>=16 ? get<uintptr_t>(p) : p,out.data(),n);
        for (auto c:out) if (c<' ' || c>'~') throw std::runtime_error("shader_port_string_ascii");
        return out;
    }
    void parameters(const std::string& key) {
        CONTEXT c{}; RtlCaptureContext(&c);
        for (unsigned i=0;i<16 && c.Rip;++i) {
            if (c.Rip>=base+0x1aca60 && c.Rip<base+0x1ad611) {
                const auto p=get<uintptr_t>(c.Rsp+0x38);
                const auto begin=get<uintptr_t>(p),end=get<uintptr_t>(p+8),cap=get<uintptr_t>(p+16);
                if (end<begin || cap<end || end-begin>72*64 || (end-begin)%72) throw std::runtime_error("shader_port_parameters");
                std::string fields="\"key\":\""+key+"\",\"parameters\":[";
                for (auto x=begin;x<end;x+=72) {
                    if (x!=begin) fields+=',';
                    fields+="[\""+escape(string(x+8))+"\",\""+escape(string(x+40))+"\"]";
                }
                fields+="]";
                const auto block=get<uintptr_t>(c.Rsp+0x40);
                const auto vt=get<uintptr_t>(block);
                if (vt<base || vt>=base+0x15f0000) throw std::runtime_error("shader_port_block_owner");
                fields+=",\"block_vtable_rva\":"+std::to_string(vt-base);
                if (vt==base+0xf41da8 || vt==base+0xf41ea8) {
                    for (auto [offset,suffix]:std::array<std::pair<unsigned,const char*>,3>{{{0x150,"weights"},{0x168,"bias"},{0x180,"activation"}}}) {
                        if (vt==base+0xf41ea8 && offset==0x180) continue;
                        const auto start=get<uintptr_t>(block+offset),tensorEnd=get<uintptr_t>(block+offset+8),tensorCap=get<uintptr_t>(block+offset+16);
                        if (!start || tensorEnd<=start || tensorCap<tensorEnd || tensorEnd-start>2000000) throw std::runtime_error("shader_port_tensor_span");
                        std::vector<unsigned char> bytes(tensorEnd-start);
                        for(size_t off=0;off<bytes.size();off+=500000) read(start+off,bytes.data()+off,std::min(size_t(500000),bytes.size()-off));
                        const auto path=captures/(key+"-"+suffix+".bin");
                        if (std::filesystem::exists(path)) {
                            std::ifstream input(path,std::ios::binary);
                            std::vector<unsigned char> prior((std::istreambuf_iterator<char>(input)),{});
                            if (prior!=bytes) throw std::runtime_error("shader_port_capture_collision");
                        } else {
                            std::ofstream output(path,std::ios::binary); output.write(reinterpret_cast<char*>(bytes.data()),bytes.size());
                            if (!output) throw std::runtime_error("shader_port_capture_write");
                        }
                        fields+=",\""+std::string(suffix)+"_bytes\":"+std::to_string(bytes.size());
                    }
                    auto metadata=get<uintptr_t>(block+0x148);
                    fields+=",\"positive_scale\":"+std::to_string(get<int>(metadata+0x8c))+
                        ",\"positive_shift\":"+std::to_string(get<unsigned char>(metadata+0x90))+
                        ",\"negative_shift\":"+std::to_string(get<unsigned char>(metadata+0x94));
                    if (vt==base+0xf41ea8) {
                        const auto descriptor=get<uintptr_t>(block+0x140);
                        fields+=",\"output_channels\":"+std::to_string(get<unsigned>(descriptor+0x60));
                    }
                }
                logEvent("shader_parameters",fields);
                return;
            }
            DWORD64 imageBase{}; auto entry=RtlLookupFunctionEntry(c.Rip,&imageBase,nullptr);
            if (!entry) break;
            PVOID data{}; DWORD64 establisher{};
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,imageBase,c.Rip,entry,&c,&data,&establisher,nullptr);
        }
        throw std::runtime_error("shader_port_pipeline_frame");
    }
    // A program from the provider's own decoder is replaced only when its decoded bytes are
    // exactly the Intel program the replacement was proven byte-identical against (SHA-256 and size);
    // any other provider build or program keeps Intel's bytes.
    void native(const std::string& key,void* output) {
        const auto o=reinterpret_cast<uintptr_t>(output);
        const auto start=get<uintptr_t>(o),end=get<uintptr_t>(o+8);
        if (!start || end<start || end-start>1000000) throw std::runtime_error("shader_port_decoded_shape");
        std::vector<unsigned char> decoded(end-start);
        if (!decoded.empty()) read(start,decoded.data(),decoded.size());
        const auto hash=xess_payload::Hash(decoded);
        {
            std::lock_guard lock(nativeKeyMutex);
            if (nativeKeys.size()<64 && nativeKeys.emplace(key,true).second)
                logEvent("shader_native_lookup","\"key\":\""+key+"\",\"bytes\":"+std::to_string(decoded.size())+
                    ",\"sha256\":\""+hash+"\"");
        }
        for (const auto& r:replacements) {
            if (r.nativeBytes!=decoded.size() || r.nativeHash!=hash) continue;
            resize(output,r.bytes.size());
            const auto at=get<uintptr_t>(o),stop=get<uintptr_t>(o+8),cap=get<uintptr_t>(o+16);
            if (!at || stop-at!=r.bytes.size() || cap<stop) throw std::runtime_error("shader_port_output_allocation");
            memcpy(reinterpret_cast<void*>(at),r.bytes.data(),r.bytes.size());
            logEvent("shader_replaced","\"key\":\""+key+"\",\"native_sha256\":\""+hash+"\",\"replacement\":\""+
                r.name+"\",\"bytes\":"+std::to_string(r.bytes.size()));
            return;
        }
    }
    static void dispatch(void* store,const void* keyObject,void* output) {
        auto self=instance;
        if (!self) TerminateProcess(GetCurrentProcess(),100);
        struct Call { std::atomic<unsigned>& count; explicit Call(std::atomic<unsigned>& c):count(c){++count;} ~Call(){--count;} } call(self->callers);
        const auto key=string(reinterpret_cast<uintptr_t>(keyObject));
        for(auto c:key) if(c<'0'||c>'9') throw std::runtime_error("shader_port_key");
        // Pipeline parameter/tensor capture is excluded from the game build.
        // The provider decoder, program bytes and dispatch math are unchanged.
#if defined(XESS_INIT_PROBE)
        if (key.starts_with("351")) self->parameters(key);
#endif
        auto found=self->programs.find(key);
        if (found==self->programs.end()) {
            try { self->decode(store,keyObject,output); }
            catch(...) {
                logEvent("shader_decode_failure","\"key\":\""+key+"\"");
                throw;
            }
            self->native(key,output);
            return;
        }
        const auto& bytes=found->second;
        self->resize(output,bytes.size());
        auto start=get<uintptr_t>(reinterpret_cast<uintptr_t>(output));
        auto end=get<uintptr_t>(reinterpret_cast<uintptr_t>(output)+8);
        auto cap=get<uintptr_t>(reinterpret_cast<uintptr_t>(output)+16);
        if (!start || end-start!=bytes.size() || cap<end) throw std::runtime_error("shader_port_output_allocation");
        memcpy(reinterpret_cast<void*>(start),bytes.data(),bytes.size());
        logEvent("shader_supplied","\"key\":\""+key+"\",\"bytes\":"+std::to_string(bytes.size()));
    }
    void change(LONG from,LONG to) {
        DWORD old{},unused{};
        if (!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&old)) throw std::runtime_error("shader_port_protection");
        LONG prior=InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(site+1),to,from);
        bool flushed=FlushInstructionCache(GetCurrentProcess(),site,5)!=FALSE;
        bool restored=VirtualProtect(site,5,old,&unused)!=FALSE;
        MEMORY_BASIC_INFORMATION state{};
        if (prior!=from || !flushed || !restored || old!=protection ||
            VirtualQuery(site,&state,sizeof(state))!=sizeof(state) || state.Protect!=protection || get<LONG>(reinterpret_cast<uintptr_t>(site+1))!=to)
            TerminateProcess(GetCurrentProcess(),101);
    }
public:
    bool hasProgram(const char* key) const { return programs.contains(key); }
    // adapterVendor = PCI vendor of the provider's device (0 = unknown).
    explicit ShaderPort(HMODULE module,HMODULE payload,const std::filesystem::path& dataRoot = {},UINT adapterVendor = 0) {
        const auto& layout=xess_layout::Get(module);
        base=reinterpret_cast<uintptr_t>(module); site=reinterpret_cast<unsigned char*>(base+layout.Rva(0x1ad217));
        MEMORY_BASIC_INFORMATION m{};
        original=get<LONG>(reinterpret_cast<uintptr_t>(site+1));
        const unsigned char decodeExpected[]{0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x48,0x81,0xec,0xb0,0,0,0};
        const unsigned char resizeExpected[]{0x40,0x53,0x48,0x83,0xec,0x20,0x4c,0x8b,0x49,0x08};
        if (instance || *site!=0xe8 || reinterpret_cast<uintptr_t>(site+5)+original!=base+layout.Rva(0x11230) ||
            memcmp(reinterpret_cast<void*>(base+layout.Rva(0x19fda0)),decodeExpected,sizeof(decodeExpected)) ||
            memcmp(reinterpret_cast<void*>(base+layout.Rva(0x1a09c0)),resizeExpected,sizeof(resizeExpected)) ||
            VirtualQuery(site,&m,sizeof(m))!=sizeof(m) || m.AllocationBase!=module || m.Type!=MEM_IMAGE || m.Protect!=PAGE_EXECUTE_READ)
            throw std::runtime_error("shader_port_identity");
        protection=m.Protect;
        captures=dataRoot/L"captures"/std::to_wstring(GetCurrentProcessId());
#if defined(XESS_INIT_PROBE)
        std::filesystem::create_directories(captures);
#endif
        // No shader-capture files are written by the game build.
        decode=reinterpret_cast<Decode>(base+layout.Rva(0x19fda0)); resize=reinterpret_cast<Resize>(base+layout.Rva(0x1a09c0));
        // Embedded resources supply the exact same 54 verified byte arrays.
        // No external program directory is consulted by the game build.
        programs=xess_payload::LoadPrograms(payload);
        // Our own splat and U-Net conv shaders replace Intel's (proven byte-identical; always validated).
        replacements=xess_payload::LoadReplacements(payload);
        // On AMD, Intel's aa016078 and efbe3788 run faster than our own (RX 7800 XT at 4K: 266 vs 337 us and 161 vs
        // 163 us), so those two keep Intel's bytes there; the outputs are identical.
        std::string kept;
        if (adapterVendor==0x1002) {
            std::erase_if(replacements,[&](const xess_payload::Replacement& r) {
                if (r.name!="shaders/unet_aa016078.dxil" && r.name!="shaders/unet_efbe3788.dxil") return false;
                kept+=(kept.empty() ? "\"" : ",\"")+r.name+"\"";
                return true;
            });
        }
        std::string replaced;
        for (const auto& r:replacements) replaced+=(replaced.empty() ? "\"" : ",\"")+r.name+"\"";
        logEvent("shader_replacement_set","\"vendor_id\":"+std::to_string(adapterVendor)+",\"kept_intel\":["+kept+
            "],\"replaced\":["+replaced+"]");
        // Search only free regions within the signed-rel32 range of this call.
        const auto callEnd=reinterpret_cast<uintptr_t>(site+5);
        const uintptr_t low=callEnd>0x7ffe0000 ? callEnd-0x7ffe0000 : 0x10000;
        const uintptr_t high=callEnd+0x7ffe0000;
        for (uintptr_t p=(low+0xffff)&~uintptr_t(0xffff);p<high;) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<void*>(p),&region,sizeof(region))!=sizeof(region)) break;
            auto end=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
            if (region.State==MEM_FREE && end-p>=0x10000) {
                relay=VirtualAlloc(reinterpret_cast<void*>(p),0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
                if (relay) break;
            }
            if (end<=p) break;
            p=(end+0xffff)&~uintptr_t(0xffff);
        }
        if (!relay) throw std::runtime_error("shader_port_relay_allocation");
        unsigned char stub[14]{0xff,0x25,0,0,0,0};
        const auto destination=reinterpret_cast<uintptr_t>(&dispatch); memcpy(stub+6,&destination,8); memcpy(relay,stub,sizeof(stub));
        DWORD prior{};
        if (!VirtualProtect(relay,0x1000,PAGE_EXECUTE_READ,&prior) || !FlushInstructionCache(GetCurrentProcess(),relay,sizeof(stub))) {
            VirtualFree(relay,0,MEM_RELEASE); relay=nullptr; throw std::runtime_error("shader_port_relay_protection");
        }
        const auto distance=reinterpret_cast<intptr_t>(relay)-reinterpret_cast<intptr_t>(site+5);
        if (distance<INT32_MIN || distance>INT32_MAX) {VirtualFree(relay,0,MEM_RELEASE);relay=nullptr;throw std::runtime_error("shader_port_relay_range");}
        replacement=LONG(distance); instance=this; change(original,replacement); active=true;
        logEvent("shader_port_applied","\"programs\":"+std::to_string(programs.size())+",\"replacements\":"+
            std::to_string(replacements.size()));
    }
    ~ShaderPort() {
        if (active) {
            if(callers.load()!=0) TerminateProcess(GetCurrentProcess(),102);
            change(replacement,original); instance=nullptr;
            if(!VirtualFree(relay,0,MEM_RELEASE)) TerminateProcess(GetCurrentProcess(),103);
            logEvent("shader_port_restored");
        }
    }
};
