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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include "backend_selection.hpp"
#include "support_files.hpp"
#include "xmx_policy.hpp"

namespace mfgunlock::xmx::generator {
using Microsoft::WRL::ComPtr;
constexpr DWORD kKnownTimestamp = 0x69CB0F4D;
constexpr uintptr_t kFactoryRva = 0x1853B0;
constexpr uintptr_t kContextVtableRva = 0xF45D48;
constexpr uintptr_t kInitRva = 0x1871C0;
constexpr uintptr_t kExecuteRva = 0x185800;
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

// Safe mode (1.8.3) is decided before the first call, at the start of DllMain.
inline bool Enabled() {
  static const bool value = backend::ReadMode() == backend::Mode::Xmx && !support::g_safe_mode.load();
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
  // Resources replaced while earlier commands may still be using them.
  policy::RetireQueue<ComPtr<ID3D12Resource>> retired;
  uint64_t pairs{};
  uint64_t lastCaptureMs{};
  bool lastGenerated{};
  uint32_t contexts{};
  uint32_t maxFrames{1};
  D3D12_RESOURCE_DESC outputDesc{};
  bool published{};
  bool failed{};
  // The last shape XeFG's Init refused, retried only once the shape changes.
  bool refused{};
  policy::ContextShape refusedShape{};
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
// For the panel: nobody may take NVIDIA's generator for XeFG's, or the reverse.
inline std::atomic<uint64_t> g_last_generated_tick{0};
inline std::mutex g_failure_lock;
inline std::string g_failure;
inline void Fail(const std::string& reason) {
  { std::lock_guard guard(g_failure_lock); g_failure = reason; }
  Log("failed: " + reason);
}
inline void ClearFailure() {
  std::lock_guard guard(g_failure_lock);
  g_failure.clear();
}
inline std::atomic_bool g_preflight_passed{false};
inline std::atomic<int> g_hooks_installed{0}, g_hooks_failed{0};
inline std::atomic<uint64_t> g_last_evaluate_tick{0};
inline std::atomic<int> g_idle{0};
inline std::atomic<uint32_t> g_idle_logged{0};
inline std::atomic<uint32_t> g_frames_limit{0};  // the model's limit once a game asks for more
// One log line per reason, however often the state flips.
inline void NoteIdle(policy::Idle idle) {
  g_idle.store(static_cast<int>(idle), std::memory_order_relaxed);
  if (idle == policy::Idle::None) return;
  const uint32_t bit = 1u << static_cast<int>(idle);
  if ((g_idle_logged.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
    Log(std::string("not generating: ") + policy::IdleText(idle));
}
// hooked: NVIDIA's generation is gone for this process, whatever happens next.
// evaluating: DLSS-G is calling the generator, so a game that stops is pausing it.
struct Report {
  bool generating{};
  bool hooked{};
  std::string failure;
  bool evaluating{};
  policy::Idle idle{policy::Idle::None};
  bool partiallyHooked{};
  uint32_t framesLimit{};
  bool everEvaluated{};
};
inline Report Snapshot() {
  const auto now = GetTickCount64();
  const auto tick = g_last_generated_tick.load(std::memory_order_relaxed);
  const auto evaluated = g_last_evaluate_tick.load(std::memory_order_relaxed);
  const auto hooks = g_hooks_installed.load(), failed = g_hooks_failed.load();
  Report report;
  report.generating = tick != 0 && now - tick < 2000;
  report.hooked = hooks > 0;
  report.evaluating = evaluated != 0 && now - evaluated < 2000;
  report.everEvaluated = evaluated != 0;
  report.idle = static_cast<policy::Idle>(g_idle.load(std::memory_order_relaxed));
  report.partiallyHooked = hooks > 0 && failed > 0;
  report.framesLimit = g_frames_limit.load(std::memory_order_relaxed);
  std::lock_guard guard(g_failure_lock);
  report.failure = g_failure;
  // Only with no hook at all is this a failure to start; one hooked copy still generates.
  if (report.failure.empty() && hooks == 0 && failed > 0) report.failure = "the hook on NVIDIA's generator could not be installed";
  return report;
}
inline DWORD FileTimestamp(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS nt{};
  if (!file.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || !file.seekg(dos.e_lfanew) ||
      !file.read(reinterpret_cast<char*>(&nt), sizeof(nt)))
    return 0;
  return nt.FileHeader.TimeDateStamp;
}
// Before any hook: hooking stops NVIDIA's generation, so a mode that cannot run
// must leave it alone. Reads the files without loading them; this can run under
// the loader lock.
inline bool Preflight() {
  static const bool passed = [] {
    const auto directory = backend::Directory();
    std::error_code error;
    if (!std::filesystem::exists(directory / L"libxess_fg.dll", error)) {
      Fail("libxess_fg.dll not found");
      return false;
    }
    if (FileTimestamp(directory / L"libxess_fg.dll") != kKnownTimestamp) {
      Fail("libxess_fg.dll is not the supported 1.3.1.78");
      return false;
    }
    if (!std::filesystem::exists(directory / L"libxell.dll", error)) {
      Fail("libxell.dll not found");
      return false;
    }
    g_preflight_passed = true;
    return true;
  }();
  return passed;
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
    Fail("libxess_fg.dll could not be loaded");
    return false;
  }
  auto* base = reinterpret_cast<unsigned char*>(state.fg);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kKnownTimestamp) {
    Fail("unknown libxess_fg.dll build");
    return false;
  }
  if (!state.published) {
    state.published = true;
    auto ll = GetModuleHandleW(L"libxell.dll");
    if (ll == nullptr) ll = LoadLibraryExW((backend::Directory() / L"libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    // 1.8.3: a copy the game loaded wins over ours (TLOU Part II ships libxell 1.3.0.5).
    const auto path = [](HMODULE module) {
      wchar_t text[MAX_PATH]{};
      return module != nullptr && GetModuleFileNameW(module, text, MAX_PATH) ? std::filesystem::path(text).string()
                                                                              : std::string("not loaded");
    };
    Log("runtime " + path(state.fg) + " and " + path(ll));
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadContext), &self);
    std::string error;
#ifdef MFGUNLOCK_NATIVE_XEMFG
    if (ll != nullptr && xemfg::PublishMultiFrameModel(state.fg, ll, self, error)) state.maxFrames = kMaxFrames;
    else Log("multi-frame model refused, 2x only: " + (ll == nullptr ? std::string("libxell.dll not found") : error));
#endif
  }
  void** wrapper = nullptr;
  const auto created = Guarded(base + kFactoryRva, device, &wrapper, nullptr);
  if (created != 0 || wrapper == nullptr) {
    Fail("factory returned " + std::to_string(created));
    return false;
  }
  // { 'DX12' magic, XefgContext* }
  if (wrapper[1] == nullptr ||
      reinterpret_cast<uintptr_t>(*static_cast<void***>(wrapper[1])) - reinterpret_cast<uintptr_t>(base) != kContextVtableRva) {
    Fail("unexpected context layout");
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
// Copies between resources left in their original states. A mismatch would be
// undefined on the GPU: the target keeps what it had, and the panel says why.
inline void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState,
                 ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  const auto to = target->GetDesc(), from = source->GetDesc();
  if (!policy::CanCopy(to.Width, to.Height, to.Format, from.Width, from.Height, from.Format)) {
    NoteIdle(policy::Idle::CopyMismatch);
    return;
  }
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
// Each copy has a shape to follow. The old one may be in a command list still on
// the GPU (dynamic resolution, a DLSS mode change), so it is retired, not released.
inline bool Capture(ID3D12Device* device, ID3D12GraphicsCommandList* list, ComPtr<ID3D12Resource>& copy,
                    ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  auto& state = Get();
  if (source == nullptr) {
    if (copy) state.retired.Push(std::move(copy), state.pairs);
    copy.Reset();
    return true;
  }
  const auto desc = source->GetDesc();
  if (!copy || copy->GetDesc().Width != desc.Width || copy->GetDesc().Height != desc.Height ||
      copy->GetDesc().Format != desc.Format) {
    if (copy) state.retired.Push(std::move(copy), state.pairs);
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
inline policy::Area DeclaredArea(const NVSDK_NGX_Parameter* params, const std::string& name, const Input& input) {
  return policy::InputArea(input.width, input.height,
                           {Value<unsigned int>(params, (name + "SubrectBaseX").c_str(), 0u),
                            Value<unsigned int>(params, (name + "SubrectBaseY").c_str(), 0u),
                            Value<unsigned int>(params, (name + "SubrectWidth").c_str(), 0u),
                            Value<unsigned int>(params, (name + "SubrectHeight").c_str(), 0u)});
}
inline void Assign(Input& input, const policy::Area& area) {
  input.baseX = area.x;
  input.baseY = area.y;
  input.width = area.width;
  input.height = area.height;
}
// XeFG compares these fields, not the textures, so the copies can keep their shape.
inline void MatchDepthAndMotion(const NVSDK_NGX_Parameter* params, FrameData& data) {
  if (data.depth.width == data.motion.width && data.depth.height == data.motion.height) return;
  const auto [depth, motion] = policy::SharedArea(DeclaredArea(params, "DLSSG.Depth", data.depth),
                                                  DeclaredArea(params, "DLSSG.MVecs", data.motion));
  static std::atomic_bool logged{false};
  if (!logged.exchange(true))
    Log("depth " + std::to_string(data.depth.width) + "x" + std::to_string(data.depth.height) + " and motion " +
        std::to_string(data.motion.width) + "x" + std::to_string(data.motion.height) + " differ, using " +
        std::to_string(depth.width) + "x" + std::to_string(depth.height) + " at depth " + std::to_string(depth.x) + "," +
        std::to_string(depth.y) + " motion " + std::to_string(motion.x) + "," + std::to_string(motion.y));
  Assign(data.depth, depth);
  Assign(data.motion, motion);
}
inline bool Prepare(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, const D3D12_RESOURCE_DESC& output) {
  auto& state = Get();
  uint32_t flags = 0;
  if (Value<int>(params, "DLSSG.DepthInverted", 0) != 0) flags |= 1u << 0;
  if (Value<int>(params, "DLSSG.MvecJittered", 0) != 0) flags |= 1u << 4;
  const auto width = static_cast<uint32_t>(output.Width);
  ComPtr<ID3D12Device> device;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
  const policy::ContextShape wanted{device.Get(), width, output.Height, static_cast<uint32_t>(output.Format), flags};
  if (state.context != nullptr &&
      wanted == policy::ContextShape{state.device.Get(), state.width, state.height, static_cast<uint32_t>(state.format), state.flags})
    return true;
  // 1.8.3: a shape XeFG refused is not retried every frame, but another
  // shape (a resolution or HDR change) is.
  if (state.refused && wanted == state.refusedShape) return false;
  if (state.contexts >= policy::kMaxContexts) {
    Fail("the game changed its output size too many times; restart the game");
    state.failed = true;
    return false;
  }
  if (state.context != nullptr) {
    const auto retire = [&](ComPtr<ID3D12Resource>& resource) {
      if (resource) state.retired.Push(std::move(resource), state.pairs);
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
    state.lastGenerated = false;
  }
  state.device = device;
  ++state.contexts;
  if (!LoadContext(state.device.Get())) {
    state.failed = true;
    return false;
  }
  InitParams init{};
  init.width = width;
  init.height = output.Height;
  init.format = output.Format;
  init.flags = flags;
  init.maxFrames = state.maxFrames;
  init.creationMask = 1;
  init.visibleMask = 1;
  const auto initialize = [&] {
    const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kInitRva, state.context, &init, nullptr);
    Log("init " + std::to_string(width) + "x" + std::to_string(output.Height) + " format " +
        std::to_string(static_cast<int>(output.Format)) + " flags " + std::to_string(flags) + " frames " +
        std::to_string(init.maxFrames) + " result " + std::to_string(result));
    return result;
  };
  auto result = initialize();
  // 1.8.3: the ported multi-frame model throws inside Init (-1000) on inputs
  // Intel's own 2x model takes: HZD Remastered's jittered motion vectors
  // (flags 17). Intel's model on a fresh context still generates 2x.
  if (result != 0 && init.maxFrames > 1 && state.contexts < policy::kMaxContexts) {
    ++state.contexts;
    if (LoadContext(state.device.Get())) {
      init.maxFrames = 1;
      result = initialize();
      if (result == 0) {
        state.maxFrames = 1;
        Log("the multi-frame model refused these inputs; Intel's 2x model runs instead");
      }
    }
  }
  if (result != 0) {
    // Found out only here: the hook goes in when the module loads, long before the
    // game's output format exists, so NVIDIA's generator is already gone.
    Fail(output.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
             ? "XMX cannot use this game's scRGB HDR output (result " + std::to_string(result) + "); turn HDR off or use XeMFG"
             : "XeFG initialization returned " + std::to_string(result));
    state.context = nullptr;
    state.refused = true;
    state.refusedShape = wanted;
    return false;
  }
  if (state.refused) {
    state.refused = false;
    ClearFailure();
  }
  state.outputDesc = output;
  state.width = width;
  state.height = output.Height;
  state.format = output.Format;
  state.flags = flags;
  return true;
}
// True when this call produced what NGX expects.
inline bool Generate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  auto& state = Get();
  if (state.failed) return false;
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  auto* real = Resource(params, "DLSSG.OutputReal");
  auto* depth = Resource(params, "DLSSG.Depth");
  auto* motion = Resource(params, "DLSSG.MVecs");
  if (backbuffer == nullptr || interpolated == nullptr || depth == nullptr || motion == nullptr) {
    NoteIdle(policy::Idle::MissingInputs);
    return false;
  }
  const auto requested = (std::max)(Value<unsigned int>(params, "DLSSG.MultiFrameCount", 1), 1u);
  const auto requestedIndex = std::clamp(Value<unsigned int>(params, "DLSSG.MultiFrameIndex", 1), 1u, requested);
  // Prepare sets `failed` itself when retrying cannot help.
  if (!Prepare(list, params, interpolated->GetDesc())) return false;
  const auto plan = policy::PlanFrames(requested, requestedIndex, state.maxFrames);
  const auto frameCount = plan.count, frameIndex = plan.index;
  if (plan.limited && g_frames_limit.exchange(state.maxFrames) == 0)
    Log(std::to_string(requested) + " generated frames asked for, this model produces " + std::to_string(state.maxFrames));
  if (real != nullptr) Copy(list, real, kOutputState, backbuffer, kBackbufferState);
  // Past the model's limit: the last generated frame again, no new work.
  if (plan.repeat) {
    if (state.lastGenerated) Copy(list, interpolated, kOutputState, state.outputs[frameIndex - 1].Get(), kOutputState);
    else Copy(list, interpolated, kOutputState, backbuffer, kBackbufferState);
    return true;
  }
  auto* device = state.device.Get();
  // The real frame is captured once per pair; each generated frame only reads it.
  if (requestedIndex == 1) {
  state.retired.Collect(++state.pairs);
  const auto now = GetTickCount64();
  // After a pause the two real frames show unrelated moments.
  if (policy::PairIsStale(state.lastCaptureMs, now)) state.frames[state.current].valid = false;
  state.lastCaptureMs = now;
  state.current ^= 1;
  auto& next = state.frames[state.current];
  if (!Capture(device, list, next.backbuffer, backbuffer, kBackbufferState) ||
      !Capture(device, list, next.depth, depth, kInputState) || !Capture(device, list, next.motion, motion, kInputState) ||
      !Capture(device, list, next.hudless, Resource(params, "DLSSG.HUDLess"), kInputState)) {
    Fail("input copies");
    state.failed = true;
    return false;
  }
  auto& data = next.data;
  data = {};
  data.backbuffer = Describe(next.backbuffer.Get());
  data.depth = Describe(next.depth.Get());
  data.motion = Describe(next.motion.Get());
  MatchDepthAndMotion(params, data);
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
  auto idle = policy::Idle::None;
  if (!previous.valid) idle = policy::Idle::NoPair;
  else if (next.data.reset != 0) idle = policy::Idle::GameReset;
  else if ((next.hudless != nullptr) != (previous.hudless != nullptr)) idle = policy::Idle::HudlessChanged;
  else {
    ExecuteParams execute{};
    execute.previous = previous.data;
    execute.next = next.data;
    ID3D12Resource* outputs[kMaxFrames]{};
    for (uint32_t i = 0; i < frameCount; ++i) {
      if (!state.outputs[i])
        state.outputs[i] = Texture(device, state.outputDesc, kOutputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (!state.outputs[i]) {
        Fail("output texture");
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
      idle = policy::Idle::ExecuteError;
      Log("execute failed: " + std::to_string(result));
      if (result == -100000) {
        Fail("XeFG crashed while generating a frame");
        state.failed = true;
      }
    } else {
      generated = result == 0;
      if (!generated) idle = policy::Idle::NoOutput;
      if (generated) g_last_generated_tick.store(GetTickCount64(), std::memory_order_relaxed);
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4) Log("execute result " + std::to_string(result));
    }
  }
  state.lastGenerated = generated;
  NoteIdle(idle);
  if (generated) Copy(list, interpolated, kOutputState, state.outputs[frameIndex - 1].Get(), kOutputState);
  else Copy(list, interpolated, kOutputState, backbuffer, kBackbufferState);
  return true;
}
// NVIDIA's path cannot take over once hooked (see xmx_hook.hpp), so a failure
// repeats the real frame: less smooth, never black.
inline bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  if (list == nullptr || params == nullptr) return false;
  if (Generate(list, params)) return true;
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  if (backbuffer == nullptr || interpolated == nullptr) return false;
  if (auto* real = Resource(params, "DLSSG.OutputReal")) Copy(list, real, kOutputState, backbuffer, kBackbufferState);
  Copy(list, interpolated, kOutputState, backbuffer, kBackbufferState);
  return true;
}
}  // namespace mfgunlock::xmx::generator
