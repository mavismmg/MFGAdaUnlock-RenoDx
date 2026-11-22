// SPDX-License-Identifier: MIT
#pragma once
// 1.12.0 (DLSS 5, N2): NVIDIA's neural rendering (nvngx_dlssnr.dll, NGX feature 18) on the game's frame.
//
// The runtime is the user's own file, found beside the add-on, beside the game, in MFGUnlock-DLSS5\ or at
// [DLSS5] RuntimePath; nothing is downloaded. A file without NVIDIA's Authenticode signature (the builds
// made for RTX 20/30/40 are such files) runs only with [DLSS5] AllowUnsigned=1. The calls go through
// MFGUnlock-DLSS5\mfgunlock_nvngx.dll (nr_shim.cpp): the runtime answers only callers whose module path
// contains "nvngx.dll". Its parameters are MFG Unlock's own block (nr_parameters.hpp), so the game's NGX is
// never touched.
//
// Per frame, on a command list of MFG Unlock's own, executed on the game's queue before its Present:
// the back buffer is copied, reduced to [DLSS5] Scale percent (NR costs 6.9 ms at 1280x720 and 17 ms at
// 2560x1440 on an RTX 4070 SUPER), the runtime runs on the reduced image, and what it changed is added back
// at full resolution: out = full + upscale(NR(reduced) - reduced). The look comes from NR, the detail from
// the game's image. NR expects an sRGB-encoded SDR image, so HDR swap chains are left alone.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <softpub.h>
#include <wintrust.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include <include/reshade.hpp>

#include "hang_watch.hpp"
#include "nr_parameters.hpp"

namespace mfgunlock::nr {

using Microsoft::WRL::ComPtr;

// The look, read at feature creation; a change recreates the feature.
struct Look {
  unsigned preset = 0, style = 0;
  int intensity = 100, tone = 100, structure = 100, skin = 100;  // percent
  bool automask = true;
  bool operator==(const Look&) const = default;
};

// How the back buffer is encoded: NR runs on SDR, HDR is mapped there and back (nr_runtime shaders).
enum class Encoding : int { kSdr = 0, kPq = 1, kScRgb = 2 };

enum class State : int {
  kOff = 0,          // [DLSS5] Enabled=0
  kNoRuntime,        // no nvngx_dlssnr.dll found
  kUnsigned,         // found, not signed by NVIDIA, AllowUnsigned=0
  kNoShim,           // MFGUnlock-DLSS5\mfgunlock_nvngx.dll missing
  kInitFailed,       // the runtime refused Init (GPU, driver)
  kFormat,           // an HDR or unsupported back buffer
  kWithGameFg,       // the game's own frame generation presents through ReShade: not in this version
  kFailed,           // create or evaluate failed
  kRunning,
};

namespace internal {

inline bool SignedByNvidia(const std::wstring& path) {
  // Loaded on use: the add-on's imports stay KERNEL32, USER32 and VERSION (xemfg_addon_imports).
  const HMODULE wintrust = LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  const HMODULE crypt32 = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!wintrust || !crypt32) return false;
  const auto WinVerifyTrust =
      reinterpret_cast<LONG(WINAPI*)(HWND, GUID*, LPVOID)>(GetProcAddress(wintrust, "WinVerifyTrust"));
  const auto WTHelperProvDataFromStateData = reinterpret_cast<CRYPT_PROVIDER_DATA*(WINAPI*)(HANDLE)>(
      GetProcAddress(wintrust, "WTHelperProvDataFromStateData"));
  const auto WTHelperGetProvSignerFromChain = reinterpret_cast<CRYPT_PROVIDER_SGNR*(WINAPI*)(
      CRYPT_PROVIDER_DATA*, DWORD, BOOL, DWORD)>(GetProcAddress(wintrust, "WTHelperGetProvSignerFromChain"));
  const auto CertGetNameStringW = reinterpret_cast<DWORD(WINAPI*)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR, DWORD)>(
      GetProcAddress(crypt32, "CertGetNameStringW"));
  if (!WinVerifyTrust || !WTHelperProvDataFromStateData || !WTHelperGetProvSignerFromChain || !CertGetNameStringW)
    return false;
  WINTRUST_FILE_INFO file{sizeof(file), path.c_str(), nullptr, nullptr};
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  bool nvidia = false;
  if (WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS)
    if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
      if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
        if (signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
          wchar_t name[256]{};
          CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
          nvidia = std::wstring(name) == L"NVIDIA Corporation";
        }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  return nvidia;
}

// Fullscreen triangle; the reduction samples bilinearly; the composition adds NR's change back.
constexpr const char* kShaders = R"(
Texture2D<float4> full : register(t0);
Texture2D<float4> reduced : register(t1);
Texture2D<float4> enhanced : register(t2);
Texture2D<float4> history : register(t3);
Texture2D<float4> steady : register(t4);
SamplerState linear_clamp : register(s0);
cbuffer Encoding : register(b0) { uint mode; float paper; float keep; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
// NR sees an sRGB-encoded SDR image. HDR goes there and back through an invertible curve:
// nits -> L / (L + paper) -> sRGB, and the inverse after NR.
float3 PqToNits(float3 e) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 p = pow(saturate(e), 1.0 / m2);
  return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float3 NitsToPq(float3 l) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 y = pow(saturate(l / 10000.0), m1);
  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float3 SrgbEncode(float3 x) { return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055; }
float3 SrgbDecode(float3 x) { return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }
float3 ToNits(float3 c) { return mode == 1 ? PqToNits(c) : c * 80.0; }  // scRGB: 1.0 = 80 nits
float3 FromNits(float3 l) { return mode == 1 ? NitsToPq(l) : l / 80.0; }
float3 Encode(float3 c) {
  if (mode == 0) return c;
  float3 l = max(ToNits(c), 0.0);
  return SrgbEncode(l / (l + paper));
}
float3 Decode(float3 s) {
  if (mode == 0) return s;
  float3 t = min(SrgbDecode(saturate(s)), 0.999);
  return FromNits(paper * t / (1.0 - t));
}
float4 reduce(V i) : SV_Target {
  float4 c = full.SampleLevel(linear_clamp, i.uv, 0);
  return float4(Encode(c.rgb), c.a);
}
// NR's change to the reduced image, steadied over frames: the previous change, kept only within what
// NR gives around this pixel now (3x3) and less where the image itself changed (motion, cuts).
// Alpha holds the reduced image's luma, to see that change next frame.
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float4 stabilize(V i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float3 lo = 1e9, hi = -1e9, now = 0;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      float3 d = enhanced.Load(int3(p + int2(x, y), 0)).rgb - reduced.Load(int3(p + int2(x, y), 0)).rgb;
      lo = min(lo, d);
      hi = max(hi, d);
      if (x == 0 && y == 0) now = d;
    }
  float luma = Luma(reduced.Load(int3(p, 0)).rgb);
  float4 before = history.Load(int3(p, 0));
  float w = keep * saturate(1.0 - abs(luma - before.a) * 12.0);
  return float4(lerp(now, clamp(before.rgb, lo, hi), w), luma);
}
float4 compose(V i) : SV_Target {
  float4 f = full.Load(int3(i.pos.xy, 0));
  float3 change = steady.SampleLevel(linear_clamp, i.uv, 0).rgb;
  if (mode == 0) return float4(saturate(f.rgb + change), f.a);
  return float4(Decode(Encode(f.rgb) + change), f.a);
}
)";

inline thread_local bool t_in_apply = false;
inline LONG CALLBACK ApplyFault(EXCEPTION_POINTERS* e) {
  if (!t_in_apply || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  t_in_apply = false;  // once
  CONTEXT c = *e->ContextRecord;
  std::string text = "mfgunlock: DLSS 5: access violation while applying NR, stack:";
  for (int i = 0; i < 16 && c.Rip; ++i) {
    text += "\n  " + hangwatch::internal::Frame(c.Rip);
    DWORD64 image = 0;
    auto* entry = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (!entry) {
      c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
      c.Rsp += 8;
      continue;
    }
    void* data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, entry, &c, &data, &frame, nullptr);
  }
  reshade::log::message(reshade::log::level::error, text.c_str());
  return EXCEPTION_CONTINUE_SEARCH;
}

inline DXGI_FORMAT Unorm(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
  }
}

class Runtime {
 public:
  State state() const { return state_.load(std::memory_order_relaxed); }
  const std::wstring& runtime_path() const { return runtime_path_; }
  bool runtime_signed() const { return signed_; }
  double gpu_ms() const { return gpu_ms_.load(std::memory_order_relaxed); }
  uint32_t width() const { return low_w_; }
  uint32_t height() const { return low_h_; }

  // Where the runtime file is, and whether it may run. Once, before the first frame.
  State Locate(const std::filesystem::path& addon_dir, const std::wstring& configured, bool allow_unsigned) {
    std::error_code error;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path candidates[] = {configured, addon_dir / L"MFGUnlock-DLSS5" / L"nvngx_dlssnr.dll",
                                                addon_dir / L"nvngx_dlssnr.dll",
                                                std::filesystem::path(exe).parent_path() / L"nvngx_dlssnr.dll"};
    for (const auto& c : candidates)
      if (!c.empty() && std::filesystem::exists(c, error)) {
        runtime_path_ = c.wstring();
        break;
      }
    if (runtime_path_.empty()) return Set(State::kNoRuntime);
    signed_ = SignedByNvidia(runtime_path_);
    if (!signed_ && !allow_unsigned) return Set(State::kUnsigned);
    shim_path_ = (addon_dir / L"MFGUnlock-DLSS5" / L"mfgunlock_nvngx.dll").wstring();
    if (!std::filesystem::exists(shim_path_, error)) return Set(State::kNoShim);
    return Set(State::kOff);  // ready to start on the first frame
  }

  // The game's back buffer, before Present: NR applied in place. Present thread only.
  bool Apply(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES bb_state, const Look& look,
             int scale_percent, Encoding encoding) {
    if (queue == nullptr || backbuffer == nullptr) return false;
    struct Scope {
      Scope() { t_in_apply = true; }
      ~Scope() { t_in_apply = false; }
    } scope;
    const State s = state();
    if (s == State::kNoRuntime || s == State::kUnsigned || s == State::kNoShim || s == State::kInitFailed ||
        s == State::kFailed)
      return false;
    const D3D12_RESOURCE_DESC bb = backbuffer->GetDesc();
    const DXGI_FORMAT format = Unorm(bb.Format);
    // HDR10 on 10-bit and scRGB on FP16 go through the curve in the shaders; anything else is left alone.
    const bool known = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) encoding = Encoding::kScRgb;
    else if (format != DXGI_FORMAT_R10G10B10A2_UNORM && encoding != Encoding::kSdr) encoding = Encoding::kSdr;
    if (!known) {
      Set(State::kFormat);
      return false;
    }
    if (encoding != mode_) {
      mode_ = encoding;
      reset_ = true;
      Log(std::string("image encoding: ") + (encoding == Encoding::kSdr ? "SDR" : encoding == Encoding::kPq ? "HDR10 (PQ)" : "scRGB"));
    }
    // The queue's own device: our command list must be of the same kind as the queue. ReShade's queue
    // wrapper takes native and wrapped lists alike, a native queue only native ones; the device of the
    // back buffer gave a wrapped list to ReShade's native queue in the present event (crash in D3D12Core).
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (!device_ && !Start(device.Get())) return false;
    if (ticks_ == 1) queue->GetTimestampFrequency(&ticks_);
    if (device.Get() != device_.Get()) return false;  // another device: not this session's
    if (!Resources(bb, format, scale_percent)) return Fail("resources");

    Frame& f = frames_[frame_index_++ % kFrames];
    if (fence_->GetCompletedValue() < f.fence) {
      fence_->SetEventOnCompletion(f.fence, event_);
      WaitForSingleObject(event_, 1000);
    }
    if (f.has_time) {
      UINT64* t = nullptr;
      D3D12_RANGE range{0, 16};
      if (SUCCEEDED(f.stamps->Map(0, &range, reinterpret_cast<void**>(&t)))) {
        gpu_ms_.store(double(t[1] - t[0]) * 1000.0 / double(ticks_), std::memory_order_relaxed);
        f.stamps->Unmap(0, nullptr);
      }
    }
    f.allocator->Reset();
    list_->Reset(f.allocator.Get(), nullptr);
    ID3D12GraphicsCommandList* l = list_.Get();

    if (!(look == look_) || !feature_) {
      if (feature_) release_(release_fn_, feature_);
      feature_ = nullptr;
      look_ = look;
      reset_ = true;
      params_.Set("DLSSNR.Enabled", 1);
      params_.Set("DLSSNR.Width", low_w_);
      params_.Set("DLSSNR.Height", low_h_);
      params_.Set("CreationNodeMask", 1u);
      params_.Set("VisibilityNodeMask", 1u);
      params_.Set("DLSSNR.Hint.Render.Preset", look.preset);
      params_.Set("DLSSNR.Style", look.style);
      params_.Set("DLSSNR.Intensity", look.intensity / 100.f);
      params_.Set("DLSSNR.LocalToneStrength", look.tone / 100.f);
      params_.Set("DLSSNR.LocalStructureStrength", look.structure / 100.f);
      params_.Set("DLSSNR.SkinStructureStrength", look.skin / 100.f);
      params_.Set("DLSSNR.UseAutoMask", look.automask ? 1u : 0u);
      params_.Set("DLSSNR.UICorrection", 0u);
      params_.Set("DLSSNR.DepthInverted", 0);
      const int r = create_(create_fn_, l, 18, &params_, &feature_);
      if (r != 1 || feature_ == nullptr) {
        l->Close();
        return Fail(("CreateFeature 0x" + Hex(r)).c_str());
      }
      Log("NR feature created at " + std::to_string(low_w_) + "x" + std::to_string(low_h_) + " (preset " +
          std::to_string(look.preset) + ", style " + std::to_string(look.style) + ", intensity " +
          std::to_string(look.intensity) + "%)");
    }

    if (applied_ == 0) Log("first frame: recording");
    Barrier(l, backbuffer, bb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(full_.Get(), backbuffer);
    Barrier(l, full_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    l->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    // 1. Reduce.
    Draw(l, pso_reduce_.Get(), rtv_low_, low_w_, low_h_);
    Barrier(l, low_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 2. NR on the reduced image.
    params_.Set("DLSSNR.Color", low_.Get());
    params_.Set("DLSSNR.Output", enhanced_.Get());
    params_.Set("DLSSNR.Width", low_w_);
    params_.Set("DLSSNR.Height", low_h_);
    params_.Set("DLSSNR.Enabled", 1);
    params_.Set("DLSSNR.Reset", reset_ ? 1 : 0);
    params_.Set("DLSSNR.MVecScaleX", 1.f);
    params_.Set("DLSSNR.MVecScaleY", 1.f);
    for (const char* name : {"DLSSNR.Color", "DLSSNR.Output"}) {
      const std::string base = name;
      params_.Set((base + "SubrectBaseX").c_str(), 0u);
      params_.Set((base + "SubrectBaseY").c_str(), 0u);
      params_.Set((base + "SubrectWidth").c_str(), low_w_);
      params_.Set((base + "SubrectHeight").c_str(), low_h_);
    }
    const bool fresh = reset_;  // no history to keep either
    reset_ = false;
    if (applied_ == 0) Log("first frame: evaluate");
    const int r = evaluate_(evaluate_fn_, l, feature_, &params_, nullptr);
    if (r != 1) {
      l->Close();
      return Fail(("EvaluateFeature 0x" + Hex(r)).c_str());
    }

    // 3. Steady NR's change over frames (the runtime set its own heaps and root signature).
    Barrier(l, enhanced_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(l, low_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    parity_ ^= 1;  // t3 = steady_[parity_] (last frame), t4 = steady_[1 - parity_] (this frame)
    keep_ = fresh ? 0.f : stability_ * 0.009f;  // 100% keeps 90% of the history where nothing moved
    ID3D12Resource* const written = steady_[1 - parity_].Get();
    Barrier(l, written, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Draw(l, pso_stabilize_.Get(), rtv_steady_[1 - parity_], low_w_, low_h_);
    Barrier(l, written, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 4. Compose into the back buffer.
    Barrier(l, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    device_->CreateRenderTargetView(backbuffer, &rtv_desc_, rtv_bb_);
    Draw(l, ComposePso(format), rtv_bb_, static_cast<uint32_t>(bb.Width), bb.Height);
    l->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    l->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, f.stamps.Get(), 0);
    f.has_time = true;
    Barrier(l, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, bb_state);
    Barrier(l, enhanced_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(l, low_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(l, full_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (FAILED(l->Close())) return Fail("command list");
    ID3D12CommandList* lists[] = {l};
    if (applied_ == 0) Log("first frame: execute");
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    DebugMessages();
    f.fence = fence_value_;
    if (state_.exchange(State::kRunning) != State::kRunning) Log("NR running on the game's frames");
    if (++applied_ % 600 == 0) {
      char t[96];
      std::snprintf(t, sizeof(t), "NR %ux%u, %.2f ms GPU per frame (%llu frames)", low_w_, low_h_, gpu_ms(),
                    static_cast<unsigned long long>(applied_));
      Log(t);
    }
    return true;
  }

  // A frame NR does not see (a loading screen, the game's FG): the next one starts a new history.
  void Pause(State why) {
    reset_ = true;
    if (state() == State::kRunning || state() == State::kOff) Set(why);
  }
  void Off() { Pause(State::kOff); }
  // How much of NR's change from earlier frames is kept, 0..100 ([DLSS5] Stability); read every frame.
  void Stability(int percent) { stability_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent); }

 private:
  static constexpr int kFrames = 3;
  struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> stamps;
    UINT64 fence = 0;
    bool has_time = false;
  };
  using ShimInit = int(__cdecl*)(void*, unsigned long long, const wchar_t*, void*, int, const void*);
  using ShimCreate = int(__cdecl*)(void*, void*, int, void*, void**);
  using ShimEvaluate = int(__cdecl*)(void*, void*, const void*, const void*, void*);
  using ShimRelease = int(__cdecl*)(void*, void*);

  State Set(State s) {
    // Each reason NR is not running goes to the log once per change, so a tester's log tells why.
    const State before = state_.exchange(s, std::memory_order_relaxed);
    if (before != s) {
      const char* why = s == State::kNoRuntime   ? "nvngx_dlssnr.dll not found"
                        : s == State::kUnsigned  ? "nvngx_dlssnr.dll is not signed by NVIDIA (AllowUnsigned=0)"
                        : s == State::kNoShim    ? "MFGUnlock-DLSS5/mfgunlock_nvngx.dll not found"
                        : s == State::kFormat    ? "the back buffer format is not one NR handles"
                        : s == State::kWithGameFg ? "paused: the game's frame generation is on without the hook before it"
                                                  : nullptr;
      if (why) Log(why);
    }
    return s;
  }
  bool Fail(const char* what) {
    Log(std::string("NR stopped for this session: ") + what);
    Set(State::kFailed);
    return false;
  }
  static std::string Hex(int v) {
    char t[16];
    std::snprintf(t, sizeof(t), "%08x", static_cast<unsigned>(v));
    return t;
  }
  // With the D3D12 debug layer on: its warnings and errors, into the log (at most 40).
  void DebugMessages() {
    if (!info_queue_ || debug_logged_ >= 40) return;
    const UINT64 n = info_queue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && debug_logged_ < 40; ++i) {
      SIZE_T size = 0;
      info_queue_->GetMessage(i, nullptr, &size);
      std::string bytes(size, '\0');
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      if (SUCCEEDED(info_queue_->GetMessage(i, m, &size)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
        Log(std::string("D3D12: ") + m->pDescription);
        ++debug_logged_;
      }
    }
    info_queue_->ClearStoredMessages();
  }
  static void Log(const std::string& text) {
    reshade::log::message(reshade::log::level::info, ("mfgunlock: DLSS 5: " + text).c_str());
  }

  bool Start(ID3D12Device* device) {
    // The runtime, its shim and the fault handler stay for the process: so does this module.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&ApplyFault), &self))
      return Fail("the add-on could not stay loaded");
    AddVectoredExceptionHandler(1, &ApplyFault);  // logs a fault inside Apply with its stack
    const HMODULE shim = LoadLibraryW(shim_path_.c_str());
    const HMODULE nr = LoadLibraryExW(runtime_path_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!shim || !nr) return Fail(!shim ? "mfgunlock_nvngx.dll did not load" : "nvngx_dlssnr.dll did not load");
    const auto init = reinterpret_cast<ShimInit>(GetProcAddress(shim, "MfgNr_Init"));
    create_ = reinterpret_cast<ShimCreate>(GetProcAddress(shim, "MfgNr_Create"));
    evaluate_ = reinterpret_cast<ShimEvaluate>(GetProcAddress(shim, "MfgNr_Evaluate"));
    release_ = reinterpret_cast<ShimRelease>(GetProcAddress(shim, "MfgNr_Release"));
    void* init_fn = GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
    create_fn_ = GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
    evaluate_fn_ = GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
    release_fn_ = GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!init || !create_ || !evaluate_ || !release_ || !init_fn || !create_fn_ || !evaluate_fn_ || !release_fn_)
      return Fail("missing exports");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const int r = init(init_fn, 0x1000000ull, temp, device, 0x15, nullptr);
    if (r != 1) {
      Log("NR runtime refused this GPU or driver (Init 0x" + Hex(r) + ")");
      Set(State::kInitFailed);
      return false;
    }
    device_ = device;
    device->QueryInterface(IID_PPV_ARGS(&info_queue_));  // only with the D3D12 debug layer on
    for (Frame& f : frames_) {
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
        return Fail("allocator");
      f.stamps = Buffer(16);
      if (!f.stamps) return Fail("timestamps");
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list_))))
      return Fail("command list");
    list_->Close();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return Fail("fence");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries_)))) return Fail("query heap");
    if (!Pipeline()) return false;
    Log(std::string("NR runtime started (") + (signed_ ? "signed by NVIDIA" : "unsigned, allowed by AllowUnsigned") +
        ")");
    return true;
  }

  ComPtr<ID3D12Resource> Buffer(UINT64 bytes) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&r));
    return r;
  }

  bool Pipeline() {
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);
    const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return Fail("d3dcompiler_47.dll");
    const auto build = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
      ComPtr<ID3DBlob> errors;
      return SUCCEEDED(compile(kShaders, strlen(kShaders), "mfgunlock_nr", nullptr, nullptr, entry, target,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors));
    };
    if (!build("vs", "vs_5_0", vs_) || !build("reduce", "ps_5_0", ps_reduce_) || !build("compose", "ps_5_0", ps_compose_) ||
        !build("stabilize", "ps_5_0", ps_stabilize_))
      return Fail("shader compilation");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTable, 0, 0, 0};
    D3D12_ROOT_PARAMETER table[2]{};
    table[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table[0].DescriptorTable = {1, &range};
    table[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    table[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    table[1].Constants = {0, 0, 3};
    table[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{2, table, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errors;
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                         ID3DBlob**);
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize =
        d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_))))
      return Fail("root signature");
    pso_reduce_ = Pso(ps_reduce_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    pso_stabilize_ = Pso(ps_stabilize_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!pso_reduce_ || !pso_stabilize_) return Fail("pipeline");
    // Two tables, one per history parity: t3 the previous change, t4 the one written this frame.
    D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kTable,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device_->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtv_heap_))))
      return Fail("descriptor heaps");
    const UINT rtv_step = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_low_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv_bb_ = rtv_low_;
    rtv_bb_.ptr += rtv_step;
    for (int i = 0; i < 2; ++i) {
      rtv_steady_[i] = rtv_low_;
      rtv_steady_[i].ptr += rtv_step * (2 + i);
    }
    return true;
  }
  ComPtr<ID3D12PipelineState> Pso(ID3DBlob* ps, DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso;
  }
  ID3D12PipelineState* ComposePso(DXGI_FORMAT format) {
    if (!pso_compose_ || compose_format_ != format) {
      pso_compose_ = Pso(ps_compose_.Get(), format);
      compose_format_ = format;
    }
    return pso_compose_.Get();
  }

  bool Resources(const D3D12_RESOURCE_DESC& bb, DXGI_FORMAT format, int scale_percent) {
    const int scale = scale_percent < 25 ? 25 : (scale_percent > 100 ? 100 : scale_percent);
    const uint32_t w = static_cast<uint32_t>(bb.Width), h = bb.Height;
    const uint32_t lw = (w * scale + 50) / 100, lh = (h * scale + 50) / 100;
    if (full_ && w == full_w_ && h == full_h_ && lw == low_w_ && lh == low_h_ && format == full_format_) return true;
    // A new size: wait for every frame in flight, then new textures and a new feature.
    if (fence_value_ && fence_->GetCompletedValue() < fence_value_) {
      fence_->SetEventOnCompletion(fence_value_, event_);
      WaitForSingleObject(event_, 2000);
    }
    full_w_ = w;
    full_h_ = h;
    low_w_ = lw;
    low_h_ = lh;
    full_format_ = format;
    const auto texture = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags,
                             D3D12_RESOURCE_STATES s) {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC d{};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      d.Width = tw;
      d.Height = th;
      d.DepthOrArraySize = 1;
      d.MipLevels = 1;
      d.Format = f;
      d.SampleDesc.Count = 1;
      d.Flags = flags;
      ComPtr<ID3D12Resource> r;
      device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, s, nullptr, IID_PPV_ARGS(&r));
      return r;
    };
    full_ = texture(w, h, bb.Format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    low_ = texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    enhanced_ = texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (auto& s : steady_)
      s = texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!full_ || !low_ || !enhanced_ || !steady_[0] || !steady_[1]) return false;
    const UINT step = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    for (int parity = 0; parity < 2; ++parity) {
      D3D12_CPU_DESCRIPTOR_HANDLE h0 = srv_heap_->GetCPUDescriptorHandleForHeapStart();
      h0.ptr += step * kTable * parity;
      ID3D12Resource* const views[kTable] = {full_.Get(), low_.Get(), enhanced_.Get(), steady_[parity].Get(),
                                             steady_[1 - parity].Get()};
      for (UINT i = 0; i < kTable; ++i, h0.ptr += step) {
        sv.Format = i == 0 ? format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateShaderResourceView(views[i], &sv, h0);
      }
    }
    for (int i = 0; i < 2; ++i) device_->CreateRenderTargetView(steady_[i].Get(), nullptr, rtv_steady_[i]);
    device_->CreateRenderTargetView(low_.Get(), nullptr, rtv_low_);
    rtv_desc_ = {};
    rtv_desc_.Format = format;
    rtv_desc_.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    if (feature_) release_(release_fn_, feature_);
    feature_ = nullptr;  // created again at the new size
    return true;
  }

  void Draw(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w,
            uint32_t h) {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetGraphicsRootSignature(root_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64(device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * kTable * parity_;
    l->SetGraphicsRootDescriptorTable(0, table);
    const float values[2] = {203.f, keep_};  // nits of SDR white in HDR (ITU-R BT.2408); history weight
    uint32_t constants[3] = {static_cast<uint32_t>(mode_), 0, 0};
    std::memcpy(&constants[1], values, sizeof(values));
    l->SetGraphicsRoot32BitConstants(1, 3, constants, 0);
    l->SetPipelineState(pso);
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);
  }
  static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                      D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &b);
  }

  std::atomic<State> state_{State::kOff};
  std::wstring runtime_path_, shim_path_;
  bool signed_ = false;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12QueryHeap> queries_;
  UINT64 fence_value_ = 0, ticks_ = 1;
  HANDLE event_ = nullptr;
  Frame frames_[kFrames];
  uint64_t frame_index_ = 0;
  static constexpr UINT kTable = 5;  // full, reduced, enhanced, history, steadied
  ComPtr<ID3DBlob> vs_, ps_reduce_, ps_compose_, ps_stabilize_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pso_reduce_, pso_compose_, pso_stabilize_;
  DXGI_FORMAT compose_format_ = DXGI_FORMAT_UNKNOWN, full_format_ = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12DescriptorHeap> srv_heap_, rtv_heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_low_{}, rtv_bb_{}, rtv_steady_[2]{};
  ComPtr<ID3D12Resource> steady_[2];
  UINT parity_ = 0;
  int stability_ = 60;
  float keep_ = 0.f;
  D3D12_RENDER_TARGET_VIEW_DESC rtv_desc_{};
  ComPtr<ID3D12Resource> full_, low_, enhanced_;
  uint32_t full_w_ = 0, full_h_ = 0, low_w_ = 0, low_h_ = 0;
  Parameters params_;
  Look look_{};
  bool reset_ = true;
  Encoding mode_ = Encoding::kSdr;
  void* feature_ = nullptr;
  ShimCreate create_ = nullptr;
  ShimEvaluate evaluate_ = nullptr;
  ShimRelease release_ = nullptr;
  void *create_fn_ = nullptr, *evaluate_fn_ = nullptr, *release_fn_ = nullptr;
  std::atomic<double> gpu_ms_{0.0};
  uint64_t applied_ = 0;
  ComPtr<ID3D12InfoQueue> info_queue_;
  int debug_logged_ = 0;
};

}  // namespace internal

using Runtime = internal::Runtime;

}  // namespace mfgunlock::nr
