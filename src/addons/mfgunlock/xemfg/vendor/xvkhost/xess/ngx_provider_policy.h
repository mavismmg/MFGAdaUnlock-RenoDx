#pragma once
// ngxprov.1: the XeMFG Nvngx replacement provider's API-free decisions (xess/ngx_provider.cpp,
// xess/vulkan/vk_provider.cpp, xess/d3d12_provider.cpp). Header only, so tests/xemfg_standalone checks them on the CPU.
#include <dxgiformat.h>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>

namespace ngx_provider
{
// Generated frames per real frame: 6x.
constexpr uint32_t kMaxGenerated = 5;

// DLSS-G asks for a real frame's generated frames in turn, DLSSG.MultiFrameIndex 1..count, one evaluation each. A batch
// starts at index 1; an index that doesn't follow the previous one, or a new count, also starts one, so a skipped first
// request never reuses the last real frame's batch (vkprov.3).
inline bool NewBatch(uint32_t index, uint32_t lastIndex, uint32_t count, uint32_t batchCount, uint64_t batches) noexcept
{
    return index == 1 || index <= lastIndex || count != batchCount || batches == 0;
}

inline bool SameName(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

inline bool IsDll(std::wstring_view name) noexcept
{
    return name.size() > 4 && SameName(name.substr(name.size() - 4), L".dll");
}

// The DLL OptiScaler loads from custom\<name>\dlssg (its misc\CustomProviders.h DlssgDllPath):
// nvngx_dlssg.dll when the folder has it, else the first .dll that exports NVSDK_NGX_D3D12_Init or
// NVSDK_NGX_VULKAN_Init, else the first .dll. `names` in directory order, `exportsNgx(name)` reads that file's export
// table; empty when there's no DLL.
template <class ExportsNgx>
inline std::wstring_view OptiscalerDlssgPick(std::span<const std::wstring> names, ExportsNgx&& exportsNgx)
{
    for (const auto& name : names)
        if (SameName(name, L"nvngx_dlssg.dll")) return name;
    std::wstring_view first;
    for (const auto& name : names) {
        if (!IsDll(name)) continue;
        if (exportsNgx(std::wstring_view(name))) return name;
        if (first.empty()) first = name;
    }
    return first;
}

// D3D12: the format of the provider's back-buffer copies and of XeFG's outputs. XeFG writes its outputs through UAVs,
// so an sRGB or typeless back buffer maps to the UNORM (FLOAT) member of its group; copies within a group are allowed.
// UNKNOWN for a format XeFG's swap chain doesn't take as a back buffer.
inline DXGI_FORMAT ColorFormat(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Whether a copy between the two color formats is allowed (same group, both taken as a back buffer).
inline bool SameColorGroup(DXGI_FORMAT a, DXGI_FORMAT b) noexcept
{
    const DXGI_FORMAT group = ColorFormat(a);
    return group != DXGI_FORMAT_UNKNOWN && group == ColorFormat(b);
}

// Depth-stencil formats: copied whole (CopyResource), into a texture of the same format that allows depth-stencil.
inline bool IsDepthStencilFormat(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return true;
    default: return false;
    }
}

// D3D12: the size XeFG reads depth and motion at. An engine can pad one of the two textures to an alignment (UE 4.26:
// depth 604x452 beside motion 603x452), so a difference of a few texels is padding and the area both cover is the
// picture. A larger difference is a real scale difference, which this provider doesn't resample: zero width then.
struct InputExtent
{
    uint32_t width = 0, height = 0;
};
constexpr uint32_t kMaxInputPadding = 31; // an alignment of up to 32 texels
inline InputExtent DepthMotionExtent(uint32_t depthWidth, uint32_t depthHeight, uint32_t motionWidth, uint32_t motionHeight) noexcept
{
    const uint32_t width = depthWidth < motionWidth ? depthWidth : motionWidth;
    const uint32_t height = depthHeight < motionHeight ? depthHeight : motionHeight;
    if (!width || !height) return {};
    const uint32_t extraWidth = (depthWidth < motionWidth ? motionWidth : depthWidth) - width;
    const uint32_t extraHeight = (depthHeight < motionHeight ? motionHeight : depthHeight) - height;
    const auto padding = [](uint32_t extra, uint32_t size) { return extra <= kMaxInputPadding && extra * 8 <= size; };
    return padding(extraWidth, width) && padding(extraHeight, height) ? InputExtent{width, height} : InputExtent{};
}

// ngxrect.1: a region of an input texture, as DLSS-G's DLSSG.<Input>Subrect{BaseX,BaseY,Width,Height} give it.
struct InputRect
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const InputRect&) const = default;
};

inline bool RectFits(const InputRect& rect, uint32_t textureWidth, uint32_t textureHeight) noexcept
{
    return rect.width && rect.height && rect.x < textureWidth && rect.y < textureHeight &&
           rect.width <= textureWidth - rect.x && rect.height <= textureHeight - rect.y;
}

// The areas of depth and motion XeFG reads; XeFG refuses two of different sizes. An engine can keep the picture in the
// corner of a larger texture (UE 4.27: depth 2560x2048 beside motion 2560x1440), so the game's rectangles come first,
// with DepthMotionExtent's rule between them; else the whole textures by the same rule. Empty when neither gives one.
struct DepthMotionRects
{
    InputRect depth, motion;
    bool fromSubrects = false;
    bool operator==(const DepthMotionRects&) const = default;
};
inline DepthMotionRects ChooseDepthMotionRects(const InputRect& depth, const InputRect& motion, uint32_t depthWidth,
                                               uint32_t depthHeight, uint32_t motionWidth, uint32_t motionHeight) noexcept
{
    if (RectFits(depth, depthWidth, depthHeight) && RectFits(motion, motionWidth, motionHeight)) {
        const InputExtent area = DepthMotionExtent(depth.width, depth.height, motion.width, motion.height);
        if (area.width)
            return {{depth.x, depth.y, area.width, area.height}, {motion.x, motion.y, area.width, area.height}, true};
    }
    const InputExtent whole = DepthMotionExtent(depthWidth, depthHeight, motionWidth, motionHeight);
    const InputRect rect{0, 0, whole.width, whole.height};
    return {rect, rect, false};
}

// The network XeFG runs on this route. OptiScaler.ini [XeFG] XeFGModel decides when it names one (iniModel 1: our port
// of Intel's MFG model, the XMX port, 2: XeFG's own 2x network), then the control file's xessModel 1 (the 2x network).
// xmxdefault.1: otherwise every GPU runs the XMX port, as OptiScaler's own XeFG output does; on NVIDIA its int8 layers
// can run on the tensor cores (the CUDA port, ngxcuda.1).
inline bool ProviderUsesPort(uint32_t iniModel, bool control2x) noexcept
{
    if (iniModel == 1) return true;
    if (iniModel == 2 || control2x) return false;
    return true;
}

// pluginprov.2: the outputs an evaluation has XeFG record (ExecuteParams firstOutput and outputCount). XeFG's swap chain
// spreads a batch over its presents; so does this provider: the batch's first evaluation records the pair's shared work
// with output 0 and every output up to its own index (a skipped index 1 included), each later one only its own.
// wholeBatch (RTXMFG_PROVIDER_WHOLE_BATCH=1): the first evaluation records them all (outputCount 0), the later ones
// nothing. `through`: the outputs ready once this evaluation's are recorded.
struct OutputRange
{
    bool record = false;
    uint32_t first = 0, count = 0, through = 0;
    bool operator==(const OutputRange&) const = default;
};
inline OutputRange ExecuteRange(bool newBatch, uint32_t index, uint32_t batchCount, bool wholeBatch) noexcept
{
    if (!index || index > batchCount) return {};
    if (wholeBatch) return newBatch ? OutputRange{true, 0, 0, batchCount} : OutputRange{};
    if (newBatch) return {true, 0, index, index};
    return {true, index - 1, 1, index};
}

// pluginprov.2: GPU time of the provider's work, from three timestamps per evaluation: its start, after the batch's input
// copies (the start again when it has none) and its end. Summed per real frame: `inputs` the copies, `total` everything
// the provider recorded over a batch's evaluations (copies, XeFG, the output and real-frame copies).
struct GpuStamps
{
    uint64_t start = 0, inputs = 0, end = 0;
};
class GpuTiming
{
  public:
    // A resolved evaluation; false (nothing added) when its stamps are not one finished span of under a second.
    bool Add(const GpuStamps& stamps, bool batchStart, uint64_t frequency) noexcept
    {
        if (!frequency || !stamps.start || stamps.inputs < stamps.start || stamps.end < stamps.inputs ||
            stamps.end - stamps.start > frequency)
            return false;
        if (batchStart) {
            ++batches_;
            inputs_ += stamps.inputs - stamps.start;
        }
        total_ += stamps.end - stamps.start;
        return true;
    }
    uint64_t Batches() const noexcept { return batches_; }
    // Milliseconds per real frame; 0 before any batch.
    double InputsMs(uint64_t frequency) const noexcept { return PerBatch(inputs_, frequency); }
    double TotalMs(uint64_t frequency) const noexcept { return PerBatch(total_, frequency); }
    void Reset() noexcept { *this = {}; }

  private:
    double PerBatch(uint64_t ticks, uint64_t frequency) const noexcept
    {
        return batches_ && frequency ? double(ticks) * 1000.0 / double(frequency) / double(batches_) : 0.0;
    }
    uint64_t batches_ = 0, inputs_ = 0, total_ = 0;
};
constexpr uint32_t kTimingRecords = 32;   // evaluations in flight before a record is read and reused

// pluginprov.2: D3D12 depth history as a plain color texture, as the Vulkan provider keeps it (R32_FLOAT): XeFG only reads
// it, and a depth-stencil copy carries the driver's depth compression (AMD's HTILE) into every read. CopyResource
// copies between the members of one format group. UNKNOWN keeps the source's own format (stencil formats, color).
inline DXGI_FORMAT ReadableDepthFormat(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
}
