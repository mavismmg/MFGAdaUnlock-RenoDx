#pragma once
// ngxprov.1: XeMFG as an Nvngx (DLSS-G) replacement provider on Vulkan (xess/vulkan/vk_provider.cpp) and D3D12
// (xess/d3d12_provider.cpp). This unit owns the provider's API (MfgXeMFGProviderGetApi, mfg_xemfg_provider.h) and
// the process-wide refusal behind its Refusal and MaxGeneratedFrames entries.
#include "../mfg_xemfg_provider.h"   // MFGAdaUnlock: flat tree
#include <cstdint>
#include <string>

namespace ngx_provider
{
// A refusal that holds for the process (no XeFG runtime, XeFG not a profiled image, an Intel adapter, the route
// rejected the pair). The first one is kept and published lock-free.
void NoteRefusal(const std::string& reason) noexcept;

// Nonzero when the provider can't run here, with the reason. Lock-free and loads nothing (the shim calls it from its
// DllMain): a noted refusal, else the active SDK pair's XeFG image when OptiScaler has loaded one.
uint32_t Refused(char* reason, uint32_t capacity) noexcept;

// DLSSG.MultiFrameCountMax: 0 while Refused, else ngx_provider::kMaxGenerated.
uint32_t MaxGeneratedFrames() noexcept;

// rtx30ngx.1: a host (OptiScaler's Nvngx replacement, through the shim) asked for the maximum and was offered
// kMaxGenerated, and no refusal has been noted since. Lock-free.
bool Offered() noexcept;

// pluginprov.2: RTXMFG_PROVIDER_WHOLE_BATCH=1 has the providers record a batch's outputs in its first evaluation
// (ngx_provider::ExecuteRange). Read once.
bool WholeBatch() noexcept;

const MfgXeMFGProviderApi* GetApi(uint32_t version) noexcept;
}
