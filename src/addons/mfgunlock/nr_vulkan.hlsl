// SPDX-License-Identifier: MIT
// 1.12.0 (DLSS 5, phase E): neural rendering's passes on Vulkan, compiled to SPIR-V at build time
// (dxc -spirv). The same image path as nr_runtime.hpp's D3D12 shaders: the image reduced and encoded for
// NR, then NR's change added back at full resolution, with HDR mapped there and back.
struct Push { uint mode; float paper; uint split; uint pad; };
[[vk::push_constant]] Push pc;
[[vk::binding(0, 0)]] Texture2D<float4> full;
[[vk::binding(1, 0)]] Texture2D<float4> reduced;
[[vk::binding(2, 0)]] Texture2D<float4> enhanced;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> written;
[[vk::binding(4, 0)]] SamplerState linear_clamp;

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
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
float3 SrgbEncode(float3 x) { return select(x <= 0.0031308, 12.92 * x, 1.055 * pow(x, 1.0 / 2.4) - 0.055); }
float3 SrgbDecode(float3 x) { return select(x <= 0.04045, x / 12.92, pow((x + 0.055) / 1.055, 2.4)); }
// Modes: 0 SDR, 1 HDR10 (PQ), 2 scRGB, 4 SDR in an sRGB format (the blit decoded it to linear).
float3 ToNits(float3 c) { return pc.mode == 1 ? PqToNits(c) : c * 80.0; }
float3 FromNits(float3 l) { return pc.mode == 1 ? NitsToPq(l) : l / 80.0; }
float3 Encode(float3 c) {
  if (pc.mode == 0) return c;
  if (pc.mode == 4) return SrgbEncode(saturate(c));
  float3 l = max(ToNits(c), 0.0);
  return SrgbEncode(l / (l + pc.paper));
}
float3 Changed(float3 c, float3 change) {
  if (pc.mode == 0) return saturate(c + change);
  if (pc.mode == 4) return SrgbDecode(saturate(SrgbEncode(saturate(c)) + change));
  float3 l = max(ToNits(c), 0.0);
  change *= saturate(1.5 - 0.5 * Luma(l) / pc.paper);
  float3 t = min(SrgbDecode(saturate(SrgbEncode(l / (l + pc.paper)) + change)), 0.999);
  float3 n = pc.paper * t / (1.0 - t);
  return FromNits(clamp(n, l * 0.25, l * 4.0 + 0.1 * pc.paper));
}
[numthreads(8, 8, 1)] void reduce_vk(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float4 c = full.SampleLevel(linear_clamp, (float2(id.xy) + 0.5) / float2(w, h), 0);
  written[id.xy] = float4(Encode(c.rgb), c.a);
}
[numthreads(8, 8, 1)] void compose_vk(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  written.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float4 f = full.Load(int3(id.xy, 0));
  if (pc.split != 0) {
    if (abs(int(id.x) - int(w / 2)) < 1) { written[id.xy] = float4(0.85, 0.85, 0.2, 1); return; }
    if (id.x < w / 2) { written[id.xy] = f; return; }
  }
  float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
  float3 change = enhanced.SampleLevel(linear_clamp, uv, 0).rgb - reduced.SampleLevel(linear_clamp, uv, 0).rgb;
  written[id.xy] = float4(Changed(f.rgb, change), f.a);
}
