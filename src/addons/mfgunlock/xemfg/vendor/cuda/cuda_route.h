#pragma once
// The tensor-core route for XeFG's ported MFG model.
//
// On an NVIDIA adapter with compute capability 7.5 (RTX 20; not the Turing parts without tensor
// cores), 8.6 (RTX 30), 8.9 (RTX 40) or 12.0 (RTX 50), each Dispatch of a
// recognized int8 conv layer program becomes a launch of the equivalent CUDA kernel
// (xefg_cuda_layers.cu) in the same D3D12 command list, through NvAPI_D3D12_LaunchCubinShader:
// no CUDA context, no second queue, no fence. The kernel reads and writes the layer's own
// descriptors (the tables XeFG bound for that Dispatch), and its output is byte-identical to the
// Intel program's (tests/xess_cuda).
//
// Recognition is exact: a pipeline state counts only when the provider image created it from a
// program whose SHA-256 is in the layer table: directly, through the wrapper of its device, or
// through a module that hooks pipeline creation in between, the provider's image then
// being a frame of a bounded stack walk above that module. A Dispatch is replaced only when its recording
// state is complete on the recording thread and every handle resolves; anything else keeps the
// Dispatch, which runs the DXIL port as before.
//
// Only the provider's own command lists are hooked: the lists its image creates (the device's
// CreateCommandList/CreateCommandList1) get their first Reset there and the hooks go into the table
// each list then keeps. Agility SDK runtimes give every list its own table on that Reset; the
// runtime's shared tables, copied into every game list's, are not written.
#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
#include <span>
#include <string>

namespace xefg_cuda
{
using LogFunction = void (*)(const char* stage, const std::string& fields) noexcept;
void SetLog(LogFunction log) noexcept;
void Report(const char* stage, const std::string& fields) noexcept;

enum class State : uint32_t { NotAttempted = 0, Active = 1, Unavailable = 2, Disabled = 3 };

// Before the provider creates its pipeline states and command lists (its first context), under the
// caller's lock.
// providerModule: the libxess_fg image whose CreateComputePipelineState calls are recognized.
// fatbin: the kernels (embedded RCDATA, verified by the caller). Idempotent on the same device;
// a later different device is refused (its contexts keep the DXIL port). A device another module
// wraps (ReShade, Streamline, Special K) is resolved to the runtime's device under it; the route then
// works on that device's own lists and tables, which the wrapper forwards the provider's calls to, and
// never writes the wrapper's. A wrapper it cannot see through is refused ("wrapped_device").
bool Install(ID3D12Device* device, HMODULE providerModule, std::span<const unsigned char> fatbin,
    std::string& reason, bool adoptProviderLists = true) noexcept;

struct ListState
{
    uint8_t layer{};
    ID3D12DescriptorHeap* heap{};
    D3D12_GPU_DESCRIPTOR_HANDLE tables[2]{};
    uint32_t tableMask{};
    uint32_t constants[12]{};
    uint32_t constantMask{};
};
uint8_t LayerOf(ID3D12PipelineState*) noexcept;
bool LaunchOn(ID3D12GraphicsCommandList*, const ListState&, UINT x, UINT y, UINT z) noexcept;
bool CanRecord(ID3D12GraphicsCommandList*, std::string& reason) noexcept;
bool AdapterCapability(const LUID&, int& major, int& minor, std::string& name) noexcept;
void SetLayerMask(uint32_t mask) noexcept;
bool LayerEnabled(uint8_t layerId) noexcept;
uint64_t NewPipelineToken() noexcept;
struct PipelineScope
{
    explicit PipelineScope(uint64_t) noexcept;
    ~PipelineScope();
    PipelineScope(const PipelineScope&) = delete;
    PipelineScope& operator=(const PipelineScope&) = delete;
private:
    uint64_t previous_;
};
void ForgetPipelines(uint64_t token) noexcept;
void ReportVulkan(State, int major, int minor, uint32_t layers, const char* reason) noexcept;
void CountVulkan(uint64_t launches, uint64_t fallbacks) noexcept;

// The configuration turned the route off (OptiScaler.ini [XeFG] XeFGTensorCores = off).
void Disable(const char* reason) noexcept;

// Tests: false limits pipeline states to the caller rule (the provider is the caller, or the first frame after the
// wrapper's), which lists always keep. On by default.
void SetStackRecognition(bool enabled) noexcept;

// No provider context remains: release the recognized pipeline states, give the provider's own list
// tables their originals back and release those lists. The device hooks (and a shared table of a
// system runtime) stay installed and pass every call through until new layers appear.
void ForgetLayers() noexcept;

struct Status
{
    State state{};
    int major{}, minor{};
    uint32_t layers{};        // recognized pipeline states
    uint64_t launches{}, fallbacks{};
    std::string reason;
};
Status Snapshot() noexcept;
// Presentation-overlay reader: never wait for installation/retirement work.
bool TrySnapshot(Status& out) noexcept;
// The architecture gate (tests): compute capability major.minor of the adapter and its CUDA device name.
bool SupportedArchitecture(int major, int minor, const std::string& name) noexcept;
// Lock-free: the route is active and a layer has run on tensor cores (OptiScaler's "XeFG model" line).
bool Launched() noexcept;

// Layer table and kernel names (tests). ownSha256: RTXMFG's own DXIL for the same layer, which ShaderPort
// swaps in for Intel's program; the provider then creates the pipeline from those bytes (nullptr when there is none).
struct LayerInfo { const char* program; const char* sha256; const char* kernel; const char* ownSha256 = nullptr; };
std::span<const LayerInfo> Layers() noexcept;
}
