#include "cuda_route.h"
#include "cuda_layers.h"
#include "../adapter_discovery.h"
#include "../sha256_util.h"
#include "../overlay_native.h"
#include <dxgi1_4.h>
#include <intrin.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#if defined(MFGUNLOCK_NATIVE_XEMFG)
#include "../../native_trace.hpp"
#endif

namespace xefg_cuda
{
namespace
{
using Microsoft::WRL::ComPtr;

// ---- NVAPI: the System32 dispatcher and driver-owned code only ----------------------------------------
using NvQuery = void*(__cdecl*)(uint32_t);
using NvInitialize = int(__cdecl*)();
using NvCreate = int(__cdecl*)(ID3D12Device*, const void*, uint32_t, uint32_t, uint32_t, uint32_t, const char*, void**);
using NvLaunch = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
using NvDestroy = int(__cdecl*)(ID3D12Device*, void*);
struct DescriptorObject
{
    size_t structSizeIn, structSizeOut;
    ID3D12Device* device;
    int type;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
    uint64_t handle;
};
static_assert(sizeof(DescriptorObject) == 48 && offsetof(DescriptorObject, descriptor) == 32);
using NvDescriptorObject = int(__cdecl*)(DescriptorObject*);
constexpr int kSurfaceObject = 0, kTextureObject = 1;
// NVAPI R615 interface ids (nvapi_interface.h).
constexpr uint32_t kIdInitialize = 0x0150e828, kIdCreate = 0x1dc7261f, kIdLaunch = 0x5c52bb86,
    kIdDestroy = 0x7fb785ba, kIdDescriptorObject = 0x0ddac234;
struct Nvapi { NvInitialize initialize{}; NvCreate create{}; NvLaunch launch{}; NvDestroy destroy{}; NvDescriptorObject object{}; };

// The driver can fault inside NvAPI's cubin create (an access violation in nvcuda64.dll), which the C++ handlers here
// do not see (SEH), so the game would exit. These calls run behind an SEH frame: a fault fails the call with kNvFault
// and keeps its exception code, the route turns off and the DXIL port keeps the programs.
constexpr int kNvFault = 0x7FFFFFFF;
std::atomic<unsigned long> gFaultCode{};
int Faulted(unsigned long code) noexcept
{
    gFaultCode.store(code, std::memory_order_relaxed);
    return EXCEPTION_EXECUTE_HANDLER;
}
int GuardedCreate(NvCreate create, ID3D12Device* device, const void* data, uint32_t size, const char* name, void** handle) noexcept
{
    __try { return create(device, data, size, 128, 1, 1, name, handle); }
    __except (Faulted(GetExceptionCode())) { return kNvFault; }
}
int GuardedLaunch(NvLaunch launch, ID3D12GraphicsCommandList* list, void* kernel, uint32_t x, uint32_t y, uint32_t z,
                  const void* params, uint32_t size) noexcept
{
    __try { return launch(list, kernel, x, y, z, params, size); }
    __except (Faulted(GetExceptionCode())) { return kNvFault; }
}
int GuardedObject(NvDescriptorObject object, DescriptorObject* request) noexcept
{
    __try { return object(request); }
    __except (Faulted(GetExceptionCode())) { return kNvFault; }
}

bool LoadNvapi(Nvapi& api, std::string& reason)
{
    using namespace adapter_discovery;
    std::array<wchar_t, kPathCapacity> systemDirectory{}, stubPath{}, loadedPath{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (!systemLength || systemLength >= systemDirectory.size()
        || swprintf_s(stubPath.data(), stubPath.size(), L"%ls\\nvapi64.dll", systemDirectory.data()) < 0)
    { reason = "nvapi_path"; return false; }
    // Kept for the process lifetime (the route holds its functions).
    const HMODULE dispatcher = LoadLibraryExW(stubPath.data(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dispatcher) { reason = "nvapi_missing"; return false; }
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(dispatcher), &pinned);
    const DWORD stubLength = GetModuleFileNameW(dispatcher, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
    if (!stubLength || stubLength >= loadedPath.size() || !NvapiOwnerPath(systemDirectory.data(), loadedPath.data(), true))
    { reason = "nvapi_not_system"; return false; }
    const auto query = reinterpret_cast<NvQuery>(GetProcAddress(dispatcher, "nvapi_QueryInterface"));
    if (!OwnedCode(dispatcher, reinterpret_cast<void*>(query))) { reason = "nvapi_query_owner"; return false; }
    const std::array<uint32_t, 5> ids{kIdInitialize, kIdCreate, kIdLaunch, kIdDestroy, kIdDescriptorObject};
    std::array<void*, 5> targets{};
    for (size_t i = 0; i < ids.size(); ++i)
    {
        targets[i] = query(ids[i]);
        ModuleReference owner;
        if (!targets[i] || !owner.Acquire(targets[i])) { reason = "nvapi_function_missing"; return false; }
        const DWORD ownerLength = GetModuleFileNameW(owner.module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
        // A detour (OptiScaler hooks nvapi_QueryInterface for some ids) is never driver-owned code.
        if (!ownerLength || ownerLength >= loadedPath.size() || !NvapiOwnerPath(systemDirectory.data(), loadedPath.data())
            || !OwnerFactoryMatches(owner.module, ids[i], targets[i]) || query(ids[i]) != targets[i])
        { reason = "nvapi_function_owner"; return false; }
        // Keep the owner mapped for the process lifetime as well.
        HMODULE keep{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(targets[i]), &keep);
    }
    api = {reinterpret_cast<NvInitialize>(targets[0]), reinterpret_cast<NvCreate>(targets[1]),
        reinterpret_cast<NvLaunch>(targets[2]), reinterpret_cast<NvDestroy>(targets[3]),
        reinterpret_cast<NvDescriptorObject>(targets[4])};
    if (api.initialize() != 0) { reason = "nvapi_initialize"; return false; }
    return true;
}

// Compute capability and name of the adapter with this LUID, through the CUDA driver API (nvcuda.dll); no context.
// The name is the driver's (OptiScaler's GPU spoofing changes DXGI/NVAPI, not nvcuda).
bool Capability(const LUID& luid, int& major, int& minor, std::string& name)
{
    using CuInit = int (WINAPI*)(unsigned int);
    using CuDeviceGetCount = int (WINAPI*)(int*);
    using CuDeviceGet = int (WINAPI*)(int*, int);
    using CuDeviceComputeCapability = int (WINAPI*)(int*, int*, int);
    using CuDeviceGetLuid = int (WINAPI*)(char*, unsigned int*, int);
    using CuDeviceGetName = int (WINAPI*)(char*, int, int);
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return false;
    auto* initialize = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
    auto* getCount = reinterpret_cast<CuDeviceGetCount>(GetProcAddress(cuda, "cuDeviceGetCount"));
    auto* getDevice = reinterpret_cast<CuDeviceGet>(GetProcAddress(cuda, "cuDeviceGet"));
    auto* getCapability = reinterpret_cast<CuDeviceComputeCapability>(GetProcAddress(cuda, "cuDeviceComputeCapability"));
    auto* getLuid = reinterpret_cast<CuDeviceGetLuid>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    auto* getName = reinterpret_cast<CuDeviceGetName>(GetProcAddress(cuda, "cuDeviceGetName"));
    int count = 0;
    bool complete = initialize && getCount && getDevice && getCapability && getLuid && getName
        && initialize(0) == 0 && getCount(&count) == 0 && count > 0;
    uint32_t matches = 0;
    for (int ordinal = 0; complete && ordinal < count; ++ordinal)
    {
        int device = 0, candidateMajor = 0, candidateMinor = 0;
        std::array<char, sizeof(LUID)> raw{};
        std::array<char, 256> candidateName{};
        unsigned int nodeMask = 0;
        if (getDevice(&device, ordinal) != 0 || getCapability(&candidateMajor, &candidateMinor, device) != 0
            || getLuid(raw.data(), &nodeMask, device) != 0
            || getName(candidateName.data(), int(candidateName.size() - 1), device) != 0) { complete = false; break; }
        LUID candidate{};
        std::memcpy(&candidate, raw.data(), sizeof(candidate));
        if (candidate.LowPart == luid.LowPart && candidate.HighPart == luid.HighPart)
        {
            ++matches;
            major = candidateMajor;
            minor = candidateMinor;
            name = candidateName.data();
        }
    }
    FreeLibrary(cuda);
    return complete && matches == 1;
}

UINT VendorOf(const LUID& luid)
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))
        || FAILED(adapter->GetDesc1(&description))) return 0;
    return description.VendorId;
}

// ---- state -------------------------------------------------------------------------------------------
std::mutex gMutex;
LogFunction gLog{};
std::atomic<State> gState{State::NotAttempted};
std::string gReason;
int gMajor{}, gMinor{};
Nvapi gNvapi{};
std::array<void*, std::size(kKernels)> gKernels{};
std::atomic<bool> gActive{};
IUnknown* gDeviceIdentity{};     // held reference; the route's kernels belong to this device
ID3D12Device* gDevice{};
uintptr_t gProviderBegin{}, gProviderEnd{};
uintptr_t gWrapperBegin{}, gWrapperEnd{};   // the image of the wrapper the provider's device came through, if any
uintptr_t gRuntimeBegin{}, gRuntimeEnd{};   // the image of the runtime's device table (its D3D12Core.dll)
UINT gIncrement{};
std::atomic<uint64_t> gLaunches{}, gFallbacks{};
std::atomic<uint32_t> gLayerCount{};
std::array<std::atomic<bool>, std::size(kLayers)> gLayerSeen{};

void Log(const char* stage, const std::string& fields) noexcept
{
    if (const auto log = gLog) log(stage, fields);
}

// Recognized pipeline states: open addressing on the pointer (inserted under gMutex, read lock-free).
constexpr size_t kSlots = 512;
std::array<std::atomic<void*>, kSlots> gKeys{};
std::array<std::atomic<uint8_t>, kSlots> gValues{};
struct HeldLayer { ID3D12PipelineState* state; uint64_t token; uint8_t layer; };
std::vector<HeldLayer> gHeld;
thread_local uint64_t tPipelineToken{};
std::atomic<uint64_t> gNextPipelineToken{1};
std::atomic<uint64_t> gPipelineGeneration{};
std::atomic<uint32_t> gLayerMask{(1u << std::size(kLayers)) - 1};
std::atomic<bool> gAdoptProviderLists{};
Status gVulkanStatus;
std::atomic<bool> gVulkanReported{}, gVulkanLaunched{}, gVulkanActive{};
std::atomic<bool> gAnyLayer{};

size_t Home(const void* key) noexcept
{
    return size_t(((reinterpret_cast<uintptr_t>(key) >> 4) * 0x9E3779B97F4A7C15ull) >> 55) & (kSlots - 1);
}
uint8_t Lookup(const void* key) noexcept
{
    const auto generation = gPipelineGeneration.load(std::memory_order_acquire);
    if ((generation & 1) || !key || !gAnyLayer.load(std::memory_order_acquire)) return 0;
    for (size_t n = 0, i = Home(key); n < kSlots; ++n, i = (i + 1) & (kSlots - 1))
    {
        const void* found = gKeys[i].load(std::memory_order_acquire);
        if (found == key)
        {
            const auto value = gValues[i].load(std::memory_order_relaxed);
            return generation == gPipelineGeneration.load(std::memory_order_acquire) ? value : 0;
        }
        if (!found) return 0;
    }
    return 0;
}
bool Insert(void* key, uint8_t value) noexcept // under gMutex
{
    for (size_t n = 0, i = Home(key); n < kSlots; ++n, i = (i + 1) & (kSlots - 1))
    {
        void* found = gKeys[i].load(std::memory_order_relaxed);
        if (found == key) { gValues[i].store(value, std::memory_order_relaxed); return true; }
        if (!found)
        {
            gValues[i].store(value, std::memory_order_relaxed);
            gKeys[i].store(key, std::memory_order_release);
            return true;
        }
    }
    return false;
}


// ---- vtable patches ----------------------------------------------------------------------------------
// Slots of the D3D12 ABI (d3d12.h). Device: CreateComputePipelineState, CreateCommandList,
// ID3D12Device2::CreatePipelineState and ID3D12Device4::CreateCommandList1. Command list: the methods that carry
// compute recording state.
constexpr int kCreateComputePipelineState = 11, kCreateCommandList = 12, kCreatePipelineState = 47, kCreateCommandList1 = 51;
constexpr int kReset = 10, kDispatch = 14, kSetPipelineState = 25, kSetDescriptorHeaps = 28,
    kSetComputeRootSignature = 29, kSetComputeRootDescriptorTable = 31, kSetComputeRoot32BitConstant = 33,
    kSetComputeRoot32BitConstants = 35;
constexpr int kVtableSlots = 64;
constexpr std::array<int, 8> kListSlots{kReset, kDispatch, kSetPipelineState, kSetDescriptorHeaps, kSetComputeRootSignature,
    kSetComputeRootDescriptorTable, kSetComputeRoot32BitConstant, kSetComputeRoot32BitConstants};
using Hook = std::pair<int, void*>;

// Puts hook into slot, recording the pointer it replaces in original first (a hook running on another
// thread finds it). Another module patching the slot concurrently: its value becomes the original. A slot
// that already holds hook keeps the original the caller recorded. protect: the slot is not writable as it
// is (image data) and is made writable for the exchange only.
bool HookSlot(void** slot, void*& original, void* hook, bool protect) noexcept
{
    void* expected = *slot;
    while (expected != hook)
    {
        original = expected;
        DWORD old{}, unused{};
        if (protect && !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        void* prior = InterlockedCompareExchangePointer(slot, hook, expected);
        if (protect) VirtualProtect(slot, sizeof(void*), old, &unused);
        if (prior == expected) return true;
        expected = prior;
    }
    return true;
}

struct Patch { void** vtable{}; std::array<void*, kVtableSlots> original{}; };
constexpr size_t kMaxPatches = 8;
std::array<Patch, kMaxPatches> gDevicePatches{};
std::atomic<uint32_t> gDevicePatchCount{};

template <class F>
F DeviceOriginal(void* self, int slot) noexcept
{
    void** vtable = *static_cast<void***>(self);
    const uint32_t n = gDevicePatchCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) if (gDevicePatches[i].vtable == vtable) return reinterpret_cast<F>(gDevicePatches[i].original[slot]);
    // A copy of a patched vtable (another module's per-object swap) carries our pointer: chain to an original.
    for (uint32_t i = 0; i < n; ++i) if (gDevicePatches[i].original[slot]) return reinterpret_cast<F>(gDevicePatches[i].original[slot]);
    return nullptr;
}

// Publishes the originals before the exchange, so a hook running on another thread finds them.
bool ApplyDevice(void** vtable, std::span<const Hook> hooks) noexcept
{
    const uint32_t n = gDevicePatchCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) if (gDevicePatches[i].vtable == vtable) return true;
    if (n >= kMaxPatches) return false;
    Patch& patch = gDevicePatches[n];
    patch.vtable = vtable;
    for (const auto& [slot, hook] : hooks)
    {
        if (vtable[slot] == hook) return false; // a vtable copy that already carries our hooks
        patch.original[slot] = vtable[slot];
    }
    gDevicePatchCount.store(n + 1, std::memory_order_release);
    for (const auto& [slot, hook] : hooks)
        if (!HookSlot(&vtable[slot], patch.original[slot], hook, true)) return false;
    return true;
}

// ---- the provider's command lists ----------------------------------------------------------------------
// A list is hooked through the table it keeps after its first Reset. The system runtime's lists keep the
// table of their class (in its image). An Agility SDK runtime (a D3D12Core.dll the game ships, e.g.
// Witcher 3's SDK 619) moves each list from its image table into the list's own table, embedded in the
// list object, at creation or at the latest on the first Reset: a copy of the image table whose fast-path
// slots then call the driver directly, so a hook in the image table never sees those calls
// (tests/xess_cuda --expect-agility). The provider's lists therefore get their first Reset where they are
// created (HookCreateCommandList*), and the hooks go into the table each one then keeps. An Agility image
// table is never written: hooks copied from it into the game's own list tables would also be refused by
// modules that validate those tables (Witcher DOTS, which then falls back to raster hair). An Agility runtime that
// intercepts every call (DRED) keeps each list on its class table after the first Reset; that table is shared, as the
// system runtime's is (AdoptList).
struct ListTable
{
    std::atomic<void**> table{};               // key; never removed (a retired own table keeps its originals)
    std::array<void*, kVtableSlots> original{};
    std::atomic<bool> complete{};              // every hook installed: its Dispatch may launch
    bool shared{};
    bool runtimeClass{};                       // shared, the class table of the device's Agility runtime
    ID3D12GraphicsCommandList* list{};         // an own table's list, held until the table is retired
    ID3D12CommandAllocator* allocator{};       // the temporary allocator of a CreateCommandList1 list's first Reset
};
constexpr size_t kTables = 256;
std::array<ListTable, kTables> gTables{};
size_t gTableKeys{};                           // under gMutex
// A hooked shared table's originals by list type (direct, compute). A list that moves from that table to
// a copy of it carries our pointers there and forwards through these.
std::array<std::array<std::atomic<void*>, kVtableSlots>, 2> gSharedOriginal{};
size_t TypeIndex(D3D12_COMMAND_LIST_TYPE type) noexcept { return type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 0; }

size_t TableHome(void** table) noexcept
{
    return size_t(((reinterpret_cast<uintptr_t>(table) >> 3) * 0x9E3779B97F4A7C15ull) >> 56) & (kTables - 1);
}
ListTable* FindTable(void** table) noexcept
{
    if (!table) return nullptr;
    for (size_t n = 0, i = TableHome(table); n < kTables; ++n, i = (i + 1) & (kTables - 1))
    {
        void** key = gTables[i].table.load(std::memory_order_acquire);
        if (key == table) return &gTables[i];
        if (!key) return nullptr;
    }
    return nullptr;
}

template <class F> F ListOriginal(void* self, int slot) noexcept
{
    if (const ListTable* table = FindTable(*static_cast<void***>(self))) return reinterpret_cast<F>(table->original[slot]);
    const size_t type = TypeIndex(static_cast<ID3D12GraphicsCommandList*>(self)->GetType());
    void* original = gSharedOriginal[type][slot].load(std::memory_order_acquire);
    if (!original) original = gSharedOriginal[1 - type][slot].load(std::memory_order_acquire);
    return reinterpret_cast<F>(original);
}

// ---- the recording state of the current list, per thread ---------------------------------------------
// The replaced Dispatch needs the pipeline state, the CBV/SRV/UAV heap, root tables 0 and 1 and the
// root constants. Kept for one list (and its table) per thread: another list on the same thread starts
// over, and an incomplete state keeps the Dispatch.
struct Recording : ListState
{
    void* list{};
    void** table{};
    Recording() = default;
    Recording(void* l, void** t) : list(l), table(t) {}
};
thread_local Recording tRecording;

Recording& Track(void* list) noexcept
{
    auto& r = tRecording;
    void** table = *static_cast<void***>(list);
    if (r.list != list || r.table != table) r = Recording{list, table};
    return r;
}

using ResetFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DispatchFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using SetPipelineStateFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetDescriptorHeapsFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SetRootSignatureFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using SetTableFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using SetConstantFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using SetConstantsFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using CreateComputeFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateListFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
    ID3D12PipelineState*, REFIID, void**);
using CreateList1Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);

HRESULT STDMETHODCALLTYPE HookReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial)
{
    const HRESULT result = ListOriginal<ResetFn>(self, kReset)(self, allocator, initial);
    auto& r = tRecording;
    r = Recording{self, *reinterpret_cast<void***>(self)};
    if (SUCCEEDED(result)) r.layer = Lookup(initial);
    return result;
}
void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* state)
{
    Track(self).layer = Lookup(state);
    ListOriginal<SetPipelineStateFn>(self, kSetPipelineState)(self, state);
}
void STDMETHODCALLTYPE HookSetDescriptorHeaps(ID3D12GraphicsCommandList* self, UINT count, ID3D12DescriptorHeap* const* heaps)
{
    auto& r = Track(self);
    r.heap = nullptr;
    if (gActive.load(std::memory_order_relaxed) && heaps)
        for (UINT i = 0; i < count && i < 2; ++i)
            if (heaps[i] && heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) r.heap = heaps[i];
    ListOriginal<SetDescriptorHeapsFn>(self, kSetDescriptorHeaps)(self, count, heaps);
}
void STDMETHODCALLTYPE HookSetComputeRootSignature(ID3D12GraphicsCommandList* self, ID3D12RootSignature* signature)
{
    auto& r = Track(self);
    r.tableMask = 0;
    r.constantMask = 0;
    ListOriginal<SetRootSignatureFn>(self, kSetComputeRootSignature)(self, signature);
}
void STDMETHODCALLTYPE HookSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    auto& r = Track(self);
    if (index < 2) { r.tables[index] = table; r.tableMask |= 1u << index; }
    ListOriginal<SetTableFn>(self, kSetComputeRootDescriptorTable)(self, index, table);
}
void STDMETHODCALLTYPE HookSetComputeRoot32BitConstant(ID3D12GraphicsCommandList* self, UINT index, UINT value, UINT offset)
{
    auto& r = Track(self);
    if (index == 2 && offset < 12) { r.constants[offset] = value; r.constantMask |= 1u << offset; }
    ListOriginal<SetConstantFn>(self, kSetComputeRoot32BitConstant)(self, index, value, offset);
}
void STDMETHODCALLTYPE HookSetComputeRoot32BitConstants(ID3D12GraphicsCommandList* self, UINT index, UINT count, const void* values, UINT offset)
{
    auto& r = Track(self);
    if (index == 2 && values && count && offset < 12 && count <= 12 - offset)
    {
        std::memcpy(r.constants + offset, values, size_t(count)*4);
        r.constantMask |= ((1u << count) - 1u) << offset;
    }
    ListOriginal<SetConstantsFn>(self, kSetComputeRoot32BitConstants)(self, index, count, values, offset);
}

// Each reason is logged once, with the layer that met it first.
enum Reason : uint32_t { kShape = 1, kBindings = 2, kConstants = 4, kHeap = 8, kObject = 16, kLaunch = 32 };
std::atomic<uint32_t> gReasonsLogged{};
void Fallback(Reason reason, const char* text, const Layer& layer) noexcept
{
    gFallbacks.fetch_add(1, std::memory_order_relaxed);
    if (!(gReasonsLogged.fetch_or(reason) & reason))
        try { Log("cuda_dispatch_fallback", std::string("\"reason\":\"") + text + "\",\"program\":\"" + layer.info.program + "\""); }
        catch (...) {}
}

// A fault inside the driver while recording turns the route off for the rest of the session; this
// Dispatch and every later one run the DXIL port.
std::atomic<bool> gFaultLogged{};
void RuntimeFault(const char* where) noexcept
{
    gActive.store(false, std::memory_order_release);
    if (gFaultLogged.exchange(true)) return;
    try
    {
        char code[16]{};
        snprintf(code, sizeof(code), "0x%08lX", gFaultCode.load(std::memory_order_relaxed));
        Log("cuda_route_fault", std::string("\"call\":\"") + where + "\",\"code\":\"" + code + "\",\"route\":\"off\"");
    }
    catch (...) {}
}

// The layer's Dispatch as a CUDA launch on the same descriptors. False keeps the Dispatch.
bool Launch(ID3D12GraphicsCommandList* list, const ListState& r, UINT x, UINT y, UINT z) noexcept
{
    const Layer& layer = kLayers[r.layer - 1];
    if (!DispatchShapeOk(layer, x, y, z)) { Fallback(kShape, "dispatch_shape", layer); return false; }
    if ((r.tableMask & 3u) != 3u || !r.heap) { Fallback(kBindings, "bindings_incomplete", layer); return false; }
    if ((r.constantMask & layer.constants) != layer.constants) { Fallback(kConstants, "constants_incomplete", layer); return false; }
    const D3D12_DESCRIPTOR_HEAP_DESC heap = r.heap->GetDesc();
    if (heap.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(heap.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
    { Fallback(kHeap, "heap_type", layer); return false; }
    const auto gpuStart = r.heap->GetGPUDescriptorHandleForHeapStart();
    const auto cpuStart = r.heap->GetCPUDescriptorHandleForHeapStart();
    auto object = [&](D3D12_GPU_DESCRIPTOR_HANDLE table, uint32_t offset, int type, uint64_t& handle) {
        if (table.ptr < gpuStart.ptr || (table.ptr - gpuStart.ptr) % gIncrement) return false;
        const uint64_t index = (table.ptr - gpuStart.ptr)/gIncrement + offset;
        if (index >= heap.NumDescriptors) return false;
        DescriptorObject request{sizeof(DescriptorObject), 0, gDevice, type, {SIZE_T(cpuStart.ptr + index*gIncrement)}, 0};
        const int result = GuardedObject(gNvapi.object, &request);
        if (result == kNvFault) RuntimeFault("descriptor_object");
        if (result != 0 || !request.handle) return false;
        handle = request.handle;
        return true;
    };
    XefgCudaParams params{};
    const bool flow = layer.family == Family::Flow;
    if (!object(r.tables[0], 0, kTextureObject, params.input)
        || (layer.family == Family::Decoder && !object(r.tables[0], 1, kTextureObject, params.skip))
        || !object(r.tables[0], flow ? 1 : 2, kSurfaceObject, params.output)
        || !object(r.tables[1], 0, kTextureObject, params.weights)
        || !object(r.tables[1], 1, kTextureObject, params.scale)
        || !object(r.tables[1], 2, kTextureObject, params.bias))
    { Fallback(kObject, "descriptor_object", layer); return false; }
    uint32_t gridX{}, gridY{};
    FillLaunch(layer, x, y, r.constants, params, gridX, gridY);
    const int launched=[&] {
#if defined(MFGUNLOCK_NATIVE_XEMFG)
        mfgunlock::xemfg::native::RuntimeCall trace("nvapi_cuda_launch");
#endif
        const int result=GuardedLaunch(gNvapi.launch, list, gKernels[layer.kernel], gridX, gridY, layer.gridZ, &params, sizeof(params));
#if defined(MFGUNLOCK_NATIVE_XEMFG)
        trace.Result(result);
#endif
        return result;
    }();
    if (launched == kNvFault) RuntimeFault("launch");
    if (launched != 0) { Fallback(kLaunch, "launch", layer); return false; }
    gLaunches.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Only through a table that carries every hook: a recognized pipeline state seen by our SetPipelineState
// is then the one the Dispatch runs.
void STDMETHODCALLTYPE HookDispatch(ID3D12GraphicsCommandList* self, UINT x, UINT y, UINT z)
{
    const auto& r = tRecording;
    if (r.list == self && r.layer && r.table == *reinterpret_cast<void***>(self) && gActive.load(std::memory_order_acquire))
        if (const ListTable* table = FindTable(r.table); table && table->complete.load(std::memory_order_acquire)
            && LayerEnabled(r.layer) && Launch(self, r, x, y, z))
            return;
    ListOriginal<DispatchFn>(self, kDispatch)(self, x, y, z);
}

const std::array<void*, kVtableSlots>& ListHooks() noexcept
{
    static const auto hooks = [] {
        std::array<void*, kVtableSlots> h{};
        h[kReset] = reinterpret_cast<void*>(&HookReset);
        h[kDispatch] = reinterpret_cast<void*>(&HookDispatch);
        h[kSetPipelineState] = reinterpret_cast<void*>(&HookSetPipelineState);
        h[kSetDescriptorHeaps] = reinterpret_cast<void*>(&HookSetDescriptorHeaps);
        h[kSetComputeRootSignature] = reinterpret_cast<void*>(&HookSetComputeRootSignature);
        h[kSetComputeRootDescriptorTable] = reinterpret_cast<void*>(&HookSetComputeRootDescriptorTable);
        h[kSetComputeRoot32BitConstant] = reinterpret_cast<void*>(&HookSetComputeRoot32BitConstant);
        h[kSetComputeRoot32BitConstants] = reinterpret_cast<void*>(&HookSetComputeRoot32BitConstants);
        return h;
    }();
    return hooks;
}

// The image address lies in: its file name, and whether it is in the system directory.
bool ImageOf(const void* address, std::array<wchar_t, MAX_PATH>& path, const wchar_t*& leaf, bool& system) noexcept
{
    HMODULE owner{};
    std::array<wchar_t, MAX_PATH> directory{};
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &owner))
        return false;
    const DWORD length = GetModuleFileNameW(owner, path.data(), DWORD(path.size()));
    const UINT systemLength = GetSystemDirectoryW(directory.data(), UINT(directory.size()));
    if (!length || length >= path.size() || !systemLength || systemLength >= directory.size()) return false;
    const wchar_t* slash = wcsrchr(path.data(), L'\\');
    if (!slash) return false;
    system = size_t(slash - path.data()) == systemLength && !_wcsnicmp(path.data(), directory.data(), systemLength);
    leaf = slash + 1;
    return true;
}

// The system runtime's image (System32): its list tables are shared by its lists and never copied per list.
bool SystemRuntimeImage(const void* address) noexcept
{
    std::array<wchar_t, MAX_PATH> path{};
    const wchar_t* leaf{};
    bool system{};
    return ImageOf(address, path, leaf, system) && system && (!_wcsicmp(leaf, L"D3D12Core.dll") || !_wcsicmp(leaf, L"d3d12.dll"));
}

// The runtime's own device: its vtable and the QueryInterface it starts with are in D3D12Core.dll (the system's or
// an Agility SDK copy) or the system's d3d12.dll. A wrapper (ReShade, Streamline, the debug layer) keeps them in
// its own image; owner names it.
bool RuntimeDevice(ID3D12Device* device, std::string& owner) noexcept
{
    void** table = *reinterpret_cast<void***>(device);
    const std::array<const void*, 2> addresses{table, table ? table[0] : nullptr};
    for (const void* address : addresses)
    {
        std::array<wchar_t, MAX_PATH> path{};
        const wchar_t* leaf{};
        bool system{};
        if (!ImageOf(address, path, leaf, system)) { owner = "not_an_image"; return false; }
        if (!_wcsicmp(leaf, L"D3D12Core.dll") || (system && !_wcsicmp(leaf, L"d3d12.dll"))) continue;
        owner.clear();
        for (; *leaf && owner.size() < 64; ++leaf) owner += *leaf >= 0x20 && *leaf < 0x7F && *leaf != L'"' && *leaf != L'\\' ? char(*leaf) : '?';
        return false;
    }
    return true;
}

// The runtime's device behind the one the provider holds. A wrapper (ReShade, Streamline, Special K) is seen through
// with its public unwrap contract, else through a device child it does not wrap (a fence reports the device that
// created it). The route then works on that device's own objects, under the wrapper: the wrapper's tables are never
// written and the provider's lists are not created through it. (Patching ReShade's device table and re-creating the
// lists through it ends in a double free in ReShade's descriptor-heap bookkeeping during XeFG's initialization, and
// every list it returns carries ReShade's table and is refused anyway.)
bool ResolveNative(ID3D12Device* device, ComPtr<ID3D12Device>& native, std::string& owner, const char*& how) noexcept
{
    how = "runtime";
    if (RuntimeDevice(device, owner)) { native = device; return true; }
    std::string ignored;
    ComPtr<ID3D12Device> candidate;
    if (single_overlay::native::Unwrap(device, candidate) && candidate && RuntimeDevice(candidate.Get(), ignored))
    { how = "unwrap"; native = std::move(candidate); return true; }
    ComPtr<ID3D12Fence> fence;
    candidate.Reset();
    if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
        && SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&candidate))) && candidate && RuntimeDevice(candidate.Get(), ignored))
    { how = "fence"; native = std::move(candidate); return true; }
    return false;
}

// Where a refused provider list's table (or its creator) lies, "image.dll+0xRVA protect", else its memory kind and
// protection. Read-only; adoption does not depend on it.
std::string Where(const void* address)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) return "unreadable";
    char tail[48];
    sprintf_s(tail, "+0x%llx 0x%lx", static_cast<unsigned long long>(uintptr_t(address) - uintptr_t(memory.AllocationBase)), memory.Protect);
    std::array<wchar_t, MAX_PATH> path{};
    const wchar_t* leaf{};
    bool system{};
    std::string out = memory.Type == MEM_PRIVATE ? "private" : memory.Type == MEM_MAPPED ? "mapped" : "image";
    if (memory.Type == MEM_IMAGE && ImageOf(address, path, leaf, system))
        for (out = system ? "System32/" : ""; *leaf && out.size() < 80; ++leaf) out += *leaf >= 0x20 && *leaf < 0x7F && *leaf != L'"' && *leaf != L'\\' ? char(*leaf) : '?';
    return out + tail;
}

// A class table of the device's own runtime: the table and the slots the route hooks lie in the image
// of the runtime's device table (gRuntime*, set at Install), and each slot is that runtime's code or the route's hook
// (the table was hooked for an earlier list). A wrapper's or another module's table never matches.
bool RuntimeClassTable(void** table) noexcept // gMutex held
{
    const auto inRuntime = [](const void* address) {
        const auto at = reinterpret_cast<uintptr_t>(address);
        return at >= gRuntimeBegin && at < gRuntimeEnd;
    };
    if (!gRuntimeBegin || !inRuntime(table) || !inRuntime(table + kSetComputeRoot32BitConstants) || !inRuntime(table[0])) return false;
    const auto& hooks = ListHooks();
    for (int slot : kListSlots)
        if (table[slot] != hooks[slot] && !inRuntime(table[slot])) return false;
    return true;
}

const char* ClassifyList(ID3D12GraphicsCommandList* list, bool& shared, bool& runtimeClass, bool& writable) noexcept
{
    void** table = *reinterpret_cast<void***>(list);
    constexpr size_t needed = (kSetComputeRoot32BitConstants + 1)*sizeof(void*);
    MEMORY_BASIC_INFORMATION memory{};
    if (!table || reinterpret_cast<uintptr_t>(table) % alignof(void*) || VirtualQuery(table, &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.RegionSize < needed
        || reinterpret_cast<uintptr_t>(table) - reinterpret_cast<uintptr_t>(memory.BaseAddress) > memory.RegionSize - needed)
        return "table_memory";
    // Shared: a runtime's class table (image data) that the runtime's lists keep: the system runtime's, or the
    // device's Agility runtime's while it intercepts every call. With DRED on (SDK 618 and 619 alike, tests/xess_cuda
    // --dred), each list keeps a class table in the runtime's .data after its first Reset. Own: a fast-path Agility
    // list's table, committed private read-write memory inside the list object (list + 0x5C8 with SDK 619). Any other
    // table is not written, above all the Agility image table a fast-path list starts on (copied into every game
    // list's own table): such a list has left it by its first Reset, which happens before this call.
    const auto at = reinterpret_cast<uintptr_t>(table), object = reinterpret_cast<uintptr_t>(list);
    const bool system = memory.Type == MEM_IMAGE && SystemRuntimeImage(table);
    runtimeClass = !system && memory.Type == MEM_IMAGE && RuntimeClassTable(table);
    shared = system || runtimeClass;
    const bool own = memory.Type == MEM_PRIVATE && memory.Protect == PAGE_READWRITE && at > object && at - object < 0x10000;
    if (!shared && !own) return memory.Type == MEM_IMAGE ? "table_runtime_image" : "table_unknown";
    writable = memory.Protect == PAGE_READWRITE || memory.Protect == PAGE_EXECUTE_READWRITE;
    return nullptr;
}

// Hooks the table the list keeps (its first Reset has happened). Null on success, else the reason.
const char* AdoptList(ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* temporary) noexcept // gMutex held
{
    bool shared{}, runtimeClass{}, writable{};
    if (const char* refusal = ClassifyList(list, shared, runtimeClass, writable)) return refusal;
    void** table = *reinterpret_cast<void***>(list);
    ListTable* record = FindTable(table);
    if (record && record->complete.load(std::memory_order_acquire))
        return shared && record->shared ? nullptr : "table_in_use"; // a shared table hooked for an earlier list
    if (record && record->list) return "table_in_use";
    if (!record)
    {
        if (gTableKeys >= kTables/2) return "table_capacity";
        for (size_t n = 0, i = TableHome(table); n < kTables && !record; ++n, i = (i + 1) & (kTables - 1))
            if (!gTables[i].table.load(std::memory_order_relaxed)) record = &gTables[i];
        if (!record) return "table_capacity";
    }
    const auto& hooks = ListHooks();
    auto& sharedOriginal = gSharedOriginal[TypeIndex(type)];
    std::array<void*, kVtableSlots> original{};
    for (int slot : kListSlots)
    {
        void* current = table[slot];
        // A copy of a hooked shared table: our pointer there forwards to that table's original.
        if (current == hooks[slot]) current = sharedOriginal[slot].load(std::memory_order_acquire);
        if (!current) return "table_original";
        original[slot] = current;
    }
    if (shared)
        for (int slot : kListSlots)
        {
            void* expected{};
            sharedOriginal[slot].compare_exchange_strong(expected, original[slot], std::memory_order_acq_rel);
        }
    record->original = original;
    record->shared = shared;
    record->runtimeClass = runtimeClass;
    if (!record->table.load(std::memory_order_relaxed)) { record->table.store(table, std::memory_order_release); ++gTableKeys; }
    if (!shared)
    {
        list->AddRef();
        record->list = list;
        if (temporary) { temporary->AddRef(); record->allocator = temporary; }
    }
    for (int slot : kListSlots)
        if (!HookSlot(&table[slot], record->original[slot], hooks[slot], !writable)) return "table_exchange";
    record->complete.store(true, std::memory_order_release);
    return nullptr;
}

// A list's own table gets its originals back and its list is released. False: another module chained to one
// of our hooks there, so that table stays hooked and its list held.
bool Retire(ListTable& record) noexcept // gMutex held
{
    if (!record.list) return true;
    record.complete.store(false, std::memory_order_release);
    void** table = record.table.load(std::memory_order_relaxed);
    const auto& hooks = ListHooks();
    bool restored = true;
    for (int slot : kListSlots)
    {
        InterlockedCompareExchangePointer(&table[slot], record.original[slot], hooks[slot]);
        restored = restored && table[slot] == record.original[slot];
    }
    if (!restored) return false;
    record.list->Release();
    record.list = nullptr;
    if (record.allocator) record.allocator->Release();
    record.allocator = nullptr;
    return true;
}

// Lists only the route still holds (the provider released them).
void RetireDropped() noexcept // gMutex held
{
    for (auto& record : gTables)
        if (record.list)
        {
            record.list->AddRef();
            if (record.list->Release() == 1) Retire(record);
        }
}

bool Routable(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept
{
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
    if (!gActive.load(std::memory_order_acquire)) return false;
    ComPtr<IUnknown> identity;
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity))) && identity.Get() == gDeviceIdentity;
}

std::atomic<uint32_t> gListRefusals{};
void AdoptCreated(ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* temporary,
    const char* created, const void* caller) noexcept
{
    const char* failure = "inactive";
    bool shared{}, runtimeClass{};
    size_t held{};
    {
        std::lock_guard lock(gMutex);
        if (gActive.load(std::memory_order_relaxed))
        {
            RetireDropped();
            failure = AdoptList(list, type, temporary);
            if (const ListTable* table = FindTable(*reinterpret_cast<void***>(list))) { shared = table->shared; runtimeClass = table->runtimeClass; }
            for (const auto& table : gTables) held += table.list != nullptr;
        }
    }
    try
    {
        const std::string fields = std::string("\"created\":\"") + created + "\",\"type\":\"" +
            (type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute" : "direct") + "\"";
        if (!failure)
            Log("cuda_list", fields + ",\"table\":\"" + (runtimeClass ? "runtime_class" : shared ? "shared" : "own") + "\",\"held\":" + std::to_string(held));
        else if (gListRefusals.fetch_add(1) < 8)
            Log("cuda_list_refused", fields + ",\"reason\":\"" + failure + "\",\"table\":\"" + Where(*reinterpret_cast<void***>(list)) +
                "\",\"caller\":\"" + Where(caller) + "\"");
    }
    catch (...) {}
}

bool FromProvider(const void* address) noexcept
{
    const auto at = reinterpret_cast<uintptr_t>(address);
    return at >= gProviderBegin && at < gProviderEnd;
}
bool FromWrapper(const void* address) noexcept
{
    const auto at = reinterpret_cast<uintptr_t>(address);
    return at >= gWrapperBegin && at < gWrapperEnd;
}

// A runtime device call the provider made. Directly (caller in its image), or through the wrapper of the device it
// holds: the wrapper's image calls the runtime, and the first frame outside that image is the provider's. Anything
// else the wrapper forwards (the game's calls, the wrapper's own objects) is passed through. The provider's lists are
// adopted only on this rule; pipeline states also on the provider's frame further up (OriginOf).
bool ProviderCall(const void* caller) noexcept
{
    if (FromProvider(caller)) return true;
    if (!FromWrapper(caller)) return false;
    std::array<void*, 16> frames{};
    const USHORT count = RtlCaptureStackBackTrace(0, USHORT(frames.size()), frames.data(), nullptr);
    size_t i = 0;
    while (i < count && !FromWrapper(frames[i])) ++i;  // this module's frames, up to the wrapper's call
    while (i < count && FromWrapper(frames[i])) ++i;   // the wrapper's frames
    return i < count && FromProvider(frames[i]);
}

// Where a pipeline state call came from. A module that hooks pipeline creation between the provider and the wrapper
// (or the runtime) is the first frame after the wrapper's: the SCSKiller shader recorder's d3d12.dll under ReShade, or
// a dxgi.dll setup, would keep every layer on the DXIL port that way, with the provider's lists adopted (that recorder
// does not hook list creation). A layer pipeline is therefore also recognized when the provider's image is a frame of
// this bounded walk above that module; lists keep ProviderCall, so a launch still needs a list the provider created
// itself.
struct Origin
{
    const void* first{};  // the first frame after the wrapper's frames (the hook's caller when that is not the wrapper's)
    bool strict{};        // ProviderCall's rule: the caller, or after the wrapper's frames the first frame, is the provider's
    int provider{-1};     // frames from first up to the provider's first frame; -1: none within the walk
    USHORT frames{};      // frames walked
};
std::atomic<bool> gStackRecognition{true};
Origin OriginOf(const void* caller) noexcept
{
    Origin origin{caller};
    if (FromProvider(caller)) { origin.strict = true; origin.provider = 0; return origin; }
    std::array<void*, 32> frames{};
    origin.frames = RtlCaptureStackBackTrace(0, USHORT(frames.size()), frames.data(), nullptr);
    size_t i = 0;
    while (i < origin.frames && frames[i] != caller) ++i;      // this module's frames, up to the hook's return address
    while (i < origin.frames && FromWrapper(frames[i])) ++i;  // the wrapper's frames
    if (i >= origin.frames) return origin;                    // the hook's caller not found, or nothing above the wrapper
    origin.first = frames[i];
    origin.strict = FromWrapper(caller) && FromProvider(frames[i]);
    for (size_t j = i; j < origin.frames; ++j)
        if (FromProvider(frames[j])) { origin.provider = int(j - i); break; }
    return origin;
}
bool Accepted(const Origin& origin) noexcept
{
    return origin.strict || (origin.provider >= 0 && gStackRecognition.load(std::memory_order_relaxed));
}
std::string OriginFields(const void* caller, const Origin& origin)
{
    return "\"caller\":\"" + Where(caller) + "\",\"first\":\"" + Where(origin.first) + "\",\"provider_frame\":" +
        std::to_string(origin.provider) + ",\"frames\":" + std::to_string(origin.frames);
}

// A pipeline state the route did not recognize although its program is a layer's (caller: the provider is not within
// the walk; device: another device than the route's), or the provider created it as a stream the route does not parse.
// Each reason is logged once (a stream once per subobject type, four lines at most), so a log names the module or the
// stream shape in the way. Logging only: recognition does not depend on it.
enum PsoRefusal : uint32_t { kPsoCaller = 1, kPsoDevice = 2 };
std::atomic<uint32_t> gPsoRefusals{};
std::atomic<uint64_t> gPsoStreamKinds{};
std::atomic<uint32_t> gPsoStreamLines{};
void LayerRefused(PsoRefusal reason, const char* text, const Layer& layer, const void* caller, const Origin* known = nullptr) noexcept
{
    if (gPsoRefusals.fetch_or(reason) & reason) return;
    try
    {
        Log("cuda_pso_refused", std::string("\"reason\":\"") + text + "\",\"program\":\"" + layer.info.program + "\"," +
            OriginFields(caller, known ? *known : OriginOf(caller)));
    }
    catch (...) {}
}

// The open list CreateCommandList returns (recording into allocator from initial), made as
// CreateCommandList1 + Reset: its first Reset has happened before the provider records.
HRESULT STDMETHODCALLTYPE HookCreateCommandList(ID3D12Device* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial, REFIID iid, void** out)
{
    void* caller = _ReturnAddress();
    if (gAdoptProviderLists.load(std::memory_order_relaxed) && out && allocator && ProviderCall(caller) && Routable(self, type))
    {
        ComPtr<ID3D12Device4> device4;
        ComPtr<ID3D12GraphicsCommandList> list;
        // The original CreateCommandList1 (not our hook: the provider's list is opened below).
        CreateList1Fn create1{};
        if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&device4)))
            && (create1 = DeviceOriginal<CreateList1Fn>(device4.Get(), kCreateCommandList1)) != nullptr
            && SUCCEEDED(create1(device4.Get(), mask, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list))))
        {
            if (SUCCEEDED(list->Reset(allocator, initial)))
            {
                void* requested{};
                if (SUCCEEDED(list->QueryInterface(iid, &requested)))
                {
                    AdoptCreated(list.Get(), type, nullptr, "list", caller);
                    auto& r = tRecording;
                    r = Recording{list.Get(), *reinterpret_cast<void***>(list.Get())};
                    r.layer = Lookup(initial);
                    *out = requested;
                    return S_OK;
                }
                list->Close();
            }
        }
    }
    return DeviceOriginal<CreateListFn>(self, kCreateCommandList)(self, mask, type, allocator, initial, iid, out);
}

// A closed list: its first Reset (on a temporary allocator, held with the list) and Close happen here, so
// the provider's first Reset finds the table the list keeps.
HRESULT STDMETHODCALLTYPE HookCreateCommandList1(ID3D12Device4* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** out)
{
    void* caller = _ReturnAddress();
    const HRESULT result = DeviceOriginal<CreateList1Fn>(self, kCreateCommandList1)(self, mask, type, flags, iid, out);
    if (gAdoptProviderLists.load(std::memory_order_relaxed) && SUCCEEDED(result) && out && *out && ProviderCall(caller) && Routable(self, type))
    {
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12CommandAllocator> temporary;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list)))
            && SUCCEEDED(self->CreateCommandAllocator(type, IID_PPV_ARGS(&temporary)))
            && SUCCEEDED(list->Reset(temporary.Get(), nullptr)) && SUCCEEDED(list->Close()))
            AdoptCreated(list.Get(), type, temporary.Get(), "list1", caller);
    }
    return result;
}

// A pipeline state whose program is a layer's, on the route's device, created by the provider (OriginOf). Only a program
// of a layer's exact size is hashed: every module's compute pipelines reach this, and only the provider's
// count.
void Recognize(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, REFIID iid, void* created, const void* caller) noexcept
{
    try
    {
        const SIZE_T length = desc.CS.BytecodeLength;
        if (!desc.CS.pShaderBytecode || length < 32 || length > 1000000
            || std::none_of(std::begin(kLayers), std::end(kLayers), [length](const Layer& layer) { return length == layer.bytes || length == layer.ownBytes; }))
            return;
        sha256_util::Digest digest{};
        if (!sha256_util::Compute(desc.CS.pShaderBytecode, length, digest)) return;
        const auto hash = sha256_util::LowerHex(digest);
        for (size_t i = 0; i < std::size(kLayers); ++i)
        {
            const bool own = kLayers[i].info.ownSha256 && hash == kLayers[i].info.ownSha256;
            if (hash != kLayers[i].info.sha256 && !own) continue;
            ComPtr<IUnknown> identity;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || identity.Get() != gDeviceIdentity)
            { LayerRefused(kPsoDevice, "device", kLayers[i], caller); return; }
            const Origin origin = OriginOf(caller);
            if (!Accepted(origin)) { LayerRefused(kPsoCaller, "caller", kLayers[i], caller, &origin); return; }
            ComPtr<ID3D12PipelineState> state;
            if (FAILED(static_cast<IUnknown*>(created)->QueryInterface(IID_PPV_ARGS(&state)))) return;
            if (iid != __uuidof(ID3D12PipelineState) && state.Get() != created) return; // keyed by the pointer bound later
            // Recognized above the provider's first frame: the layer's line names the module in between.
            const std::string stack = origin.strict ? std::string() : ",\"origin\":\"stack\",\"first\":\"" + Where(origin.first) +
                "\",\"provider_frame\":" + std::to_string(origin.provider);
            std::lock_guard lock(gMutex);
            if (!gActive.load(std::memory_order_relaxed)) return;
            if (Lookup(state.Get())) return;
            if (gHeld.size() >= kSlots/2 || !Insert(state.Get(), uint8_t(i + 1))) return;
            gHeld.push_back({state.Detach(), tPipelineToken, uint8_t(i + 1)});
            gAnyLayer.store(true, std::memory_order_release);
            gLayerCount.store(uint32_t(gHeld.size()), std::memory_order_relaxed);
            if (!gLayerSeen[i].exchange(true))
                Log("cuda_layer", std::string("\"program\":\"") + kLayers[i].info.program + "\",\"kernel\":\"" +
                    kLayers[i].info.kernel + "\"" + (own ? ",\"source\":\"rtxmfg\"" : "") + stack);
            return;
        }
    }
    catch (...) {}
}

HRESULT STDMETHODCALLTYPE HookCreateComputePipelineState(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out)
{
    void* caller = _ReturnAddress();
    const HRESULT result = DeviceOriginal<CreateComputeFn>(self, kCreateComputePipelineState)(self, desc, iid, out);
    if (SUCCEEDED(result) && desc && out && *out && gActive.load(std::memory_order_acquire))
        Recognize(self, *desc, iid, *out, caller);
    return result;
}

// ---- pipeline state streams (ID3D12Device2::CreatePipelineState) ---------------------------------------------
// A wrapper whose add-ons may change pipelines creates them all as streams (ReShade with RenoDX loaded): the
// provider's CreateComputePipelineState reaches the runtime here. Each subobject is its type followed by its
// inner structure at that structure's alignment, padded to pointer size (d3dx12.h
// CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT).
template <class Inner> constexpr size_t InnerOffset() { return (sizeof(UINT) + alignof(Inner) - 1)/alignof(Inner)*alignof(Inner); }
template <class Inner> constexpr size_t SubobjectSize()
{
    constexpr size_t align = (std::max)(alignof(Inner), alignof(void*));
    return (InnerOffset<Inner>() + sizeof(Inner) + align - 1)/align*align;
}
// Where a stream's parse ended without a compute shader (for the log).
struct StreamStop { const char* reason{}; UINT type{}; size_t offset{}; };
// The stream's compute shader; null for a stream with any other shader, or a subobject not listed here. A stream with
// another subobject (even default graphics state a wrapper might add) stays unrecognized: that the runtime ignores it
// in a compute pipeline is not established here, so such a stream is only logged.
const D3D12_SHADER_BYTECODE* StreamComputeShader(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, StreamStop& stop) noexcept
{
    const auto* stream = static_cast<const unsigned char*>(desc.pPipelineStateSubobjectStream);
    if (!stream || reinterpret_cast<uintptr_t>(stream) % alignof(void*)) { stop.reason = "stream_alignment"; return nullptr; }
    const D3D12_SHADER_BYTECODE* compute{};
    for (size_t at = 0; at + sizeof(UINT) <= desc.SizeInBytes;)
    {
        size_t size{};
        const UINT type = *reinterpret_cast<const UINT*>(stream + at);
        switch (type)
        {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            if (at + SubobjectSize<D3D12_SHADER_BYTECODE>() > desc.SizeInBytes) { stop = {"stream_truncated", type, at}; return nullptr; }
            compute = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(stream + at + InnerOffset<D3D12_SHADER_BYTECODE>());
            size = SubobjectSize<D3D12_SHADER_BYTECODE>();
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: size = SubobjectSize<ID3D12RootSignature*>(); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: size = SubobjectSize<UINT>(); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: size = SubobjectSize<D3D12_CACHED_PIPELINE_STATE>(); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: size = SubobjectSize<D3D12_PIPELINE_STATE_FLAGS>(); break;
        default: stop = {"stream_subobject", type, at}; return nullptr; // another shader stage, graphics state, or unknown
        }
        at += size;
    }
    if (!compute) stop.reason = "stream_no_compute_shader";
    return compute;
}

// A stream the provider created (either origin rule) that the parse refused: once per subobject type or
// reason, four lines at most. Once those are logged, refused streams are not walked.
void StreamRefused(const StreamStop& stop, const void* caller) noexcept
{
    if (!stop.reason) return;
    const bool subobject = !strcmp(stop.reason, "stream_subobject"), truncated = !strcmp(stop.reason, "stream_truncated");
    const uint64_t kind = 1ull << (subobject ? (std::min)(stop.type, 60u) : truncated ? 61 : !strcmp(stop.reason, "stream_alignment") ? 62 : 63);
    if (gPsoStreamLines.load(std::memory_order_relaxed) >= 4 || (gPsoStreamKinds.load(std::memory_order_relaxed) & kind)) return;
    const Origin origin = OriginOf(caller);
    if (!origin.strict && origin.provider < 0) return;
    if ((gPsoStreamKinds.fetch_or(kind) & kind) || gPsoStreamLines.fetch_add(1) >= 4) return;
    try
    {
        Log("cuda_pso_refused", std::string("\"reason\":\"") + stop.reason + "\"" + (subobject || truncated
            ? ",\"subobject_type\":" + std::to_string(stop.type) + ",\"offset\":" + std::to_string(stop.offset) : std::string()) +
            "," + OriginFields(caller, origin));
    }
    catch (...) {}
}

using CreateStreamFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE HookCreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** out)
{
    void* caller = _ReturnAddress();
    const HRESULT result = DeviceOriginal<CreateStreamFn>(self, kCreatePipelineState)(self, desc, iid, out);
    if (SUCCEEDED(result) && desc && out && *out && gActive.load(std::memory_order_acquire))
    {
        StreamStop stop;
        if (const D3D12_SHADER_BYTECODE* shader = StreamComputeShader(*desc, stop))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
            compute.CS = *shader;
            Recognize(self, compute, iid, *out, caller);
        }
        else if (FromProvider(caller) || FromWrapper(caller)) // not walked for the game's own pipelines on the runtime's device
            StreamRefused(stop, caller);
    }
    return result;
}

// Every distinct vtable of the device's interfaces, with the highest interface level it serves.
template <class Interface>
void AddDeviceVtable(ID3D12Device* device, int level, std::vector<std::pair<void**, int>>& out)
{
    ComPtr<Interface> view;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&view)))) return;
    void** vtable = *reinterpret_cast<void***>(view.Get());
    for (auto& entry : out) if (entry.first == vtable) { entry.second = (std::max)(entry.second, level); return; }
    out.emplace_back(vtable, level);
}

bool PatchDevice(ID3D12Device* device, std::string& reason)
{
    std::vector<std::pair<void**, int>> vtables;
    AddDeviceVtable<ID3D12Device>(device, 0, vtables);
    AddDeviceVtable<ID3D12Device1>(device, 1, vtables);
    AddDeviceVtable<ID3D12Device2>(device, 2, vtables);
    AddDeviceVtable<ID3D12Device3>(device, 3, vtables);
    AddDeviceVtable<ID3D12Device4>(device, 4, vtables);
    AddDeviceVtable<ID3D12Device5>(device, 5, vtables);
    AddDeviceVtable<ID3D12Device6>(device, 6, vtables);
    AddDeviceVtable<ID3D12Device7>(device, 7, vtables);
    AddDeviceVtable<ID3D12Device8>(device, 8, vtables);
    AddDeviceVtable<ID3D12Device9>(device, 9, vtables);
    AddDeviceVtable<ID3D12Device10>(device, 10, vtables);
    // The provider's lists are created through these (ID3D12Device4 for CreateCommandList1).
    if (std::none_of(vtables.begin(), vtables.end(), [](const auto& entry) { return entry.second >= 4; }))
    { reason = "device4"; return false; }
    // ID3D12Device2 adds CreatePipelineState (streams), ID3D12Device4 CreateCommandList1.
    const Hook hooks[] = {{kCreateComputePipelineState, reinterpret_cast<void*>(&HookCreateComputePipelineState)},
        {kCreateCommandList, reinterpret_cast<void*>(&HookCreateCommandList)},
        {kCreatePipelineState, reinterpret_cast<void*>(&HookCreatePipelineState)},
        {kCreateCommandList1, reinterpret_cast<void*>(&HookCreateCommandList1)}};
    for (const auto& [vtable, level] : vtables)
        if (!ApplyDevice(vtable, std::span<const Hook>(hooks, level >= 4 ? 4 : level >= 2 ? 3 : 2)))
        { reason = "device_patch"; return false; }
    return true;
}

bool ProviderRange(HMODULE module, uintptr_t& begin, uintptr_t& end)
{
    if (!module) return false;
    const auto base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    begin = reinterpret_cast<uintptr_t>(base);
    end = begin + nt->OptionalHeader.SizeOfImage;
    return true;
}

bool Contains(std::span<const unsigned char> bytes, const char* name)
{
    const size_t n = std::strlen(name) + 1; // with the terminator: exactly this symbol
    return std::search(bytes.begin(), bytes.end(), name, name + n) != bytes.end();
}

void MarkUnavailable(const std::string& reason)
{
    gReason = reason;
    gState.store(State::Unavailable, std::memory_order_release);
    Log("cuda_route", "\"state\":\"unavailable\",\"reason\":\"" + reason + "\",\"compute_capability\":\"" +
        std::to_string(gMajor) + "." + std::to_string(gMinor) + "\"");
}
}

void SetLog(LogFunction log) noexcept { gLog = log; }
void Report(const char* stage, const std::string& fields) noexcept { Log(stage, fields); }

void SetStackRecognition(bool enabled) noexcept { gStackRecognition.store(enabled, std::memory_order_relaxed); }

bool Install(ID3D12Device* device, HMODULE providerModule, std::span<const unsigned char> fatbin, std::string& reason, bool adoptProviderLists) noexcept
{
    std::lock_guard lock(gMutex);
    try
    {
        const auto state = gState.load(std::memory_order_acquire);
        std::string owner;
        const char* how{};
        ComPtr<ID3D12Device> native;
        if (state == State::Active)
        {
            ComPtr<IUnknown> identity;
            if (device && ResolveNative(device, native, owner, how) && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&identity)))
                && identity.Get() == gDeviceIdentity)
            {
                uintptr_t begin{}, end{};
                if (ProviderRange(providerModule, begin, end) && begin == gProviderBegin)
                {
                    if (adoptProviderLists) gAdoptProviderLists.store(true, std::memory_order_relaxed);
                    return true;
                }
                reason = "other_provider_image";
                return false;
            }
            reason = "other_device";
            return false;
        }
        if (state != State::NotAttempted) { reason = gReason; return false; }
        if (!device) { MarkUnavailable("no_device"); reason = gReason; return false; }
        if (!ResolveNative(device, native, owner, how))
        {
            Log("cuda_route_device", "\"owner\":\"" + owner + "\",\"native\":\"unresolved\"");
            MarkUnavailable("wrapped_device");
            reason = gReason;
            return false;
        }
        if (native.Get() != device)
        {
            // The provider's runtime calls arrive from the wrapper's image (kept mapped: its device lives on).
            HMODULE wrapper{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    static_cast<LPCWSTR>(*reinterpret_cast<void**>(device)), &wrapper)
                || !ProviderRange(wrapper, gWrapperBegin, gWrapperEnd))
            { MarkUnavailable("wrapper_image"); reason = gReason; return false; }
            Log("cuda_route_device", "\"owner\":\"" + owner + "\",\"native\":\"" + how + "\"");
        }
        device = native.Get(); // from here on, the runtime's device
        // The runtime's image (kept mapped: the device lives on), whose class tables a list may keep.
        {
            HMODULE runtime{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    static_cast<LPCWSTR>(*reinterpret_cast<void**>(device)), &runtime)
                || !ProviderRange(runtime, gRuntimeBegin, gRuntimeEnd))
                gRuntimeBegin = gRuntimeEnd = 0;
        }
        const LUID luid = device->GetAdapterLuid();
        if (VendorOf(luid) != 0x10DE) { MarkUnavailable("not_nvidia"); reason = gReason; return false; }
        std::string name;
        if (!Capability(luid, gMajor, gMinor, name)) { MarkUnavailable("compute_capability_unknown"); reason = gReason; return false; }
        // Other architectures, and Turing without tensor cores, keep the DXIL port.
        if (!SupportedArchitecture(gMajor, gMinor, name))
        {
            MarkUnavailable(gMajor == 7 && gMinor == 5 ? "no_tensor_cores" : "unsupported_architecture");
            reason = gReason;
            return false;
        }
        if (fatbin.empty()) { MarkUnavailable("no_kernels"); reason = gReason; return false; }
        for (const char* name : kKernels)
            if (!Contains(fatbin, name)) { MarkUnavailable("kernel_missing"); reason = gReason; return false; }
        std::string failure;
        if (!LoadNvapi(gNvapi, failure)) { MarkUnavailable(failure); reason = gReason; return false; }
        if (!ProviderRange(providerModule, gProviderBegin, gProviderEnd)) { MarkUnavailable("provider_image"); reason = gReason; return false; }
        for (size_t i = 0; i < std::size(kKernels); ++i)
        {
            void* handle{};
            const int created = GuardedCreate(gNvapi.create, device, fatbin.data(), uint32_t(fatbin.size()), kKernels[i], &handle);
            if (created != 0 || !handle)
            {
                // After a fault inside the driver nothing more is asked of it (the kernels already made stay).
                if (created != kNvFault)
                    for (size_t j = 0; j < i; ++j) { gNvapi.destroy(device, gKernels[j]); gKernels[j] = nullptr; }
                char code[16]{};
                snprintf(code, sizeof(code), "0x%08lX", gFaultCode.load(std::memory_order_relaxed));
                MarkUnavailable(created == kNvFault ? std::string("kernel_create_fault_") + code : std::string("kernel_create_") + kKernels[i]);
                reason = gReason;
                return false;
            }
            gKernels[i] = handle;
        }
        ComPtr<IUnknown> identity;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity)))) { MarkUnavailable("device_identity"); reason = gReason; return false; }
        gIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // The hooks check the device identity: it is published before they are installed.
        gDeviceIdentity = identity.Detach();
        gDevice = device;
        gDevice->AddRef();
        gAdoptProviderLists.store(adoptProviderLists, std::memory_order_relaxed);
        if (!PatchDevice(device, failure)) { MarkUnavailable(failure); reason = gReason; return false; }
        gReason.clear();
        gActive.store(true, std::memory_order_release);
        gState.store(State::Active, std::memory_order_release);
        Log("cuda_route", "\"state\":\"active\",\"compute_capability\":\"" + std::to_string(gMajor) + "." + std::to_string(gMinor) +
            "\",\"kernels\":" + std::to_string(std::size(kKernels)) + ",\"layers\":" + std::to_string(std::size(kLayers)) +
            ",\"device_vtables\":" + std::to_string(gDevicePatchCount.load()) + ",\"device\":\"" + how + "\"");
        return true;
    }
    catch (...)
    {
        MarkUnavailable("exception");
        reason = gReason;
        return false;
    }
}

void Disable(const char* reason) noexcept
{
    std::lock_guard lock(gMutex);
    if (gState.load() != State::NotAttempted) return;
    try { gReason = reason ? reason : "disabled"; } catch (...) {}
    gState.store(State::Disabled, std::memory_order_release);
    Log("cuda_route", std::string("\"state\":\"disabled\",\"reason\":\"") + (reason ? reason : "disabled") + "\"");
}

void ForgetLayers() noexcept
{
    std::lock_guard lock(gMutex);
    gPipelineGeneration.fetch_add(1, std::memory_order_acq_rel);
    gAnyLayer.store(false, std::memory_order_release);
    for (auto& key : gKeys) key.store(nullptr, std::memory_order_release);
    for (const auto& held : gHeld) held.state->Release();
    gHeld.clear();
    gPipelineGeneration.fetch_add(1, std::memory_order_release);
    gLayerCount.store(0, std::memory_order_relaxed);
    // The provider's lists' own tables get their originals back and the lists are released.
    for (auto& table : gTables) Retire(table);
}

Status Snapshot() noexcept
{
    Status status;
    std::lock_guard lock(gMutex);
    if (gVulkanReported.load(std::memory_order_acquire)) return gVulkanStatus;
    status.state = gState.load(std::memory_order_acquire);
    status.major = gMajor;
    status.minor = gMinor;
    status.layers = gLayerCount.load(std::memory_order_relaxed);
    status.launches = gLaunches.load(std::memory_order_relaxed);
    status.fallbacks = gFallbacks.load(std::memory_order_relaxed);
    try { status.reason = gReason; } catch (...) {}
    return status;
}

bool Launched() noexcept
{
    return (gVulkanActive.load(std::memory_order_acquire) && gVulkanLaunched.load(std::memory_order_relaxed)) || (gState.load(std::memory_order_acquire) == State::Active && gLaunches.load(std::memory_order_relaxed) != 0);
}

bool TrySnapshot(Status& status) noexcept {
    std::unique_lock guard(gMutex,std::try_to_lock);
    if(!guard.owns_lock()) return false;
    status.state=gState.load(std::memory_order_acquire);
    status.major=gMajor; status.minor=gMinor;
    status.layers=gLayerCount.load(std::memory_order_relaxed);
    status.launches=gLaunches.load(std::memory_order_relaxed);
    status.fallbacks=gFallbacks.load(std::memory_order_relaxed);
    try { status.reason=gReason; } catch(...) {}
    return true;
}

// Architectures with SASS in the fatbin: 7.5 (RTX 20), 8.6 (RTX 30; also runs RTX 40), 8.9 (RTX 40) and 12.0
// (RTX 50; 12.1, GB10, is not admitted).
// Compute capability 7.5 also covers the Turing parts without tensor cores (TU116/TU117: GTX 16, MX, T-series
// such as the T1000); they keep the DXIL port, so 7.5 needs an RTX name or the Tesla T4.
bool SupportedArchitecture(int major, int minor, const std::string& name) noexcept
{
    if ((major == 8 && (minor == 6 || minor == 9)) || (major == 12 && minor == 0)) return true;
    if (major != 7 || minor != 5) return false;
    constexpr std::string_view t4 = " T4";
    return name.find("RTX") != std::string::npos
        || (name.size() >= t4.size() && name.compare(name.size() - t4.size(), t4.size(), t4) == 0);
}

uint8_t LayerOf(ID3D12PipelineState* state) noexcept { return Lookup(state); }
void SetLayerMask(uint32_t mask) noexcept { gLayerMask.store(mask, std::memory_order_relaxed); }
bool LayerEnabled(uint8_t id) noexcept
{
    return id && id <= std::size(kLayers) && (gLayerMask.load(std::memory_order_relaxed) & (1u << (id - 1)));
}
bool LaunchOn(ID3D12GraphicsCommandList* real, const ListState& state, UINT x, UINT y, UINT z) noexcept
{
    return real && gActive.load(std::memory_order_acquire) && LayerEnabled(state.layer) && Launch(real, state, x, y, z);
}
bool CanRecord(ID3D12GraphicsCommandList* real, std::string& reason) noexcept
{
    try
    {
        std::lock_guard lock(gMutex);
        if (!real || !gActive.load(std::memory_order_acquire)) { reason = "inactive"; return false; }
        const auto type = real->GetType();
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        { reason = "list_type"; return false; }
        ComPtr<IUnknown> identity;
        if (FAILED(real->GetDevice(IID_PPV_ARGS(&identity))) || identity.Get() != gDeviceIdentity)
        { reason = "list_device"; return false; }
        bool shared{}, runtimeClass{}, writable{};
        if (ClassifyList(real, shared, runtimeClass, writable)) { reason = "list_table"; return false; }
        reason = runtimeClass ? "runtime_class" : shared ? "system_class" : "own";
        return true;
    }
    catch (...) { return false; }
}
bool AdapterCapability(const LUID& luid, int& major, int& minor, std::string& name) noexcept
{
    try { return Capability(luid, major, minor, name); } catch (...) { return false; }
}
uint64_t NewPipelineToken() noexcept { return gNextPipelineToken.fetch_add(1, std::memory_order_relaxed); }
PipelineScope::PipelineScope(uint64_t token) noexcept : previous_(tPipelineToken) { tPipelineToken = token; }
PipelineScope::~PipelineScope() { tPipelineToken = previous_; }
void ForgetPipelines(uint64_t token) noexcept
{
    if (!token) return;
    std::lock_guard lock(gMutex);
    gPipelineGeneration.fetch_add(1, std::memory_order_acq_rel);
    gAnyLayer.store(false, std::memory_order_release);
    for (auto& key : gKeys) key.store(nullptr, std::memory_order_release);
    std::array<ID3D12PipelineState*, kSlots/2> removed{};
    size_t count{};
    auto out = gHeld.begin();
    for (auto it = gHeld.begin(); it != gHeld.end(); ++it)
        if (it->token == token) removed[count++] = it->state;
        else *out++ = *it;
    gHeld.erase(out, gHeld.end());
    for (const auto& held : gHeld) Insert(held.state, held.layer);
    gLayerCount.store(uint32_t(gHeld.size()), std::memory_order_relaxed);
    gAnyLayer.store(!gHeld.empty(), std::memory_order_release);
    gPipelineGeneration.fetch_add(1, std::memory_order_release);
    for (size_t i = 0; i < count; ++i) removed[i]->Release();
}
void ReportVulkan(State state, int major, int minor, uint32_t layers, const char* reason) noexcept
{
    std::lock_guard lock(gMutex);
    gVulkanStatus.state = state; gVulkanStatus.major = major; gVulkanStatus.minor = minor; gVulkanStatus.layers = layers;
    try { gVulkanStatus.reason = reason ? reason : ""; } catch (...) {}
    gVulkanActive.store(state == State::Active, std::memory_order_release);
    gVulkanReported.store(true, std::memory_order_release);
}
void CountVulkan(uint64_t launches, uint64_t fallbacks) noexcept
{
    std::lock_guard lock(gMutex);
    gVulkanStatus.launches += launches; gVulkanStatus.fallbacks += fallbacks;
    if (launches) gVulkanLaunched.store(true, std::memory_order_relaxed);
}

std::span<const LayerInfo> Layers() noexcept { return kLayerInfos; }
}
