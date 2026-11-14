#include "xvk_internal.h"

namespace xvk {

namespace {
Descriptor* At(D3D12_CPU_DESCRIPTOR_HANDLE handle) { return reinterpret_cast<Descriptor*>(handle.ptr); }

VkImageSubresourceLayers Layers(Resource* resource, UINT subresource)
{
    const auto& desc = resource->desc();
    const UINT mips = desc.MipLevels ? desc.MipLevels : 1;
    const FormatInfo format = Format(desc.Format, (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
    VkImageAspectFlags aspect = format.aspect;
    const UINT layers = desc.DepthOrArraySize;
    UINT plane = subresource / (mips * layers);
    subresource %= mips * layers;
    if (format.depth && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect = plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    return {aspect, subresource % mips, subresource / mips, 1};
}

VkExtent3D MipExtent(Resource* resource, uint32_t mip)
{
    const auto& desc = resource->desc();
    return {std::max<uint32_t>(1, uint32_t(desc.Width >> mip)), std::max<uint32_t>(1, desc.Height >> mip), 1};
}
} // namespace

CommandList::~CommandList() = default;

HRESULT STDMETHODCALLTYPE CommandList::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12GraphicsCommandList, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList, ID3D12GraphicsCommandList>(this, riid, out);
}

void CommandList::Fail(const char* what)
{
    if (!failed_) Log("command list error: %s", what);
    failed_ = true;
}

HRESULT CommandList::Begin(CommandAllocator* allocator)
{
    if (!allocator) return E_INVALIDARG;
    if (device_->recorded()) {
        // vkprov.1: straight into the open recording (begun and ended by its owner, never here).
        buffer_ = device_->RecordingBuffer();
        if (!buffer_) {
            static std::atomic<int> logged{0};
            if (logged++ < 8) Log("CommandList::Begin: no recording open on the recorded device");
            return E_FAIL;
        }
    } else {
        buffer_ = allocator->Allocate();
        if (!buffer_) return E_OUTOFMEMORY;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (device_->vk().vkBeginCommandBuffer(buffer_, &begin) != VK_SUCCESS) return E_FAIL;
    }
    // D3D12 makes earlier ExecuteCommandLists work on the queue visible to the next list (state decay); Vulkan
    // has no implicit dependency between submissions, so each list starts with one full barrier.
    Barrier();
    recording_ = true;
    failed_ = false;
    ClearState(nullptr);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandList::Close()
{
    if (!recording_) return E_FAIL;
    recording_ = false;
    if (!device_->recorded() && device_->vk().vkEndCommandBuffer(buffer_) != VK_SUCCESS) Fail("vkEndCommandBuffer");
    return failed_ ? E_INVALIDARG : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandList::Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial)
{
    if (recording_) return E_FAIL;
    const HRESULT hr = Begin(Own<CommandAllocator>(allocator));
    if (SUCCEEDED(hr) && initial) SetPipelineState(initial);
    return hr;
}

void STDMETHODCALLTYPE CommandList::ClearState(ID3D12PipelineState* pso)
{
    pso_ = nullptr;
    rootSignature_ = nullptr;
    boundPipeline_ = VK_NULL_HANDLE;
    tables_.fill(nullptr);
    constantMasks_.fill(0);
    if (pso) SetPipelineState(pso);
}

void STDMETHODCALLTYPE CommandList::SetPipelineState(ID3D12PipelineState* pso)
{
    pso_ = Own<PipelineState>(pso);
}

void STDMETHODCALLTYPE CommandList::SetComputeRootSignature(ID3D12RootSignature* rootSignature)
{
    RootSignature* next = Own<RootSignature>(rootSignature);
    if (next != rootSignature_) {
        // D3D12: changing the root signature invalidates all root arguments.
        tables_.fill(nullptr);
        constantMasks_.fill(0);
        rootSignature_ = next;
    }
}

void STDMETHODCALLTYPE CommandList::SetComputeRootDescriptorTable(UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    if (parameter >= tables_.size()) { Fail("root parameter index"); return; }
    tables_[parameter] = reinterpret_cast<const Descriptor*>(base.ptr);
}

void STDMETHODCALLTYPE CommandList::SetComputeRoot32BitConstant(UINT parameter, UINT value, UINT offset)
{
    if (parameter >= constants_.size() || offset >= constants_[0].size()) { Fail("root constant index"); return; }
    constants_[parameter][offset] = value;
    if (offset < 32) constantMasks_[parameter] |= 1u << offset;
}

void STDMETHODCALLTYPE CommandList::SetComputeRoot32BitConstants(UINT parameter, UINT count, const void* data, UINT offset)
{
    if (parameter >= constants_.size() || offset + count > constants_[0].size()) { Fail("root constants range"); return; }
    memcpy(constants_[parameter].data() + offset, data, size_t(count) * 4);
    for (UINT i = offset; i < std::min<UINT>(offset + count, 32); ++i) constantMasks_[parameter] |= 1u << i;
}

void STDMETHODCALLTYPE CommandList::Dispatch(UINT x, UINT y, UINT z)
{
    if (!recording_ || failed_) return;
    if (!pso_) { Fail("Dispatch without a pipeline state"); return; }
    if (device_->tensor().TryLaunch(*device_, buffer_, pso_->tensorLayer(), tables_[0], tables_[1], constants_[2].data(), constantMasks_[2], x, y, z))
    { boundPipeline_ = VK_NULL_HANDLE; return; }
    const auto& vk = device_->vk();
    if (boundPipeline_ != pso_->pipeline()) {
        vk.vkCmdBindPipeline(buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pso_->pipeline());
        boundPipeline_ = pso_->pipeline();
    }
    const auto& ranges = pso_->ranges();
    std::array<VkWriteDescriptorSet, 64> writes{};
    std::array<VkDescriptorImageInfo, 64> images{};
    std::array<VkDescriptorBufferInfo, 64> buffers{};
    if (ranges.size() > writes.size()) { Fail("too many bindings"); return; }
    for (size_t i = 0; i < ranges.size(); ++i) {
        const BoundRange& r = ranges[i];
        const Descriptor* table = tables_[r.parameter];
        if (!table) { Fail("descriptor table not set before Dispatch"); return; }
        const Descriptor& d = table[r.offset];
        VkWriteDescriptorSet& w = writes[i];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, r.binding, 0, 1, r.type};
        switch (r.type) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            if (d.kind != DescriptorKind::SrvImage) { Fail("binding expects a texture SRV"); return; }
            images[i] = d.image;
            w.pImageInfo = &images[i];
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (d.kind != DescriptorKind::UavImage) { Fail("binding expects a texture UAV"); return; }
            images[i] = d.image;
            w.pImageInfo = &images[i];
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            if (d.kind != DescriptorKind::SrvBuffer && d.kind != DescriptorKind::UavBuffer) { Fail("binding expects a buffer view"); return; }
            buffers[i] = d.buffer;
            w.pBufferInfo = &buffers[i];
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            if (d.kind != DescriptorKind::Cbv) { Fail("binding expects a CBV"); return; }
            buffers[i] = d.buffer;
            w.pBufferInfo = &buffers[i];
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            if (d.kind != DescriptorKind::Sampler) { Fail("binding expects a sampler"); return; }
            images[i] = d.image;
            w.pImageInfo = &images[i];
            break;
        default: Fail("descriptor type"); return;
        }
    }
    size_t count = ranges.size();
    const auto& samplers = pso_->staticSamplers();
    if (count + samplers.size() > writes.size()) { Fail("too many bindings"); return; }
    for (size_t s = 0; s < samplers.size(); ++s, ++count) {
        images[count] = {samplers[s], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, pso_->staticBindings()[s], 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
                         &images[count]};
    }
    if (count)
        vk.vkCmdPushDescriptorSetKHR(buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pso_->layout(), 0, uint32_t(count), writes.data());
    if (pso_->pushConstantBytes())
        vk.vkCmdPushConstants(buffer_, pso_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pso_->pushConstantBytes(),
                              constants_[pso_->constantsParameter()].data());
    vk.vkCmdDispatch(buffer_, x, y, z);
    for (const auto& range : ranges)
        if (const auto* table = tables_[range.parameter]; table && table[range.offset].kind == DescriptorKind::UavBuffer && table[range.offset].resource)
            table[range.offset].resource->NoteWrite();
}

void CommandList::Barrier()
{
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    device_->vk().vkCmdPipelineBarrier2(buffer_, &dependency);
}

namespace {
struct Scope
{
    VkPipelineStageFlags2 stage = 0;
    VkAccessFlags2 access = 0;
    bool write = false;
};

// Stages and accesses a D3D12 resource state implies (compute/copy usage; graphics states map conservatively).
Scope FromState(D3D12_RESOURCE_STATES state)
{
    constexpr VkPipelineStageFlags2 kAll = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (state == D3D12_RESOURCE_STATE_COMMON) return {kAll, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, true};
    Scope s;
    UINT rest = UINT(state);
    auto take = [&](UINT bit, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write) {
        if (!(rest & bit)) return;
        rest &= ~bit;
        s.stage |= stage;
        s.access |= access;
        s.write |= write;
    };
    constexpr VkPipelineStageFlags2 kShaders = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    take(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    take(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kShaders, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kShaders, VK_ACCESS_2_SHADER_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, kShaders, VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_INDEX_BUFFER, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_INDEX_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_COPY_SOURCE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_COPY_DEST, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    take(D3D12_RESOURCE_STATE_RESOLVE_SOURCE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    take(D3D12_RESOURCE_STATE_RESOLVE_DEST, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    if (rest) return {kAll, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, true};   // render target, depth, video...
    return s;
}
} // namespace

void STDMETHODCALLTYPE CommandList::ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    // Images stay in GENERAL, so barriers only order and publish memory. Each call becomes at most one
    // VkMemoryBarrier2 covering exactly the stages and accesses its states imply; read -> read transitions need none.
    if (!recording_ || failed_ || !count || !barriers) return;
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    bool needed = false;
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        switch (b.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: {
            const Scope before = FromState(b.Transition.StateBefore), after = FromState(b.Transition.StateAfter);
            if (!before.write && !after.write) break;   // read -> read: no hazard
            memory.srcStageMask |= before.stage;
            memory.srcAccessMask |= before.write ? (before.access & ~(VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
                                                                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT))
                                                 : 0;
            memory.dstStageMask |= after.stage;
            memory.dstAccessMask |= after.access;
            needed = true;
            break;
        }
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            memory.srcStageMask |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            memory.srcAccessMask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            memory.dstStageMask |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            memory.dstAccessMask |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            needed = true;
            break;
        default:   // aliasing: everything
            memory.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            memory.srcAccessMask |= VK_ACCESS_2_MEMORY_WRITE_BIT;
            memory.dstStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            memory.dstAccessMask |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            needed = true;
            break;
        }
    }
    if (!needed) return;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    device_->vk().vkCmdPipelineBarrier2(buffer_, &dependency);
}

void STDMETHODCALLTYPE CommandList::CopyBufferRegion(ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 bytes)
{
    if (!recording_ || failed_) return;
    auto* d = Own<Resource>(dst);
    auto* s = Own<Resource>(src);
    if (!d->IsBuffer() || !s->IsBuffer()) { Fail("CopyBufferRegion on a texture"); return; }
    d->NoteWrite();
    VkBufferCopy region{srcOffset, dstOffset, bytes};
    device_->vk().vkCmdCopyBuffer(buffer_, s->buffer(), d->buffer(), 1, &region);
}

void STDMETHODCALLTYPE CommandList::CopyResource(ID3D12Resource* dst, ID3D12Resource* src)
{
    if (!recording_ || failed_) return;
    auto* d = Own<Resource>(dst);
    auto* s = Own<Resource>(src);
    const auto& vk = device_->vk();
    if (d->IsBuffer() != s->IsBuffer()) { Fail("CopyResource between a buffer and a texture"); return; }
    if (d->IsBuffer()) {
        d->NoteWrite();
        VkBufferCopy region{0, 0, std::min(d->size(), s->size())};
        vk.vkCmdCopyBuffer(buffer_, s->buffer(), d->buffer(), 1, &region);
        return;
    }
    const auto& desc = d->desc();
    const UINT mips = desc.MipLevels ? desc.MipLevels : 1;
    const FormatInfo format = Format(desc.Format, (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
    std::vector<VkImageCopy> regions;
    std::vector<VkImageAspectFlags> aspects;
    if (format.depth && format.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspects = {VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT};
    else aspects = {format.aspect};
    for (VkImageAspectFlags aspect : aspects)
        for (UINT mip = 0; mip < mips; ++mip) {
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {aspect, mip, 0, desc.DepthOrArraySize};
            region.extent = MipExtent(d, mip);
            regions.push_back(region);
        }
    vk.vkCmdCopyImage(buffer_, s->image(), VK_IMAGE_LAYOUT_GENERAL, d->image(), VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()), regions.data());
}

void STDMETHODCALLTYPE CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
    if (!recording_ || failed_) return;
    if (!dst || !src) { Fail("CopyTextureRegion arguments"); return; }
    auto* d = Own<Resource>(dst->pResource);
    auto* s = Own<Resource>(src->pResource);
    const auto& vk = device_->vk();
    if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        const auto& f = src->PlacedFootprint;
        const FormatInfo format = Format(f.Footprint.Format);
        if (!format.bytes) { Fail("footprint format"); return; }
        VkBufferImageCopy region{};
        region.bufferOffset = f.Offset;
        region.bufferRowLength = f.Footprint.RowPitch / format.bytes;
        region.bufferImageHeight = f.Footprint.Height;
        region.imageSubresource = Layers(d, dst->SubresourceIndex);
        region.imageOffset = {int32_t(x), int32_t(y), int32_t(z)};
        region.imageExtent = {f.Footprint.Width, f.Footprint.Height, f.Footprint.Depth};
        if (box) {
            region.bufferOffset += uint64_t(box->front) * f.Footprint.RowPitch * f.Footprint.Height + uint64_t(box->top) * f.Footprint.RowPitch +
                                   uint64_t(box->left) * format.bytes;
            region.imageExtent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
        }
        vk.vkCmdCopyBufferToImage(buffer_, s->buffer(), d->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return;
    }
    if (dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        const auto& f = dst->PlacedFootprint;
        const FormatInfo format = Format(f.Footprint.Format);
        if (!format.bytes) { Fail("footprint format"); return; }
        VkBufferImageCopy region{};
        region.bufferOffset = f.Offset + uint64_t(z) * f.Footprint.RowPitch * f.Footprint.Height + uint64_t(y) * f.Footprint.RowPitch +
                              uint64_t(x) * format.bytes;
        region.bufferRowLength = f.Footprint.RowPitch / format.bytes;
        region.bufferImageHeight = f.Footprint.Height;
        region.imageSubresource = Layers(s, src->SubresourceIndex);
        const VkExtent3D full = MipExtent(s, region.imageSubresource.mipLevel);
        region.imageExtent = box ? VkExtent3D{box->right - box->left, box->bottom - box->top, box->back - box->front} : full;
        if (box) region.imageOffset = {int32_t(box->left), int32_t(box->top), int32_t(box->front)};
        d->NoteWrite();
        vk.vkCmdCopyImageToBuffer(buffer_, s->image(), VK_IMAGE_LAYOUT_GENERAL, d->buffer(), 1, &region);
        return;
    }
    if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        VkImageCopy region{};
        region.srcSubresource = Layers(s, src->SubresourceIndex);
        region.dstSubresource = Layers(d, dst->SubresourceIndex);
        region.dstOffset = {int32_t(x), int32_t(y), int32_t(z)};
        region.extent = MipExtent(s, region.srcSubresource.mipLevel);
        if (box) {
            region.srcOffset = {int32_t(box->left), int32_t(box->top), int32_t(box->front)};
            region.extent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
        }
        vk.vkCmdCopyImage(buffer_, s->image(), VK_IMAGE_LAYOUT_GENERAL, d->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return;
    }
    Fail("CopyTextureRegion between two footprints");
}

void CommandList::ClearUav(Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE cpu, const VkClearColorValue& value, uint32_t fillValue)
{
    const Descriptor& d = *At(cpu);
    const auto& vk = device_->vk();
    if (d.kind == DescriptorKind::UavBuffer) {
        if (resource) resource->NoteWrite();
        vk.vkCmdFillBuffer(buffer_, d.buffer.buffer, d.buffer.offset, d.buffer.range, fillValue);
        return;
    }
    if (d.kind != DescriptorKind::UavImage || !resource || resource->IsBuffer()) { Fail("ClearUnorderedAccessView descriptor"); return; }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vk.vkCmdClearColorImage(buffer_, resource->image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource* resource,
    const UINT values[4], UINT rects, const D3D12_RECT*)
{
    if (!recording_ || failed_) return;
    if (rects) Unimplemented("ClearUnorderedAccessViewUint with rects (whole view cleared)");
    VkClearColorValue value{};
    memcpy(value.uint32, values, sizeof(value.uint32));
    ClearUav(Own<Resource>(resource), cpu, value, values[0]);
}

void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource* resource,
    const FLOAT values[4], UINT rects, const D3D12_RECT*)
{
    if (!recording_ || failed_) return;
    if (rects) Unimplemented("ClearUnorderedAccessViewFloat with rects (whole view cleared)");
    VkClearColorValue value{};
    memcpy(value.float32, values, sizeof(value.float32));
    uint32_t bits = 0;
    memcpy(&bits, &values[0], 4);
    ClearUav(Own<Resource>(resource), cpu, value, bits);
}

void STDMETHODCALLTYPE CommandList::EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
{
    if (!recording_ || failed_) return;
    if (type != D3D12_QUERY_TYPE_TIMESTAMP) { Unimplemented("EndQuery(non-timestamp)"); return; }
    const VkQueryPool pool = Own<QueryHeap>(heap)->pool();
    device_->vk().vkCmdResetQueryPool(buffer_, pool, index, 1);
    device_->vk().vkCmdWriteTimestamp2(buffer_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, index);
}

void STDMETHODCALLTYPE CommandList::ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* dst,
    UINT64 offset)
{
    if (!recording_ || failed_) return;
    if (type != D3D12_QUERY_TYPE_TIMESTAMP) { Unimplemented("ResolveQueryData(non-timestamp)"); return; }
    device_->vk().vkCmdCopyQueryPoolResults(buffer_, Own<QueryHeap>(heap)->pool(), start, count, Own<Resource>(dst)->buffer(), offset, 8,
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

} // namespace xvk
