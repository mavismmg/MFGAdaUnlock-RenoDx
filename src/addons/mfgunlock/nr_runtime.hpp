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
// Two routes, one image path: the image is copied, reduced to [DLSS5] Scale percent (NR costs 6.9 ms at
// 1280x720 and 17 ms at 2560x1440 on an RTX 4070 SUPER), the runtime runs on the reduced image, and what it
// changed is added back at full resolution: out = full + upscale(NR(reduced) - reduced). The look comes
// from NR, the detail from the game's image.
// - After upscaling (phase A): right after the game's DLSS SR or Ray Reconstruction, on the game's own
//   command list, on its output, with the game's motion vectors and depth as NR's guides. NR is temporal:
//   without guides it changes a static image by 0.39/255 a frame, with them 0.25 (nr_probe --temporal).
//   The HUD and the game's screen effects come after, so NR never sees them.
// - Present: on the back buffer before Present, on a command list of MFG Unlock's own, without guides.
// NR expects an sRGB-encoded SDR image: HDR10, scRGB and a linear (pre-tonemap) image are mapped there
// and back through invertible curves; the linear one around a metered average brightness.
#include <windows.h>
#include <d3d12.h>
#pragma comment(lib, "d3d12.lib")  // D3D12GetDebugInterface (DRED, [Diagnostics] ForceDRED)
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <softpub.h>
#include <wintrust.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

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
enum class Encoding : int { kSdr = 0, kPq = 1, kScRgb = 2, kLinear = 3 };

// The game's upscaler inputs NR takes as guides (after upscaling). Vectors point to the previous frame,
// in pixels of their own texture once multiplied by the scale (DLSS's convention, NR's too).
// The game's own TAA motion and depth found before Present (phase F, addon_dlss5.inl's finder): used once
// they agree with the motion estimated from the image (Calibrate), which also gives their scale.
struct TaaInputs {
  ID3D12Resource* motion = nullptr;
  DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
  ID3D12Resource* depth = nullptr;
  DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;  // a format a shader reads (R32_FLOAT, R24_UNORM_X8_TYPELESS...)
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
  bool depth_inverted = false;
};

struct Guides {
  ID3D12Resource* mvec = nullptr;
  ID3D12Resource* depth = nullptr;
  uint32_t mv_x = 0, mv_y = 0, mv_w = 0, mv_h = 0;
  uint32_t depth_x = 0, depth_y = 0, depth_w = 0, depth_h = 0;
  float mv_scale_x = 1.f, mv_scale_y = 1.f;
  bool depth_inverted = false;
  bool reset = false;
  // 1.12.1 (N2): the game's image before the HUD (same size and format as the frame), readable by compute.
  ID3D12Resource* hudless = nullptr;
};

// 1.12.1 (D1): the game's own guides on the Present route (tagged to its Streamline, nr_sl_guides.hpp),
// with the states they are in at Present.
struct GameGuides {
  Guides g;
  D3D12_RESOURCE_STATES mvec_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};

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
  kVram,             // paused: the GPU's memory is nearly full ([DLSS5] VramMargin)
  kForeign,          // standing aside: another DLSS 5 mod runs NR in this game ([DLSS5] Coexist)
  kNotNr,            // 1.12.1 (D4): the nvngx_dlssnr.dll found lacks NR's exports (not NVIDIA's NR runtime)
};

// For the performance overlay and the panel: what NR does now. Written by the runtime, read anywhere.
struct Status {
  std::atomic<int> route{0};    // 0 none, 1 before Present, 2 after the game's upscaler, 3 Vulkan before Present
  std::atomic<int> guides{0};   // 0 none, 1 the game's upscaler inputs, 2 estimated motion, 3 the game's TAA
  std::atomic<float> ms{0.f};
  std::atomic<uint32_t> width{0}, height{0};
  std::atomic<bool> compare{false};
};
inline Status g_status;

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
Texture2D<float4> meter_before : register(t5);
Texture2D<float4> meter_now : register(t6);
Texture2D<float4> luma_before : register(t7);    // the reduced image's luma, last frame and this one
Texture2D<float4> luma_now : register(t8);
Texture2D<float4> motion_coarse : register(t9);  // estimated motion, 8x8 blocks of the reduced image
Texture2D<float4> motion : register(t10);        // estimated motion, half the reduced image's size
Texture2D<float4> taa_motion : register(t11);    // the game's TAA motion (phase F)
Texture2D<float4> hudless : register(t12);       // 1.12.1 (N2): the game's image before the HUD
Texture2D<float4> game_mv : register(t13);       // 1.12.1 (N2): the game's motion vectors
RWTexture2D<float4> written : register(u0);  // the compute pass's output
SamplerState linear_clamp : register(s0);
cbuffer Encoding : register(b0) { uint mode; float paper; float keep; float rate; uint split; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
// NR sees an sRGB-encoded SDR image. HDR goes there and back through an invertible curve:
// nits -> L / (L + paper) -> sRGB, and the inverse after NR. A linear image (before the game's tonemap)
// is first brought to where its metered average is mid grey (0.18).
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
float Level() { return clamp(meter_now.Load(int3(0, 0, 0)).r, 1e-4, 1e4); }
// Light: nits for HDR10 and scRGB, a linear image scaled so its metered average is mid grey (0.18).
// Pivot: the light that encodes to the middle of the curve.
float Pivot() { return mode == 3 ? 1.0 : paper; }
float3 ToLight(float3 c) { return max(mode == 3 ? c * (0.18 / Level()) : ToNits(c), 0.0); }
float3 FromLight(float3 l) { return mode == 3 ? l * (Level() / 0.18) : FromNits(l); }
float3 Encode(float3 c) {
  if (mode == 0) return c;
  float3 l = ToLight(c);
  return SrgbEncode(l / (l + Pivot()));
}
// NR's change on top of the encoded image, back to light. The curve's inverse grows without bound near
// the top, so the result stays within a quarter and four times the original light (plus a tenth of the
// pivot, so dark areas can still lift): NR changes the look; it does not make light (the Streamline
// sample went white without this).
float3 Changed(float3 c, float3 change) {
  float3 l = ToLight(c);
  // Highlights past white keep their light: NR's change fades out from the pivot to three times it.
  change *= saturate(1.5 - 0.5 * Luma(l) / Pivot());
  float3 t = min(SrgbDecode(saturate(SrgbEncode(l / (l + Pivot())) + change)), 0.999);
  float3 n = Pivot() * t / (1.0 - t);
  return FromLight(clamp(n, l * 0.25, l * 4.0 + 0.1 * Pivot()));
}
// The linear image's average brightness (log average over a 16x16 grid), eased over frames: `rate` of
// the way to this frame's value each frame, all of it after a reset. r: the eased level; g: this frame's.
[numthreads(1, 1, 1)] void meter_cs(uint3 id : SV_DispatchThreadID) {
  float sum = 0;
  [loop] for (int y = 0; y < 16; ++y)
    [loop] for (int x = 0; x < 16; ++x) {
      float3 c = full.SampleLevel(linear_clamp, (float2(x, y) + 0.5) / 16.0, 0).rgb;
      sum += log2(max(Luma(max(c, 0.0)), 1e-4));
    }
  float now = exp2(sum / 256.0);
  float before = meter_before.Load(int3(0, 0, 0)).r;
  float level = (rate >= 1.0 || !(before > 0.0)) ? now : exp2(lerp(log2(before), log2(now), rate));
  written[uint2(0, 0)] = float4(level, now, 0, 1);
}
// Phase D: motion estimated from the image when the game gives none. Block matching on the reduced
// image's luma: 8x8 blocks over +-24 px in steps of 3, then each 2x2 refined over +-2 px. A vector points
// from this frame's position to the previous one (DLSS's convention), in the output texture's pixels.
[numthreads(8, 8, 1)] void luma_cs(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  written[id.xy] = Luma(reduced.Load(int3(id.xy, 0)).rgb);
}
float Sad(float2 p, float2 v, float spread, float2 inv) {
  float sad = 0;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      float2 q = p + float2(x, y) * spread;
      sad += abs(luma_now.SampleLevel(linear_clamp, q * inv, 0).r - luma_before.SampleLevel(linear_clamp, (q + v) * inv, 0).r);
    }
  return sad;
}
[numthreads(8, 8, 1)] void coarse_cs(uint3 id : SV_DispatchThreadID) {
  uint cw, ch, lw, lh;
  written.GetDimensions(cw, ch);
  if (id.x >= cw || id.y >= ch) return;
  luma_now.GetDimensions(lw, lh);
  const float2 inv = 1.0 / float2(lw, lh), center = (float2(id.xy) + 0.5) * 8.0;
  if (rate >= 1.0) { written[id.xy] = 0; return; }  // no previous frame
  // Stillness unless a vector matches clearly better (flat areas and jitter otherwise give noise).
  const float still = Sad(center, 0, 3.0, inv);
  float best = still;
  float2 found = 0;
  [loop] for (int dy = -8; dy <= 8; ++dy)
    [loop] for (int dx = -8; dx <= 8; ++dx) {
      const float2 v = float2(dx, dy) * 3.0;
      const float sad = Sad(center, v, 3.0, inv) + 0.0015 * length(v);
      if (sad < best) { best = sad; found = v; }
    }
  if (best > still * 0.7 || still < 0.02) found = 0;
  written[id.xy] = float4(found, best, 0);
}
[numthreads(8, 8, 1)] void fine_cs(uint3 id : SV_DispatchThreadID) {
  uint mw, mh, lw, lh, cw, ch;
  written.GetDimensions(mw, mh);
  if (id.x >= mw || id.y >= mh) return;
  luma_now.GetDimensions(lw, lh);
  motion_coarse.GetDimensions(cw, ch);
  const float2 inv = 1.0 / float2(lw, lh), center = (float2(id.xy) + 0.5) * 2.0;
  if (rate >= 1.0) { written[id.xy] = 0; return; }
  const float2 start = motion_coarse.SampleLevel(linear_clamp, center / (float2(cw, ch) * 8.0), 0).xy;
  const float still = Sad(center, 0, 1.5, inv);
  float best = 1e9;
  float2 found = start;
  [loop] for (int dy = -2; dy <= 2; ++dy)
    [loop] for (int dx = -2; dx <= 2; ++dx) {
      const float2 v = start + float2(dx, dy);
      const float sad = Sad(center, v, 1.5, inv) + 0.001 * length(v);
      if (sad < best) { best = sad; found = v; }
    }
  if (best > still * 0.8 || still < 0.01) found = 0;
  written[id.xy] = float4(found * 0.5, 0, 0);  // in this texture's pixels (half the reduced size)
}
[numthreads(8, 8, 1)] void flat_cs(uint3 id : SV_DispatchThreadID) { written[id.xy] = 0.5; }
// Phase F: how the game's TAA motion compares with the estimate, over a 32x18 grid: sums for a fit
// estimate = scale * taa per axis. [0]: e.x*t.x, t.x^2, e.x^2, moving samples; [1]: the same for y.
[numthreads(1, 1, 1)] void calibrate_cs(uint3 id : SV_DispatchThreadID) {
  float4 x = 0, y = 0;
  [loop] for (int j = 0; j < 18; ++j)
    [loop] for (int i = 0; i < 32; ++i) {
      const float2 uv = (float2(i, j) + 0.5) / float2(32, 18);
      const float2 e = motion.SampleLevel(linear_clamp, uv, 0).xy;
      const float2 t = taa_motion.SampleLevel(linear_clamp, uv, 0).xy;
      if (dot(e, e) < 1.0 || any(isnan(t)) || any(abs(t) > 1e4)) continue;  // clear motion only
      x += float4(e.x * t.x, t.x * t.x, e.x * e.x, 1);
      y += float4(e.y * t.y, t.y * t.y, e.y * e.y, 0);
    }
  written[uint2(0, 0)] = x;
  written[uint2(1, 0)] = y;
}
[numthreads(8, 8, 1)] void reduce_cs(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float4 c = full.SampleLevel(linear_clamp, (float2(id.xy) + 0.5) / float2(w, h), 0);
  written[id.xy] = float4(Encode(c.rgb), c.a);
}
// NR's change to the reduced image, steadied over frames: the previous change, kept only within what
// NR gives around this pixel now (3x3) and less where the image itself changed (motion, cuts).
// Alpha holds the reduced image's luma, to see that change next frame. Only without guides.
[numthreads(8, 8, 1)] void stabilize_cs(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  int2 p = int2(id.xy);
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
  float k = keep * saturate(1.0 - abs(luma - before.a) * 12.0);
  written[id.xy] = float4(lerp(now, clamp(before.rgb, lo, hi), k), luma);
}
// 1.12.1 (N2): where the frame differs from the image before the HUD. A HUD does not move with the camera,
// but the game's vectors there do: NR then pulled its history of the HUD from the scene behind (measured
// with nr_probe --hud: 23% more change between frames on a still HUD).
bool Hud(float3 a, float3 b) {
  float3 d = abs(a - b);
  return max(d.r, max(d.g, d.b)) > 0.02 * max(1.0, max(a.r, max(a.g, a.b)));
}
// The game's vectors, zero on the HUD and a pixel around it. In the vectors' own texels (often the render
// resolution): each looks at the frame's pixels it covers.
[numthreads(8, 8, 1)] void hudmask_cs(uint3 id : SV_DispatchThreadID) {
  uint w, h, cw, ch;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  full.GetDimensions(cw, ch);
  const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
  const float2 step = max(1.0, float(cw) / float(w)) / float2(cw, ch);
  bool hud = false;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      const float2 q = uv + float2(x, y) * step;
      hud = hud || Hud(full.SampleLevel(linear_clamp, q, 0).rgb, hudless.SampleLevel(linear_clamp, q, 0).rgb);
    }
  written[id.xy] = float4(hud ? float2(0, 0) : game_mv.Load(int3(id.xy, 0)).xy, 0, 0);
}
// 1.12.1 (N2, N3), over a 32x18 grid: r the mean luma change of the reduced image since the last frame (a
// camera cut), g the share of the screen the HUD covers.
[numthreads(1, 1, 1)] void stats_cs(uint3 id : SV_DispatchThreadID) {
  float cut = 0, hud = 0;
  [loop] for (int j = 0; j < 18; ++j)
    [loop] for (int i = 0; i < 32; ++i) {
      const float2 uv = (float2(i, j) + 0.5) / float2(32, 18);
      cut += abs(luma_now.SampleLevel(linear_clamp, uv, 0).r - luma_before.SampleLevel(linear_clamp, uv, 0).r);
      hud += Hud(full.SampleLevel(linear_clamp, uv, 0).rgb, hudless.SampleLevel(linear_clamp, uv, 0).rgb) ? 1.0 : 0.0;
    }
  written[uint2(0, 0)] = float4(cut / 576.0, hud / 576.0, 0, 1);
}
float4 Composed(int2 p, float2 uv) {
  float4 f = full.Load(int3(p, 0));
  // Compare: the left half without NR, a thin line between.
  if (split != 0) {
    uint w, h;
    full.GetDimensions(w, h);
    if (abs(p.x - int(w / 2)) < 1) return float4(mode == 1 ? 0.58 : (mode == 0 ? 0.85 : 2.0), 0.85 * (mode == 0), 0.2, 1);
    if (p.x < int(w / 2)) return f;
  }
  float3 change = steady.SampleLevel(linear_clamp, uv, 0).rgb;
  if (mode == 0) return float4(saturate(f.rgb + change), f.a);
  return float4(Changed(f.rgb, change), f.a);
}
float4 compose(V i) : SV_Target { return Composed(int2(i.pos.xy), i.uv); }
// After upscaling: the upscaler's output takes unordered access, not always a render target.
[numthreads(8, 8, 1)] void compose_cs(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  written[id.xy] = Composed(int2(id.xy), (float2(id.xy) + 0.5) / float2(w, h));
}
)";

inline thread_local bool t_in_apply = false;
// Set by the NGX create hook when the game or another mod creates feature 18 through the driver's NGX.
inline std::atomic_bool g_foreign_create{false};
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

// [Diagnostics] ForceDRED=1: every serious exception of the process with its stack (the first 24), to tell
// a game's crash with DLSS 5 on from one of NR's own. Returns to the game's handlers unchanged.
inline std::atomic_bool g_nr_started{false};  // the game's own start-up exceptions are not counted
inline LONG CALLBACK AnyFault(EXCEPTION_POINTERS* e) {
  if (!g_nr_started.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = e->ExceptionRecord->ExceptionCode;
  if (code < 0xC0000000u || code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;  // informational, C++ throws
  // Not REFramework's own probing (dinput8.dll), nor code outside any module (the game's protection);
  // each place once.
  HMODULE at = nullptr;
  wchar_t at_path[MAX_PATH]{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(e->ContextRecord->Rip), &at) ||
      !GetModuleFileNameW(at, at_path, MAX_PATH))
    return EXCEPTION_CONTINUE_SEARCH;
  const wchar_t* at_name = std::wcsrchr(at_path, L'\\');
  if (_wcsicmp(at_name ? at_name + 1 : at_path, L"dinput8.dll") == 0) return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<DWORD64> places[32]{};
  for (auto& place : places) {
    DWORD64 expected = 0;
    if (place.load() == e->ContextRecord->Rip) return EXCEPTION_CONTINUE_SEARCH;
    if (place.compare_exchange_strong(expected, e->ContextRecord->Rip)) break;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 24) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT c = *e->ContextRecord;
  char head[160];
  const auto* r = e->ExceptionRecord;
  std::snprintf(head, sizeof(head), "mfgunlock: DLSS 5: exception 0x%08lx on thread %lu (NR %s, %s 0x%llx), stack:",
                static_cast<unsigned long>(code), GetCurrentThreadId(), t_in_apply ? "applying" : "idle",
                r->NumberParameters >= 2 ? (r->ExceptionInformation[0] ? "writing" : "reading") : "at",
                static_cast<unsigned long long>(r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0));
  std::string text = head;
  for (int i = 0; i < 20 && c.Rip; ++i) {
    text += "\n  " + hangwatch::internal::Frame(c.Rip);
    DWORD64 image = 0;
    auto* entry = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (!entry) {
      if (c.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
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

// The game's compute state on its own command list. NR binds its heaps, root signature and pipeline (and
// NGX its own) inside the game's list; a game that does not set them again after its upscaler goes on with
// NR's bound (RE Requiem: SetComputeRootDescriptorTable against NR's root signature, an access violation in
// the driver). SetDescriptorHeaps, SetComputeRootSignature and SetPipelineState are watched on the game's
// list class, per recording thread, and what the game had bound is put back after NR.
namespace heaps {
struct Seen {
  ID3D12GraphicsCommandList* list = nullptr;
  UINT count = 0;
  ID3D12DescriptorHeap* heap[2]{};
  ID3D12GraphicsCommandList* sig_list = nullptr;
  ID3D12RootSignature* compute_sig = nullptr;
  ID3D12GraphicsCommandList* pso_list = nullptr;
  ID3D12PipelineState* pso = nullptr;
};
inline thread_local Seen t_seen;
using HeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SigFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PsoFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
constexpr int kPso = 25, kHeaps = 28, kComputeSig = 29;  // ID3D12GraphicsCommandList vtable slots
struct Class {
  void** table = nullptr;
  void* original[3]{};  // pso, heaps, compute root signature
};
inline std::mutex g_lock;
inline Class g_classes[4];
inline const Class* Find(ID3D12GraphicsCommandList* l) {
  void** table = *reinterpret_cast<void***>(l);
  for (const Class& c : g_classes)
    if (c.table == table) return &c;
  return nullptr;
}
inline void STDMETHODCALLTYPE Heaps(ID3D12GraphicsCommandList* l, UINT n, ID3D12DescriptorHeap* const* h) {
  t_seen.list = l;
  t_seen.count = n > 2 ? 2 : n;
  for (UINT i = 0; i < t_seen.count; ++i) t_seen.heap[i] = h[i];
  if (const Class* c = Find(l)) reinterpret_cast<HeapsFn>(c->original[1])(l, n, h);
}
inline void STDMETHODCALLTYPE ComputeSig(ID3D12GraphicsCommandList* l, ID3D12RootSignature* sig) {
  t_seen.sig_list = l;
  t_seen.compute_sig = sig;
  if (const Class* c = Find(l)) reinterpret_cast<SigFn>(c->original[2])(l, sig);
}
inline void STDMETHODCALLTYPE Pso(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso) {
  t_seen.pso_list = l;
  t_seen.pso = pso;
  if (const Class* c = Find(l)) reinterpret_cast<PsoFn>(c->original[0])(l, pso);
}
inline void Watch(ID3D12GraphicsCommandList* l) {
  void** table = *reinterpret_cast<void***>(l);
  std::lock_guard guard(g_lock);
  Class* free = nullptr;
  for (Class& c : g_classes) {
    if (c.table == table) return;
    if (c.table == nullptr && free == nullptr) free = &c;
  }
  if (free == nullptr) return;
  DWORD old = 0;
  if (!VirtualProtect(&table[kPso], sizeof(void*) * (kComputeSig - kPso + 1), PAGE_READWRITE, &old)) return;
  free->original[0] = table[kPso];
  free->original[1] = table[kHeaps];
  free->original[2] = table[kComputeSig];
  free->table = table;
  table[kPso] = reinterpret_cast<void*>(&Pso);
  table[kHeaps] = reinterpret_cast<void*>(&Heaps);
  table[kComputeSig] = reinterpret_cast<void*>(&ComputeSig);
  VirtualProtect(&table[kPso], sizeof(void*) * (kComputeSig - kPso + 1), old, &old);
}
inline bool Known(ID3D12GraphicsCommandList* l) { return t_seen.list == l && t_seen.count != 0; }
// NR's own calls on the list pass the watch too: put back what was taken before NR recorded.
inline void Restore(ID3D12GraphicsCommandList* l, const Seen& game) {
  if (game.list == l && game.count != 0) {
    ID3D12DescriptorHeap* h[2] = {game.heap[0], game.heap[1]};
    l->SetDescriptorHeaps(game.count, h);
  }
  if (game.sig_list == l && game.compute_sig != nullptr) l->SetComputeRootSignature(game.compute_sig);
  if (game.pso_list == l && game.pso != nullptr) l->SetPipelineState(game.pso);
}
}  // namespace heaps

inline DXGI_FORMAT Unorm(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
  }
}

class Runtime {
 public:
  State state() const { return state_.load(std::memory_order_relaxed); }
  const std::wstring& runtime_path() const { return runtime_path_; }
  const std::wstring& shim_path() const { return shim_path_; }
  bool runtime_signed() const { return signed_; }
  // 1.12.1 (D4): the runtime file's version ("" unknown), and the exports it lacks (kNotNr).
  const std::string& runtime_version() const { return version_; }
  const std::string& missing_exports() const { return missing_; }
  // 1.12.1 (D6): about how much video memory NR took when it started (MiB; 0 unknown).
  int vram_mib() const { return vram_mib_.load(std::memory_order_relaxed); }
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
    // 1.12.1 (D4): what the file is, before anything loads it (Once Human, 2026-11-24: an unsigned
    // nvngx_dlssnr.dll without NR's exports, found only when NR started).
    version_ = FileVersion(runtime_path_);
    missing_ = MissingExports(runtime_path_);
    {
      char t[MAX_PATH + 160];
      std::snprintf(t, sizeof(t), "runtime %ls, version %s", runtime_path_.c_str(),
                    version_.empty() ? "unknown" : version_.c_str());
      Log(t);
    }
    if (!missing_.empty()) {
      Log("this nvngx_dlssnr.dll lacks " + missing_ + ": it is not NVIDIA's NR runtime (or of another kind)");
      return Set(State::kNotNr);
    }
    signed_ = SignedByNvidia(runtime_path_);
    if (!signed_ && !allow_unsigned) return Set(State::kUnsigned);
    // The add-on's own copy first (always of its version); MFGUnlock-DLSS5's only if it cannot be written.
    shim_path_ = ExtractShim();
    if (shim_path_.empty()) {
      shim_path_ = (addon_dir / L"MFGUnlock-DLSS5" / L"mfgunlock_nvngx.dll").wstring();
      if (!std::filesystem::exists(shim_path_, error)) shim_path_.clear();
    }
    if (shim_path_.empty()) return Set(State::kNoShim);
    return Set(State::kOff);  // ready to start on the first frame
  }

  // 1.12.1 (D4): the exports NR needs that `path` lacks, read from its export table without loading it
  // ("" when all are there or the file cannot be read: Start checks again).
  static std::string MissingExports(const std::wstring& path) {
    static const char* kNeeded[] = {"NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_CreateFeature",
                                    "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_ReleaseFeature"};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (mapping == nullptr) return {};
    const auto* base = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (base == nullptr) return {};
    std::vector<std::string> names;
    const bool read = ReadExports(base, static_cast<uint64_t>(size.QuadPart), names);
    UnmapViewOfFile(base);
    if (!read) return {};
    std::string missing;
    for (const char* n : kNeeded) {
      bool found = false;
      for (const auto& e : names) found = found || e == n;
      if (!found) missing += (missing.empty() ? "" : ", ") + std::string(n);
    }
    return missing;
  }
  // A fault reading a damaged file is a file that cannot be read.
  static bool ReadExports(const uint8_t* base, uint64_t length, std::vector<std::string>& names) {
    __try {
      return ExportNames(base, length, names);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
  }
  static bool ExportNames(const uint8_t* base, uint64_t length, std::vector<std::string>& names) {
    if (length < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || uint64_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > length) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0) return true;  // no exports at all
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const WORD count = nt->FileHeader.NumberOfSections;
    const auto offset = [&](uint32_t rva) -> uint64_t {
      for (WORD i = 0; i < count; ++i) {
        const auto& sec = sections[i];
        const uint32_t span = sec.Misc.VirtualSize > sec.SizeOfRawData ? sec.Misc.VirtualSize : sec.SizeOfRawData;
        if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + span)
          return uint64_t(sec.PointerToRawData) + (rva - sec.VirtualAddress);
      }
      return UINT64_MAX;
    };
    const uint64_t at = offset(dir.VirtualAddress);
    if (at == UINT64_MAX || at + sizeof(IMAGE_EXPORT_DIRECTORY) > length) return false;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + at);
    const uint64_t list = offset(exports->AddressOfNames);
    if (list == UINT64_MAX || list + uint64_t(exports->NumberOfNames) * 4 > length) return false;
    const auto* rvas = reinterpret_cast<const uint32_t*>(base + list);
    for (DWORD i = 0; i < exports->NumberOfNames && i < 4096; ++i) {
      const uint64_t name = offset(rvas[i]);
      if (name == UINT64_MAX || name >= length) continue;
      const char* text = reinterpret_cast<const char*>(base + name);
      size_t n = 0;
      while (name + n < length && n < 256 && text[n]) ++n;
      names.emplace_back(text, n);
    }
    return true;
  }
  static std::string FileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<uint8_t> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT bytes = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &bytes) || info == nullptr)
      return {};
    char text[64];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
  }
  // 1.12.1 (D6): the video memory this process uses on `device`'s adapter, in bytes (0 unknown).
  static uint64_t LocalUsage(ID3D12Device* device) {
    if (device == nullptr) return 0;
    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!create || FAILED(create(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
      return 0;
    return info.CurrentUsage;
  }

  // The shim the add-on carries (resource MFGUNLOCK_NR_SHIM), written once to
  // %LOCALAPPDATA%\MFGUnlock\DLSS5\<size>-<hash>\: so a game needs only nvngx_dlssnr.dll beside its exe.
  static std::wstring ExtractShim() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ApplyFault), &self);
    HRSRC info = FindResourceW(self, L"MFGUNLOCK_NR_SHIM", MAKEINTRESOURCEW(10));
    HGLOBAL data = info ? LoadResource(self, info) : nullptr;
    const auto* bytes = data ? static_cast<const uint8_t*>(LockResource(data)) : nullptr;
    const DWORD size = info ? SizeofResource(self, info) : 0;
    if (!bytes || !size) return {};
    uint64_t hash = 1469598103934665603ull;
    for (DWORD i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) GetTempPathW(MAX_PATH, base);
    wchar_t leaf[64];
    std::swprintf(leaf, 64, L"%lu-%016llx", static_cast<unsigned long>(size), static_cast<unsigned long long>(hash));
    const std::filesystem::path dir = std::filesystem::path(base) / L"MFGUnlock" / L"DLSS5" / leaf;
    const std::filesystem::path file = dir / L"mfgunlock_nvngx.dll";
    std::error_code error;
    if (std::filesystem::exists(file, error) && std::filesystem::file_size(file, error) == size) return file.wstring();
    std::filesystem::create_directories(dir, error);
    const std::filesystem::path temp = dir / (std::wstring(L"mfgunlock_nvngx.dll.") + std::to_wstring(GetCurrentProcessId()));
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes, size, &written, nullptr) && written == size;
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      DeleteFileW(temp.c_str());
      if (!std::filesystem::exists(file, error)) return {};
    }
    char t[MAX_PATH + 64];
    std::snprintf(t, sizeof(t), "the shim was written from the add-on to %ls", file.c_str());
    Log(t);
    return file.wstring();
  }

  // The game's back buffer, before Present: NR applied in place, on a list of MFG Unlock's own.
  bool Apply(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES bb_state, const Look& look,
             int scale_percent, Encoding encoding, const TaaInputs* taa = nullptr, const GameGuides* game = nullptr) {
    if (queue == nullptr || backbuffer == nullptr) return false;
    std::lock_guard guard(lock_);
    struct Scope {
      Scope() { t_in_apply = true; }
      ~Scope() { t_in_apply = false; }
    } scope;
    // The queue's own device: our command list must be of the same kind as the queue. ReShade's queue
    // wrapper takes native and wrapped lists alike, a native queue only native ones; the device of the
    // back buffer gave a wrapped list to ReShade's native queue in the present event (crash in D3D12Core).
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (FAILED(device->GetDeviceRemovedReason())) {
      Dred(device.Get());
      return false;
    }
    DXGI_FORMAT format{};
    if (!Prepare(device.Get(), backbuffer, encoding, scale_percent, false, format, queue)) return false;
    Frame& f = frames_[frame_index_++ % kFrames];
    if (fence_->GetCompletedValue() < f.fence) {
      fence_->SetEventOnCompletion(f.fence, event_);
      WaitForSingleObject(event_, 1000);
    }
    ReadTime(f);
    f.allocator->Reset();
    list_->Reset(f.allocator.Get(), nullptr);
    ID3D12GraphicsCommandList* l = list_.Get();
    taa_ = taa != nullptr && taa->motion != nullptr ? *taa : TaaInputs{};
    game_on_ = game != nullptr && game->g.mvec != nullptr && game->g.depth != nullptr;
    if (game_on_) game_ = *game;
    if (game_on_ && game_.g.reset && motion_mode_ != 2) reset_ = true;  // 1.12.1 (N1): the game's camera cut
    if (!Record(l, backbuffer, bb_state, format, look, nullptr, f)) {
      l->Close();
      return false;
    }
    if (FAILED(l->Close())) return Fail("command list");
    ID3D12CommandList* lists[] = {l};
    if (applied_ == 0) Log("first frame: execute");
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    DebugMessages();
    f.fence = fence_value_;
    return Ran("NR running on the game's frames, before Present");
  }

  // After the game's upscaler, on the game's command list: NR on its output, with its guides. The output
  // is in unordered access (the state the upscaler wrote it in) before and after.
  // `state`: the target's state before and after (an upscaler's output: unordered access; frame
  // generation's input: shader read). `encoding`: -1 from the format (an upscaler's output), else this one.
  bool ApplyAfterUpscale(ID3D12GraphicsCommandList* list, ID3D12Resource* output, const Guides& guides,
                         const Look& look, int scale_percent,
                         D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, int encoding = -1,
                         int route = 2) {
    if (list == nullptr || output == nullptr) return false;
    std::lock_guard guard(lock_);
    struct Scope {
      Scope() { t_in_apply = true; }
      ~Scope() { t_in_apply = false; }
    } scope;
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
    const DXGI_FORMAT f0 = Unorm(output->GetDesc().Format);
    // A float output is the scene before the game's tonemap; a UNORM one already display-ready.
    const bool linear = f0 == DXGI_FORMAT_R16G16B16A16_FLOAT || f0 == DXGI_FORMAT_R11G11B10_FLOAT;
    Encoding chosen = linear ? Encoding::kLinear : Encoding::kSdr;
    if (encoding >= 0) {
      chosen = static_cast<Encoding>(encoding);
      if (f0 == DXGI_FORMAT_R16G16B16A16_FLOAT) chosen = Encoding::kScRgb;
      else if (f0 != DXGI_FORMAT_R10G10B10A2_UNORM) chosen = Encoding::kSdr;
    }
    DXGI_FORMAT format{};
    if (!Prepare(device.Get(), output, chosen, scale_percent, true, format)) return false;
    heaps::Watch(list);
    if (!heaps::Known(list)) return false;  // the first frame after the watch went in: the game's heaps unknown
    route_kind_ = route;
    if (guides.reset) reset_ = true;
    Frame& f = frames_[frame_index_++ % kFrames];
    ReadTime(f);  // written three frames ago; the game's lists run in order
    taa_ = TaaInputs{};
    game_on_ = false;
    DebugMessages();  // the game's lists of earlier frames ran: what the debug layer said of them
    const heaps::Seen game_heaps = heaps::t_seen;
    const bool recorded = Record(list, output, state, format, look, &guides, f);
    heaps::Restore(list, game_heaps);
    if (!recorded) return false;
    return Ran(route == 4 ? "NR running on the game's frames, before its frame generation (with its motion vectors and depth)"
                          : "NR running on the game's frames, after its upscaler (with its motion vectors and depth)");
  }

  // A frame NR does not see (a loading screen, the game's FG): the next one starts a new history.
  void Pause(State why) {
    reset_ = true;
    if (state() == State::kRunning || state() == State::kOff) Set(why);
  }
  void Off() { Pause(State::kOff); }
  // How much of NR's change from earlier frames is kept, 0..100 ([DLSS5] Stability); read every frame.
  void Stability(int percent) { stability_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent); }
  // Passes: NR again on its own output, each pass a feature with its own history (1 to 3).
  void Passes(int n) { passes_ = n < 1 ? 1 : (n > pass_cap_ ? pass_cap_ : n); }
  // The light of SDR white in an HDR image, in nits (the game's, or Windows' SDR content brightness).
  void Paper(float nits) { paper_ = nits < 48.f ? 48.f : (nits > 1000.f ? 1000.f : nits); }
  // Compare: the left half of the image without NR (the hotkey).
  void Compare(bool on) {
    compare_ = on;
    g_status.compare.store(on, std::memory_order_relaxed);
  }
  // Motion on the Present route: 0 Auto (the game's TAA once proved, else estimated), 1 the game's only, 2 none.
  void Motion(int mode) { motion_mode_ = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }
  // Whether the game's TAA motion was proved (phase F), and the scale it was given.
  bool taa_proved() const { return taa_proved_; }
  // VRAM reserve in MB that NR keeps free (0: never pause for memory).
  void VramMargin(int mb) { vram_margin_mb_ = mb < 0 ? 0 : mb; }
  // Stand aside while another DLSS 5 mod runs NR (Off: run anyway).
  void Coexist(bool stand_aside) { stand_aside_ = stand_aside; }
  const std::wstring& foreign() const { return foreign_; }
  float paper() const { return paper_; }

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
  using ShimShutdown = int(__cdecl*)(void*, void*);

  // The checks and setup both routes share: state, format, encoding, device, textures.
  bool Prepare(ID3D12Device* device, ID3D12Resource* target, Encoding encoding, int scale_percent, bool after_upscale,
               DXGI_FORMAT& format, ID3D12CommandQueue* queue = nullptr) {
    // Whether the frames so far were recorded into the game's own lists (after the upscaler, before frame
    // generation): MFG Unlock's fence does not see those.
    const bool recorded_in_game_lists = route_after_upscale_;
    const State s = state();
    if (s == State::kNoRuntime || s == State::kUnsigned || s == State::kNoShim || s == State::kInitFailed ||
        s == State::kFailed || s == State::kNotNr)
      return false;
    if (stand_aside_ && Foreign()) {
      Set(State::kForeign);
      return false;
    }
    if (state() == State::kForeign) Set(State::kOff);
    if (device_ && VramShort()) {
      if (state() != State::kVram) {
        Retire(true);  // its memory back to the game, a few frames from now
        Set(State::kVram);
      }
      return false;
    }
    if (state() == State::kVram) Set(State::kOff);
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    format = Unorm(desc.Format);
    bool known = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (after_upscale) {
      known = known || format == DXGI_FORMAT_R11G11B10_FLOAT;
    } else {
      // HDR10 on 10-bit and scRGB on FP16 go through the curve in the shaders; anything else is left alone.
      if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) encoding = Encoding::kScRgb;
      else if (format != DXGI_FORMAT_R10G10B10A2_UNORM && encoding != Encoding::kSdr) encoding = Encoding::kSdr;
    }
    if (!known) {
      Set(State::kFormat);
      return false;
    }
    if (encoding != mode_) {
      mode_ = encoding;
      reset_ = true;
      Log(std::string("image encoding: ") +
          (encoding == Encoding::kSdr     ? "SDR"
           : encoding == Encoding::kPq    ? "HDR10 (PQ)"
           : encoding == Encoding::kScRgb ? "scRGB"
                                          : "linear, before the game's tonemap (metered)"));
    }
    if (route_after_upscale_ != after_upscale || (after_upscale && route_seen_ != route_kind_)) {
      route_seen_ = route_kind_;
      route_after_upscale_ = after_upscale;
      reset_ = true;
    }
    // A device of another kind (ReShade's wrapper or the native one behind it): the runtime starts again
    // on it, since lists, heaps and pipelines of one are not used with the other.
    // 1.12.1: at most one such restart every few seconds. CONTROL Resonant (game's DLSS-G with XeMFG DLSS-G,
    // 2026-11-24) turned its frame generation off and on in menus: NR went back and forth between the game's
    // Present and the frame generation's list, restarting four times in six seconds, and the GPU hung right
    // after two restarts 2 s apart. Within the window the other route's frames go without NR.
    if (device_ && device != device_.Get()) {
      const uint64_t now = GetTickCount64();
      if (switched_tick_ != 0 && now - switched_tick_ < kSwitchHoldMs) {
        if (!switch_held_logged_) {
          switch_held_logged_ = true;
          Log("the route changed again within a few seconds: NR stays on its device a little longer");
        }
        return false;
      }
      switched_tick_ = now;
      switch_held_logged_ = false;
      Log("another device object (the route changed): NR starts again on it");
      if (recorded_in_game_lists) DrainGameWork(device, queue);
      Teardown();
    }
    if (!device_ && !Start(device)) return false;
    if (!Resources(desc, format, scale_percent)) return Fail("resources");
    return true;
  }

  // Another DLSS 5 mod loaded in the game (its add-on's file name has "dlss5" in it, or it created NR
  // through the driver's NGX), checked every two seconds.
  bool Foreign() {
    const uint64_t now = GetTickCount64();
    if (now - foreign_checked_ < 2000) return !foreign_.empty();
    foreign_checked_ = now;
    foreign_.clear();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ApplyFault), &self);
    using EnumFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    static const auto enum_modules = reinterpret_cast<EnumFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (enum_modules && enum_modules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
      for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
        if (modules[i] == self) continue;
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(modules[i], path, MAX_PATH);
        std::wstring name = std::filesystem::path(path).filename().wstring();
        std::wstring lower = name;
        for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        if (lower.find(L"dlss5") != std::wstring::npos && lower.find(L".addon") != std::wstring::npos) {
          foreign_ = name;
          break;
        }
      }
    if (foreign_.empty() && g_foreign_create.load(std::memory_order_relaxed)) foreign_ = L"NGX feature 18";
    if (!foreign_.empty() && !foreign_logged_) {
      foreign_logged_ = true;
      char t[MAX_PATH + 96];
      std::snprintf(t, sizeof(t), "another DLSS 5 mod is loaded (%ls): MFG Unlock's NR stands aside", foreign_.c_str());
      Log(t);
    }
    return !foreign_.empty();
  }
  // Whether the GPU's memory budget has less than the reserve left (checked once a second).
  bool VramShort() {
    if (vram_margin_mb_ <= 0) return false;
    const uint64_t now = GetTickCount64();
    if (now - vram_checked_ < 1000) return vram_short_;
    vram_checked_ = now;
    if (!adapter_) {
      using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
      const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      const auto create = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
      ComPtr<IDXGIFactory4> factory;
      if (!create || FAILED(create(IID_PPV_ARGS(&factory))) ||
          FAILED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter_))))
        return vram_short_ = false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return vram_short_ = false;
    const uint64_t margin = uint64_t(vram_margin_mb_) << 20;
    // Hysteresis: back on once 256 MB more than the reserve is free.
    const uint64_t free_now = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
    const bool short_now = vram_short_ ? free_now < margin + (256ull << 20) : free_now < margin;
    if (short_now != vram_short_) {
      char t[128];
      std::snprintf(t, sizeof(t), short_now ? "paused: %llu MB of VRAM left (reserve %d MB)" : "VRAM back (%llu MB free): NR resumes",
                    static_cast<unsigned long long>(free_now >> 20), vram_margin_mb_);
      Log(t);
    }
    return vram_short_ = short_now;
  }

  void ReadTime(Frame& f) {
    if (!f.has_time) return;
    UINT64* t = nullptr;
    D3D12_RANGE range{0, 16};
    if (SUCCEEDED(f.stamps->Map(0, &range, reinterpret_cast<void**>(&t)))) {
      if (t[1] > t[0]) gpu_ms_.store(double(t[1] - t[0]) * 1000.0 / double(ticks_), std::memory_order_relaxed);
      D3D12_RANGE none{0, 0};
      f.stamps->Unmap(0, &none);
    }
  }

  bool Ran(const char* how) {
    if (state_.exchange(State::kRunning) != State::kRunning) Log(how);
    g_status.route.store(route_after_upscale_ ? route_kind_ : 1, std::memory_order_relaxed);
    g_status.guides.store(guides_kind_, std::memory_order_relaxed);
    g_status.ms.store(static_cast<float>(gpu_ms()), std::memory_order_relaxed);
    g_status.width.store(low_w_, std::memory_order_relaxed);
    g_status.height.store(low_h_, std::memory_order_relaxed);
    if (++applied_ % 600 == 0) {
      char t[128];
      std::snprintf(t, sizeof(t), "NR %ux%u, %.2f ms GPU per frame (%llu frames%s)", low_w_, low_h_, gpu_ms(),
                    static_cast<unsigned long long>(applied_), route_after_upscale_ ? ", after upscaling" : "");
      Log(t);
    }
    return true;
  }

  // The passes, on `l`: copy, meter (linear), reduce, NR, steady (without guides), compose into `target`.
  // `target` is in `state` before and after; MFG Unlock's own textures end where they started.
  bool Record(ID3D12GraphicsCommandList* l, ID3D12Resource* target, D3D12_RESOURCE_STATES state, DXGI_FORMAT format,
              const Look& look, const Guides* g, Frame& f) {
    Bury();
    const bool after_upscale = g != nullptr;  // the game's list and its upscaler's output
    // 1.12.1 (D1): the game's tagged guides on the Present route (unless motion is off).
    const bool game_now = g == nullptr && game_on_ && motion_mode_ != 2;
    const bool inverted = (g != nullptr && g->depth_inverted) || (game_now && game_.g.depth_inverted) ||
                          (g == nullptr && !game_now && taa_proved_ && taa_.depth_inverted);
    if (!(look == look_) || !feature_ || inverted != created_inverted_ || passes_ != created_passes_) {
      Retire(false);
      look_ = look;
      created_inverted_ = inverted;
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
      params_.Set("DLSSNR.DepthInverted", inverted ? 1 : 0);
      const int r = create_(create_fn_, l, 18, &params_, &feature_);
      if (r != 1 || feature_ == nullptr) return Fail(("CreateFeature 0x" + Hex(r)).c_str());
      // The later passes: their own features (history) and the textures between them.
      created_passes_ = 1;
      for (int i = 0; i + 1 < passes_; ++i) {
        if (!chain_[i]) chain_[i] = Texture(low_w_, low_h_, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!chain_[i]) break;
        const int c = create_(create_fn_, l, 18, &params_, &extra_[i]);
        if (c != 1 || extra_[i] == nullptr) {
          extra_[i] = nullptr;
          Log("a later NR pass could not be created (0x" + Hex(c) + "): fewer passes");
          break;
        }
        created_passes_ = i + 2;
      }
      if (created_passes_ < passes_) passes_ = pass_cap_ = created_passes_;  // not again each frame
      // 1.12.1 (D6): what NR took, from before Init to its first features and textures (the game may
      // allocate meanwhile: "about").
      if (!vram_measured_ && vram_before_ != 0) {
        vram_measured_ = true;
        const uint64_t now = LocalUsage(device_.Get());
        if (now > vram_before_) {
          vram_mib_.store(static_cast<int>((now - vram_before_) >> 20), std::memory_order_relaxed);
          Log("about " + std::to_string((now - vram_before_) >> 20) + " MiB of video memory for NR");
        }
      }
      Log("NR feature created at " + std::to_string(low_w_) + "x" + std::to_string(low_h_) + " (preset " +
          std::to_string(look.preset) + ", style " + std::to_string(look.style) + ", intensity " +
          std::to_string(look.intensity) + "%, " + std::to_string(created_passes_) + " pass" +
          (created_passes_ > 1 ? "es)" : ")"));
    }

    if (applied_ == 0) Log("first frame: recording");
    Barrier(l, target, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(full_.Get(), target);
    Barrier(l, full_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    l->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const bool fresh = reset_;  // no history to keep either
    // t3/t5/t7 = last frame's steadied change, meter and luma; t4/t6/t8 = this frame's.
    parity_ ^= 1;
    rate_ = fresh ? 1.f : 0.05f;
    // The Present route's motion: the game's TAA once proved, else estimated from the image.
    const bool estimate = g == nullptr && motion_mode_ == 0 && !game_now;
    const bool taa_now = g == nullptr && !game_now && motion_mode_ != 2 && taa_.motion != nullptr &&
                         taa_.motion != taa_rejected_;
    if (taa_.motion != taa_candidate_) {  // another texture: prove it again
      taa_candidate_ = taa_.motion;
      taa_proved_ = false;
      taa_good_ = taa_bad_ = 0;
      for (auto& v : pool_) v = 0;
    }
    const bool use_taa = taa_now && taa_proved_;
    guides_kind_ = g != nullptr ? (g->mvec ? 1 : 0) : game_now ? 1 : use_taa ? 3 : estimate ? 2 : 0;
    keep_ = (fresh || guides_kind_ != 0) ? 0.f : stability_ * 0.009f;  // 100% keeps 90% where nothing moved
    // 1.12.1 (N2): the game's vectors and its image before the HUD, when it gives one of the frame's size and
    // format (before its frame generation, or tagged to its Streamline on the Present route).
    const Guides* given = game_now ? &game_.g : g;
    const bool mask_inputs = look.automask && given != nullptr && given->mvec != nullptr && given->hudless != nullptr &&
                             HudlessFits(given->hudless, target);
    WriteTable(format, taa_now ? &taa_ : nullptr, mask_inputs ? given : nullptr);

    // Compute passes only, as the upscaler itself: a game restores its compute state after it, not
    // necessarily its graphics state (the Streamline sample went black when these were draws).
    // 0. Meter (a linear image).
    if (mode_ == Encoding::kLinear) {
      ID3D12Resource* const m = meter_[1 - parity_].Get();
      Barrier(l, m, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Dispatch(l, pso_meter_cs_.Get(), kUavMeter + (1 - parity_), 1, 1, 1);
      Barrier(l, m, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
      // The meter's value in the log every 600 frames (read back three frames later).
      if (!meter_read_) meter_read_ = Buffer(256);
      if (meter_read_ && applied_ % 600 == 3) {
        uint16_t* v = nullptr;
        D3D12_RANGE range{0, 8};
        if (SUCCEEDED(meter_read_->Map(0, &range, reinterpret_cast<void**>(&v)))) {
          const auto half = [](uint16_t h) {
            const uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
            float f = e == 0 ? m / 1024.f / 16384.f : e == 31 ? 65504.f : (1.f + m / 1024.f) * std::ldexp(1.f, int(e) - 15);
            return sign ? -f : f;
          };
          char t[96];
          std::snprintf(t, sizeof(t), "meter: level %.4g, this frame %.4g", half(v[0]), half(v[1]));
          Log(t);
          D3D12_RANGE none{0, 0};
          meter_read_->Unmap(0, &none);
        }
      }
      if (meter_read_ && applied_ % 600 == 0) {
        Barrier(l, m, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{m, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{meter_read_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 1, 256};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(l, m, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
      }
    }

    // 1. Reduce.
    Dispatch(l, pso_reduce_cs_.Get(), kUavLow, low_w_, low_h_, 8);
    Barrier(l, low_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 1b. Motion from the image (phase D), and the game's TAA motion checked against it (phase F).
    if (estimate || taa_now) {
      ID3D12Resource* const luma = luma_[1 - parity_].Get();
      Barrier(l, luma, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Dispatch(l, pso_luma_cs_.Get(), kUavLuma + (1 - parity_), low_w_, low_h_, 8);
      Barrier(l, luma, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
      Barrier(l, motion_coarse_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Dispatch(l, pso_coarse_cs_.Get(), kUavCoarse, (low_w_ + 7) / 8, (low_h_ + 7) / 8, 8);
      Barrier(l, motion_coarse_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
      Barrier(l, motion_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Dispatch(l, pso_fine_cs_.Get(), kUavMotion, motion_w_, motion_h_, 8);
      Barrier(l, motion_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
      if (!flat_done_) {
        Barrier(l, flat_depth_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(l, pso_flat_cs_.Get(), kUavFlat, motion_w_, motion_h_, 8);
        Barrier(l, flat_depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        flat_done_ = true;
      }
      if (taa_now && !taa_proved_) Calibrate(l);
    }
    if (taa_now) {
      Barrier(l, taa_.motion, taa_.motion_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      if (taa_.depth) Barrier(l, taa_.depth, taa_.depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Guides present_guides;
    if (game_now) {
      Barrier(l, game_.g.mvec, game_.mvec_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Barrier(l, game_.g.depth, game_.depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      g = &game_.g;
    } else if (use_taa) {
      const auto md = taa_.motion->GetDesc();
      present_guides.mvec = taa_.motion;
      present_guides.mv_w = static_cast<uint32_t>(md.Width);
      present_guides.mv_h = md.Height;
      present_guides.mv_scale_x = taa_scale_x_;
      present_guides.mv_scale_y = taa_scale_y_;
      if (taa_.depth) {
        const auto dd = taa_.depth->GetDesc();
        present_guides.depth = taa_.depth;
        present_guides.depth_w = static_cast<uint32_t>(dd.Width);
        present_guides.depth_h = dd.Height;
        present_guides.depth_inverted = taa_.depth_inverted;
      } else {
        present_guides.depth = flat_depth_.Get();
        present_guides.depth_w = motion_w_;
        present_guides.depth_h = motion_h_;
      }
      g = &present_guides;
    } else if (estimate) {
      present_guides.mvec = motion_.Get();
      present_guides.depth = flat_depth_.Get();
      present_guides.mv_w = present_guides.depth_w = motion_w_;
      present_guides.mv_h = present_guides.depth_h = motion_h_;
      g = &present_guides;
    }

    // 1c. 1.12.1 (N3, N2): a camera cut where the game gives no reset (motion from the image or its TAA), and
    // how much of the screen the HUD covers; read three frames later.
    Stats(l, estimate || use_taa, mask_inputs);
    // 1d. 1.12.1 (N2): the game's vectors, zero on the HUD, unless the image "before the HUD" differs from the
    // frame over much of the screen (an effect after it: RenoDX, a filter).
    Guides masked_guides;
    if (mask_inputs && hud_mask_ok_ && g != nullptr && g->mvec == given->mvec) {
      const D3D12_RESOURCE_DESC md = given->mvec->GetDesc();
      const DXGI_FORMAT mf = VectorFormat(md.Format) == DXGI_FORMAT_R32G32_FLOAT ? DXGI_FORMAT_R32G32_FLOAT
                                                                                : DXGI_FORMAT_R16G16_FLOAT;
      if (MaskedVectors(static_cast<uint32_t>(md.Width), md.Height, mf)) {
        Barrier(l, masked_mv_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(l, pso_hudmask_cs_.Get(), kUavMasked, static_cast<UINT>(md.Width), md.Height, 8);
        Barrier(l, masked_mv_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        masked_guides = *g;
        masked_guides.mvec = masked_mv_.Get();
        g = &masked_guides;
      }
    }

    // 2. NR on the reduced image, with the game's guides where there are some; each later pass on the
    // output of the one before.
    params_.Set("DLSSNR.MVec", g ? g->mvec : static_cast<ID3D12Resource*>(nullptr));
    params_.Set("DLSSNR.Depth", g ? g->depth : static_cast<ID3D12Resource*>(nullptr));
    params_.Set("DLSSNR.Width", low_w_);
    params_.Set("DLSSNR.Height", low_h_);
    params_.Set("DLSSNR.Enabled", 1);
    params_.Set("DLSSNR.Reset", reset_ ? 1 : 0);
    params_.Set("DLSSNR.MVecScaleX", g ? g->mv_scale_x : 1.f);
    params_.Set("DLSSNR.MVecScaleY", g ? g->mv_scale_y : 1.f);
    const auto subrect = [&](const std::string& base, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
      params_.Set((base + "SubrectBaseX").c_str(), x);
      params_.Set((base + "SubrectBaseY").c_str(), y);
      params_.Set((base + "SubrectWidth").c_str(), w);
      params_.Set((base + "SubrectHeight").c_str(), h);
    };
    subrect("DLSSNR.Color", 0, 0, low_w_, low_h_);
    subrect("DLSSNR.Output", 0, 0, low_w_, low_h_);
    if (g) {
      subrect("DLSSNR.MVec", g->mv_x, g->mv_y, g->mv_w, g->mv_h);
      subrect("DLSSNR.Depth", g->depth_x, g->depth_y, g->depth_w, g->depth_h);
    }
    reset_ = false;
    if (applied_ == 0) Log("first frame: evaluate");
    const int passes = created_passes_;
    for (int i = 0; i < passes; ++i) {
      ID3D12Resource* const in = i == 0 ? low_.Get() : chain_[i - 1].Get();
      ID3D12Resource* const out = i + 1 == passes ? enhanced_.Get() : chain_[i].Get();
      params_.Set("DLSSNR.Color", in);
      params_.Set("DLSSNR.Output", out);
      const int r = evaluate_(evaluate_fn_, l, i == 0 ? feature_ : extra_[i - 1], &params_, nullptr);
      if (r != 1) return Fail(("EvaluateFeature 0x" + Hex(r)).c_str());
      if (i + 1 < passes) Barrier(l, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (int i = 0; i + 1 < passes; ++i)
      Barrier(l, chain_[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 3. Steady NR's change over frames (the runtime set its own heaps and root signature).
    Barrier(l, enhanced_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* const written = steady_[1 - parity_].Get();
    Barrier(l, written, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Dispatch(l, pso_stabilize_cs_.Get(), kUavSteady + (1 - parity_), low_w_, low_h_, 8);
    Barrier(l, written, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);

    if (taa_now) {
      Barrier(l, taa_.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, taa_.motion_state);
      if (taa_.depth) Barrier(l, taa_.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, taa_.depth_state);
    }
    if (game_now) {
      Barrier(l, game_.g.mvec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, game_.mvec_state);
      Barrier(l, game_.g.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, game_.depth_state);
    }

    // 4. Compose into the target: a render target before Present, unordered access after upscaling.
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    if (after_upscale) {
      Barrier(l, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const UINT slot = kUavRingStart + static_cast<UINT>(uav_next_++ % kUavRing);  // a list may still be pending
      D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
      uv.Format = format;
      uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device_->CreateUnorderedAccessView(target, nullptr, &uv, Cpu(slot));
      Dispatch(l, pso_compose_cs_.Get(), slot, static_cast<UINT>(desc.Width), desc.Height, 8);
      D3D12_RESOURCE_BARRIER uav_done{};
      uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      uav_done.UAV.pResource = target;
      l->ResourceBarrier(1, &uav_done);
      Barrier(l, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state);
    } else {
      Barrier(l, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      ID3D12Resource* const read[] = {full_.Get(), steady_[1 - parity_].Get(), meter_[1 - parity_].Get()};
      for (auto* r : read) Barrier(l, r, kRead, kPixelRead);
      device_->CreateRenderTargetView(target, &rtv_desc_, rtv_bb_);
      Draw(l, ComposePso(format), rtv_bb_, static_cast<uint32_t>(desc.Width), desc.Height);
      for (auto* r : read) Barrier(l, r, kPixelRead, kRead);
      Barrier(l, target, D3D12_RESOURCE_STATE_RENDER_TARGET, state);
    }
    l->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    l->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, f.stamps.Get(), 0);
    f.has_time = true;
    Barrier(l, enhanced_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(l, low_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(l, full_.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    return true;
  }

  // The feature (and with `textures`, the textures) out of use, released kRetireFrames frames later.
  void Retire(bool textures) {
    Retired r;
    r.feature = feature_;
    feature_ = nullptr;
    for (void*& extra : extra_) {
      if (extra == nullptr) continue;
      Retired e;
      e.feature = extra;
      e.after = frame_index_ + kRetireFrames;
      retired_.push_back(std::move(e));
      extra = nullptr;
    }
    created_passes_ = 0;
    if (textures) {
      r.textures = {full_, low_, enhanced_, steady_[0], steady_[1], luma_[0], luma_[1], motion_coarse_, motion_, flat_depth_,
                    chain_[0], chain_[1]};
      chain_[0].Reset(); chain_[1].Reset();
      full_.Reset(); low_.Reset(); enhanced_.Reset(); steady_[0].Reset(); steady_[1].Reset();
      luma_[0].Reset(); luma_[1].Reset(); motion_coarse_.Reset(); motion_.Reset(); flat_depth_.Reset();
    }
    r.after = frame_index_ + kRetireFrames;
    if (r.feature || textures) retired_.push_back(std::move(r));
  }
  void Bury() {
    for (size_t i = 0; i < retired_.size();)
      if (retired_[i].after <= frame_index_) {
        if (retired_[i].feature) release_(release_fn_, retired_[i].feature);
        retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
  }

  // Before a restart that follows frames recorded into the game's lists: those lists may still be queued on
  // the GPU with NR's feature and textures in them. The game's queue at hand (the Present route) is waited
  // for; frame generation's other queues get a few frames' time.
  static void DrainGameWork(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (queue != nullptr && device != nullptr) {
      ComPtr<ID3D12Fence> fence;
      if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
          SUCCEEDED(queue->Signal(fence.Get(), 1)) && fence->GetCompletedValue() < 1) {
        const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done != nullptr) {
          if (SUCCEEDED(fence->SetEventOnCompletion(1, done))) WaitForSingleObject(done, 1000);
          CloseHandle(done);
        }
      }
    }
    Sleep(50);
  }

  // 1.12.1 (N2): a format a shader reads the game's vectors with.
  static DXGI_FORMAT VectorFormat(DXGI_FORMAT f) {
    switch (f) {
      case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
      case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
      default: return f;
    }
  }
  // The image before the HUD compares with the frame: same size, same kind of format.
  static bool HudlessFits(ID3D12Resource* hudless, ID3D12Resource* frame) {
    const D3D12_RESOURCE_DESC h = hudless->GetDesc(), f = frame->GetDesc();
    return h.Width == f.Width && h.Height == f.Height && Unorm(h.Format) == Unorm(f.Format);
  }
  // The HUD-masked vectors' texture, of the game's vectors' size.
  bool MaskedVectors(uint32_t w, uint32_t h, DXGI_FORMAT f) {
    if (masked_mv_) {
      const D3D12_RESOURCE_DESC d = masked_mv_->GetDesc();
      if (d.Width == w && d.Height == h && d.Format == f) return true;
      masked_old_ = std::move(masked_mv_);  // lists of earlier frames may still read it
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, kRead, nullptr,
                                                IID_PPV_ARGS(&masked_mv_))))
      return false;
    masked_mv_->SetName(L"MFG Unlock DLSS 5 vectors without the HUD");
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = f;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(masked_mv_.Get(), nullptr, &uv, Cpu(kUavMasked));
    return true;
  }
  // 1.12.1 (N2, N3): the statistics of three frames ago, then this frame's, into a ring of four readbacks.
  // `cut_route`: this frame's motion is the image's or the TAA's (no reset of the game's); `hud`: the image
  // before the HUD is bound.
  void Stats(ID3D12GraphicsCommandList* l, bool cut_route, bool hud) {
    if (!stats_read_) stats_read_ = Buffer(4 * 256);
    if (!stats_read_) return;
    const UINT read_slot = static_cast<UINT>((frame_index_ + 1) % 4), write_slot = static_cast<UINT>(frame_index_ % 4);
    if (stats_written_[read_slot] != 0 && frame_index_ + 1 >= stats_written_[read_slot] + 3) {
      float* v = nullptr;
      D3D12_RANGE range{read_slot * 256, read_slot * 256 + 16};
      if (SUCCEEDED(stats_read_->Map(0, &range, reinterpret_cast<void**>(&v)))) {
        const float cut = v[read_slot * 64], share = v[read_slot * 64 + 1];
        D3D12_RANGE none{0, 0};
        stats_read_->Unmap(0, &none);
        // N3: a camera cut: NR's history is of the scene before it.
        const uint64_t now = GetTickCount64();
        if (stats_cut_route_[read_slot] && cut > 0.12f && now - last_cut_ > 500) {
          last_cut_ = now;
          reset_ = true;
          if (cut_reports_++ < 5) {
            char t[96];
            std::snprintf(t, sizeof(t), "camera cut (luma change %.2f): NR's history starts again", cut);
            Log(t);
          }
        }
        // N2: the mask only while the HUD is a small part of the screen.
        if (hud) {
          const bool ok = share < 0.3f;
          if (ok != hud_mask_ok_ && hud_reports_++ < 6) {
            char t[160];
            std::snprintf(t, sizeof(t), ok ? "HUD: the game's vectors are zeroed on the HUD (%.0f%% of the screen)"
                                           : "HUD: the image before the HUD differs over %.0f%% of the screen: the game's "
                                             "vectors are used as they are",
                          share * 100.f);
            Log(t);
          }
          hud_mask_ok_ = ok;
        }
      }
      stats_written_[read_slot] = 0;
    }
    if (!hud) hud_mask_ok_ = false;
    if (!cut_route && !hud) return;
    Barrier(l, stats_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Dispatch(l, pso_stats_cs_.Get(), kUavStats, 1, 1, 1);
    Barrier(l, stats_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{stats_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{stats_read_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Offset = write_slot * 256;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 256};
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l, stats_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    stats_written_[write_slot] = frame_index_ + 1;  // 0 stays "none"
    stats_cut_route_[write_slot] = cut_route;
  }

  // Everything made on the device, released, so the runtime can start again on another one.
  void Teardown() {
    if (fence_ && fence_value_ && fence_->GetCompletedValue() < fence_value_) {
      fence_->SetEventOnCompletion(fence_value_, event_);
      WaitForSingleObject(event_, 2000);
    }
    Retire(true);
    for (auto& r : retired_)
      if (r.feature) release_(release_fn_, r.feature);
    retired_.clear();
    // The runtime's state for this device (Shutdown1 takes it); Init on the next one starts clean.
    if (shutdown_ && shutdown_fn_ && device_) shutdown_(shutdown_fn_, device_.Get());
    for (Frame& f : frames_) f = {};
    list_.Reset();
    fence_.Reset();
    fence_value_ = 0;
    queries_.Reset();
    vs_.Reset(); ps_compose_.Reset(); cs_reduce_.Reset(); cs_stabilize_.Reset(); cs_meter_.Reset(); cs_compose_.Reset();
    root_.Reset();
    pso_compose_.Reset(); pso_reduce_cs_.Reset(); pso_stabilize_cs_.Reset(); pso_meter_cs_.Reset(); pso_compose_cs_.Reset();
    pso_luma_cs_.Reset(); pso_coarse_cs_.Reset(); pso_fine_cs_.Reset(); pso_flat_cs_.Reset(); pso_calibrate_cs_.Reset();
    cs_luma_.Reset(); cs_coarse_.Reset(); cs_fine_.Reset(); cs_flat_.Reset(); cs_calibrate_.Reset();
    calib_.Reset(); calib_read_.Reset(); calib_pending_ = false;
    stats_.Reset(); stats_read_.Reset(); masked_mv_.Reset(); masked_old_.Reset();  // 1.12.1 (N2, N3)
    cs_hudmask_.Reset(); cs_stats_.Reset(); pso_hudmask_cs_.Reset(); pso_stats_cs_.Reset();
    for (auto& w : stats_written_) w = 0;
    hud_mask_ok_ = false;
    srv_heap_.Reset();
    rtv_heap_.Reset();
    for (auto& t : steady_) t.Reset();
    for (auto& t : meter_) t.Reset();
    meter_read_.Reset();
    full_.Reset(); low_.Reset(); enhanced_.Reset();
    full_w_ = full_h_ = low_w_ = low_h_ = 0;
    compose_format_ = full_format_ = DXGI_FORMAT_UNKNOWN;
    info_queue_.Reset();
    adapter_.Reset();
    device_.Reset();
    reset_ = true;
  }

  State Set(State s) {
    // Each reason NR is not running goes to the log once per change, so a tester's log tells why.
    const State before = state_.exchange(s, std::memory_order_relaxed);
    if (before != s) {
      const char* why = s == State::kNoRuntime   ? "nvngx_dlssnr.dll not found"
                        : s == State::kUnsigned  ? "nvngx_dlssnr.dll is not signed by NVIDIA (AllowUnsigned=0)"
                        : s == State::kNoShim    ? "MFGUnlock-DLSS5/mfgunlock_nvngx.dll not found"
                        : s == State::kFormat    ? "the back buffer format is not one NR handles"
                        : s == State::kWithGameFg ? "paused: the game's frame generation is on without the hook before it"
                        : s == State::kVram       ? "paused for VRAM"
                        : s == State::kNotNr      ? "nvngx_dlssnr.dll is not NVIDIA's NR runtime (see the line above)"
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
    if (!info_queue_ || debug_logged_ >= 60) return;
    const UINT64 n = info_queue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && debug_logged_ < 60; ++i) {
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
 public:
  // After a device loss: which command lists the GPU had not finished (DRED, when [Diagnostics] ForceDRED=1
  // turned it on before the game made its device). Once per process.
  static void Dred(ID3D12Device* device) {
    static std::atomic<bool> done{false};
    if (device == nullptr || SUCCEEDED(device->GetDeviceRemovedReason()) || done.exchange(true)) return;
    char t[512];
    std::snprintf(t, sizeof(t), "device lost (reason 0x%08lx)", static_cast<unsigned long>(device->GetDeviceRemovedReason()));
    Log(t);
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return Log("DRED: not available");
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (FAILED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) return Log("DRED: no breadcrumbs (set [Diagnostics] ForceDRED=1)");
    const auto name = [](const wchar_t* w, const char* a) {
      char out[128]{};
      if (a) std::snprintf(out, sizeof(out), "%s", a);
      else if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, int(sizeof(out) - 1), nullptr, nullptr);
      return std::string(out[0] ? out : "(unnamed)");
    };
    int shown = 0;
    for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n != nullptr && shown < 24; n = n->pNext) {
      const UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      if (last >= n->BreadcrumbCount) continue;
      ++shown;
      std::string ops;
      for (UINT i = (last > 3 ? last - 3 : 0); i < n->BreadcrumbCount && i < last + 4; ++i)
        ops += (i == last ? "[" : "") + std::to_string(int(n->pCommandHistory[i])) + (i == last ? "] " : " ");
      std::snprintf(t, sizeof(t), "DRED: unfinished list '%s' on queue '%s': %u of %u ops done; ops %s",
                    name(n->pCommandListDebugNameW, n->pCommandListDebugNameA).c_str(),
                    name(n->pCommandQueueDebugNameW, n->pCommandQueueDebugNameA).c_str(), last, n->BreadcrumbCount,
                    ops.c_str());
      Log(t);
    }
    if (shown == 0) Log("DRED: every recorded list had finished");
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA != 0) {
      std::string existing, freed;
      for (auto* a = fault.pHeadExistingAllocationNode; a != nullptr; a = a->pNext) existing += name(a->ObjectNameW, a->ObjectNameA) + "; ";
      for (auto* a = fault.pHeadRecentFreedAllocationNode; a != nullptr; a = a->pNext) freed += name(a->ObjectNameW, a->ObjectNameA) + "; ";
      std::snprintf(t, sizeof(t), "DRED: page fault; at the address: %s recently freed: %s", existing.substr(0, 200).c_str(),
                    freed.substr(0, 200).c_str());
      Log(t);
    }
  }
 private:

  bool Start(ID3D12Device* device) {
    // The runtime, its shim and the fault handler stay for the process: so does this module.
    static bool pinned = false;
    if (!pinned) {
      HMODULE self = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&ApplyFault), &self))
        return Fail("the add-on could not stay loaded");
      AddVectoredExceptionHandler(1, &ApplyFault);  // logs a fault inside Apply with its stack
      pinned = true;
    }
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
    shutdown_ = reinterpret_cast<ShimShutdown>(GetProcAddress(shim, "MfgNr_ShutdownDevice"));
    shutdown_fn_ = GetProcAddress(nr, "NVSDK_NGX_D3D12_Shutdown1");
    if (!init || !create_ || !evaluate_ || !release_ || !init_fn || !create_fn_ || !evaluate_fn_ || !release_fn_) {
      // 1.12.1: which ones (Once Human, 2026-11-24: an unsigned nvngx_dlssnr.dll, "missing exports" alone).
      std::string missing;
      const auto add = [&](const void* fn, const char* name) {
        if (fn == nullptr) missing += (missing.empty() ? "" : ", ") + std::string(name);
      };
      add(reinterpret_cast<const void*>(init), "mfgunlock_nvngx.dll!MfgNr_Init");
      add(reinterpret_cast<const void*>(create_), "mfgunlock_nvngx.dll!MfgNr_Create");
      add(reinterpret_cast<const void*>(evaluate_), "mfgunlock_nvngx.dll!MfgNr_Evaluate");
      add(reinterpret_cast<const void*>(release_), "mfgunlock_nvngx.dll!MfgNr_Release");
      add(init_fn, "nvngx_dlssnr.dll!NVSDK_NGX_D3D12_Init_Ext");
      add(create_fn_, "nvngx_dlssnr.dll!NVSDK_NGX_D3D12_CreateFeature");
      add(evaluate_fn_, "nvngx_dlssnr.dll!NVSDK_NGX_D3D12_EvaluateFeature");
      add(release_fn_, "nvngx_dlssnr.dll!NVSDK_NGX_D3D12_ReleaseFeature");
      Log("missing exports: " + missing +
          (init_fn == nullptr || create_fn_ == nullptr ? " (this nvngx_dlssnr.dll is not NVIDIA's NR runtime, or of "
                                                         "another kind; use NVIDIA's own file)"
                                                       : ""));
      return Fail("missing exports");
    }
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    vram_before_ = LocalUsage(device);  // 1.12.1 (D6)
    vram_measured_ = false;
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
    list_->SetName(L"MFG Unlock DLSS 5 NR");
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return Fail("fence");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {  // A device loss signals every fence with UINT64_MAX: report it then, even when the game aborts at once.
      // Also signalled when a fence goes away (NR restarted on another device object): Dred checks the device.
      static HANDLE lost = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      static HANDLE wait = nullptr;
      static std::atomic<ID3D12Device*> watched{nullptr};
      if (lost != nullptr && SUCCEEDED(fence_->SetEventOnCompletion(UINT64_MAX, lost))) {
        device->AddRef();  // kept: the callback may run after NR let it go
        watched.store(device);
        if (wait == nullptr)
          RegisterWaitForSingleObject(&wait, lost, [](void*, BOOLEAN) { Dred(watched.load()); }, nullptr, INFINITE,
                                      WT_EXECUTEDEFAULT);
      }
    }
    {  // the timestamp frequency (after upscaling there is no queue of the game's to ask)
      D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
      ComPtr<ID3D12CommandQueue> q;
      if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) q->GetTimestampFrequency(&ticks_);
    }
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries_)))) return Fail("query heap");
    if (!Pipeline()) return false;
    g_nr_started.store(true);
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
    if (!build("vs", "vs_5_0", vs_) || !build("compose", "ps_5_0", ps_compose_) ||
        !build("reduce_cs", "cs_5_0", cs_reduce_) || !build("stabilize_cs", "cs_5_0", cs_stabilize_) ||
        !build("meter_cs", "cs_5_0", cs_meter_) || !build("compose_cs", "cs_5_0", cs_compose_) ||
        !build("luma_cs", "cs_5_0", cs_luma_) || !build("coarse_cs", "cs_5_0", cs_coarse_) ||
        !build("fine_cs", "cs_5_0", cs_fine_) || !build("flat_cs", "cs_5_0", cs_flat_) ||
        !build("calibrate_cs", "cs_5_0", cs_calibrate_) || !build("hudmask_cs", "cs_5_0", cs_hudmask_) ||
        !build("stats_cs", "cs_5_0", cs_stats_))
      return Fail("shader compilation");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTable, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER table[3]{};
    table[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table[0].DescriptorTable = {1, &range};
    table[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    table[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    table[1].Constants = {0, 0, 5};
    table[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    table[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table[2].DescriptorTable = {1, &uav_range};
    table[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{3, table, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errors;
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                         ID3DBlob**);
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize =
        d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_))))
      return Fail("root signature");
    const auto compute = [&](ID3DBlob* cs) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
      cd.pRootSignature = root_.Get();
      cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
      ComPtr<ID3D12PipelineState> pso;
      device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso));
      return pso;
    };
    pso_reduce_cs_ = compute(cs_reduce_.Get());
    pso_stabilize_cs_ = compute(cs_stabilize_.Get());
    pso_meter_cs_ = compute(cs_meter_.Get());
    pso_compose_cs_ = compute(cs_compose_.Get());
    pso_luma_cs_ = compute(cs_luma_.Get());
    pso_coarse_cs_ = compute(cs_coarse_.Get());
    pso_fine_cs_ = compute(cs_fine_.Get());
    pso_flat_cs_ = compute(cs_flat_.Get());
    pso_calibrate_cs_ = compute(cs_calibrate_.Get());
    pso_hudmask_cs_ = compute(cs_hudmask_.Get());
    pso_stats_cs_ = compute(cs_stats_.Get());
    if (!pso_reduce_cs_ || !pso_stabilize_cs_ || !pso_meter_cs_ || !pso_compose_cs_ || !pso_luma_cs_ || !pso_coarse_cs_ ||
        !pso_fine_cs_ || !pso_flat_cs_ || !pso_calibrate_cs_ || !pso_hudmask_cs_ || !pso_stats_cs_)
      return Fail("pipeline");
    // Two tables, one per history parity (t3/t5 last frame's, t4/t6 this frame's), the passes' outputs,
    // then a ring of unordered-access views of the upscaler's output.
    D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kBase + kUavRingStart + kUavRing,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device_->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtv_heap_))))
      return Fail("descriptor heaps");
    rtv_bb_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
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
    // A new size: new textures and a new feature. The old ones wait a few frames (Retire): after the
    // upscaler, the game's lists of earlier frames may still use them, and there is no fence of ours.
    Retire(true);
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
    low_ = texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    enhanced_ = texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (auto& s : steady_)
      s = texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    for (auto& m : meter_)
      if (!m) m = texture(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    motion_w_ = (lw + 1) / 2;
    motion_h_ = (lh + 1) / 2;
    for (auto& t : luma_) t = texture(lw, lh, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    motion_coarse_ = texture((lw + 7) / 8, (lh + 7) / 8, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    motion_ = texture(motion_w_, motion_h_, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    flat_depth_ = texture(motion_w_, motion_h_, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    flat_done_ = false;
    if (!calib_) calib_ = texture(2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    if (!stats_) stats_ = texture(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRead);
    if (!full_ || !low_ || !enhanced_ || !steady_[0] || !steady_[1] || !meter_[0] || !meter_[1] || !luma_[0] ||
        !luma_[1] || !motion_coarse_ || !motion_ || !flat_depth_ || !calib_ || !stats_)
      return false;
    const auto uav = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
      uv.Format = f;
      uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device_->CreateUnorderedAccessView(r, nullptr, &uv, Cpu(slot));
    };
    uav(low_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUavLow);
    for (UINT i = 0; i < 2; ++i) {
      uav(steady_[i].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUavSteady + i);
      uav(meter_[i].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUavMeter + i);
      uav(luma_[i].Get(), DXGI_FORMAT_R16_FLOAT, kUavLuma + i);
    }
    uav(motion_coarse_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUavCoarse);
    uav(motion_.Get(), DXGI_FORMAT_R16G16_FLOAT, kUavMotion);
    uav(flat_depth_.Get(), DXGI_FORMAT_R32_FLOAT, kUavFlat);
    uav(calib_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, kUavCalib);
    uav(stats_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, kUavStats);
    rtv_desc_ = {};
    rtv_desc_.Format = format;
    rtv_desc_.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    return true;
  }

  void Draw(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w,
            uint32_t h) {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetGraphicsRootSignature(root_.Get());
    l->SetGraphicsRootDescriptorTable(0, Table());
    uint32_t constants[5];
    Constants(constants);
    l->SetGraphicsRoot32BitConstants(1, 5, constants, 0);
    l->SetPipelineState(pso);
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * (kBase + slot);
    return h;
  }
  // A compute pass writing the view at `slot`, over w x h in groups of `group`.
  void Dispatch(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso, UINT slot, UINT w, UINT h, UINT group) {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(root_.Get());
    l->SetComputeRootDescriptorTable(0, Table());
    uint32_t constants[5];
    Constants(constants);
    l->SetComputeRoot32BitConstants(1, 5, constants, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE out = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    out.ptr += UINT64(device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * (kBase + slot);
    l->SetComputeRootDescriptorTable(2, out);
    l->SetPipelineState(pso);
    l->Dispatch((w + group - 1) / group, (h + group - 1) / group, 1);
  }
  // This frame's table: a ring of kTableRing, since lists of earlier frames may still read theirs.
  D3D12_GPU_DESCRIPTOR_HANDLE Table() const {
    D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64(device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) *
                 (kBase + kTable * table_slot_);
    return table;
  }
  void WriteTable(DXGI_FORMAT format, const TaaInputs* taa, const Guides* masked = nullptr) {
    table_slot_ = (table_slot_ + 1) % kTableRing;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    const UINT p = parity_;
    struct View { ID3D12Resource* r; DXGI_FORMAT f; };
    const View views[kTable] = {
        {full_.Get(), format},
        {low_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {enhanced_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {steady_[p].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {steady_[1 - p].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {meter_[p].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {meter_[1 - p].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {luma_[p].Get(), DXGI_FORMAT_R16_FLOAT},
        {luma_[1 - p].Get(), DXGI_FORMAT_R16_FLOAT},
        {motion_coarse_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
        {motion_.Get(), DXGI_FORMAT_R16G16_FLOAT},
        {taa ? taa->motion : nullptr, taa ? taa->motion_format : DXGI_FORMAT_R16G16_FLOAT},
        // 1.12.1 (N2): the image before the HUD and the game's vectors, for the mask and the statistics.
        {masked ? masked->hudless : nullptr, masked ? Unorm(masked->hudless->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM},
        {masked ? masked->mvec : nullptr, masked ? VectorFormat(masked->mvec->GetDesc().Format) : DXGI_FORMAT_R16G16_FLOAT},
    };
    for (UINT i = 0; i < kTable; ++i) {
      sv.Format = views[i].f;
      device_->CreateShaderResourceView(views[i].r, &sv, Cpu(kTable * table_slot_ + i));
    }
  }
  // The pooled fit, snapped to the usual conventions of TAA motion: pixels, UV or NDC, either sign. The
  // estimate is in pixels of the motion texture; NR wants the TAA texture's own pixels.
  void Judge() {
    const double sex = pool_[0], sxx = pool_[1], seex = pool_[2], sey = pool_[4], syy = pool_[5], seey = pool_[6];
    const double sx = sex / sxx, r2x = 1.0 - (seex - sx * sex) / seex;
    const bool y_moves = seey > 0.25 * seex && syy > 0.0;
    const double sy = y_moves ? sey / syy : 0.0, r2y = y_moves ? 1.0 - (seey - sy * sey) / seey : 1.0;
    const auto md = taa_.motion->GetDesc();
    const double mw = motion_w_, mh = motion_h_, cw = double(md.Width), ch = double(md.Height);
    struct Convention { const char* name; double x, y; double nr_x, nr_y; };
    const Convention conventions[] = {
        {"pixels", mw / cw, mh / ch, 1.0, 1.0},        {"pixels, reversed", -mw / cw, -mh / ch, -1.0, -1.0},
        {"UV", mw, mh, cw, ch},                         {"UV, reversed", -mw, -mh, -cw, -ch},
        {"NDC", mw / 2, -mh / 2, cw / 2, -ch / 2},      {"NDC, reversed", -mw / 2, mh / 2, -cw / 2, ch / 2},
    };
    const Convention* best = nullptr;
    double best_error = 1e9;
    for (const auto& c : conventions) {
      double error = std::fabs(sx - c.x) / std::fabs(c.x);
      if (y_moves) error = (std::max)(error, std::fabs(sy - c.y) / std::fabs(c.y));
      if (error < best_error) { best_error = error; best = &c; }
    }
    char t[256];
    std::snprintf(t, sizeof(t), "TAA motion: fit %.2f / %.2f over %d samples, scale %.3g / %.3g, nearest %s (%.0f%% off)",
                  r2x, r2y, int(pool_[3]), sx, sy, best ? best->name : "-", best_error * 100.0);
    Log(t);
    // The conventions lie orders of magnitude apart (only the sign is close): the nearest within half
    // of its scale, with some agreement, names it; the estimate's own caution makes its scale run low.
    if (best && r2x > 0.2 && r2y > 0.15 && best_error < 0.5) {
      taa_scale_x_ = static_cast<float>(best->nr_x);
      taa_scale_y_ = static_cast<float>(best->nr_y);
      taa_proved_ = true;
      Log(std::string("the game's TAA motion agrees with the image: NR uses it (") + best->name + ")");
    } else if (pool_[3] >= 600) {
      taa_rejected_ = taa_.motion;
      Log("the candidate TAA motion does not match the image: NR keeps its estimate");
    } else {
      return;  // more samples first
    }
    for (auto& v : pool_) v = 0;
  }
  // Phase F: the sums of the fit, read back three frames later; three good fits prove the game's TAA
  // motion (and give its scale), ten bad ones reject that texture.
  void Calibrate(ID3D12GraphicsCommandList* l) {
    if (!calib_read_) calib_read_ = Buffer(512);
    if (!calib_read_) return;
    if (calib_pending_ && frame_index_ >= calib_frame_ + 3) {
      float* v = nullptr;
      D3D12_RANGE range{0, 32};
      if (SUCCEEDED(calib_read_->Map(0, &range, reinterpret_cast<void**>(&v)))) {
        const float sex = v[0], sxx = v[1], seex = v[2], n = v[3], sey = v[4], syy = v[5], seey = v[6];
        D3D12_RANGE none{0, 0};
        calib_read_->Unmap(0, &none);
        calib_pending_ = false;
        // Pooled over checks: the game's motion may be one character crossing a still view.
        const float add[7] = {sex, sxx, seex, n, sey, syy, seey};
        for (int i = 0; i < 7; ++i) pool_[i] += add[i];
        if (pool_[3] >= 120 && pool_[1] > 0 && pool_[2] > 0) Judge();
      }
    }
    if (!calib_pending_ && frame_index_ % 8 == 0) {
      Barrier(l, calib_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Dispatch(l, pso_calibrate_cs_.Get(), kUavCalib, 1, 1, 1);
      Barrier(l, calib_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{calib_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION dst{calib_read_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 1, 1, 256};
      l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      Barrier(l, calib_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
      calib_pending_ = true;
      calib_frame_ = frame_index_;
    }
  }
  // mode; nits of SDR white in HDR; history weight; meter easing; compare split.
  void Constants(uint32_t (&c)[5]) const {
    const float values[3] = {paper_, keep_, rate_};
    c[0] = static_cast<uint32_t>(mode_);
    std::memcpy(&c[1], values, sizeof(values));
    c[4] = compare_ ? 1u : 0u;
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
  // full, reduced, enhanced, steadied before/now, meter before/now, luma before/now, coarse and fine
  // estimated motion, the game's TAA motion.
  static constexpr UINT kTable = 14;
  static constexpr UINT kTableRing = 8;
  static constexpr UINT kUavLow = kTableRing * kTable, kUavSteady = kUavLow + 1, kUavMeter = kUavSteady + 2;
  static constexpr UINT kUavLuma = kUavMeter + 2, kUavCoarse = kUavLuma + 2, kUavMotion = kUavCoarse + 1;
  static constexpr UINT kUavFlat = kUavMotion + 1, kUavCalib = kUavFlat + 1;
  static constexpr UINT kUavStats = kUavCalib + 1, kUavMasked = kUavStats + 1;  // 1.12.1 (N2, N3)
  static constexpr UINT kUavRingStart = kUavMasked + 1;
  static constexpr UINT kUavRing = 8;  // views of the upscaler's output, one per frame in flight
  static constexpr UINT kBase = 0;  // where our views start in the heap
  // Our textures rest readable by compute only: the upscaler's command list can be a compute list, where
  // a pixel-shader state is invalid and the whole list is lost (the Streamline sample went black). The
  // Present route adds the pixel state around its composition.
  static constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  static constexpr D3D12_RESOURCE_STATES kPixelRead =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ComPtr<ID3DBlob> vs_, ps_compose_, cs_reduce_, cs_stabilize_, cs_meter_, cs_compose_, cs_luma_, cs_coarse_, cs_fine_,
      cs_flat_, cs_calibrate_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pso_compose_, pso_reduce_cs_, pso_stabilize_cs_, pso_meter_cs_, pso_compose_cs_,
      pso_luma_cs_, pso_coarse_cs_, pso_fine_cs_, pso_flat_cs_, pso_calibrate_cs_;
  DXGI_FORMAT compose_format_ = DXGI_FORMAT_UNKNOWN, full_format_ = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12DescriptorHeap> srv_heap_, rtv_heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_bb_{};
  ComPtr<ID3D12Resource> steady_[2], meter_[2], luma_[2], motion_coarse_, motion_, flat_depth_, calib_, calib_read_;
  uint32_t motion_w_ = 0, motion_h_ = 0;
  UINT table_slot_ = 0;
  int route_seen_ = 0;
  int motion_mode_ = 0, guides_kind_ = 0, taa_good_ = 0, taa_bad_ = 0, route_kind_ = 2;
  bool flat_done_ = false, calib_pending_ = false, taa_proved_ = false;
  uint64_t calib_frame_ = 0;
  float taa_scale_x_ = 1.f, taa_scale_y_ = 1.f;
  double pool_[7]{};
  TaaInputs taa_{};
  GameGuides game_{};  // 1.12.1 (D1)
  // 1.12.1 (N2, N3): the statistics pass's texture and its readback ring, the HUD-masked vectors.
  ComPtr<ID3D12Resource> stats_, stats_read_, masked_mv_, masked_old_;
  ComPtr<ID3DBlob> cs_hudmask_, cs_stats_;
  ComPtr<ID3D12PipelineState> pso_hudmask_cs_, pso_stats_cs_;
  uint64_t stats_written_[4]{};  // the frame each readback slot was written in (0: none)
  bool stats_cut_route_[4]{};    // whether that frame's route had no reset of the game's
  bool hud_mask_ok_ = false;
  int hud_reports_ = 0, cut_reports_ = 0;
  uint64_t last_cut_ = 0;
  std::string version_, missing_;  // 1.12.1 (D4)
  uint64_t vram_before_ = 0;       // 1.12.1 (D6)
  bool vram_measured_ = false;
  std::atomic<int> vram_mib_{0};
  bool game_on_ = false;
  ID3D12Resource* taa_candidate_ = nullptr;
  ID3D12Resource* taa_rejected_ = nullptr;
  uint64_t uav_next_ = 0;
  static constexpr uint64_t kRetireFrames = 8;
  struct Retired {
    std::vector<ComPtr<ID3D12Resource>> textures;
    void* feature = nullptr;
    uint64_t after = 0;
  };
  std::vector<Retired> retired_;
  float paper_ = 203.f;
  bool compare_ = false, stand_aside_ = true, vram_short_ = false, foreign_logged_ = false;
  int vram_margin_mb_ = 512;
  uint64_t vram_checked_ = 0, foreign_checked_ = 0;
  ComPtr<IDXGIAdapter3> adapter_;
  std::wstring foreign_;
  ComPtr<ID3D12Resource> meter_read_;
  float rate_ = 1.f;
  bool created_inverted_ = false, route_after_upscale_ = false;
  // The last restart on another device object, and whether the hold after it was logged (Prepare).
  static constexpr uint64_t kSwitchHoldMs = 5000;
  uint64_t switched_tick_ = 0;
  bool switch_held_logged_ = false;
  std::mutex lock_;
  UINT parity_ = 0;
  int stability_ = 60;
  int passes_ = 1, created_passes_ = 0, pass_cap_ = 3;
  void* extra_[2]{};                   // the features of passes 2 and 3
  ComPtr<ID3D12Resource> chain_[2];    // between the passes
  ComPtr<ID3D12Resource> Texture(uint32_t w, uint32_t h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES s) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, s, nullptr, IID_PPV_ARGS(&r));
    return r;
  }
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
  ShimShutdown shutdown_ = nullptr;
  void *create_fn_ = nullptr, *evaluate_fn_ = nullptr, *release_fn_ = nullptr, *shutdown_fn_ = nullptr;
  std::atomic<double> gpu_ms_{0.0};
  uint64_t applied_ = 0;
  ComPtr<ID3D12InfoQueue> info_queue_;
  int debug_logged_ = 0;
};

}  // namespace internal

using Runtime = internal::Runtime;

// Phase A: the game's DLSS SR and Ray Reconstruction features (addon_provider.inl's NGX hooks), and the
// call after each of their evaluations, on the game's command list (addon_dlss5.inl).
namespace upscaler {
struct Feature {
  const void* handle = nullptr;
  int feature = 0;
  int flags = 0;
};
inline std::mutex g_lock;
inline Feature g_features[16];
inline std::atomic<void (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Parameter*, int)> g_after{nullptr};
inline std::atomic<void (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Parameter*)> g_before_fg{nullptr};
inline bool Upscaler(int feature) { return feature == 1 /* SuperSampling */ || feature == 13 /* RayReconstruction */; }
// Every feature the game creates, with its kind: frame generation's evaluations carry the same
// parameter names (Output, MotionVectors, Depth) and must not be taken for an upscaler's.
inline void Created(int feature, const NVSDK_NGX_Parameter* params, const void* handle) {
  if (handle == nullptr) return;
  int flags = 0;
  if (params) params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
  std::lock_guard guard(g_lock);
  for (auto& f : g_features)
    if (f.handle == nullptr || f.handle == handle) {
      f = {handle, feature, flags};
      return;
    }
}
inline void Released(const void* handle) {
  std::lock_guard guard(g_lock);
  for (auto& f : g_features)
    if (f.handle == handle) f = {};
}
// Before the game's frame generation (NVIDIA's or XeMFG DLSS-G) evaluates: its real frame, with its
// motion vectors and depth, before it is interpolated and presented. Games with frame generation and no
// DLSS SR (The Last of Us Part II with FSR) reach NR here.
inline void BeforeEvaluate(ID3D12GraphicsCommandList* list, const void* handle, const NVSDK_NGX_Parameter* params) {
  const auto before = g_before_fg.load(std::memory_order_acquire);
  if (before == nullptr || list == nullptr || params == nullptr) return;
  bool known = false, fg = false;
  {
    std::lock_guard guard(g_lock);
    for (const auto& f : g_features)
      if (f.handle == handle) {
        known = true;
        fg = f.feature == 11;  // FrameGeneration
      }
  }
  if (!known) {  // created before the hooks, or XeMFG's own handle
    ID3D12Resource* backbuffer = nullptr;
    params->Get("DLSSG.Backbuffer", &backbuffer);
    fg = backbuffer != nullptr;
  }
  if (fg) before(list, params);
}
// What the first evaluations were, once each kind, for a tester's log.
inline void Note(const char* what, int feature, int flags) {
  static std::atomic<int> seen{0};
  const int bit = what[0] == 'k' ? 1 : what[0] == 'u' ? 2 : what[0] == 'o' ? 4 : 8;
  if (seen.fetch_or(bit) & bit) return;
  char t[160];
  std::snprintf(t, sizeof(t), "mfgunlock: DLSS 5: NGX evaluation seen: %s (feature %d, flags 0x%x)", what, feature, flags);
  reshade::log::message(reshade::log::level::info, t);
}
inline void Evaluated(ID3D12GraphicsCommandList* list, const void* handle, const NVSDK_NGX_Parameter* params) {
  const auto after = g_after.load(std::memory_order_acquire);
  if (after == nullptr || list == nullptr || params == nullptr) return;
  int flags = -1;
  {
    std::lock_guard guard(g_lock);
    for (const auto& f : g_features)
      if (f.handle == handle) {
        if (!Upscaler(f.feature)) {
          Note("other feature", f.feature, f.flags);
          return;  // frame generation, or another feature
        }
        flags = f.flags;
        Note("known upscaler", f.feature, f.flags);
      }
  }
  // Created before the hooks went in: an upscaler is a call with an output, vectors and depth, and
  // no frame generation parameters. The vectors' size tells their resolution.
  if (flags < 0) {
    ID3D12Resource* fg = nullptr;
    params->Get("DLSSG.Backbuffer", &fg);
    if (fg != nullptr) return;
    Note("upscaler created before the hooks", -1, 0);
    ID3D12Resource *out = nullptr, *mv = nullptr, *depth = nullptr;
    params->Get(NVSDK_NGX_Parameter_Output, &out);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, &mv);
    params->Get(NVSDK_NGX_Parameter_Depth, &depth);
    if (!out || !mv || !depth) return;
    flags = mv->GetDesc().Width < out->GetDesc().Width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
  }
  after(list, params, flags);
}
}  // namespace upscaler

}  // namespace mfgunlock::nr
