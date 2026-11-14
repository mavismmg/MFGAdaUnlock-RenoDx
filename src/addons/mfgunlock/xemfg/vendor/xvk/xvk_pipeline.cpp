#include "xvk_internal.h"

namespace xvk {

VkSamplerCreateInfo SamplerInfo(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE u, D3D12_TEXTURE_ADDRESS_MODE v, D3D12_TEXTURE_ADDRESS_MODE w,
                                float bias, UINT anisotropy, D3D12_COMPARISON_FUNC comparison, VkBorderColor border, float minLod, float maxLod);

namespace {
uint32_t ClassBase(D3D12_DESCRIPTOR_RANGE_TYPE type)
{
    switch (type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return kSrvBase;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return kUavBase;
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return kCbvBase;
    default: return kSamplerBase;
    }
}

bool Compatible(D3D12_DESCRIPTOR_RANGE_TYPE range, VkDescriptorType type)
{
    switch (range) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    default: return type == VK_DESCRIPTOR_TYPE_SAMPLER;
    }
}

VkBorderColor StaticBorder(D3D12_STATIC_BORDER_COLOR color)
{
    switch (color) {
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    default: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
}
} // namespace

PipelineState::~PipelineState()
{
    Device* owner = device_;
    const VkPipeline pipeline = pipeline_;
    const VkPipelineLayout layout = layout_;
    const VkDescriptorSetLayout setLayout = setLayout_;
    owner->DeferDestroy([owner, pipeline, layout, setLayout] {
        const auto& vk = owner->vk();
        const VkDevice device = owner->vkDevice();
        if (pipeline) vk.vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vk.vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout) vk.vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    });
}

HRESULT PipelineState::Init(RootSignature* rootSignature, const Program& program)
{
    const auto& vk = device_->vk();
    const VkDevice device = device_->vkDevice();
    const RootLayout& root = rootSignature->layout();
    program_ = program.sha256;
    auto fail = [&](const std::string& why) {
        Log("pipeline %s: %s", program_.c_str(), why.c_str());
        return E_INVALIDARG;
    };

    std::map<uint32_t, VkDescriptorType> declared;
    for (const auto& b : program.bindings) declared[b.binding] = b.type;
    std::map<uint32_t, bool> covered;

    for (uint32_t p = 0; p < root.parameters.size(); ++p) {
        const auto& parameter = root.parameters[p];
        if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (constantsParameter_ >= 0) return fail("more than one root-constants parameter (contract violation)");
            constantsParameter_ = int(p);
            pushConstantBytes_ = parameter.num32 * 4;
            if (parameter.num32 > 64) return fail("root constants above 64 dwords");
            continue;
        }
        if (parameter.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) return fail("root descriptors are not supported");
        for (const auto& range : parameter.ranges) {
            if (range.space) return fail("register space other than 0");
            for (uint32_t i = 0; i < range.count; ++i) {
                const uint32_t binding = ClassBase(range.type) + range.baseRegister + i;
                auto it = declared.find(binding);
                if (it == declared.end()) continue;   // the program does not use this register
                if (!Compatible(range.type, it->second)) return fail("binding " + std::to_string(binding) + " type does not match its range");
                ranges_.push_back({p, range.offset + i, binding, it->second});
                covered[binding] = true;
            }
        }
    }
    if (program.pushConstantBytes > pushConstantBytes_) return fail("program push constants exceed the root constants");
    if (constantsParameter_ >= 16) return fail("root constants parameter index above 15");

    std::vector<VkSampler> immutable(root.samplers.size());
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const auto& [binding, type] : declared) {
        VkDescriptorSetLayoutBinding b{binding, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        if (type == VK_DESCRIPTOR_TYPE_SAMPLER && !covered[binding]) {
            for (size_t s = 0; s < root.samplers.size(); ++s) {
                const auto& d = root.samplers[s].desc;
                if (kSamplerBase + d.ShaderRegister != binding || d.RegisterSpace) continue;
                const VkSamplerCreateInfo info = SamplerInfo(d.Filter, d.AddressU, d.AddressV, d.AddressW, d.MipLODBias, d.MaxAnisotropy,
                                                             d.ComparisonFunc, StaticBorder(d.BorderColor), d.MinLOD, d.MaxLOD);
                Log("pipeline %.16s: static sampler s%u filter 0x%x address %d,%d,%d lod %g..%g -> min %d mag %d mip %d", program_.c_str(),
                    d.ShaderRegister, unsigned(d.Filter), int(d.AddressU), int(d.AddressV), int(d.AddressW), d.MinLOD, d.MaxLOD, int(info.minFilter),
                    int(info.magFilter), int(info.mipmapMode));
                immutable[s] = device_->Sampler(info);
                if (!immutable[s]) return fail("static sampler creation failed");
                // Static samplers are pushed with the other descriptors at every Dispatch.
                samplers_.push_back(immutable[s]);
                staticBindings_.push_back(binding);
                covered[binding] = true;
            }
        }
        if (!covered[binding]) return fail("binding " + std::to_string(binding) + " is not covered by the root signature");
        bindings.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = uint32_t(bindings.size());
    setInfo.pBindings = bindings.data();
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout_) != VK_SUCCESS) return fail("descriptor set layout");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantBytes_};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = pushConstantBytes_ ? 1 : 0;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) return fail("pipeline layout");

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = program.spirv.size() * 4;
    moduleInfo.pCode = program.spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return fail("shader module");
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
    info.layout = layout_;
    const VkResult result = vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vk.vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) return fail("vkCreateComputePipelines " + std::to_string(int(result)));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE PipelineState::QueryInterface(REFIID riid, void** out)
{
    return QueryAs<ID3D12PipelineState, IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12PipelineState>(this, riid, out);
}

} // namespace xvk
