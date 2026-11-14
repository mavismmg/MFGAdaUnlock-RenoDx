#pragma once
#include <windows.h>
#include <span>
#include <string>
struct ID3D12Device;
// Small host adapter for the MIT program port and pacing component. The
// original project's standalone renderer, HUD logic and DLSS backend are absent.
namespace xess_mod {
HMODULE Self() noexcept;
void WriteLog(const char* stage, const std::string& fields) noexcept;
// 1.7 (plan F): what the Vulkan host (vendor/xvkhost/vk_host.cpp) asks of the
// route. Its calls come from this module, so the backend already treats them
// as the host's own (HostAddress): the scope has nothing left to mark.
struct VulkanHostCallScope {
  VulkanHostCallScope() noexcept {}
};
// The validated libxess_fg/libxell pair the backend hooked (starts it if needed).
bool ActiveSdkPair(HMODULE& fg, HMODULE& ll) noexcept;
// The Vulkan host drives exactly that pair.
bool AdoptVulkanHostPair(HMODULE fg, HMODULE ll) noexcept;
// OptiScaler's XeLL redirects; never present in this build.
bool HostRedirectedExport(HMODULE module, const char* name) noexcept;
// The xvk device: no CUDA route, NVAPI flip metering or flip queue on it.
void SetVulkanHostDevice(ID3D12Device* device) noexcept;
bool VulkanHostDevice(const ID3D12Device* device) noexcept;
// 1.10.0 (dashdogy v1.7.1): the CUDA layers the Vulkan host's tensor-core route
// runs (xvk::InstallTensorCores); the same embedded, verified fatbin as the D3D12
// route. Empty when it cannot be read.
std::span<const unsigned char> TensorCoreFatbin();
// 1.10.0 (plan XMX/Vulkan/VRAM, item 3): what dashdogy's Vulkan DLSS-G provider asks of its host module
// (implemented in native_vulkan.cpp, on the XMX path). PrepareProvider publishes the ported multi-frame model and
// the frame-count unlock on the Vulkan host's XeSS pair and returns its libxess_fg, or null with `reason`.
HMODULE PrepareProvider(ID3D12Device* device, std::string& reason) noexcept;
const char* ProviderModelName() noexcept;
void NoteProviderBatch(bool generated) noexcept;
}
