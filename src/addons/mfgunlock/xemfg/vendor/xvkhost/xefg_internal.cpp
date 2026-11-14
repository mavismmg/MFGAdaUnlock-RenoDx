#include "xefg_internal.h"
#include "xefg_internal_profiles.h"
#include <cstring>

namespace xefg_internal
{
namespace
{
bool Executable(const uint8_t* address, HMODULE module) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
        info.AllocationBase != module) return false;
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ;
    return (info.Protect & kExecute) != 0 && !(info.Protect & PAGE_GUARD);
}

int32_t Guard(int32_t (*call)(void*), void* context) noexcept
{
    __try { return call(context); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kException; }
}

enum class Check { eOk, eNotPe, eNotPe64, eBuild, eNotExecutable, eBytes, eFault };

// No C++ objects here: the reads are guarded with SEH. `profile` is the image's (its PE identity), then every entry
// must be the witnessed bytes in its executable image pages.
Check CheckImage(const uint8_t* base, HMODULE fg, const Profile*& profile, size_t& failed) noexcept
{
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return Check::eNotPe;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return Check::eNotPe64;
        profile = FindProfile(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum);
        if (!profile) return Check::eBuild;
        for (size_t i = 0; i < kEntryCount; ++i) {
            failed = i;
            const uint8_t* address = base + profile->entries[i].rva;
            if (!Executable(address, fg)) return Check::eNotExecutable;
            if (std::memcmp(address, profile->entries[i].bytes, sizeof(profile->entries[i].bytes)) != 0) return Check::eBytes;
        }
        return Check::eOk;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Check::eFault; }
}
}

bool Resolve(HMODULE fg, Api& api, std::string& reason) noexcept
{
    api = {};
    if (!fg) { reason = "no_module"; return false; }
    const auto* base = reinterpret_cast<const uint8_t*>(fg);
    const Profile* profile = nullptr;
    size_t failed = 0;
    switch (CheckImage(base, fg, profile, failed)) {
    case Check::eOk: break;
    case Check::eNotPe: reason = "not_pe"; return false;
    case Check::eNotPe64: reason = "not_pe64"; return false;
    case Check::eBuild: reason = "build_not_profiled"; return false;
    case Check::eNotExecutable: reason = std::string(profile->family) + "_entry_not_executable_" + profile->entries[failed].name; return false;
    case Check::eBytes: reason = std::string(profile->family) + "_entry_bytes_" + profile->entries[failed].name; return false;
    default: reason = "header_read_fault"; return false;
    }
    auto* image = const_cast<uint8_t*>(base);
    api.module = fg;
    api.family = profile->family;
    api.createContext = reinterpret_cast<decltype(api.createContext)>(image + profile->entries[kCreateContext].rva);
    api.init = reinterpret_cast<decltype(api.init)>(image + profile->entries[kInit].rva);
    api.execute = reinterpret_cast<decltype(api.execute)>(image + profile->entries[kExecute].rva);
    api.destroy = reinterpret_cast<decltype(api.destroy)>(image + profile->entries[kDestroy].rva);
    reason.clear();
    return true;
}

namespace
{
struct CreateArgs { const Api* api; ID3D12Device* device; Handle* context; };
struct InitArgs { const Api* api; Handle context; const InitParams* params; };
struct ExecuteArgs { const Api* api; Handle context; ID3D12GraphicsCommandList* list; const ExecuteParams* params; };
struct DestroyArgs { const Api* api; Handle context; };
}

int32_t Api::CreateContext(ID3D12Device* device, Handle* context) const noexcept
{
    if (!createContext || !device || !context) return kException;
    CreateArgs args{this, device, context};
    return Guard([](void* p) { auto* a = static_cast<CreateArgs*>(p); return a->api->createContext(a->device, a->context); }, &args);
}

int32_t Api::Init(Handle context, const InitParams& params) const noexcept
{
    if (!init || !context) return kException;
    InitArgs args{this, context, &params};
    return Guard([](void* p) { auto* a = static_cast<InitArgs*>(p); return a->api->init(a->context, a->params); }, &args);
}

int32_t Api::Execute(Handle context, ID3D12GraphicsCommandList* list, const ExecuteParams& params) const noexcept
{
    if (!execute || !context || !list) return kException;
    ExecuteArgs args{this, context, list, &params};
    return Guard([](void* p) { auto* a = static_cast<ExecuteArgs*>(p); return a->api->execute(a->context, a->list, a->params); }, &args);
}

int32_t Api::Destroy(Handle context) const noexcept
{
    if (!destroy || !context) return kException;
    DestroyArgs args{this, context};
    return Guard([](void* p) { auto* a = static_cast<DestroyArgs*>(p); return a->api->destroy(a->context); }, &args);
}
}
