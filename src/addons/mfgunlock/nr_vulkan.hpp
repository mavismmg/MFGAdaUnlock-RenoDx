// SPDX-License-Identifier: MIT
#pragma once
// 1.12.0 (DLSS 5, phase E): neural rendering in Vulkan games, before Present.
//
// The same image path as the D3D12 runtime (nr_runtime.hpp), recorded on ReShade's immediate command list
// in its present event, so ReShade submits it after the game's frame with the game's semaphores:
// the back buffer blitted to an RGBA16F copy, reduced and encoded for NR (compute), NR, NR's change added
// back at full resolution (compute) and blitted to the back buffer. The runtime's Vulkan path needs device
// extensions the game may not enable: [DLSS5] Enabled=1 at load hooks vkCreateDevice and adds the ones NR
// asks for (NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements); a device made before that may lack
// them, and the page then asks for a restart.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <include/reshade.hpp>

#include "nr_vk_bridge.hpp"

#ifdef MFGUNLOCK_NR_VULKAN
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_defs_vk.h>
#include "nr_vulkan_spv.h"
#endif

namespace mfgunlock::nr::vk {
inline std::atomic<int> g_passes{1};  // NR passes (Passes setting, 1 to 3)

// Where the runtime and the shim are (dlss5's Locate); set by addon_dlss5.inl.
inline std::atomic<bool (*)(std::wstring&, std::wstring&)> g_locate{nullptr};

#ifndef MFGUNLOCK_NR_VULKAN
inline void Hook() {}
inline bool NeedsRestart() { return false; }
inline void Present(reshade::api::command_queue*, reshade::api::resource, reshade::api::resource_usage, const Look&, int,
                    Encoding, float, int) {}
#else

namespace internal {
inline void Log(const std::string& text) {
  reshade::log::message(reshade::log::level::info, ("mfgunlock: DLSS 5 (Vulkan): " + text).c_str());
}
inline PFN_vkGetInstanceProcAddr g_gipa = nullptr;
inline PFN_vkCreateDevice g_real_create_device = nullptr;
inline PFN_vkEnumeratePhysicalDevices g_real_enumerate = nullptr;
inline std::atomic<VkInstance> g_instance{VK_NULL_HANDLE};
inline std::atomic<VkPhysicalDevice> g_physical{VK_NULL_HANDLE};
inline std::atomic<VkDevice> g_device{VK_NULL_HANDLE};
inline std::atomic_bool g_hooked{false};

// The runtime and the shim, loaded for the extension query and for NR itself.
struct Modules {
  HMODULE shim = nullptr, nr = nullptr;
  bool Load() {
    if (shim && nr) return true;
    const auto locate = g_locate.load();
    std::wstring runtime, shim_path;
    if (!locate || !locate(runtime, shim_path)) return false;
    shim = LoadLibraryW(shim_path.c_str());
    nr = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return shim && nr;
  }
};
inline Modules g_modules;

// The device extensions NR asks for on this GPU (or the usual NGX ones when it does not say).
inline std::vector<std::string> Required(VkInstance instance, VkPhysicalDevice physical) {
  std::vector<std::string> out;
  if (g_modules.Load()) {
    using QueryFn = int(__cdecl*)(void*, void*, void*, const void*, unsigned*, void**);
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(g_modules.shim, "MfgNr_VkDeviceExtensions"));
    void* fn = GetProcAddress(g_modules.nr, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = static_cast<NVSDK_NGX_Version>(0x15);
    info.FeatureID = static_cast<NVSDK_NGX_Feature>(18);
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
    info.Identifier.v.ApplicationId = 0x1000000ull;
    info.ApplicationDataPath = temp;
    unsigned count = 0;
    VkExtensionProperties* properties = nullptr;
    if (query && fn && query(fn, instance, physical, &info, &count, reinterpret_cast<void**>(&properties)) == 1 && properties)
      for (unsigned i = 0; i < count; ++i) out.emplace_back(properties[i].extensionName);
  }
  if (out.empty()) out = {"VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor"};
  return out;
}

inline VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* devices) {
  g_instance.store(instance);
  t_forwarding_game_call = true;
  const VkResult result = g_real_enumerate(instance, count, devices);
  t_forwarding_game_call = false;
  return result;
}
// The game's device, with NR's extensions added where the GPU has them.
inline VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                        const VkAllocationCallbacks* allocator, VkDevice* device) {
  const VkInstance instance = g_instance.load();
  std::vector<const char*> names(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
  std::vector<std::string> added;
  if (instance != VK_NULL_HANDLE && g_gipa) {
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        g_gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    uint32_t n = 0;
    std::vector<VkExtensionProperties> have;
    if (enumerate && enumerate(physical, nullptr, &n, nullptr) == VK_SUCCESS) {
      have.resize(n);
      enumerate(physical, nullptr, &n, have.data());
    }
    for (const auto& want : Required(instance, physical)) {
      bool supported = false, present = false;
      for (const auto& h : have) supported |= want == h.extensionName;
      for (const char* name : names) present |= want == name;
      if (supported && !present) added.push_back(want);
    }
  }
  for (const auto& a : added) names.push_back(a.c_str());
  VkDeviceCreateInfo edited = *info;
  edited.enabledExtensionCount = static_cast<uint32_t>(names.size());
  edited.ppEnabledExtensionNames = names.data();
  // Forwarded as the game's call: XeMFG's own hook may come next in this add-on.
  t_forwarding_game_call = true;
  VkResult result = g_real_create_device(physical, &edited, allocator, device);
  if (result != VK_SUCCESS && !added.empty()) result = g_real_create_device(physical, info, allocator, device);
  t_forwarding_game_call = false;
  if (result == VK_SUCCESS) {
    g_physical.store(physical);
    g_device.store(*device);
    std::string list;
    for (const auto& a : added) list += (list.empty() ? "" : ", ") + a;
    Log("device created" + (list.empty() ? std::string(" (NR's extensions were already there)") : " with " + list));
  }
  return result;
}
}  // namespace internal

// At load with [DLSS5] Enabled=1, in a Vulkan game: the device's creation, watched.
inline void Hook() {
  using namespace internal;
  if (g_hooked.exchange(true)) return;
  const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  if (!loader) return;
  g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
  g_real_create_device = reinterpret_cast<PFN_vkCreateDevice>(GetProcAddress(loader, "vkCreateDevice"));
  g_real_enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(loader, "vkEnumeratePhysicalDevices"));
  if (g_real_create_device)
    mfgunlock::hook::InstallOne(reinterpret_cast<void**>(&g_real_create_device), reinterpret_cast<void*>(&CreateDevice));
  if (g_real_enumerate)
    mfgunlock::hook::InstallOne(reinterpret_cast<void**>(&g_real_enumerate), reinterpret_cast<void*>(&EnumeratePhysicalDevices));
}

namespace internal {
// Everything of MFG Unlock's own is made and recorded through ReShade's API: ReShade does not tell other
// add-ons about those, and RenoDX's shader add-ons (renodx-rdr2vk in RDR2) crashed on NR's pipeline
// layout when it went through the Vulkan layer. Only NR itself gets the native handles.
class Runtime {
 public:
  bool failed() const { return failed_; }
  bool needs_restart() const { return needs_restart_; }

  // `bb` is in `state` before and after (render target in ReShade's begin_effects event).
  void Apply(reshade::api::command_queue* queue, reshade::api::resource bb, reshade::api::resource_usage state,
             const Look& look, int scale, Encoding encoding, float paper, bool compare) {
    using namespace reshade::api;
    if (failed_) return;
    // A fault inside goes to the log with its stack (nr_runtime.hpp's handler).
    static const bool handler = [] {
      AddVectoredExceptionHandler(1, &mfgunlock::nr::internal::ApplyFault);
      return true;
    }();
    (void)handler;
    struct Scope {
      Scope() { mfgunlock::nr::internal::t_in_apply = true; }
      ~Scope() { mfgunlock::nr::internal::t_in_apply = false; }
    } scope;
    device* dev = queue->get_device();
    const VkDevice vk_device = reinterpret_cast<VkDevice>(dev->get_native());
    if (!dev_ && !Start(dev, vk_device)) return;
    if (dev != dev_) return;
    const auto desc = dev->get_resource_desc(bb);
    const format f = desc.texture.format;
    uint32_t mode = 0;
    if (f == format::r8g8b8a8_unorm_srgb || f == format::b8g8r8a8_unorm_srgb) mode = 4;
    else if (encoding == Encoding::kPq && (f == format::r10g10b10a2_unorm || f == format::b10g10r10a2_unorm)) mode = 1;
    else if (f == format::r16g16b16a16_float) mode = 2;
    const uint32_t w = desc.texture.width, h = desc.texture.height;
    const uint32_t lw = (w * static_cast<uint32_t>(scale) + 50) / 100, lh = (h * static_cast<uint32_t>(scale) + 50) / 100;
    ++frame_;
    Bury();
    if (!Resources(w, h, lw, lh)) return;
    // No GPU timestamps here: reading them back crashed NVIDIA's driver after the swap chain was made again
    // (with renodx-rdr2vk resizing it); the panel shows no cost for Vulkan.
    Record(queue->get_immediate_command_list(), bb, state, look, mode, paper, compare);
  }

 private:
  struct Image {
    reshade::api::resource res{0};
    reshade::api::resource_view view{0};  // unordered access (storage)
    reshade::api::resource_view srv{0};   // shader resource (sampled)
    uint32_t width = 0, height = 0;
  };
  bool Fail(const char* what) {
    Log(std::string("NR stopped for this session: ") + what);
    failed_ = true;
    mfgunlock::nr::g_status.route.store(0, std::memory_order_relaxed);
    return false;
  }

  bool Start(reshade::api::device* dev, VkDevice device) {
    const VkInstance instance = g_instance.load();
    const VkPhysicalDevice physical = g_physical.load();
    if (instance == VK_NULL_HANDLE || physical == VK_NULL_HANDLE || g_device.load() != device || !g_gipa) {
      needs_restart_ = true;
      return Fail("the game's Vulkan device was made before DLSS 5 was on (restart the game)");
    }
    if (!g_modules.Load()) return Fail("nvngx_dlssnr.dll or the shim did not load");
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_gipa(instance, "vkGetDeviceProcAddr"));
    using InitFn = int(__cdecl*)(void*, unsigned long long, const wchar_t*, void*, void*, void*, void*, void*, int, const void*);
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(g_modules.shim, "MfgNr_VkInit"));
    create_ = reinterpret_cast<CreateFn>(GetProcAddress(g_modules.shim, "MfgNr_Create"));
    evaluate_ = reinterpret_cast<EvaluateFn>(GetProcAddress(g_modules.shim, "MfgNr_Evaluate"));
    release_ = reinterpret_cast<ReleaseFn>(GetProcAddress(g_modules.shim, "MfgNr_Release"));
    void* init_fn = GetProcAddress(g_modules.nr, "NVSDK_NGX_VULKAN_Init_Ext2");
    create_fn_ = GetProcAddress(g_modules.nr, "NVSDK_NGX_VULKAN_CreateFeature");
    evaluate_fn_ = GetProcAddress(g_modules.nr, "NVSDK_NGX_VULKAN_EvaluateFeature");
    release_fn_ = GetProcAddress(g_modules.nr, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!gdpa || !init || !create_ || !evaluate_ || !release_ || !init_fn || !create_fn_ || !evaluate_fn_ || !release_fn_)
      return Fail("missing exports (the shim and the runtime must be of this version)");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    // NR's own objects go straight to NVIDIA's driver, past the Vulkan layers: through ReShade's layer
    // other add-ons see and change them (renodx-rdr2vk rewrote NR's pipeline layouts and the game crashed).
    PFN_vkGetDeviceProcAddr nr_gdpa = gdpa;
    if (const HMODULE icd = GetModuleHandleW(L"nvoglv64.dll")) {
      using IcdGipa = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);
      const auto icd_gipa = reinterpret_cast<IcdGipa>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
      auto direct = icd_gipa ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(icd_gipa(VK_NULL_HANDLE, "vkGetDeviceProcAddr")) : nullptr;
      const auto by_instance = icd_gipa ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(icd_gipa(instance, "vkGetDeviceProcAddr")) : nullptr;
      const auto exported = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(icd, "vkGetDeviceProcAddr"));
      if (!direct) direct = by_instance;
      if (!direct) direct = exported;
      if (direct && direct(device, "vkCreatePipelineLayout")) {
        nr_gdpa = direct;
        Log("NR's objects go to the driver directly, past the Vulkan layers");
      }
    }
    const int r = init(init_fn, 0x1000000ull, temp, instance, physical, device, reinterpret_cast<void*>(g_gipa),
                       reinterpret_cast<void*>(nr_gdpa), 0x15, nullptr);
    if (r != 1) {
      char t[96];
      std::snprintf(t, sizeof(t), "the runtime refused this device (Init 0x%08x)", static_cast<unsigned>(r));
      needs_restart_ = true;  // most often: the device lacks NR's extensions
      return Fail(t);
    }
    dev_ = dev;
    return Pipeline();
  }

  bool Pipeline() {
    using namespace reshade::api;
    sampler_desc sd{};
    sd.filter = filter_mode::min_mag_mip_linear;
    sd.address_u = sd.address_v = sd.address_w = texture_address_mode::clamp;
    if (!dev_->create_sampler(sd, &sampler_)) return Fail("sampler");
    descriptor_range ranges[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
      ranges[i].binding = i;
      ranges[i].dx_register_index = i < 3 ? i : 0;
      ranges[i].count = 1;
      ranges[i].array_size = 1;
      ranges[i].visibility = shader_stage::all_compute;
      ranges[i].type = i < 3 ? descriptor_type::shader_resource_view
                             : i == 3 ? descriptor_type::unordered_access_view : descriptor_type::sampler;
    }
    constant_range push{};
    push.count = 4;
    push.visibility = shader_stage::all_compute;
    const pipeline_layout_param params[2] = {pipeline_layout_param(5, ranges), pipeline_layout_param(push)};
    if (!dev_->create_pipeline_layout(2, params, &layout_)) return Fail("pipeline layout");
    const auto pipeline = [&](const uint32_t* code, size_t bytes, const char* entry, reshade::api::pipeline& out) {
      shader_desc cs{};
      cs.code = code;
      cs.code_size = bytes;
      cs.entry_point = entry;
      pipeline_subobject sub{pipeline_subobject_type::compute_shader, 1, &cs};
      return dev_->create_pipeline(layout_, 1, &sub, &out);
    };
    if (!pipeline(kNrReduceVk, sizeof(kNrReduceVk), "reduce_vk", reduce_) ||
        !pipeline(kNrComposeVk, sizeof(kNrComposeVk), "compose_vk", compose_))
      return Fail("pipelines");
    Log("NR runtime started");
    return true;
  }

  bool Allocate(Image& img, uint32_t w, uint32_t h, reshade::api::resource_usage initial) {
    using namespace reshade::api;
    const resource_desc d(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::gpu_only,
                          resource_usage::shader_resource | resource_usage::unordered_access | resource_usage::copy_source |
                              resource_usage::copy_dest);
    if (!dev_->create_resource(d, nullptr, initial, &img.res)) return false;
    img.width = w;
    img.height = h;
    return dev_->create_resource_view(img.res, resource_usage::unordered_access,
                                      resource_view_desc(format::r16g16b16a16_float), &img.view) &&
           dev_->create_resource_view(img.res, resource_usage::shader_resource,
                                      resource_view_desc(format::r16g16b16a16_float), &img.srv);
  }
  void Free(Image& img) {
    if (img.view.handle) dev_->destroy_resource_view(img.view);
    if (img.srv.handle) dev_->destroy_resource_view(img.srv);
    if (img.res.handle) dev_->destroy_resource(img.res);
    img = {};
  }

  // A new size: new images (the old ones freed eight frames later) and a new feature.
  bool Resources(uint32_t w, uint32_t h, uint32_t lw, uint32_t lh) {
    using namespace reshade::api;
    if (full_.res.handle && full_.width == w && full_.height == h && low_.width == lw && low_.height == lh) return true;
    if (full_.res.handle) {
      Retired r;
      r.images = {full_, low_, enhanced_, out_};
      for (Image& c : chain_)
        if (c.res.handle) r.images.push_back(c);
      r.feature = feature_;
      r.tables[0] = tables_[0];
      r.tables[1] = tables_[1];
      r.after = frame_ + 8;
      retired_.push_back(r);
      RetireExtras();
    }
    for (Image& c : chain_) c = {};
    // New tables per size: a table a pending command buffer still uses is not written again.
    if (!dev_->allocate_descriptor_tables(2, layout_, 0, tables_)) return Fail("descriptor tables");
    full_ = low_ = enhanced_ = out_ = {};
    feature_ = nullptr;
    if (!Allocate(full_, w, h, resource_usage::copy_dest) || !Allocate(low_, lw, lh, resource_usage::unordered_access) ||
        !Allocate(enhanced_, lw, lh, resource_usage::unordered_access) || !Allocate(out_, w, h, resource_usage::unordered_access))
      return Fail("images");
    reset_ = true;
    for (int s = 0; s < 2; ++s) {
      const resource_view views[4] = {full_.srv, low_.srv, enhanced_.srv, s == 0 ? low_.view : out_.view};
      descriptor_table_update updates[3]{};
      updates[0] = {tables_[s], 0, 0, 3, descriptor_type::shader_resource_view, views};
      updates[1] = {tables_[s], 3, 0, 1, descriptor_type::unordered_access_view, &views[3]};
      updates[2] = {tables_[s], 4, 0, 1, descriptor_type::sampler, &sampler_};
      dev_->update_descriptor_tables(3, updates);
    }
    return true;
  }
  void Bury() {
    for (size_t i = 0; i < retired_.size();)
      if (retired_[i].after <= frame_) {
        for (auto& img : retired_[i].images) Free(img);
        if (retired_[i].tables[0].handle) dev_->free_descriptor_tables(2, retired_[i].tables);
        if (retired_[i].feature) release_(release_fn_, retired_[i].feature);
        retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
  }
  static NVSDK_NGX_Resource_VK Resource(const Image& img, bool write) {
    NVSDK_NGX_Resource_VK r{};
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    r.ReadWrite = write;
    r.Resource.ImageViewInfo.ImageView = reinterpret_cast<VkImageView>((write ? img.view : img.srv).handle);
    r.Resource.ImageViewInfo.Image = reinterpret_cast<VkImage>(img.res.handle);
    r.Resource.ImageViewInfo.SubresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r.Resource.ImageViewInfo.Format = VK_FORMAT_R16G16B16A16_SFLOAT;
    r.Resource.ImageViewInfo.Width = img.width;
    r.Resource.ImageViewInfo.Height = img.height;
    return r;
  }
  void Dispatch(reshade::api::command_list* cmd, reshade::api::pipeline pipeline, reshade::api::descriptor_table table,
                uint32_t w, uint32_t h, const uint32_t (&push)[4]) {
    using namespace reshade::api;
    cmd->bind_pipeline(pipeline_stage::all_compute, pipeline);
    cmd->bind_descriptor_table(shader_stage::all_compute, layout_, 0, table);
    cmd->push_constants(shader_stage::all_compute, layout_, 1, 0, 4, push);
    cmd->dispatch((w + 7) / 8, (h + 7) / 8, 1);
  }

  void Record(reshade::api::command_list* cmd, reshade::api::resource bb, reshade::api::resource_usage state,
              const Look& look, uint32_t mode, float paper, bool compare) {
    using namespace reshade::api;
    using U = resource_usage;
    const VkCommandBuffer native = reinterpret_cast<VkCommandBuffer>(cmd->get_native());
    const int passes = (std::clamp)(g_passes.load(std::memory_order_relaxed), 1, pass_cap_);
    if (!(look == look_) || !feature_ || passes != created_passes_) {
      if (feature_) {
        Retired r;
        r.feature = feature_;
        r.after = frame_ + 8;
        retired_.push_back(r);
        feature_ = nullptr;
      }
      RetireExtras();
      look_ = look;
      reset_ = true;
      params_.Set("DLSSNR.Enabled", 1);
      params_.Set("DLSSNR.Width", low_.width);
      params_.Set("DLSSNR.Height", low_.height);
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
      const int r = create_(create_fn_, native, 18, &params_, &feature_);
      if (r != 1 || !feature_) {
        char t[64];
        std::snprintf(t, sizeof(t), "CreateFeature 0x%08x", static_cast<unsigned>(r));
        Fail(t);
        return;
      }
      // The later passes: their own features (history) and the images between them.
      created_passes_ = 1;
      for (int i = 0; i + 1 < passes; ++i) {
        if (!chain_[i].res.handle && !Allocate(chain_[i], low_.width, low_.height, resource_usage::unordered_access)) break;
        if (create_(create_fn_, native, 18, &params_, &extra_[i]) != 1 || !extra_[i]) {
          extra_[i] = nullptr;
          break;
        }
        created_passes_ = i + 2;
      }
      if (created_passes_ < passes) {
        pass_cap_ = created_passes_;  // not again each frame
        Log("a later NR pass could not be created: fewer passes");
      }
      char t[96];
      std::snprintf(t, sizeof(t), "NR feature created at %ux%u, %d pass%s", low_.width, low_.height, created_passes_,
                    created_passes_ > 1 ? "es" : "");
      Log(t);
    }
    float paper_value = paper;
    uint32_t push[4] = {mode, 0, compare ? 1u : 0u, 0};
    std::memcpy(&push[1], &paper_value, sizeof(paper_value));
    // The back buffer into the copy.
    cmd->barrier(bb, state, U::copy_source);
    cmd->copy_texture_region(bb, 0, nullptr, full_.res, 0, nullptr, filter_mode::min_mag_mip_point);
    cmd->barrier(full_.res, U::copy_dest, U::shader_resource);
    // Reduce and encode; NR on the reduced image (all in the general layout for NR).
    Dispatch(cmd, reduce_, tables_[0], low_.width, low_.height, push);
    cmd->barrier(low_.res, U::unordered_access, U::general);
    cmd->barrier(enhanced_.res, U::unordered_access, U::general);
    for (int i = 0; i + 1 < created_passes_; ++i) cmd->barrier(chain_[i].res, U::unordered_access, U::general);
    params_.Set("DLSSNR.Color", static_cast<void*>(&color_));
    params_.Set("DLSSNR.Output", static_cast<void*>(&output_));
    params_.Set("DLSSNR.Width", low_.width);
    params_.Set("DLSSNR.Height", low_.height);
    params_.Set("DLSSNR.Enabled", 1);
    params_.Set("DLSSNR.Reset", reset_ ? 1 : 0);
    params_.Set("DLSSNR.MVecScaleX", 1.f);
    params_.Set("DLSSNR.MVecScaleY", 1.f);
    for (const char* base : {"DLSSNR.Color", "DLSSNR.Output"}) {
      const std::string b = base;
      params_.Set((b + "SubrectBaseX").c_str(), 0u);
      params_.Set((b + "SubrectBaseY").c_str(), 0u);
      params_.Set((b + "SubrectWidth").c_str(), low_.width);
      params_.Set((b + "SubrectHeight").c_str(), low_.height);
    }
    reset_ = false;
    // Each later pass on the output of the one before.
    for (int i = 0; i < created_passes_; ++i) {
      const Image& in = i == 0 ? low_ : chain_[i - 1];
      const Image& out = i + 1 == created_passes_ ? enhanced_ : chain_[i];
      color_ = Resource(in, false);
      output_ = Resource(out, true);
      const int r = evaluate_(evaluate_fn_, native, i == 0 ? feature_ : extra_[i - 1], &params_, nullptr);
      if (r != 1) {
        char t[64];
        std::snprintf(t, sizeof(t), "EvaluateFeature 0x%08x", static_cast<unsigned>(r));
        cmd->barrier(bb, U::copy_source, state);
        Fail(t);
        return;
      }
      if (i + 1 < created_passes_) {  // written, then read by the next pass
        cmd->barrier(out.res, U::general, U::unordered_access);
        cmd->barrier(out.res, U::unordered_access, U::general);
      }
    }
    // Compose (reads the reduced image and NR's, writes the output).
    cmd->barrier(low_.res, U::general, U::unordered_access);
    cmd->barrier(enhanced_.res, U::general, U::unordered_access);
    for (int i = 0; i + 1 < created_passes_; ++i) cmd->barrier(chain_[i].res, U::general, U::unordered_access);
    Dispatch(cmd, compose_, tables_[1], out_.width, out_.height, push);
    // The result into the back buffer.
    cmd->barrier(out_.res, U::unordered_access, U::copy_source);
    Probe(cmd);
    cmd->barrier(bb, U::copy_source, U::copy_dest);
    cmd->copy_texture_region(out_.res, 0, nullptr, bb, 0, nullptr, filter_mode::min_mag_mip_point);
    cmd->barrier(bb, U::copy_dest, state);
    cmd->barrier(out_.res, U::copy_source, U::unordered_access);
    cmd->barrier(full_.res, U::shader_resource, U::copy_dest);
    auto& st = mfgunlock::nr::g_status;
    st.route.store(3, std::memory_order_relaxed);
    st.guides.store(0, std::memory_order_relaxed);
    st.ms.store(static_cast<float>(gpu_ms_), std::memory_order_relaxed);
    st.width.store(low_.width, std::memory_order_relaxed);
    st.height.store(low_.height, std::memory_order_relaxed);
    if (++applied_ == 1) Log("NR running on the game's frames");
    if (applied_ % 600 == 0) {
      char t[96];
      std::snprintf(t, sizeof(t), "NR %ux%u (%llu frames)", low_.width, low_.height, static_cast<unsigned long long>(applied_));
      Log(t);
    }
  }

  // Once, at frame 120: the centre pixel of each stage (copy, reduced, NR's output, composed) into the log,
  // read back four frames later; it tells a tester's log where a black image comes from.
  void Probe(reshade::api::command_list* cmd) {
    using namespace reshade::api;
    using U = resource_usage;
    if (applied_ == 124 && probe_.handle) {
      void* data = nullptr;
      if (dev_->map_buffer_region(probe_, 0, 4 * kRow, map_access::read_only, &data) && data) {
        const auto half = [](uint16_t h) {
          const uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
          const float f = e == 0 ? m / 1024.f / 16384.f : e == 31 ? 65504.f : (1.f + m / 1024.f) * std::ldexp(1.f, int(e) - 15);
          return sign ? -f : f;
        };
        static const char* kNames[] = {"copy", "reduced", "NR", "composed"};
        const Image* images[4] = {&full_, &low_, &enhanced_, &out_};
        std::string text = "brightest pixel of a row at 2/3 height:";
        for (int i = 0; i < 4; ++i) {
          const auto* v = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(data) + i * kRow);
          float best = 0.f;
          for (uint32_t x = 0; x < images[i]->width && x < kRow / 8; ++x)
            best = (std::max)(best, half(v[x * 4]) + half(v[x * 4 + 1]) + half(v[x * 4 + 2]));
          char t[64];
          std::snprintf(t, sizeof(t), " %s %.3f;", kNames[i], best);
          text += t;
        }
        dev_->unmap_buffer_region(probe_);
        Log(text);
      }
    }
    if (applied_ != 120) return;
    if (!probe_.handle &&
        !dev_->create_resource(resource_desc(4 * kRow, memory_heap::gpu_to_cpu, U::copy_dest), nullptr, U::copy_dest, &probe_))
      return;
    const Image* images[4] = {&full_, &low_, &enhanced_, &out_};
    const U states[4] = {U::shader_resource, U::unordered_access, U::unordered_access, U::copy_source};
    for (int i = 0; i < 4; ++i) {
      const Image& img = *images[i];
      const uint32_t py = img.height * 2 / 3, w = (std::min)(img.width, kRow / 8);
      const subresource_box box{0, py, 0, w, py + 1, 1};
      if (states[i] != U::copy_source) cmd->barrier(img.res, states[i], U::copy_source);
      cmd->copy_texture_to_buffer(img.res, 0, &box, probe_, uint64_t(i) * kRow);
      if (states[i] != U::copy_source) cmd->barrier(img.res, U::copy_source, states[i]);
    }
  }
  reshade::api::resource probe_{0};
  static constexpr uint32_t kRow = 65536;  // bytes per probed row (8192 RGBA16F pixels)

  using CreateFn = int(__cdecl*)(void*, void*, int, void*, void**);
  using EvaluateFn = int(__cdecl*)(void*, void*, const void*, const void*, void*);
  using ReleaseFn = int(__cdecl*)(void*, void*);
  static constexpr uint32_t kRing = 4;
  struct Retired {
    std::vector<Image> images;
    void* feature = nullptr;
    reshade::api::descriptor_table tables[2]{};
    uint64_t after = 0;
  };
  reshade::api::device* dev_ = nullptr;
  reshade::api::sampler sampler_{0};
  reshade::api::pipeline_layout layout_{0};
  reshade::api::pipeline reduce_{0}, compose_{0};
  reshade::api::descriptor_table tables_[2]{};
  reshade::api::query_heap queries_{0};
  uint64_t ticks_ = 1;
  double gpu_ms_ = 0.0;
  Image full_, low_, enhanced_, out_;
  Image chain_[2];             // between the passes
  void* extra_[2]{};           // the features of passes 2 and 3
  int created_passes_ = 0, pass_cap_ = 3;
  void RetireExtras() {
    for (void*& extra : extra_) {
      if (!extra) continue;
      Retired r;
      r.feature = extra;
      r.after = frame_ + 8;
      retired_.push_back(r);
      extra = nullptr;
    }
    created_passes_ = 0;
  }
  std::vector<Retired> retired_;
  Parameters params_;
  NVSDK_NGX_Resource_VK color_{}, output_{};
  Look look_{};
  void* feature_ = nullptr;
  CreateFn create_ = nullptr;
  EvaluateFn evaluate_ = nullptr;
  ReleaseFn release_ = nullptr;
  void *create_fn_ = nullptr, *evaluate_fn_ = nullptr, *release_fn_ = nullptr;
  uint64_t frame_ = 0, applied_ = 0;
  bool failed_ = false, needs_restart_ = false, reset_ = true;
};
inline Runtime g_runtime;
}  // namespace internal
inline void Passes(int n) { g_passes.store(n, std::memory_order_relaxed); }

inline bool NeedsRestart() { return internal::g_runtime.needs_restart(); }
// In a Vulkan game, from ReShade's begin_effects event: after every add-on's Present (an add-on that writes
// the back buffer there, renodx-rdr2vk's HDR, is done) and before ReShade's effects, with the back buffer
// in the state ReShade put it in.
inline void Present(reshade::api::command_queue* queue, reshade::api::resource bb, reshade::api::resource_usage state,
                    const Look& look, int scale, Encoding encoding, float paper, int /*stability: D3D12 only*/) {
  if (queue == nullptr || bb.handle == 0) return;
  internal::g_runtime.Apply(queue, bb, state, look, scale, encoding, paper,
                            mfgunlock::nr::g_status.compare.load(std::memory_order_relaxed));
}
#endif

}  // namespace mfgunlock::nr::vk
