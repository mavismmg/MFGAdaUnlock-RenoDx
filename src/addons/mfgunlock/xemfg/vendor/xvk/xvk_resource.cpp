#include "xvk_internal.h"

namespace xvk {

bool ImageCreateInfo(const D3D12_RESOURCE_DESC& desc, VkImageCreateInfo& info, FormatInfo& format)
{
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count > 1) {
        Unimplemented("non-2D or multisampled texture");
        return false;
    }
    const bool depthResource = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    format = Format(desc.Format, depthResource);
    if (format.format == VK_FORMAT_UNDEFINED) {
        Log("unsupported texture format %d", int(desc.Format));
        return false;
    }
    uint32_t mips = desc.MipLevels;
    if (!mips) {
        mips = 1;
        for (uint64_t size = std::max<uint64_t>(desc.Width, desc.Height); size > 1; size >>= 1) ++mips;
    }
    info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format.format;
    info.extent = {uint32_t(desc.Width), desc.Height, 1};
    info.mipLevels = mips;
    info.arrayLayers = desc.DepthOrArraySize;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (depthResource) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!format.depth) info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    return true;
}

VkBufferCreateInfo BufferCreateInfo(const D3D12_RESOURCE_DESC& desc)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = desc.Width ? desc.Width : 1;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return info;
}

// ---------------------------------------------------------------- Resource
Resource::Resource(Device* device, const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags)
    : DeviceChild(device), desc_(desc), heapProperties_(heap), heapFlags_(flags)
{
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !desc_.MipLevels) {
        desc_.MipLevels = 1;
        for (uint64_t size = std::max<uint64_t>(desc_.Width, desc_.Height); size > 1; size >>= 1) ++desc_.MipLevels;
    }
    if (!desc_.Alignment) desc_.Alignment = kPlacementAlignment;
}

Resource::~Resource()
{
    if (va_) device_->FreeVa(va_);
    std::vector<VkImageView> views;
    for (auto& [key, view] : views_) views.push_back(view);
    Device* owner = device_;
    const VkBuffer buffer = buffer_;
    const VkImage image = image_;
    const VkDeviceMemory memory = ownMemory_;
    Heap* heap = heap_;
    auto shadows = std::move(shadows_);
    owner->DeferDestroy([owner, views, buffer, image, memory, heap, shadows] {
        const auto& vk = owner->vk();
        const VkDevice device = owner->vkDevice();
        for (VkImageView view : views) vk.vkDestroyImageView(device, view, nullptr);
        if (buffer) vk.vkDestroyBuffer(device, buffer, nullptr);
        if (image) vk.vkDestroyImage(device, image, nullptr);
        if (memory) vk.vkFreeMemory(device, memory, nullptr);
        for (const auto& [key, shadow] : shadows) {
            if (shadow.view) vk.vkDestroyImageView(device, shadow.view, nullptr);
            if (shadow.image) vk.vkDestroyImage(device, shadow.image, nullptr);
            if (shadow.memory) vk.vkFreeMemory(device, shadow.memory, nullptr);
        }
        if (heap) heap->Release();
    });
}

HRESULT Resource::Init(Heap* placedHeap, UINT64 placedOffset)
{
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    VkMemoryRequirements requirements{};
    FormatInfo format;
    if (IsBuffer()) {
        const VkBufferCreateInfo info = BufferCreateInfo(desc_);
        if (vk.vkCreateBuffer(device, &info, nullptr, &buffer_) != VK_SUCCESS) return E_OUTOFMEMORY;
        vk.vkGetBufferMemoryRequirements(device, buffer_, &requirements);
        size_ = desc_.Width;
    } else {
        VkImageCreateInfo info{};
        if (!ImageCreateInfo(desc_, info, format)) return E_INVALIDARG;
        // D3D12 views keep resolving against the D3D12 format; a game swap-chain image only changes the VkImage.
        imageFormat_ = info.format;
        Device::BackbufferHint hint;
        if (device_->MatchBackbuffer(desc_, hint)) {
            if (hint.imageFormat != VK_FORMAT_UNDEFINED) info.format = hint.imageFormat;
            info.usage |= hint.usage;
            info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        }
        if (vk.vkCreateImage(device, &info, nullptr, &image_) != VK_SUCCESS) return E_OUTOFMEMORY;
        vk.vkGetImageMemoryRequirements(device, image_, &requirements);
        size_ = requirements.size;
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* hostBase = nullptr;
    if (placedHeap) {
        const D3D12_HEAP_DESC heap = placedHeap->GetDesc();
        if (placedOffset % requirements.alignment || placedOffset + requirements.size > heap.SizeInBytes ||
            !(requirements.memoryTypeBits & (1u << placedHeap->memoryType()))) {
            Log("CreatePlacedResource: offset %llu size %llu align %llu does not fit heap %llu (type bits 0x%x, heap type %u)", placedOffset,
                requirements.size, requirements.alignment, heap.SizeInBytes, requirements.memoryTypeBits, placedHeap->memoryType());
            return E_INVALIDARG;
        }
        heap_ = placedHeap;
        heap_->AddRef();
        memory = placedHeap->memory();
        offset = placedOffset;
        hostBase = placedHeap->mapped();
    } else {
        uint32_t type = 0;
        if (!device_->FindMemoryType(requirements.memoryTypeBits, heapProperties_.Type, type)) return E_OUTOFMEMORY;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image_, buffer_};
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, requirements.size, type};
        if (vk.vkAllocateMemory(device, &allocate, nullptr, &ownMemory_) != VK_SUCCESS) return E_OUTOFMEMORY;
        memory = ownMemory_;
        if (heapProperties_.Type == D3D12_HEAP_TYPE_UPLOAD || heapProperties_.Type == D3D12_HEAP_TYPE_READBACK) {
            void* host = nullptr;
            if (vk.vkMapMemory(device, ownMemory_, 0, VK_WHOLE_SIZE, 0, &host) != VK_SUCCESS) return E_OUTOFMEMORY;
            hostBase = static_cast<uint8_t*>(host);
        }
    }
    if (buffer_ ? vk.vkBindBufferMemory(device, buffer_, memory, offset) != VK_SUCCESS
                : vk.vkBindImageMemory(device, image_, memory, offset) != VK_SUCCESS) return E_OUTOFMEMORY;
    if (hostBase) {
        mapped_ = hostBase + offset;
        memset(mapped_, 0, size_t(requirements.size));
    }
    if (buffer_) {
        va_ = device_->AllocateVa(size_, this);
        if (!mapped_) device_->InitializeResource(VK_NULL_HANDLE, 0, buffer_);
    } else {
        device_->InitializeResource(image_, format.aspect, VK_NULL_HANDLE);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Resource::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12Resource, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Resource>(this, riid, out);
}

HRESULT STDMETHODCALLTYPE Resource::Map(UINT subresource, const D3D12_RANGE*, void** data)
{
    if (subresource || !mapped_) return E_INVALIDARG;
    if (data) *data = mapped_;
    return S_OK;
}

void STDMETHODCALLTYPE Resource::Unmap(UINT, const D3D12_RANGE*) {}

HRESULT STDMETHODCALLTYPE Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags)
{
    if (properties) *properties = heapProperties_;
    if (flags) *flags = heapFlags_;
    return S_OK;
}

VkImageView Resource::View(VkFormat format, VkImageViewType type, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mips,
                           uint32_t baseLayer, uint32_t layers, VkImageUsageFlags usage)
{
    if (!image_) return VK_NULL_HANDLE;
    const std::array<uint32_t, 8> key{uint32_t(format), uint32_t(type), aspect, baseMip, mips, baseLayer, layers, usage};
    std::lock_guard lock(viewMutex_);
    if (auto it = views_.find(key); it != views_.end()) return it->second;
    VkImageViewUsageCreateInfo usageInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, usage};
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usageInfo};
    info.image = image_;
    info.viewType = type;
    info.format = format;
    info.subresourceRange = {aspect, baseMip, mips, baseLayer, layers};
    VkImageView view = VK_NULL_HANDLE;
    if (device_->vk().vkCreateImageView(device_->vkDevice(), &info, nullptr, &view) != VK_SUCCESS) {
        Log("vkCreateImageView failed (format %d, type %d, usage 0x%x)", int(format), int(type), usage);
        return VK_NULL_HANDLE;
    }
    views_[key] = view;
    return view;
}

// ---------------------------------------------------------------- Heap
Heap::Heap(Device* device, const D3D12_HEAP_DESC& desc) : DeviceChild(device), desc_(desc)
{
    if (!desc_.Alignment) desc_.Alignment = kPlacementAlignment;
}

Heap::~Heap()
{
    Device* owner = device_;
    const VkDeviceMemory memory = memory_;
    if (memory) owner->DeferDestroy([owner, memory] { owner->vk().vkFreeMemory(owner->vkDevice(), memory, nullptr); });
}

HRESULT Heap::Init()
{
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    uint32_t bits = ~0u;
    if (!(desc_.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)) {
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 65536, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        VkBufferCreateInfo info = BufferCreateInfo(buffer);
        VkDeviceBufferMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, nullptr, &info};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vk.vkGetDeviceBufferMemoryRequirements(device, &query, &requirements);
        bits &= requirements.memoryRequirements.memoryTypeBits;
    }
    if (!(desc_.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) && desc_.Properties.Type == D3D12_HEAP_TYPE_DEFAULT) {
        D3D12_RESOURCE_DESC texture{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        VkImageCreateInfo info{};
        FormatInfo format;
        ImageCreateInfo(texture, info, format);
        VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, nullptr, &info};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vk.vkGetDeviceImageMemoryRequirements(device, &query, &requirements);
        bits &= requirements.memoryRequirements.memoryTypeBits;
    }
    if (!device_->FindMemoryType(bits, desc_.Properties.Type, memoryType_)) {
        Log("CreateHeap: no memory type for heap type %d flags 0x%x (bits 0x%x)", int(desc_.Properties.Type), desc_.Flags, bits);
        return E_OUTOFMEMORY;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, desc_.SizeInBytes, memoryType_};
    if (vk.vkAllocateMemory(device, &allocate, nullptr, &memory_) != VK_SUCCESS) return E_OUTOFMEMORY;
    if (desc_.Properties.Type == D3D12_HEAP_TYPE_UPLOAD || desc_.Properties.Type == D3D12_HEAP_TYPE_READBACK) {
        void* host = nullptr;
        if (vk.vkMapMemory(device, memory_, 0, VK_WHOLE_SIZE, 0, &host) != VK_SUCCESS) return E_OUTOFMEMORY;
        mapped_ = static_cast<uint8_t*>(host);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Heap::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12Heap, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Heap>(this, riid, out);
}

// ---------------------------------------------------------------- DescriptorHeap / RootSignature
DescriptorHeap::DescriptorHeap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC& desc)
    : DeviceChild(device), desc_(desc), descriptors_(desc.NumDescriptors)
{
}

HRESULT STDMETHODCALLTYPE DescriptorHeap::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12DescriptorHeap, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12DescriptorHeap>(this, riid, out);
}

HRESULT STDMETHODCALLTYPE RootSignature::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12RootSignature, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12RootSignature>(this, riid, out);
}


uint64_t Resource::CudaHandle(VkImageView view, bool storage, TensorRoute& route)
{
    std::lock_guard lock(viewMutex_);
    auto& handle = cudaHandles_[{view, storage}];
    if (!handle) handle = route.ImageHandle(view, storage);
    return handle;
}
uint64_t Resource::TensorShadow(VkCommandBuffer command, uint64_t offset, uint64_t range, TensorRoute& route, std::string& reason)
{
    if (heapProperties_.Type != D3D12_HEAP_TYPE_DEFAULT) { reason = "shadow_heap"; return 0; }
    if (!IsBuffer() || offset > size_ || range > size_ - offset || !range || range % 16
        || range / 16 > device_->properties().limits.maxImageDimension1D) { reason = "shadow_shape"; return 0; }
    std::lock_guard lock(viewMutex_);
    const auto& vk = device_->vk(); const auto device = device_->vkDevice();
    auto& shadow = shadows_[{offset, range}];
    if (!shadow.image)
    {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_1D; image.format = VK_FORMAT_R32G32B32A32_UINT;
        image.extent = {uint32_t(range / 16), 1, 1}; image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vk.vkCreateImage(device, &image, nullptr, &shadow.image) != VK_SUCCESS) { reason = "shadow_image"; return 0; }
        VkMemoryRequirements requirements{}; vk.vkGetImageMemoryRequirements(device, shadow.image, &requirements);
        uint32_t type{};
        if (!device_->FindMemoryType(requirements.memoryTypeBits, D3D12_HEAP_TYPE_DEFAULT, type)) { reason = "shadow_memory_type"; return 0; }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
        if (vk.vkAllocateMemory(device, &allocate, nullptr, &shadow.memory) != VK_SUCCESS
            || vk.vkBindImageMemory(device, shadow.image, shadow.memory, 0) != VK_SUCCESS) { reason = "shadow_memory"; return 0; }
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = shadow.image; view.viewType = VK_IMAGE_VIEW_TYPE_1D; view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk.vkCreateImageView(device, &view, nullptr, &shadow.view) != VK_SUCCESS) { reason = "shadow_view"; return 0; }
        shadow.handle = route.ImageHandle(shadow.view, false);
        if (!shadow.handle) { reason = "shadow_handle"; return 0; }
    }
    if (!shadow.handle) { reason = "shadow_incomplete"; return 0; }
    const auto generation = writeGeneration_.load(std::memory_order_relaxed);
    if (!shadow.initialized || shadow.generation != generation)
    {
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        image.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; image.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        image.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; image.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        image.oldLayout = shadow.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        image.newLayout = VK_IMAGE_LAYOUT_GENERAL; image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = shadow.image; image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = dep.imageMemoryBarrierCount = 1; dep.pMemoryBarriers = &memory; dep.pImageMemoryBarriers = &image;
        vk.vkCmdPipelineBarrier2(command, &dep);
        VkBufferImageCopy copy{}; copy.bufferOffset = offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {uint32_t(range / 16), 1, 1};
        vk.vkCmdCopyBufferToImage(command, buffer_, shadow.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        dep.imageMemoryBarrierCount = 0;
        vk.vkCmdPipelineBarrier2(command, &dep);
        shadow.initialized = true; shadow.generation = generation;
    }
    return shadow.handle;
}

} // namespace xvk
