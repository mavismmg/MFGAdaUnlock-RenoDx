// xellbind.1: XeFG's lookups of libxell.dll answer with the host pair's (host_xell_binding.h).
#include "host_xell_binding.h"
#include "xemfg_host_common.h"
#include "protected_pointer.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace xess_host_xell
{
namespace
{
using HandleExAFn = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);
using HandleAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);

// Published once, before any slot points at the functions below; never changed (one pair per process).
std::atomic<HMODULE> gLl{nullptr};
HandleExAFn gHandleExA = nullptr;
HandleAFn gHandleA = nullptr;
LoadAFn gLoadA = nullptr;
LoadExAFn gLoadExA = nullptr;
std::mutex gBindMutex;
HMODULE gBoundFg = nullptr;

// The bare module name only (any case, with or without .dll): a path names a file of its own.
bool IsXell(LPCSTR name) noexcept
{
    return name && gLl.load(std::memory_order_acquire) && (!_stricmp(name, "libxell.dll") || !_stricmp(name, "libxell"));
}

// The pair's libxell, with the reference a load or a counted handle lookup takes.
HMODULE Referenced(DWORD keep) noexcept
{
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | keep,
               reinterpret_cast<LPCWSTR>(gLl.load(std::memory_order_acquire)), &module) ? module : nullptr;
}

BOOL WINAPI BoundHandleExA(DWORD flags, LPCSTR name, HMODULE* module)
{
    if (module && !(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) && IsXell(name)) {
        *module = Referenced(flags & (GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT));
        return *module != nullptr;
    }
    return gHandleExA(flags, name, module);
}

HMODULE WINAPI BoundHandleA(LPCSTR name)
{
    return IsXell(name) ? gLl.load(std::memory_order_acquire) : gHandleA(name);
}

HMODULE WINAPI BoundLoadA(LPCSTR name)
{
    return IsXell(name) ? Referenced(0) : gLoadA(name);
}

HMODULE WINAPI BoundLoadExA(LPCSTR name, HANDLE file, DWORD flags)
{
    constexpr DWORD kNotCode = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    return IsXell(name) && !file && !(flags & kNotCode) ? Referenced(0) : gLoadExA(name, file, flags);
}

// The DLLs whose imports of `symbol` are the system's module loader (KERNEL32 and its api-set contracts).
bool LoaderDll(const char* dll) noexcept
{
    return !_stricmp(dll, "KERNEL32.dll") || !_strnicmp(dll, "api-ms-win-core-libraryloader-", 30);
}

enum class Slot { eAbsent, eBound, eAlready, eForeign, eFailed };

// Every IAT slot of `module` importing `symbol` from the loader DLLs moves from the system function to `replacement`.
// The slot holds KERNEL32's export or, through an api-set contract, KERNELBASE's: either is the system's. Loaded image
// only; every PE offset and name is bounded before it is read.
Slot BindImport(HMODULE module, const char* symbol, void* replacement) noexcept
{
    void* const kernel = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), symbol));
    const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    void* const base2 = kernelBase ? reinterpret_cast<void*>(GetProcAddress(kernelBase, symbol)) : nullptr;
    if (!kernel) return Slot::eFailed;
    Slot outcome = Slot::eAbsent;
    __try {
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return Slot::eFailed;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return Slot::eFailed;
        const size_t size = nt->OptionalHeader.SizeOfImage;
        const auto fits = [=](size_t at, size_t bytes) { return at < size && bytes <= size - at; };
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const size_t sectionOffset = reinterpret_cast<const unsigned char*>(sections) - base;
        if (nt->FileHeader.NumberOfSections > 96 || !fits(sectionOffset, nt->FileHeader.NumberOfSections * sizeof(*sections)))
            return Slot::eFailed;
        const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !fits(dir.VirtualAddress, dir.Size) || dir.Size > 4096 * sizeof(IMAGE_IMPORT_DESCRIPTOR))
            return Slot::eFailed;
        const size_t symbolBytes = strlen(symbol) + 1;
        const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (size_t i = 0; i < dir.Size / sizeof(*imports) && imports[i].Name; ++i) {
            const auto& imp = imports[i];
            if (!imp.FirstThunk || !imp.OriginalFirstThunk || !fits(imp.Name, 64)) continue;
            const char* dll = reinterpret_cast<const char*>(base + imp.Name);
            if (!memchr(dll, 0, 64) || !LoaderDll(dll)) continue;
            for (size_t j = 0; j < 16384; ++j) {
                const size_t nameAt = size_t(imp.OriginalFirstThunk) + j * 8, slotAt = size_t(imp.FirstThunk) + j * 8;
                if (!fits(nameAt, 8) || !fits(slotAt, 8)) return Slot::eFailed;
                const auto value = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + nameAt)->u1.AddressOfData;
                if (!value) break;
                if (IMAGE_SNAP_BY_ORDINAL64(value) || !fits(size_t(value), 2 + symbolBytes)) continue;
                if (memcmp(base + value + 2, symbol, symbolBytes)) continue;
                bool data = false;
                for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
                    const auto& sec = sections[s];
                    if (slotAt >= sec.VirtualAddress && sec.Misc.VirtualSize >= 8 && slotAt - sec.VirtualAddress <= sec.Misc.VirtualSize - 8)
                        data = !(sec.Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE));
                }
                auto** slot = reinterpret_cast<void**>(base + slotAt);
                if (!data || (reinterpret_cast<uintptr_t>(slot) & 7)) return Slot::eFailed;
                void* const current = *slot;
                if (current == replacement) { if (outcome == Slot::eAbsent) outcome = Slot::eAlready; continue; }
                if (current != kernel && (!base2 || current != base2)) return Slot::eForeign;   // another layer's hook: left as it is
                const auto result = protected_pointer::ReplaceProtectedPointer(reinterpret_cast<uintptr_t>(slot),
                    reinterpret_cast<uintptr_t>(current), reinterpret_cast<uintptr_t>(replacement));
                if (result.disposition != protected_pointer::PublishDisposition::ePublishedRestored) return Slot::eFailed;
                outcome = Slot::eBound;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Slot::eFailed;
    }
    return outcome;
}

const char* Name(Slot slot) noexcept
{
    switch (slot) {
    case Slot::eAbsent: return "absent";
    case Slot::eBound: return "bound";
    case Slot::eAlready: return "already";
    case Slot::eForeign: return "foreign";
    case Slot::eFailed: return "failed";
    }
    return "?";
}
}

void Bind(HMODULE fg, HMODULE ll) noexcept
{
    try {
        std::lock_guard lock(gBindMutex);
        if (!fg || !ll || fg == ll) return;
        if (gBoundFg) {
            if (gBoundFg != fg || gLl.load() != ll) xemfg_host::Log("host_xell_binding", "\"result\":\"other_pair\"");
            return;
        }
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        gHandleExA = reinterpret_cast<HandleExAFn>(GetProcAddress(kernel, "GetModuleHandleExA"));
        gHandleA = reinterpret_cast<HandleAFn>(GetProcAddress(kernel, "GetModuleHandleA"));
        gLoadA = reinterpret_cast<LoadAFn>(GetProcAddress(kernel, "LoadLibraryA"));
        gLoadExA = reinterpret_cast<LoadExAFn>(GetProcAddress(kernel, "LoadLibraryExA"));
        if (!gHandleExA || !gHandleA || !gLoadA || !gLoadExA) {
            xemfg_host::Log("host_xell_binding", "\"result\":\"loader_exports\"");
            return;
        }
        gLl.store(ll, std::memory_order_release);
        gBoundFg = fg;
        const Slot handleEx = BindImport(fg, "GetModuleHandleExA", reinterpret_cast<void*>(&BoundHandleExA));
        const Slot handle = BindImport(fg, "GetModuleHandleA", reinterpret_cast<void*>(&BoundHandleA));
        const Slot load = BindImport(fg, "LoadLibraryA", reinterpret_cast<void*>(&BoundLoadA));
        const Slot loadEx = BindImport(fg, "LoadLibraryExA", reinterpret_cast<void*>(&BoundLoadExA));
        // What a lookup by name gives anyone else: the game's own copy when it loaded one first.
        HMODULE byName = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"libxell.dll", &byName);
        wchar_t other[MAX_PATH]{};
        if (byName && byName != ll && !GetModuleFileNameW(byName, other, MAX_PATH)) other[0] = 0;
        xemfg_host::Log("host_xell_binding", std::string("\"GetModuleHandleExA\":\"") + Name(handleEx) + "\",\"GetModuleHandleA\":\"" +
            Name(handle) + "\",\"LoadLibraryA\":\"" + Name(load) + "\",\"LoadLibraryExA\":\"" + Name(loadEx) +
            "\",\"other_libxell_loaded\":" + (byName && byName != ll ? "true" : "false") +
            (other[0] ? ",\"other_libxell\":\"" + xemfg_host::Escape(xemfg_host::Narrow(other)) + "\"" : std::string()));
    } catch (...) {}
}
}
