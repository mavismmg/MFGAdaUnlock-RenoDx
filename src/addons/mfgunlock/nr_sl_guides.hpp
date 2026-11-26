// SPDX-License-Identifier: MIT
#pragma once
// 1.12.1 (D1): the game's own motion vectors and depth for DLSS 5 on the Present route of a game with its
// own Streamline (the game's Present on Streamline's proxy, addon_dlss5.inl's ProxyFrame).
//
// Such a game tags its motion vectors and depth for its frame generation (slSetTag / slSetTagForFrame,
// hooked in framecount.hpp). Before this, NR there ran with motion estimated from the image: CONTROL
// Resonant (2026-11-24, 4070 Ti, 1440p) measured 10.6 ms that way against 5.2 ms with the game's guides.
// The tags are copied on the game's own command list at tag time, as Streamline does for eOnlyValidNow:
// at Present their contents may be gone, and the copies' state is ours. A tag without a command list is
// used as it is only when the game promised it stays valid until Present.
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <sl.h>

namespace mfgunlock::nr::slguides {

// Set at load when DLSS 5 is on: otherwise the tags are not looked at.
inline std::atomic_bool g_wanted{false};

struct Guide {
  ID3D12Resource* resource = nullptr;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t x = 0, y = 0, w = 0, h = 0;
};

namespace internal {
struct Slot {
  Microsoft::WRL::ComPtr<ID3D12Resource> copy;
  D3D12_RESOURCE_STATES copy_state = D3D12_RESOURCE_STATE_COMMON;
  Guide latest;
  uint64_t tick = 0;
};
inline std::mutex lock;
inline Slot slots[3];  // 0 motion vectors, 1 depth, 2 the color before the HUD (1.12.1, N2)
// Replaced copies, kept a while: frames still on the GPU may read them.
struct Retired {
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  uint64_t tick;
};
inline std::vector<Retired> retired;

// The format of the copy: one NR reads, in the source's copy group. `color`: the image before the HUD.
inline DXGI_FORMAT CopyFormat(DXGI_FORMAT f, bool depth, bool color = false) {
  if (color) {
    switch (f) {
      case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
      case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
      case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
      default: return DXGI_FORMAT_UNKNOWN;
    }
  }
  if (depth) {
    switch (f) {
      case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
      case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
      default: return DXGI_FORMAT_UNKNOWN;
    }
  }
  switch (f) {
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case DXGI_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
inline void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Transition.pResource = r;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  l->ResourceBarrier(1, &b);
}
// The tag copied into `slot`'s texture on the game's list. False when it cannot be copied.
inline bool CopyTag(Slot& slot, ID3D12Resource* source, D3D12_RESOURCE_STATES state, bool depth,
                    ID3D12GraphicsCommandList* list, bool color = false) {
  const D3D12_RESOURCE_DESC sd = source->GetDesc();
  const DXGI_FORMAT format = CopyFormat(sd.Format, depth, color);
  if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.MipLevels != 1 || sd.DepthOrArraySize != 1 ||
      sd.SampleDesc.Count != 1 || format == DXGI_FORMAT_UNKNOWN)
    return false;
  if (slot.copy) {
    const D3D12_RESOURCE_DESC cd = slot.copy->GetDesc();
    if (cd.Width != sd.Width || cd.Height != sd.Height || cd.Format != format) {
      retired.push_back({std::move(slot.copy), GetTickCount64()});
      slot.copy.Reset();
    }
  }
  if (!slot.copy) {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) return false;
    D3D12_RESOURCE_DESC d = sd;
    d.Format = format;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.Alignment = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&slot.copy))))
      return false;
    slot.copy->SetName(color ? L"MFG Unlock DLSS 5 game image before the HUD"
                       : depth ? L"MFG Unlock DLSS 5 game depth" : L"MFG Unlock DLSS 5 game motion");
    slot.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  Barrier(list, slot.copy.Get(), slot.copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
  Barrier(list, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(slot.copy.Get(), source);
  Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  Barrier(list, slot.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  slot.copy_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  slot.latest.resource = slot.copy.Get();
  slot.latest.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return true;
}
}  // namespace internal

// framecount.hpp's slSetTag / slSetTagForFrame hooks, before Streamline gets the tags.
inline void Observe(const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* command_buffer) {
  if (!g_wanted.load(std::memory_order_relaxed) || tags == nullptr) return;
  using namespace internal;
  std::lock_guard guard(lock);
  const uint64_t now = GetTickCount64();
  for (size_t i = 0; i < retired.size();)
    if (now - retired[i].tick > 2000) retired.erase(retired.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const sl::ResourceTag& tag = tags[i];
    const bool motion = tag.type == sl::kBufferTypeMotionVectors, depth = tag.type == sl::kBufferTypeDepth;
    const bool hudless = tag.type == sl::kBufferTypeHUDLessColor;  // 1.12.1 (N2)
    if (!(motion || depth || hudless) || tag.resource == nullptr || tag.resource->native == nullptr ||
        tag.resource->state == UINT_MAX)
      continue;
    auto* source = static_cast<ID3D12Resource*>(tag.resource->native);
    const auto state = static_cast<D3D12_RESOURCE_STATES>(tag.resource->state);
    Slot& slot = slots[hudless ? 2 : depth ? 1 : 0];
    bool ok = false;
    // Copied only on a direct list: a compute list takes no pixel-shader states in its barriers.
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(command_buffer);
    if (list != nullptr && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
      ok = CopyTag(slot, source, state, depth, list, hudless);
    else if (tag.lifecycle == sl::ResourceLifecycle::eValidUntilPresent) {
      slot.latest.resource = source;
      slot.latest.state = state;
      ok = true;
    }
    if (!ok) {
      slot.tick = 0;
      continue;
    }
    const D3D12_RESOURCE_DESC d = source->GetDesc();
    const bool whole = tag.extent.width == 0 || tag.extent.height == 0;
    slot.latest.x = whole ? 0 : tag.extent.left;
    slot.latest.y = whole ? 0 : tag.extent.top;
    slot.latest.w = whole ? static_cast<uint32_t>(d.Width) : tag.extent.width;
    slot.latest.h = whole ? d.Height : tag.extent.height;
    slot.tick = now;
  }
}

// This frame's motion vectors and depth, when both were tagged in the last 250 ms; and the image before the
// HUD, when the game tags one (null otherwise).
inline bool Take(Guide& motion, Guide& depth, Guide* hudless = nullptr) {
  using namespace internal;
  std::lock_guard guard(lock);
  const uint64_t now = GetTickCount64();
  if (slots[0].tick == 0 || slots[1].tick == 0 || now - slots[0].tick > 250 || now - slots[1].tick > 250) return false;
  motion = slots[0].latest;
  depth = slots[1].latest;
  if (hudless) *hudless = slots[2].tick != 0 && now - slots[2].tick <= 250 ? slots[2].latest : Guide{};
  return true;
}

}  // namespace mfgunlock::nr::slguides
