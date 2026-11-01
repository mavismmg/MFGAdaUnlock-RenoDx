// SPDX-License-Identifier: MIT
#pragma once
// 1.8 (plan D.2): the XMX mode's generator, off unless MFGAdaUnlock.ini has
// [DLSSG] XmxGenerator=1. NGX still owns presentation; NVIDIA's generation is
// replaced in EvaluateFeature by XeFG's own context, without its proxy swap chain.
// Runs the ported multi-frame model, without CUDA. Offsets are for libxess_fg.dll 1.3.1.78
// (docs/research/xemfg-modo-xmx-2026-11-01.md); any other build is refused.
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "backend_selection.hpp"

namespace mfgunlock::xmx::generator {
using Microsoft::WRL::ComPtr;
constexpr DWORD kKnownTimestamp = 0x69CB0F4D;
constexpr uintptr_t kFactoryRva = 0x1853B0;
constexpr uintptr_t kContextVtableRva = 0xF45D48;
constexpr uintptr_t kInitRva = 0x1871C0;
constexpr uintptr_t kExecuteRva = 0x185800;
constexpr uintptr_t kDestroyRva = 0x1874C0;  // for the size changes the next step handles
constexpr auto kBackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kInputState = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kXefgInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

#pragma pack(push, 8)
struct InitParams {
  uint32_t width, height, format, flags, maxFrames, creationMask, visibleMask, reserved;
  void* tempBufferHeap;
  uint64_t bufferOffset;
  void* tempTextureHeap;
  uint64_t textureOffset;
  void* pipelineLibrary;
  uint32_t uiMode, reserved2;
};
struct Input {
  ID3D12Resource* resource;
  uint32_t baseX, baseY, width, height;
};
struct FrameData {
  Input backbuffer, depth, motion, ui, hudless;
  float view[16], projection[16], resolution[2], jitter[2], motionScale[2];
  uint32_t reset;
  float renderTimeMs;
};
struct ExecuteParams {
  FrameData previous, next;
  ID3D12Resource* const* outputs;
  const uint32_t* outputBases;
  uint8_t outputCount;
  uint8_t reserved[3];
  uint32_t networkScales;  // 0: the default
  void* descriptorHeap;    // null: XeFG's own
  uint32_t heapOffset, uiComposition;
  // The per-pair work runs in the call starting at 0; later calls reuse it.
  uint32_t firstFrame, frameCount;
};
#pragma pack(pop)
static_assert(sizeof(InitParams) == 80 && sizeof(Input) == 24 && sizeof(FrameData) == 0x118);
static_assert(offsetof(FrameData, view) == 0x78 && offsetof(FrameData, reset) == 0x110);
static_assert(offsetof(ExecuteParams, next) == 0x118 && offsetof(ExecuteParams, outputs) == 0x230);
static_assert(offsetof(ExecuteParams, outputCount) == 0x240 && offsetof(ExecuteParams, descriptorHeap) == 0x248);
static_assert(offsetof(ExecuteParams, firstFrame) == 0x258 && sizeof(ExecuteParams) == 0x260);
constexpr uint32_t kMaxFrames = 5;

inline bool Enabled() {
  static const bool value =
      GetPrivateProfileIntW(L"DLSSG", L"XmxGenerator", 0, (backend::Directory() / L"MFGAdaUnlock.ini").c_str()) == 1;
  return value;
}

struct Copies {
  ComPtr<ID3D12Resource> backbuffer, depth, motion, hudless;
  FrameData data{};
  bool valid{};
};
struct State {
  ComPtr<ID3D12Device> device;
  void* context{};  // the C functions' handle: { 'DX12', XefgContext* }
  HMODULE fg{};
  uint32_t width{}, height{}, flags{};
  DXGI_FORMAT format{};
  std::array<Copies, 2> frames;
  uint32_t current{};
  std::array<ComPtr<ID3D12Resource>, kMaxFrames> outputs;
  // Earlier contexts and their resources may still be in flight; a size change is rare.
  std::vector<ComPtr<ID3D12Resource>> retired;
  uint32_t maxFrames{1};
  D3D12_RESOURCE_DESC outputDesc{};
  bool published{};
  bool failed{};
  uint32_t logs{};
};
inline State& Get() {
  static auto* state = new State;
  return *state;
}
inline void Log(const std::string& text) {
  auto& state = Get();
  if (state.logs++ < 48) reshade::log::message(reshade::log::level::info, ("mfgunlock: xmx " + text).c_str());
}

inline int Guarded(void* function, void* context, void* a, void* b) noexcept {
  using Call = int(__cdecl*)(void*, void*, void*);
  __try {
    return reinterpret_cast<Call>(function)(context, a, b);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -100000;
  }
}

inline bool LoadContext(ID3D12Device* device) {
  auto& state = Get();
  state.fg = GetModuleHandleW(L"libxess_fg.dll");
  if (state.fg == nullptr) {
    state.fg = LoadLibraryExW((backend::Directory() / L"libxess_fg.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  }
  if (state.fg == nullptr) {
    Log("failed: libxess_fg.dll not found");
    return false;
  }
  auto* base = reinterpret_cast<unsigned char*>(state.fg);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kKnownTimestamp) {
    Log("failed: unknown libxess_fg.dll build");
    return false;
  }
  if (!state.published) {
    state.published = true;
    auto ll = GetModuleHandleW(L"libxell.dll");
    if (ll == nullptr) ll = LoadLibraryExW((backend::Directory() / L"libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadContext), &self);
    std::string error;
    if (ll != nullptr && xemfg::PublishMultiFrameModel(state.fg, ll, self, error)) state.maxFrames = kMaxFrames;
    else Log("multi-frame model refused, 2x only: " + (ll == nullptr ? std::string("libxell.dll not found") : error));
  }
  void** wrapper = nullptr;
  const auto created = Guarded(base + kFactoryRva, device, &wrapper, nullptr);
  if (created != 0 || wrapper == nullptr) {
    Log("failed: factory returned " + std::to_string(created));
    return false;
  }
  // { 'DX12' magic, XefgContext* }
  if (wrapper[1] == nullptr ||
      reinterpret_cast<uintptr_t>(*static_cast<void***>(wrapper[1])) - reinterpret_cast<uintptr_t>(base) != kContextVtableRva) {
    Log("failed: unexpected context layout");
    return false;
  }
  state.context = wrapper;
  return true;
}
inline ComPtr<ID3D12Resource> Texture(ID3D12Device* device, D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES initial,
                                      D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc.Flags = flags;
  desc.MipLevels = 1;
  desc.DepthOrArraySize = 1;
  desc.SampleDesc = {1, 0};
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Alignment = 0;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource))))
    return nullptr;
  return resource;
}
inline void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  list->ResourceBarrier(1, &barrier);
}
// Copies between resources left in their original states.
inline void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState,
                 ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  Barrier(list, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
  Barrier(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(target, source);
  Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
  Barrier(list, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
}
inline ID3D12Resource* Resource(const NVSDK_NGX_Parameter* params, const char* name) {
  ID3D12Resource* value = nullptr;
  return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : nullptr;
}
template <class T>
inline T Value(const NVSDK_NGX_Parameter* params, const char* name, T fallback) {
  T value{};
  return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}
// Each copy has a shape to follow; the old one is released when it changes.
inline bool Capture(ID3D12Device* device, ID3D12GraphicsCommandList* list, ComPtr<ID3D12Resource>& copy,
                    ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  if (source == nullptr) {
    copy.Reset();
    return true;
  }
  const auto desc = source->GetDesc();
  if (!copy || copy->GetDesc().Width != desc.Width || copy->GetDesc().Height != desc.Height ||
      copy->GetDesc().Format != desc.Format) {
    copy = Texture(device, desc, kXefgInputState, D3D12_RESOURCE_FLAG_NONE);
    if (!copy) return false;
  }
  Copy(list, copy.Get(), kXefgInputState, source, sourceState);
  return true;
}
inline Input Describe(ID3D12Resource* copy) {
  Input input{};
  input.resource = copy;
  if (copy != nullptr) {
    const auto desc = copy->GetDesc();
    input.width = static_cast<uint32_t>(desc.Width);
    input.height = desc.Height;
  }
  return input;
}
inline bool Prepare(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, const D3D12_RESOURCE_DESC& output) {
  auto& state = Get();
  uint32_t flags = 0;
  if (Value<int>(params, "DLSSG.DepthInverted", 0) != 0) flags |= 1u << 0;
  if (Value<int>(params, "DLSSG.MvecJittered", 0) != 0) flags |= 1u << 4;
  const auto width = static_cast<uint32_t>(output.Width);
  if (state.context != nullptr && state.width == width && state.height == output.Height &&
      state.format == output.Format && state.flags == flags)
    return true;
  if (state.context != nullptr) {
    const auto retire = [&](ComPtr<ID3D12Resource>& resource) {
      if (resource) state.retired.push_back(std::move(resource));
      resource.Reset();
    };
    for (auto& frame : state.frames) {
      retire(frame.backbuffer);
      retire(frame.depth);
      retire(frame.motion);
      retire(frame.hudless);
      frame = {};
    }
    for (auto& texture : state.outputs) retire(texture);
    state.context = nullptr;  // its commands may be in flight, so it stays alive
    state.device.Reset();
  }
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&state.device)))) return false;
  if (!LoadContext(state.device.Get())) return false;
  InitParams init{};
  init.width = width;
  init.height = output.Height;
  init.format = output.Format;
  init.flags = flags;
  init.maxFrames = state.maxFrames;
  init.creationMask = 1;
  init.visibleMask = 1;
  const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kInitRva, state.context, &init, nullptr);
  Log("init " + std::to_string(width) + "x" + std::to_string(output.Height) + " format " +
      std::to_string(static_cast<int>(output.Format)) + " flags " + std::to_string(flags) + " result " + std::to_string(result));
  if (result != 0) return false;
  state.outputDesc = output;
  state.width = width;
  state.height = output.Height;
  state.format = output.Format;
  state.flags = flags;
  return true;
}
// True when this call produced what NGX expects; false lets NVIDIA's path keep the frame.
inline bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  auto& state = Get();
  if (state.failed || list == nullptr || params == nullptr) return false;
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  auto* real = Resource(params, "DLSSG.OutputReal");
  auto* depth = Resource(params, "DLSSG.Depth");
  auto* motion = Resource(params, "DLSSG.MVecs");
  if (backbuffer == nullptr || interpolated == nullptr || depth == nullptr || motion == nullptr) return false;
  const auto frameCount = (std::max)(Value<unsigned int>(params, "DLSSG.MultiFrameCount", 1), 1u);
  const auto frameIndex = std::clamp(Value<unsigned int>(params, "DLSSG.MultiFrameIndex", 1), 1u, frameCount);
  if (!Prepare(list, params, interpolated->GetDesc())) {
    state.failed = true;
    return false;
  }
  if (frameCount > state.maxFrames) {
    Log("failed: " + std::to_string(frameCount) + " generated frames, this context has " + std::to_string(state.maxFrames));
    state.failed = true;
    return false;
  }
  if (real != nullptr) Copy(list, real, kOutputState, backbuffer, kBackbufferState);
  auto* device = state.device.Get();
  // The real frame is captured once per pair; each generated frame only reads it.
  if (frameIndex == 1) {
  state.current ^= 1;
  auto& next = state.frames[state.current];
  if (!Capture(device, list, next.backbuffer, backbuffer, kBackbufferState) ||
      !Capture(device, list, next.depth, depth, kInputState) || !Capture(device, list, next.motion, motion, kInputState) ||
      !Capture(device, list, next.hudless, Resource(params, "DLSSG.HUDLess"), kInputState)) {
    Log("failed: input copies");
    state.failed = true;
    return false;
  }
  auto& data = next.data;
  data = {};
  data.backbuffer = Describe(next.backbuffer.Get());
  data.depth = Describe(next.depth.Get());
  data.motion = Describe(next.motion.Get());
  data.hudless = Describe(next.hudless.Get());
  void* projection = nullptr;
  if (params->Get("DLSSG.CameraViewToClip", &projection) == NVSDK_NGX_Result_Success && projection != nullptr)
    std::memcpy(data.projection, projection, sizeof(data.projection));
  data.resolution[0] = static_cast<float>(state.width);
  data.resolution[1] = static_cast<float>(state.height);
  data.jitter[0] = Value<float>(params, "DLSSG.JitterOffsetX", 0.f);
  data.jitter[1] = Value<float>(params, "DLSSG.JitterOffsetY", 0.f);
  data.motionScale[0] = Value<float>(params, "DLSSG.MvecScaleX", 1.f);
  data.motionScale[1] = Value<float>(params, "DLSSG.MvecScaleY", 1.f);
  const bool reset = Value<unsigned int>(params, "DLSSG.Reset", 0) != 0;
  data.reset = reset ? 1 : 0;
  next.valid = true;
  }
  auto& next = state.frames[state.current];
  auto& previous = state.frames[state.current ^ 1];
  // Without a pair the real frame is shown again.
  bool generated = false;
  if (previous.valid && next.data.reset == 0 && (next.hudless != nullptr) == (previous.hudless != nullptr)) {
    ExecuteParams execute{};
    execute.previous = previous.data;
    execute.next = next.data;
    ID3D12Resource* outputs[kMaxFrames]{};
    for (uint32_t i = 0; i < frameCount; ++i) {
      if (!state.outputs[i])
        state.outputs[i] = Texture(device, state.outputDesc, kOutputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (!state.outputs[i]) {
        Log("failed: output texture");
        state.failed = true;
        return false;
      }
      outputs[i] = state.outputs[i].Get();
    }
    const uint32_t bases[kMaxFrames * 2]{};
    execute.outputs = outputs;
    execute.outputBases = bases;
    execute.outputCount = static_cast<uint8_t>(frameCount);
    execute.firstFrame = frameIndex - 1;
    execute.frameCount = 1;
    const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kExecuteRva, state.context, list, &execute);
    if (result < 0) {
      Log("execute failed: " + std::to_string(result));
      if (result == -100000) state.failed = true;
    } else {
      generated = result == 0;
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4) Log("execute result " + std::to_string(result));
    }
  }
  if (generated) Copy(list, interpolated, kOutputState, state.outputs[frameIndex - 1].Get(), kOutputState);
  else Copy(list, interpolated, kOutputState, backbuffer, kBackbufferState);
  return true;
}
}  // namespace mfgunlock::xmx::generator
