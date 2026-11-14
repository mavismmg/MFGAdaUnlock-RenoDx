#pragma once
// vkprov.1: XeFG's internal D3D12 frame-generation API, which libxess_fg does not export. Its proxy swap chain drives
// it: one context per swap chain, Init with the back buffer's description, then one Execute per real frame that
// records every interpolated frame between the previous and the current real frame into the given command list.
//
// Layouts and entry points come from our own reverse engineering of XeFG 1.3.1.78 (XeSS SDK 3.0.2, libxess_fg
// EC5E0C65, the "fg78" family): CreateContext 0x1853B0 ("pDevice is null." / "phContext is null."), Init 0x1871C0
// ("pInitParams is null."), Execute 0x185800 ("pCommandList is null." / "pExecParams is null."), Destroy 0x1874C0.
// The swap chain's own calls fill them (0x219070 Init, 0x21A0F0 Execute); field names follow the validators' messages
// and the public xefg_swapchain headers.
// fg95prov.1: XeFG 1.3.4.95 ("fg95") has the same entries and layouts at other RVAs. Only the exact images in
// xefg_internal_profiles.h are used (Resolve).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace xefg_internal
{
#pragma pack(push, 8)
// A frame input, in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (the swap chain moves every tagged resource there).
struct Resource
{
    ID3D12Resource* resource;
    uint32_t baseX, baseY;     // valid region
    uint32_t width, height;
};
static_assert(sizeof(Resource) == 0x18);

// One real frame; the order is the swap chain's (back buffer first, HUD-less last).
struct Frame
{
    Resource backbuffer;               // +0x00, with UI
    Resource depth;                    // +0x18
    Resource motionVectors;            // +0x30
    Resource ui;                       // +0x48, UI texture with alpha
    Resource hudless;                  // +0x60
    float viewMatrix[16];              // +0x78, row-major
    float projectionMatrix[16];        // +0xB8, row-major
    float outputWidth, outputHeight;   // +0xF8, the back buffer's size
    float jitterX, jitterY;            // +0x100
    float motionVectorScaleX, motionVectorScaleY;   // +0x108
    uint32_t reserved0, reserved1;     // +0x110, the swap chain passes 0
};
static_assert(sizeof(Frame) == 0x118);
static_assert(offsetof(Frame, viewMatrix) == 0x78 && offsetof(Frame, outputWidth) == 0xF8);

struct OutputBase { uint32_t x, y; };

struct ExecuteParams
{
    Frame previous;                         // +0x000
    Frame current;                          // +0x118
    ID3D12Resource* const* outputs;         // +0x230, one per interpolated frame, earliest first
    const OutputBase* outputBases;          // +0x238, one per output; the swap chain passes (0, 0)
    uint8_t interpolatedFrames;             // +0x240
    uint8_t reserved241[3];
    uint32_t optional244;                   // +0x244, the swap chain passes 0 unless its pacing supplies a value
    ID3D12DescriptorHeap* descriptorHeap;   // +0x248, with XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP
    uint32_t descriptorHeapOffset;          // +0x250
    uint32_t uiComposition;                 // +0x254, 0: the back buffer alone (UI mode NONE)
    // pluginprov.2: the outputs this call records, which the swap chain sets on its later passes. firstOutput 0 also
    // records the pair's shared work; outputCount 0 records every output from firstOutput (0, 0: the whole batch).
    uint32_t firstOutput;                   // +0x258
    uint32_t outputCount;                   // +0x25C
};
static_assert(sizeof(ExecuteParams) == 0x260);
static_assert(offsetof(ExecuteParams, current) == 0x118 && offsetof(ExecuteParams, outputs) == 0x230 &&
              offsetof(ExecuteParams, interpolatedFrames) == 0x240 && offsetof(ExecuteParams, descriptorHeap) == 0x248 &&
              offsetof(ExecuteParams, uiComposition) == 0x254);

struct InitParams
{
    uint32_t width, height;                 // +0x00, the back buffer
    uint32_t format;                        // +0x08, DXGI_FORMAT of the back buffer
    uint32_t flags;                         // +0x0C, xefg_swapchain_init_flags_t
    uint32_t maxInterpolatedFrames;         // +0x10, 1 .. the (unlocked) maximum
    uint32_t creationNodeMask;              // +0x14
    uint32_t visibleNodeMask;               // +0x18
    uint32_t reserved1c;
    ID3D12Heap* tempBufferHeap;             // +0x20, null: allocated inside
    uint64_t bufferHeapOffset;              // +0x28
    ID3D12Heap* tempTextureHeap;            // +0x30, null: allocated inside
    uint64_t textureHeapOffset;             // +0x38
    void* pipelineLibrary;                  // +0x40
    uint32_t uiMode;                        // +0x48, xefg_swapchain_ui_mode_t (< 6)
    uint32_t reserved4c;
};
static_assert(sizeof(InitParams) == 0x50);
static_assert(offsetof(InitParams, maxInterpolatedFrames) == 0x10 && offsetof(InitParams, tempBufferHeap) == 0x20 &&
              offsetof(InitParams, uiMode) == 0x48);
#pragma pack(pop)

using Handle = void*;

// The entry points of the exact profiled image; every call returns XeFG's result code (0: success). XeFG throws C++
// exceptions out of its validators; the calls below catch them (SEH) and return kException.
constexpr int32_t kException = -1000;
struct Api
{
    HMODULE module = nullptr;
    const char* family = nullptr;   // the profile's ("fg78", "fg95"), static
    int32_t (*createContext)(ID3D12Device* device, Handle* context) = nullptr;
    int32_t (*init)(Handle context, const InitParams* params) = nullptr;
    int32_t (*execute)(Handle context, ID3D12GraphicsCommandList* list, const ExecuteParams* params) = nullptr;
    int32_t (*destroy)(Handle context) = nullptr;

    int32_t CreateContext(ID3D12Device* device, Handle* context) const noexcept;
    int32_t Init(Handle context, const InitParams& params) const noexcept;
    int32_t Execute(Handle context, ID3D12GraphicsCommandList* list, const ExecuteParams& params) const noexcept;
    int32_t Destroy(Handle context) const noexcept;
};

// The module must be a profiled image (its PE timestamp, size and checksum: fg78 or fg95), each entry that image's
// witnessed bytes in its executable image pages. False with `reason` otherwise ("build_not_profiled" for any other
// image); nothing is written to the module.
bool Resolve(HMODULE fg, Api& api, std::string& reason) noexcept;
}
