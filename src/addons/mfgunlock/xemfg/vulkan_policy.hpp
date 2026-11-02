// SPDX-License-Identifier: MIT
#pragma once
// 1.7.1: the Vulkan adapter's decisions, free of hooks and devices so the gate
// can check them (native_vulkan.cpp has no game to run on in tests).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace mfgunlock::xemfg::native::vulkan::policy {
constexpr bool DepthFormat(VkFormat format) {
  return format==VK_FORMAT_D16_UNORM || format==VK_FORMAT_X8_D24_UNORM_PACK32
      || format==VK_FORMAT_D32_SFLOAT || format==VK_FORMAT_D16_UNORM_S8_UINT
      || format==VK_FORMAT_D24_UNORM_S8_UINT || format==VK_FORMAT_D32_SFLOAT_S8_UINT;
}
// The single-aspect view the input copy samples. The image's own format wins:
// NGX's view may name another (a D24S8 depth declared as color). Only a mutable
// color image can be viewed in NGX's format.
struct InputView { VkFormat format; VkImageAspectFlags aspect; };
constexpr InputView ChooseInputView(bool known,VkFormat imageFormat,VkImageCreateFlags flags,VkFormat ngxFormat) {
  const VkFormat own=known?imageFormat:ngxFormat;
  const bool depth=DepthFormat(own);
  const bool mutableFormat=known && (flags&VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
  return {depth || !mutableFormat?own:ngxFormat,VkImageAspectFlags(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT)};
}
// MAILBOX becomes IMMEDIATE on a host device (the layer's chain in mailbox mode
// kept an old picture on screen in RDR2), but IMMEDIATE is optional in Vulkan: a
// surface that does not offer it keeps the game's request.
inline VkPresentModeKHR HostPresentMode(VkPresentModeKHR requested,const VkPresentModeKHR* modes,uint32_t count) {
  if(requested!=VK_PRESENT_MODE_MAILBOX_KHR || !modes) return requested;
  for(uint32_t i=0;i<count;++i) if(modes[i]==VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
  return requested;
}
// The host's limit counts presented frames per second. Rounded down, so an
// interval is never exceeded; 0 means no limit.
constexpr uint32_t LimitFps(uint32_t intervalUs) {
  if(!intervalUs) return 0;
  const uint32_t fps=1000000/intervalUs;
  return fps?fps:1;
}
// ReShade's create_device writes Vulkan versions as major<<12 | minor<<8 (1.3 is
// 0x1300), not VK_MAKE_API_VERSION. The program set needs 1.3.
struct InstanceVersion { uint32_t major,minor; bool raised; };
constexpr InstanceVersion ReadInstanceVersion(uint32_t reshadeVersion) {
  const uint32_t requested=reshadeVersion?reshadeVersion:0x1000;
  const uint32_t major=(requested>>12)&0xF,minor=(requested>>8)&0xF;
  return {major,minor,major==1 && minor<3};
}
// Private data of the host's swap chains, which the driver never saw. A chain's
// entries go with it: its handle is an address a later chain can reuse.
class PrivateDataStore {
  std::map<std::pair<uint64_t,uint64_t>,uint64_t> values; // (object, slot) -> value
 public:
  static constexpr size_t kCapacity=4096;
  bool Set(uint64_t object,uint64_t slot,uint64_t value) {
    const auto key=std::make_pair(object,slot);
    if(values.size()>=kCapacity && !values.contains(key)) return false;
    values[key]=value; return true;
  }
  uint64_t Get(uint64_t object,uint64_t slot) const {
    const auto found=values.find({object,slot});
    return found==values.end()?0:found->second;
  }
  void Forget(uint64_t object) {
    values.erase(values.lower_bound({object,0}),values.upper_bound({object,UINT64_MAX}));
  }
  size_t Size() const { return values.size(); }
};
// 1.8.1: the size of each structure Vulkan allows on a swap chain's create info;
// 0 for any other, whose size cannot be known.
constexpr size_t SwapchainStructSize(VkStructureType type) {
  switch(type) {
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR: return sizeof(VkDeviceGroupSwapchainCreateInfoKHR);
    case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: return sizeof(VkImageCompressionControlEXT);
    case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: return sizeof(VkImageFormatListCreateInfo);
    case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT: return sizeof(VkSurfaceFullScreenExclusiveInfoEXT);
    case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT: return sizeof(VkSurfaceFullScreenExclusiveWin32InfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT: return sizeof(VkSwapchainCounterCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_DISPLAY_NATIVE_HDR_CREATE_INFO_AMD: return sizeof(VkSwapchainDisplayNativeHdrCreateInfoAMD);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV: return sizeof(VkSwapchainLatencyCreateInfoNV);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_BARRIER_CREATE_INFO_NV: return sizeof(VkSwapchainPresentBarrierCreateInfoNV);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT: return sizeof(VkSwapchainPresentModesCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT: return sizeof(VkSwapchainPresentScalingCreateInfoEXT);
    default: return 0;
  }
}
// 1.8.1: asks for a chain disallowed from full-screen exclusive without touching
// the game's structures, which Vulkan declares const and a game may keep in
// read-only memory (Bloodborne crashed on the write). Vulkan chains have no way
// to replace a middle node, so what precedes the request is copied; the tail
// stays the game's. A preceding structure of unknown size keeps the game's chain.
class DisallowedChain {
 public:
  enum class Outcome { NoRequest,AlreadyDisallowed,Copied,UnknownStructure };
  Outcome Edit(const void* chain) {
    const VkBaseInStructure* request{};
    for(auto* next=static_cast<const VkBaseInStructure*>(chain);next && !request;next=next->pNext) {
      if(next->sType==VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT) request=next;
      else if(!SwapchainStructSize(next->sType) && !unknown) unknown=next->sType;
    }
    if(!request) return Outcome::NoRequest;
    requested=reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(request)->fullScreenExclusive;
    if(requested==VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT) return Outcome::AlreadyDisallowed;
    if(unknown) return Outcome::UnknownStructure;
    VkBaseOutStructure* last{};
    for(auto* next=static_cast<const VkBaseInStructure*>(chain);;next=next->pNext) {
      const size_t size=SwapchainStructSize(next->sType);
      auto& copy=copies.emplace_back(std::make_unique<std::max_align_t[]>(size/sizeof(std::max_align_t)+1));
      std::memcpy(copy.get(),next,size);
      auto* node=reinterpret_cast<VkBaseOutStructure*>(copy.get());
      if(last) last->pNext=node; else head=node;
      last=node;
      if(next==request) {
        reinterpret_cast<VkSurfaceFullScreenExclusiveInfoEXT*>(node)->fullScreenExclusive=VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
        return Outcome::Copied;
      }
    }
  }
  const void* Head() const { return head; }
  VkFullScreenExclusiveEXT Requested() const { return requested; }
  VkStructureType Unknown() const { return unknown; }
 private:
  std::vector<std::unique_ptr<std::max_align_t[]>> copies;
  VkBaseOutStructure* head{};
  VkFullScreenExclusiveEXT requested{};
  VkStructureType unknown{};
};
// The loader's dispatch table pointer, shared by a device and its command
// buffers (the key Vulkan layers use). It names the device behind a command
// buffer, which is all a DLSS evaluation carries.
template<class Handle> const void* DispatchKey(Handle handle) { return *reinterpret_cast<const void* const*>(handle); }
}
