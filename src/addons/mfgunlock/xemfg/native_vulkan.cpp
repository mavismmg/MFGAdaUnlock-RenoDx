// SPDX-License-Identifier: MIT
#include "native_vulkan.hpp"
#include "vendor/xvkhost/vk_host.h"
#include "vulkan_policy.hpp"
#include "vendor/xvk/single_module.h"
#include "vendor/xess_mod.h"
#include "vendor/third_party/minhook/include/MinHook.h"
#include "../backend_selection.hpp"
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_params.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mfgunlock::xemfg::native { bool Hook(void* entry,void* replacement,void** trampoline); }

// The xvk layer's log and program module (vendor/xvk/single_module.h).
namespace single_module {
void Log(const wchar_t* text) noexcept {
  try {
    if(!text) return;
    const int count=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    if(count<=1) return;
    std::string utf8(size_t(count-1),'\0');
    WideCharToMultiByte(CP_UTF8,0,text,-1,utf8.data(),count,nullptr,nullptr);
    std::string escaped;
    for(const char c:utf8) {
      if(c=='"' || c=='\\') escaped+='\\';
      if(static_cast<unsigned char>(c)>=0x20) escaped+=c;
    }
    xess_mod::WriteLog("xvk","\"text\":\""+escaped+"\"");
  } catch(...) {}
}
HMODULE Self() noexcept { return xess_mod::Self(); }
}

namespace mfgunlock::xemfg::native::vulkan {
namespace {
std::atomic<bool> installed{},gameVulkan{};
std::atomic<uint32_t> frames{4};
std::atomic<uint32_t> instanceApi{0}; // 0: the instance creation was not seen
thread_local int internalDepth{};
struct Internal { Internal() { ++internalDepth; } ~Internal() { --internalDepth; } };

// Calls from this module (the host and the xvk layer) pass straight on: they
// reach these hooks through the loader's exports, never to be taken over.
bool FromSelf(const void* caller) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  return caller && VirtualQuery(caller,&m,sizeof(m))==sizeof(m) && m.AllocationBase==xess_mod::Self();
}
bool Passed(const void* caller) noexcept { return internalDepth>0 || FromSelf(caller); }
std::atomic<bool> takeoverSeen{};
std::atomic<uint64_t> presentsDone{}; // the game's presents through this hook (hang watch)
std::atomic<VkDevice> inputDevice{}; // the newest host device, for an input image created before the hooks
std::atomic<VkInstance> gameInstance{}; // for the surface's present modes
std::atomic<bool> exclusiveExtension{}; // the game's device enabled VK_EXT_full_screen_exclusive
void FlushLog();

#define MFG_VK_EXPORTS(X) \
  X(vkGetInstanceProcAddr) X(vkGetDeviceProcAddr) X(vkEnumeratePhysicalDevices) X(vkCreateDevice) X(vkDestroyDevice) \
  X(vkGetDeviceQueue) X(vkGetDeviceQueue2) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkDestroyInstance) \
  X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
  X(vkAcquireNextImage2KHR) X(vkQueuePresentKHR) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage)
#define MFG_FIELD(name) PFN_##name original_##name{};
MFG_VK_EXPORTS(MFG_FIELD)
#undef MFG_FIELD

// Device extension commands vulkan-1 does not export: the originals per
// device (vkGetDeviceProcAddr) or the loader's dispatching trampoline
// (vkGetInstanceProcAddr).
#define MFG_VK_EXTENSIONS(X) \
  X(vkSetHdrMetadataEXT) X(vkSetLatencySleepModeNV) X(vkLatencySleepNV) X(vkSetLatencyMarkerNV) \
  X(vkGetLatencyTimingsNV) X(vkWaitForPresentKHR) X(vkAcquireFullScreenExclusiveModeEXT) X(vkReleaseFullScreenExclusiveModeEXT)   X(vkSetPrivateData) X(vkGetPrivateData) X(vkSetPrivateDataEXT) X(vkGetPrivateDataEXT)   X(vkSetDebugUtilsObjectNameEXT) X(vkSetDebugUtilsObjectTagEXT)
struct Extensions {
#define MFG_FIELD(name) PFN_##name name{};
  MFG_VK_EXTENSIONS(MFG_FIELD)
#undef MFG_FIELD
  PFN_vkSignalSemaphore vkSignalSemaphore{}; // set only on a device the host prepared
  VkPhysicalDevice physical{};
};
std::mutex extensionLock;
std::unordered_map<VkDevice,Extensions> deviceExtensions;
Extensions instanceExtensions;
template<class F> F Extension(VkDevice device,F Extensions::*member) {
  std::lock_guard guard(extensionLock);
  const auto found=deviceExtensions.find(device);
  if(found!=deviceExtensions.end() && found->second.*member) return found->second.*member;
  return instanceExtensions.*member;
}

// A device the host prepared: its chains can be taken over. Any other device
// stays native and keeps what the game asked for.
bool HostPrepared(VkDevice device) {
  std::lock_guard guard(extensionLock);
  const auto found=deviceExtensions.find(device);
  return found!=deviceExtensions.end() && found->second.vkSignalSemaphore;
}
VkPresentModeKHR SwapchainPresentMode(VkDevice device,const VkSwapchainCreateInfoKHR& info) {
  if(info.presentMode!=VK_PRESENT_MODE_MAILBOX_KHR) return info.presentMode;
  VkPhysicalDevice physical{};
  {
    std::lock_guard guard(extensionLock);
    if(const auto found=deviceExtensions.find(device);found!=deviceExtensions.end()) physical=found->second.physical;
  }
  const auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
      original_vkGetInstanceProcAddr(gameInstance.load(),"vkGetPhysicalDeviceSurfacePresentModesKHR"));
  VkPresentModeKHR modes[16]{}; uint32_t count=16;
  if(!physical || !query || query(physical,info.surface,&count,modes)<0) count=0;
  return policy::HostPresentMode(info.presentMode,modes,count);
}

// The host's own resolvers: the loader's originals, never these hooks.
PFN_vkVoidFunction VKAPI_CALL HostGdpa(VkDevice device,const char* name) {
  if(name && !strcmp(name,"vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&HostGdpa);
  return original_vkGetDeviceProcAddr(device,name);
}
PFN_vkVoidFunction VKAPI_CALL HostGipa(VkInstance instance,const char* name) {
  if(name && !strcmp(name,"vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&HostGdpa);
  if(name && !strcmp(name,"vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&HostGipa);
  return original_vkGetInstanceProcAddr(instance,name);
}

// The game's instance, first seen after its creation (the addon loads inside it).
std::mutex instanceLock;
std::map<VkInstance,bool> instances;
void SeeInstance(VkInstance instance) {
  if(!instance) return;
  gameInstance=instance;
  {
    std::lock_guard guard(instanceLock);
    if(!instances.emplace(instance,true).second) return;
  }
  uint32_t api=instanceApi.load();
  const bool seen=api!=0;
  if(!seen) api=VK_API_VERSION_1_3; // NVIDIA accepts 1.3 device use; the device itself is checked
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.apiVersion=api;
  VkInstanceCreateInfo used{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  used.pApplicationInfo=&application;
  xess_vulkan::InstanceCreated(instance,&used,&HostGipa);
  xess_mod::WriteLog("vk_instance","\"api\":\""+std::to_string(VK_API_VERSION_MAJOR(api))+"."+std::to_string(VK_API_VERSION_MINOR(api))
      +"\",\"creation_seen\":"+(seen?"true":"false"));
}
void ForgetInstance(VkInstance instance) {
  std::lock_guard guard(instanceLock);
  instances.erase(instance);
}

PFN_vkVoidFunction Wrap(const char* name,PFN_vkVoidFunction original,VkDevice device);

__declspec(noinline) PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance,const char* name) {
  const auto result=original_vkGetInstanceProcAddr(instance,name);
  return Passed(_ReturnAddress())?result:Wrap(name,result,VK_NULL_HANDLE);
}
__declspec(noinline) PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(VkDevice device,const char* name) {
  const auto result=original_vkGetDeviceProcAddr(device,name);
  return Passed(_ReturnAddress())?result:Wrap(name,result,device);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkEnumeratePhysicalDevices(VkInstance instance,uint32_t* count,VkPhysicalDevice* devices) {
  if(!Passed(_ReturnAddress())) SeeInstance(instance);
  return original_vkEnumeratePhysicalDevices(instance,count,devices);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkDevice* output) {
  if(Passed(_ReturnAddress())) return original_vkCreateDevice(physical,info,allocator,output);
  Internal internal;
  auto edit=xess_vulkan::PrepareDevice(physical,info);
  VkResult result=original_vkCreateDevice(physical,xess_vulkan::EditedInfo(edit.get(),info),allocator,output);
  if(result!=VK_SUCCESS && edit) {
    xess_mod::WriteLog("vk_device_edit_refused","\"result\":"+std::to_string(int(result)));
    edit.reset();
    result=original_vkCreateDevice(physical,info,allocator,output);
  }
  if(result==VK_SUCCESS && output && *output && edit) {
    gameVulkan=true;
    for(uint32_t i=0;info && i<info->enabledExtensionCount;++i)
      if(info->ppEnabledExtensionNames[i] && !strcmp(info->ppEnabledExtensionNames[i],VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
        exclusiveExtension=true;
    {
      std::lock_guard guard(extensionLock);
      auto& e=deviceExtensions[*output];
      e.vkSignalSemaphore=reinterpret_cast<PFN_vkSignalSemaphore>(original_vkGetDeviceProcAddr(*output,"vkSignalSemaphore"));
      e.physical=physical;
    }
    xess_vulkan::DeviceCreated(std::move(edit),*output);
    inputDevice=*output;
  }
  return result;
}
void ForgetDeviceImages(VkDevice device);
__declspec(noinline) void VKAPI_CALL Hook_vkDestroyDevice(VkDevice device,const VkAllocationCallbacks* allocator) {
  if(!Passed(_ReturnAddress())) {
    Internal internal;
    xess_vulkan::DeviceDestroying(device);
    ForgetDeviceImages(device);
    VkDevice expected=device;
    inputDevice.compare_exchange_strong(expected,VK_NULL_HANDLE);
    std::lock_guard guard(extensionLock);
    deviceExtensions.erase(device);
  }
  original_vkDestroyDevice(device,allocator);
}
__declspec(noinline) void VKAPI_CALL Hook_vkGetDeviceQueue(VkDevice device,uint32_t family,uint32_t index,VkQueue* output) {
  original_vkGetDeviceQueue(device,family,index,output);
  if(!Passed(_ReturnAddress()) && output && *output) xess_vulkan::QueueSeen(device,family,index,*output);
}
__declspec(noinline) void VKAPI_CALL Hook_vkGetDeviceQueue2(VkDevice device,const VkDeviceQueueInfo2* info,VkQueue* output) {
  original_vkGetDeviceQueue2(device,info,output);
  if(!Passed(_ReturnAddress()) && info && !info->flags && output && *output)
    xess_vulkan::QueueSeen(device,info->queueFamilyIndex,info->queueIndex,*output);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkCreateWin32SurfaceKHR(VkInstance instance,const VkWin32SurfaceCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator,VkSurfaceKHR* output) {
  const auto result=original_vkCreateWin32SurfaceKHR(instance,info,allocator,output);
  if(!Passed(_ReturnAddress()) && result==VK_SUCCESS && info && output) {
    SeeInstance(instance);
    xess_vulkan::SurfaceCreated(instance,*output,info->hwnd);
  }
  return result;
}
__declspec(noinline) void VKAPI_CALL Hook_vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks* allocator) {
  if(!Passed(_ReturnAddress())) xess_vulkan::SurfaceDestroyed(surface);
  original_vkDestroySurfaceKHR(instance,surface,allocator);
}
__declspec(noinline) void VKAPI_CALL Hook_vkDestroyInstance(VkInstance instance,const VkAllocationCallbacks* allocator) {
  if(!Passed(_ReturnAddress())) { xess_vulkan::InstanceDestroyed(instance); ForgetInstance(instance); }
  original_vkDestroyInstance(instance,allocator);
}
void ForgetPrivateData(VkSwapchainKHR swapchain);
__declspec(noinline) VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator,VkSwapchainKHR* output) {
  if(Passed(_ReturnAddress()) || !HostPrepared(device)) return original_vkCreateSwapchainKHR(device,info,allocator,output);
  Internal internal;
  // Full-screen exclusive is disallowed on every chain of the game while the
  // host is on: an exclusive native chain kept the display after the takeover
  // (RDR2 in focus showed its last picture). The mode commands are answered
  // by the hooks below. The game's structure is restored before returning.
  const VkSwapchainCreateInfoKHR* used=info;
  VkSurfaceFullScreenExclusiveInfoEXT* exclusive{};
  VkFullScreenExclusiveEXT requested{};
  for(auto* next=info?static_cast<const VkBaseInStructure*>(info->pNext):nullptr;next;next=next->pNext)
    if(next->sType==VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT)
      exclusive=reinterpret_cast<VkSurfaceFullScreenExclusiveInfoEXT*>(const_cast<VkBaseInStructure*>(next));
  if(exclusive) {
    requested=exclusive->fullScreenExclusive;
    exclusive->fullScreenExclusive=VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
    static std::atomic<int> logged{-1};
    if(logged.exchange(int(requested))!=int(requested))
      xess_mod::WriteLog("vk_swapchain_fullscreen_exclusive","\"requested\":"+std::to_string(int(requested))+",\"used\":\"disallowed\"");
  }
  struct Restore {
    VkSurfaceFullScreenExclusiveInfoEXT* exclusive; VkFullScreenExclusiveEXT mode;
    ~Restore() { if(exclusive) exclusive->fullScreenExclusive=mode; }
  } restore{exclusive,requested};
  // A game that enables the extension but names no mode (RDR2 in its fullscreen
  // mode) leaves the choice to the driver, which gave its own chain the display
  // for good: the chain is created disallowed explicitly.
  VkSwapchainCreateInfoKHR named{};
  VkSurfaceFullScreenExclusiveInfoEXT disallowed{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,nullptr,
      VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT};
  if(info && !exclusive && exclusiveExtension.load()) {
    named=*info; disallowed.pNext=const_cast<void*>(info->pNext); named.pNext=&disallowed; used=&named;
    static std::atomic<bool> logged{};
    if(!logged.exchange(true)) xess_mod::WriteLog("vk_swapchain_fullscreen_exclusive","\"requested\":\"none\",\"used\":\"disallowed\"");
  }
  // MAILBOX becomes IMMEDIATE (VSync stays off): the layer's chain in mailbox
  // mode kept an old picture on screen while the game had the focus (RDR2).
  VkSwapchainCreateInfoKHR immediate{};
  if(info && info->presentMode==VK_PRESENT_MODE_MAILBOX_KHR) {
    const auto mode=SwapchainPresentMode(device,*info);
    if(mode!=info->presentMode) { immediate=*used; immediate.presentMode=mode; used=&immediate; }
    static std::atomic<bool> logged{};
    if(!logged.exchange(true)) xess_mod::WriteLog("vk_swapchain_present_mode",std::string("\"requested\":\"mailbox\",\"used\":\"")
        +(mode==VK_PRESENT_MODE_IMMEDIATE_KHR?"immediate":"mailbox")+"\"");
  }
  VkResult result=VK_SUCCESS;
  if(xess_vulkan::CreateSwapchain(device,used,allocator,output,original_vkCreateSwapchainKHR,result)) {
    if(result==VK_SUCCESS && output && xess_vulkan::OwnsSwapchain(*output)) takeoverSeen=true;
    return result;
  }
  return original_vkCreateSwapchainKHR(device,used,allocator,output);
}
__declspec(noinline) void VKAPI_CALL Hook_vkDestroySwapchainKHR(VkDevice device,VkSwapchainKHR swapchain,const VkAllocationCallbacks* allocator) {
  if(!Passed(_ReturnAddress())) {
    Internal internal;
    ForgetPrivateData(swapchain);
    if(xess_vulkan::DestroySwapchain(device,swapchain)) return;
  }
  original_vkDestroySwapchainKHR(device,swapchain,allocator);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkGetSwapchainImagesKHR(VkDevice device,VkSwapchainKHR swapchain,uint32_t* count,VkImage* images) {
  VkResult result=VK_SUCCESS;
  if(!Passed(_ReturnAddress()) && xess_vulkan::GetSwapchainImages(device,swapchain,count,images,result)) return result;
  return original_vkGetSwapchainImagesKHR(device,swapchain,count,images);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR swapchain,uint64_t timeout,
    VkSemaphore semaphore,VkFence fence,uint32_t* index) {
  if(!Passed(_ReturnAddress())) {
    Internal internal;
    VkResult result=VK_SUCCESS;
    if(xess_vulkan::AcquireNextImage(device,swapchain,timeout,semaphore,fence,index,result)) return result;
  }
  return original_vkAcquireNextImageKHR(device,swapchain,timeout,semaphore,fence,index);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR* info,uint32_t* index) {
  if(!Passed(_ReturnAddress()) && info) {
    Internal internal;
    VkResult result=VK_SUCCESS;
    if(xess_vulkan::AcquireNextImage(device,info->swapchain,info->timeout,info->semaphore,info->fence,index,result)) return result;
  }
  return original_vkAcquireNextImage2KHR(device,info,index);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR* info) {
  if(Passed(_ReturnAddress())) return original_vkQueuePresentKHR(queue,info);
  Internal internal;
  struct Count { ~Count() { presentsDone.fetch_add(1,std::memory_order_relaxed); } } count;
  VkResult result=VK_SUCCESS;
  if(xess_vulkan::QueuePresent(queue,info,result)) return result;
  result=original_vkQueuePresentKHR(queue,info);
  return xess_vulkan::AfterNativePresent(queue,info,result);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkDeviceWaitIdle(VkDevice device) {
  VkResult result=VK_SUCCESS;
  if(!Passed(_ReturnAddress()) && xess_vulkan::DeviceWaitIdle(device,result)) return result;
  return original_vkDeviceWaitIdle(device);
}

// The game's images (usage, format, flags): the DLSS input copy samples them,
// which needs VK_IMAGE_USAGE_SAMPLED_BIT.
struct ImageInfo { VkImageUsageFlags usage{}; VkFormat format{}; VkImageCreateFlags flags{}; VkSampleCountFlagBits samples{}; VkDevice device{}; };
std::mutex imageLock;
std::unordered_map<VkImage,ImageInfo> images;
__declspec(noinline) VkResult VKAPI_CALL Hook_vkCreateImage(VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* output) {
  const auto result=original_vkCreateImage(device,info,allocator,output);
  if(result==VK_SUCCESS && info && output && *output && !FromSelf(_ReturnAddress())) {
    std::lock_guard guard(imageLock);
    if(images.size()<200000) images[*output]={info->usage,info->format,info->flags,info->samples,device};
    else {
      // The inputs of a later image are refused: frame generation stops, so it is said once.
      static std::atomic<bool> logged{};
      if(!logged.exchange(true)) xess_mod::WriteLog("vk_image_table_full","\"images\":"+std::to_string(images.size()));
    }
  }
  return result;
}
// Single-aspect views of the DLSS input images for the copy, per image.
// A view lives until its image or device goes, so the table is bounded by the
// live input images. Function pointers belong to one device (a game may recreate it).
struct OwnedView { VkImageView view{}; VkDevice device{}; };
std::unordered_map<VkImage,OwnedView> inputViews;
struct ViewFunctions { PFN_vkCreateImageView create{}; PFN_vkDestroyImageView destroy{}; };
std::unordered_map<VkDevice,ViewFunctions> viewFunctions;
const ViewFunctions& FunctionsFor(VkDevice device) {
  auto& functions=viewFunctions[device];
  if(!functions.create) {
    functions.create=reinterpret_cast<PFN_vkCreateImageView>(original_vkGetDeviceProcAddr(device,"vkCreateImageView"));
    functions.destroy=reinterpret_cast<PFN_vkDestroyImageView>(original_vkGetDeviceProcAddr(device,"vkDestroyImageView"));
  }
  return functions;
}
VkImageView InputView(VkImage image,VkFormat format,bool depth) noexcept {
  if(!image) return VK_NULL_HANDLE;
  std::lock_guard guard(imageLock);
  if(const auto found=inputViews.find(image);found!=inputViews.end()) return found->second.view;
  const auto known=images.find(image);
  const VkDevice device=known!=images.end()?known->second.device:inputDevice.load();
  if(!device) return VK_NULL_HANDLE;
  const auto& functions=FunctionsFor(device);
  if(!functions.create || !functions.destroy) return VK_NULL_HANDLE;
  // The image's own format (NGX's view may name another), and the aspect it has.
  const VkFormat imageFormat=known!=images.end()?known->second.format:format;
  const auto choice=policy::ChooseInputView(known!=images.end(),imageFormat,known!=images.end()?known->second.flags:0,format);
  const bool depthFormat=policy::DepthFormat(imageFormat);
  const VkFormat viewFormat=choice.format;
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image=image; info.viewType=VK_IMAGE_VIEW_TYPE_2D; info.format=viewFormat;
  info.subresourceRange={choice.aspect,0,1,0,1};
  // Only sampled: the image may carry usages a plain view could not support.
  VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,nullptr,VK_IMAGE_USAGE_SAMPLED_BIT};
  info.pNext=&usage;
  VkImageView view{};
  const auto result=functions.create(device,&info,nullptr,&view);
  xess_mod::WriteLog("vk_input_view","\"input\":\""+std::string(depth?"depth":"motion")+"\",\"ngx_format\":"+std::to_string(int(format))
      +",\"image_known\":"+(known!=images.end()?"true":"false")+",\"image_format\":"+std::to_string(int(imageFormat))
      +",\"view_format\":"+std::to_string(int(viewFormat))+",\"depth_aspect\":"+(depthFormat?"true":"false")
      +",\"result\":"+std::to_string(int(result)));
  if(result!=VK_SUCCESS) return VK_NULL_HANDLE;
  inputViews[image]={view,device};
  return view;
}
// The device is going: its views go first, and no later device inherits its tables.
void ForgetDeviceImages(VkDevice device) {
  std::lock_guard guard(imageLock);
  for(auto i=inputViews.begin();i!=inputViews.end();) {
    if(i->second.device!=device) { ++i; continue; }
    if(const auto destroy=FunctionsFor(device).destroy) destroy(device,i->second.view,nullptr);
    i=inputViews.erase(i);
  }
  std::erase_if(images,[&](const auto& entry) { return entry.second.device==device; });
  viewFunctions.erase(device);
}
__declspec(noinline) void VKAPI_CALL Hook_vkDestroyImage(VkDevice device,VkImage image,const VkAllocationCallbacks* allocator) {
  {
    std::lock_guard guard(imageLock);
    images.erase(image);
    if(const auto found=inputViews.find(image);found!=inputViews.end()) {
      if(const auto destroy=FunctionsFor(found->second.device).destroy) destroy(found->second.device,found->second.view,nullptr);
      inputViews.erase(found);
    }
  }
  original_vkDestroyImage(device,image,allocator);
}
bool KnownImage(VkImage image,ImageInfo& out) {
  std::lock_guard guard(imageLock);
  const auto found=images.find(image);
  if(found==images.end()) return false;
  out=found->second; return true;
}

// Extension commands on the host's chains: HDR metadata goes to XeFG's
// swap chain; present wait and VK_NV_low_latency2 (Streamline's Reflex) are
// answered here, since the driver never saw the host's handle. XeLL paces the
// game's frames at each acquisition instead.
void VKAPI_CALL Hook_vkSetHdrMetadataEXT(VkDevice device,uint32_t count,const VkSwapchainKHR* swapchains,const VkHdrMetadataEXT* metadata) {
  const auto original=Extension(device,&Extensions::vkSetHdrMetadataEXT);
  if(!count || !swapchains || !metadata) { if(original) original(device,count,swapchains,metadata); return; }
  std::vector<VkSwapchainKHR> native; std::vector<VkHdrMetadataEXT> nativeMetadata;
  for(uint32_t i=0;i<count;++i)
    if(!xess_vulkan::SetHdrMetadata(device,swapchains[i],&metadata[i])) { native.push_back(swapchains[i]); nativeMetadata.push_back(metadata[i]); }
  if(!native.empty() && original) original(device,uint32_t(native.size()),native.data(),nativeMetadata.data());
}
VkResult VKAPI_CALL Hook_vkSetLatencySleepModeNV(VkDevice device,VkSwapchainKHR swapchain,const VkLatencySleepModeInfoNV* info) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) return VK_SUCCESS;
  const auto original=Extension(device,&Extensions::vkSetLatencySleepModeNV);
  return original?original(device,swapchain,info):VK_ERROR_INITIALIZATION_FAILED;
}
VkResult VKAPI_CALL Hook_vkLatencySleepNV(VkDevice device,VkSwapchainKHR swapchain,const VkLatencySleepInfoNV* info) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) {
    const auto signal=Extension(device,&Extensions::vkSignalSemaphore);
    if(!info || !signal) return VK_SUCCESS;
    VkSemaphoreSignalInfo value{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,nullptr,info->signalSemaphore,info->value};
    return signal(device,&value);
  }
  const auto original=Extension(device,&Extensions::vkLatencySleepNV);
  return original?original(device,swapchain,info):VK_ERROR_INITIALIZATION_FAILED;
}
void VKAPI_CALL Hook_vkSetLatencyMarkerNV(VkDevice device,VkSwapchainKHR swapchain,const VkSetLatencyMarkerInfoNV* info) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) return;
  if(const auto original=Extension(device,&Extensions::vkSetLatencyMarkerNV)) original(device,swapchain,info);
}
void VKAPI_CALL Hook_vkGetLatencyTimingsNV(VkDevice device,VkSwapchainKHR swapchain,VkGetLatencyMarkerInfoNV* info) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) { if(info) info->timingCount=0; return; }
  if(const auto original=Extension(device,&Extensions::vkGetLatencyTimingsNV)) original(device,swapchain,info);
}
VkResult VKAPI_CALL Hook_vkWaitForPresentKHR(VkDevice device,VkSwapchainKHR swapchain,uint64_t presentId,uint64_t timeout) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) return xess_vulkan::hosted::WaitForPresent(swapchain,presentId,timeout);
  const auto original=Extension(device,&Extensions::vkWaitForPresentKHR);
  return original?original(device,swapchain,presentId,timeout):VK_ERROR_INITIALIZATION_FAILED;
}

// Full-screen exclusive: XeFG presents its own chain borderless, so the
// game's requests on the host's chain succeed without a mode change.
VkResult VKAPI_CALL Hook_vkAcquireFullScreenExclusiveModeEXT(VkDevice device,VkSwapchainKHR swapchain) {
  if(xess_vulkan::OwnsSwapchain(swapchain) || HostPrepared(device)) return VK_SUCCESS;   // chains are created disallowed (above)
  const auto original=Extension(device,&Extensions::vkAcquireFullScreenExclusiveModeEXT);
  return original?original(device,swapchain):VK_ERROR_INITIALIZATION_FAILED;
}
VkResult VKAPI_CALL Hook_vkReleaseFullScreenExclusiveModeEXT(VkDevice device,VkSwapchainKHR swapchain) {
  if(xess_vulkan::OwnsSwapchain(swapchain) || HostPrepared(device)) return VK_SUCCESS;
  const auto original=Extension(device,&Extensions::vkReleaseFullScreenExclusiveModeEXT);
  return original?original(device,swapchain):VK_ERROR_INITIALIZATION_FAILED;
}

// Streamline's sl.common keeps private data on the game's swap chain
// (vkSetPrivateData with VK_OBJECT_TYPE_SWAPCHAIN_KHR) and names objects for
// debugging. The driver never saw the host's handle: its data is kept here.
std::mutex privateLock;
policy::PrivateDataStore privateData;
void ForgetPrivateData(VkSwapchainKHR swapchain) {
  std::lock_guard guard(privateLock);
  privateData.Forget(reinterpret_cast<uint64_t>(swapchain));
}
bool HostObject(VkObjectType type,uint64_t handle) {
  return type==VK_OBJECT_TYPE_SWAPCHAIN_KHR && xess_vulkan::OwnsSwapchain(reinterpret_cast<VkSwapchainKHR>(handle));
}
VkResult SetPrivate(uint64_t handle,VkPrivateDataSlot slot,uint64_t data) {
  std::lock_guard guard(privateLock);
  const bool stored=privateData.Set(handle,uint64_t(slot),data);
  static std::atomic<bool> logged{};
  if(!logged.exchange(true)) xess_mod::WriteLog("vk_private_data","\"object\":\"swapchain\",\"emulated\":true");
  return stored?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;
}
void GetPrivate(uint64_t handle,VkPrivateDataSlot slot,uint64_t* data) {
  if(!data) return;
  std::lock_guard guard(privateLock);
  *data=privateData.Get(handle,uint64_t(slot));
}
VkResult VKAPI_CALL Hook_vkSetPrivateData(VkDevice device,VkObjectType type,uint64_t handle,VkPrivateDataSlot slot,uint64_t data) {
  if(HostObject(type,handle)) return SetPrivate(handle,slot,data);
  const auto original=Extension(device,&Extensions::vkSetPrivateData);
  return original?original(device,type,handle,slot,data):VK_ERROR_OUT_OF_HOST_MEMORY;
}
void VKAPI_CALL Hook_vkGetPrivateData(VkDevice device,VkObjectType type,uint64_t handle,VkPrivateDataSlot slot,uint64_t* data) {
  if(HostObject(type,handle)) { GetPrivate(handle,slot,data); return; }
  if(const auto original=Extension(device,&Extensions::vkGetPrivateData)) original(device,type,handle,slot,data);
  else if(data) *data=0;
}
VkResult VKAPI_CALL Hook_vkSetPrivateDataEXT(VkDevice device,VkObjectType type,uint64_t handle,VkPrivateDataSlot slot,uint64_t data) {
  if(HostObject(type,handle)) return SetPrivate(handle,slot,data);
  const auto original=Extension(device,&Extensions::vkSetPrivateDataEXT);
  return original?original(device,type,handle,slot,data):VK_ERROR_OUT_OF_HOST_MEMORY;
}
void VKAPI_CALL Hook_vkGetPrivateDataEXT(VkDevice device,VkObjectType type,uint64_t handle,VkPrivateDataSlot slot,uint64_t* data) {
  if(HostObject(type,handle)) { GetPrivate(handle,slot,data); return; }
  if(const auto original=Extension(device,&Extensions::vkGetPrivateDataEXT)) original(device,type,handle,slot,data);
  else if(data) *data=0;
}
VkResult VKAPI_CALL Hook_vkSetDebugUtilsObjectNameEXT(VkDevice device,const VkDebugUtilsObjectNameInfoEXT* info) {
  if(info && HostObject(info->objectType,info->objectHandle)) return VK_SUCCESS;
  const auto original=Extension(device,&Extensions::vkSetDebugUtilsObjectNameEXT);
  return original?original(device,info):VK_SUCCESS;
}
VkResult VKAPI_CALL Hook_vkSetDebugUtilsObjectTagEXT(VkDevice device,const VkDebugUtilsObjectTagInfoEXT* info) {
  if(info && HostObject(info->objectType,info->objectHandle)) return VK_SUCCESS;
  const auto original=Extension(device,&Extensions::vkSetDebugUtilsObjectTagEXT);
  return original?original(device,info):VK_SUCCESS;
}

PFN_vkVoidFunction Wrap(const char* name,PFN_vkVoidFunction original,VkDevice device) {
  if(!name || !original || name[0]!='v' || name[1]!='k') return original;
#define MFG_EXPORT(fn) if(!strcmp(name,#fn)) return original_##fn?reinterpret_cast<PFN_vkVoidFunction>(&Hook_##fn):original;
  MFG_VK_EXPORTS(MFG_EXPORT)
#undef MFG_EXPORT
#define MFG_EXTENSION(fn) if(!strcmp(name,#fn)) { \
    std::lock_guard guard(extensionLock); \
    (device?deviceExtensions[device]:instanceExtensions).fn=reinterpret_cast<PFN_##fn>(original); \
    return reinterpret_cast<PFN_vkVoidFunction>(&Hook_##fn); }
  MFG_VK_EXTENSIONS(MFG_EXTENSION)
#undef MFG_EXTENSION
  return original;
}

// Field diagnostics: a call through a null pointer names its caller (the
// return address on top of the stack) and nearby image addresses in the log.
std::string ModuleOffset(uint64_t address) {
  MEMORY_BASIC_INFORMATION m{};
  if(!address || VirtualQuery(reinterpret_cast<void*>(address),&m,sizeof(m))!=sizeof(m) || m.Type!=MEM_IMAGE) return {};
  wchar_t path[MAX_PATH]{};
  if(!GetModuleFileNameW(static_cast<HMODULE>(m.AllocationBase),path,MAX_PATH)) return {};
  const wchar_t* leaf=wcsrchr(path,L'\\'); leaf=leaf?leaf+1:path;
  std::string name; for(const wchar_t* c=leaf;*c;++c) name+=*c<0x80?char(*c):'?';
  char offset[32]; snprintf(offset,sizeof(offset),"+0x%llx",static_cast<unsigned long long>(address-reinterpret_cast<uint64_t>(m.AllocationBase)));
  return name+offset;
}
void FlushLog() {
  using Flush=bool(*)(uint32_t);
  if(const auto flush=reinterpret_cast<Flush>(GetProcAddress(xess_mod::Self(),"MFGAXeLogFlush"))) flush(1000);
}
// [Diagnostics] VulkanFaults=1: the first access violations after a takeover,
// calls through a null pointer and faults inside another RenoDX addon, with
// the image addresses on the stack (how the 1.7 field crashes were found).
// Handled probes are logged too; the budget keeps it to a few lines.
LONG CALLBACK NullCall(EXCEPTION_POINTERS* info) {
  const auto* record=info?info->ExceptionRecord:nullptr;
  if(!record || record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  // Also faults inside another RenoDX addon (field diagnostics of a pipeline-creation conflict).
  const auto faultModule=ModuleOffset(reinterpret_cast<uint64_t>(record->ExceptionAddress));
  const bool renodx=faultModule.rfind("renodx-",0)==0 && faultModule.rfind("renodx-mfgunlock",0)!=0;
  if(record->ExceptionAddress!=nullptr && !takeoverSeen.load() && !renodx) return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<int> budget{};
  if(budget.fetch_add(1)>=6) return EXCEPTION_CONTINUE_SEARCH;
  std::string frames;
  const auto* stack=reinterpret_cast<const uint64_t*>(info->ContextRecord->Rsp);
  for(unsigned i=0,found=0;i<256 && found<12;++i) {
    uint64_t value{};
    if(!ReadProcessMemory(GetCurrentProcess(),stack+i,&value,sizeof(value),nullptr)) break;
    const auto name=ModuleOffset(value);
    if(name.empty()) continue;
    frames+=std::string(found?",":"")+"\""+std::to_string(i)+":"+name+"\"";
    ++found;
  }
  const auto at=ModuleOffset(reinterpret_cast<uint64_t>(record->ExceptionAddress));
  const auto target=record->NumberParameters>=2?record->ExceptionInformation[1]:0;
  xess_mod::WriteLog("access_violation","\"thread\":"+std::to_string(GetCurrentThreadId())+",\"at\":\""+at
      +"\",\"kind\":"+std::to_string(record->NumberParameters?record->ExceptionInformation[0]:0)
      +",\"target\":"+std::to_string(target)+",\"stack\":["+frames+"]");
  FlushLog();
  return EXCEPTION_CONTINUE_SEARCH;
}

bool Install() noexcept {
  static std::mutex lock; std::lock_guard guard(lock);
  if(installed) return true;
  InstallDiagnostics();
  const HMODULE loader=GetModuleHandleW(L"vulkan-1.dll");
  if(!loader) { xess_mod::WriteLog("vk_hooks","\"installed\":false,\"reason\":\"vulkan-1_not_loaded\""); return false; }
  HMODULE pinned{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(loader),&pinned);
  struct Entry { const char* name; void* hook; void** original; };
  const Entry entries[]={
#define MFG_ENTRY(fn) {#fn,reinterpret_cast<void*>(&Hook_##fn),reinterpret_cast<void**>(&original_##fn)},
    MFG_VK_EXPORTS(MFG_ENTRY)
#undef MFG_ENTRY
  };
  std::string missing;
  for(const auto& e:entries) {
    void* target=reinterpret_cast<void*>(GetProcAddress(loader,e.name));
    if(!target || !Hook(target,e.hook,e.original)) missing+=(missing.empty()?"":",")+std::string(e.name);
  }
  // The host takes a chain over only when every entry its handle reaches is ours.
  for(const char* name:{"vkCreateSwapchainKHR","vkQueuePresentKHR","vkDestroySwapchainKHR","vkGetSwapchainImagesKHR","vkAcquireNextImageKHR"})
    if(missing.find(name)==std::string::npos) xess_vulkan::EntryWrapped(name);
  installed=missing.empty() || (original_vkGetInstanceProcAddr && original_vkGetDeviceProcAddr);
  xess_mod::WriteLog("vk_hooks","\"installed\":"+std::string(installed?"true":"false")+",\"missing\":\""+missing+"\"");
  return installed;
}

// NGX Vulkan: the DLSS Super Resolution (and Ray Reconstruction) inputs are
// copied on the game's command buffer before DLSS reads them.
using CreateFn=NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Create1Fn=NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice,VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using EvaluateFn=NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
CreateFn ngxCreate{}; Create1Fn ngxCreate1{}; EvaluateFn ngxEvaluate{}; ReleaseFn ngxRelease{};
std::mutex featureLock;
std::map<const NVSDK_NGX_Handle*,NVSDK_NGX_Feature> features;
std::atomic<uint32_t> evaluateLog{};
bool InputFeature(NVSDK_NGX_Feature feature) {
  return feature==NVSDK_NGX_Feature_SuperSampling || feature==NVSDK_NGX_Feature_RayReconstruction;
}
void Remember(NVSDK_NGX_Result result,NVSDK_NGX_Feature feature,NVSDK_NGX_Handle** out) {
  if(!NVSDK_NGX_SUCCEED(result) || !out || !*out) return;
  { std::lock_guard guard(featureLock); features[*out]=feature; }
  xess_mod::WriteLog("vk_ngx_feature","\"feature\":"+std::to_string(int(feature))+",\"handle\":"+std::to_string(uintptr_t(*out)));
}
NVSDK_NGX_Result NVSDK_CONV NgxCreate(VkCommandBuffer command,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** out) {
  const auto result=ngxCreate(command,feature,params,out);
  Remember(result,feature,out); return result;
}
NVSDK_NGX_Result NVSDK_CONV NgxCreate1(VkDevice device,VkCommandBuffer command,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** out) {
  const auto result=ngxCreate1(device,command,feature,params,out);
  Remember(result,feature,out); return result;
}
NVSDK_NGX_Result NVSDK_CONV NgxEvaluate(VkCommandBuffer command,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  if(command && handle && params) {
    int feature=-1;
    {
      std::lock_guard guard(featureLock);
      const auto found=features.find(handle);
      if(found!=features.end()) feature=int(found->second);
    }
    // A feature created before the hooks counts when it carries DLSS inputs
    // (DLSS-G is held off under XeMFG, so its evaluation never runs).
    if(feature<0 || InputFeature(NVSDK_NGX_Feature(feature))) {
      // The copy samples both inputs: images without SAMPLED usage (or
      // multisampled ones) are refused here, with what they are in the log.
      void* depthValue{}; void* motionValue{};
      params->Get(NVSDK_NGX_Parameter_Depth,&depthValue);
      params->Get(NVSDK_NGX_Parameter_MotionVectors,&motionValue);
      const auto* depth=static_cast<const NVSDK_NGX_Resource_VK*>(depthValue);
      const auto* motion=static_cast<const NVSDK_NGX_Resource_VK*>(motionValue);
      ImageInfo depthImage{},motionImage{};
      const bool views=depth && motion && depth->Type==NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW
          && motion->Type==NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
      const bool depthKnown=views && KnownImage(depth->Resource.ImageViewInfo.Image,depthImage);
      const bool motionKnown=views && KnownImage(motion->Resource.ImageViewInfo.Image,motionImage);
      const auto sampleable=[](bool known,const ImageInfo& i) {
        return known && (i.usage&VK_IMAGE_USAGE_SAMPLED_BIT) && i.samples==VK_SAMPLE_COUNT_1_BIT;
      };
      const bool usable=sampleable(depthKnown,depthImage) && sampleable(motionKnown,motionImage);
      static std::atomic<bool> described{};
      if(views && !described.exchange(true)) {
        const auto& d=depth->Resource.ImageViewInfo; const auto& m=motion->Resource.ImageViewInfo;
        xess_mod::WriteLog("vk_dlss_inputs","\"feature\":"+std::to_string(feature)
            +",\"depth\":{\"known\":"+(depthKnown?"true":"false")+",\"usage\":"+std::to_string(depthImage.usage)
            +",\"image_format\":"+std::to_string(int(depthImage.format))+",\"view_format\":"+std::to_string(int(d.Format))
            +",\"aspect\":"+std::to_string(d.SubresourceRange.aspectMask)+",\"flags\":"+std::to_string(depthImage.flags)
            +",\"samples\":"+std::to_string(int(depthImage.samples))+",\"size\":\""+std::to_string(d.Width)+"x"+std::to_string(d.Height)
            +"\",\"levels\":"+std::to_string(d.SubresourceRange.levelCount)+",\"layers\":"+std::to_string(d.SubresourceRange.layerCount)+"}"
            +",\"motion\":{\"known\":"+(motionKnown?"true":"false")+",\"usage\":"+std::to_string(motionImage.usage)
            +",\"image_format\":"+std::to_string(int(motionImage.format))+",\"view_format\":"+std::to_string(int(m.Format))
            +",\"aspect\":"+std::to_string(m.SubresourceRange.aspectMask)+",\"flags\":"+std::to_string(motionImage.flags)
            +",\"samples\":"+std::to_string(int(motionImage.samples))+",\"size\":\""+std::to_string(m.Width)+"x"+std::to_string(m.Height)
            +"\",\"levels\":"+std::to_string(m.SubresourceRange.levelCount)+",\"layers\":"+std::to_string(m.SubresourceRange.layerCount)+"}"
            +",\"usable\":"+(usable?"true":"false"));
        FlushLog();
      }
      const bool captured=usable && xess_vulkan::CaptureDlssEvaluate(command,params);
      if(evaluateLog.fetch_add(1)<4)
        xess_mod::WriteLog("vk_ngx_evaluate","\"feature\":"+std::to_string(feature)+",\"captured\":"+(captured?"true":"false"));
    }
  }
  return ngxEvaluate(command,handle,params,callback);
}
NVSDK_NGX_Result NVSDK_CONV NgxRelease(NVSDK_NGX_Handle* handle) {
  if(handle) { std::lock_guard guard(featureLock); features.erase(handle); }
  return ngxRelease(handle);
}
} // namespace

// [Diagnostics] VulkanFaults=1, hang watch: when a taken-over chain stops
// presenting for 3 s, every thread's instruction pointer and the image
// addresses on its stack go to the log once (module+offset).
std::string ThreadStack(HANDLE thread) {
  CONTEXT context{}; context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
  if(!GetThreadContext(thread,&context)) return {};
  std::string frames="\"rip:"+ModuleOffset(context.Rip)+"\"";
  const auto* stack=reinterpret_cast<const uint64_t*>(context.Rsp);
  for(unsigned i=0,found=0;i<512 && found<16;++i) {
    uint64_t value{};
    if(!ReadProcessMemory(GetCurrentProcess(),stack+i,&value,sizeof(value),nullptr)) break;
    const auto name=ModuleOffset(value);
    if(name.empty()) continue;
    frames+=",\""+std::to_string(i)+":"+name+"\"";
    ++found;
  }
  return frames;
}
DWORD WINAPI HangWatch(void*) {
  uint64_t last=0; unsigned still=0; bool reported=false;
  for(;;) {
    Sleep(1000);
    if(!takeoverSeen.load()) continue;
    const uint64_t done=presentsDone.load(std::memory_order_relaxed);
    if(done!=last) { last=done; still=0; reported=false; continue; }
    if(++still<3 || reported) continue;
    reported=true;
    const DWORD self=GetCurrentThreadId();
    const HANDLE threads=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(threads==INVALID_HANDLE_VALUE) continue;
    THREADENTRY32 entry{sizeof(entry)};
    unsigned logged=0;
    for(BOOL ok=Thread32First(threads,&entry);ok && logged<64;ok=Thread32Next(threads,&entry)) {
      if(entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==self) continue;
      const HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
      if(!thread) continue;
      std::string frames;
      if(SuspendThread(thread)!=DWORD(-1)) { frames=ThreadStack(thread); ResumeThread(thread); }
      CloseHandle(thread);
      // Threads parked in the kernel's idle waits say nothing: keep those with one of our modules or the game's.
      if(frames.find(".dll+")==std::string::npos && frames.find(".exe+")==std::string::npos) continue;
      xess_mod::WriteLog("vk_hang_thread","\"thread\":"+std::to_string(entry.th32ThreadID)+",\"base_presents\":"
          +std::to_string(last)+",\"stack\":["+frames+"]");
      ++logged;
    }
    CloseHandle(threads);
    FlushLog();
  }
}

void InstallDiagnostics() noexcept {
  static std::atomic<bool> added{};
  if(added.exchange(true)) return;
  const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
  if(!GetPrivateProfileIntW(L"Diagnostics",L"VulkanFaults",0,ini.c_str())) return;
  AddVectoredExceptionHandler(0,&NullCall);
  if(const auto thread=CreateThread(nullptr,0,&HangWatch,nullptr,0,nullptr)) CloseHandle(thread);
}

bool Enabled() noexcept {
  static const bool enabled=[] {
    if(backend::Read()!=backend::Kind::XeMfg) return false;
    const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
    return GetPrivateProfileIntW(L"XeFG",L"Vulkan",1,ini.c_str())!=0;
  }();
  return enabled;
}

bool BeforeInstance(uint32_t& apiVersion) noexcept {
  if(!Enabled()) return false;
  xess_vulkan::Configure(false,true,frames.load());
  {
    const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
    const int copyMode=int(GetPrivateProfileIntW(L"XeFG",L"VulkanInputCopy",1,ini.c_str()));
    const bool copy=copyMode!=0;
    xess_vulkan::SetInputCopy(copyMode);
    if(copyMode>1) xess_mod::WriteLog("vk_input_copy","\"mode\":"+std::to_string(copyMode));
    xess_vulkan::SetInputView(&InputView);
    const bool generate=GetPrivateProfileIntW(L"XeFG",L"VulkanGenerate",1,ini.c_str())!=0;
    xess_vulkan::SetGenerate(generate);
    const bool metering=GetPrivateProfileIntW(L"XeFG",L"VulkanPresentMetering",1,ini.c_str())!=0;
    xess_vulkan::SetPresentMetering(metering);
    xess_mod::WriteLog("vk_present_metering_config","\"enabled\":"+std::string(metering?"true":"false"));
    if(!generate) xess_mod::WriteLog("vk_generate","\"enabled\":false");
    static std::atomic<bool> logged{};
    if(!copy && !logged.exchange(true)) xess_mod::WriteLog("vk_input_copy","\"enabled\":false");
  }
  Install();
  // ReShade's create_device writes Vulkan versions as major<<12 | minor<<8
  // (1.3 = 0x1300), not VK_MAKE_API_VERSION.
  const auto [major,minor,raised]=policy::ReadInstanceVersion(apiVersion);
  if(raised) apiVersion=0x1300;
  instanceApi=VK_MAKE_API_VERSION(0,raised?1:major,raised?3:minor,0);
  xess_mod::WriteLog("vk_instance_version","\"requested\":\""+std::to_string(major)+"."+std::to_string(minor)
      +"\",\"raised\":"+(raised?"true":"false"));
  return raised;
}

bool GameUsesVulkan() noexcept { return gameVulkan.load(); }
bool InstanceSeen() noexcept { return instanceApi.load()!=0; }

PanelStatus Status() noexcept {
  PanelStatus out;
  if(!InstanceSeen()) return out;
  try {
    const auto snapshot=xess_vulkan::ReadSnapshot();
    out.game=true; out.active=snapshot.active; out.frames=snapshot.frames;
    const bool fresh=snapshot.fpsAgeMs<3000;
    out.baseFpsMilli=fresh?snapshot.baseFpsMilli:0; out.outputFpsMilli=fresh?snapshot.outputFpsMilli:0;
    out.inputFrames=snapshot.inputFrames; out.reason=snapshot.reason;
  } catch(...) {}
  return out;
}

void SetFrames(uint32_t multiplier) noexcept {
  if(multiplier<2 || multiplier>6) return;
  frames=multiplier;
  if(installed) xess_vulkan::Configure(false,true,multiplier);
}

void SetGameFrameGeneration(bool on) noexcept {
  if(installed) xess_vulkan::SetGameFrameGeneration(on);
}

void DiscoverNgx() noexcept {
  if(!installed) return;
  static std::mutex once; std::lock_guard guard(once);
  if(ngxEvaluate) return;
  const HMODULE module=GetModuleHandleW(L"_nvngx.dll");
  if(!module) return;
  HMODULE pinned{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pinned)) return;
  const auto entry=[&](const char* function) { return reinterpret_cast<void*>(GetProcAddress(module,function)); };
  const bool hooked[]={
    Hook(entry("NVSDK_NGX_VULKAN_CreateFeature"),reinterpret_cast<void*>(&NgxCreate),reinterpret_cast<void**>(&ngxCreate)),
    Hook(entry("NVSDK_NGX_VULKAN_CreateFeature1"),reinterpret_cast<void*>(&NgxCreate1),reinterpret_cast<void**>(&ngxCreate1)),
    Hook(entry("NVSDK_NGX_VULKAN_ReleaseFeature"),reinterpret_cast<void*>(&NgxRelease),reinterpret_cast<void**>(&ngxRelease)),
    Hook(entry("NVSDK_NGX_VULKAN_EvaluateFeature"),reinterpret_cast<void*>(&NgxEvaluate),reinterpret_cast<void**>(&ngxEvaluate))};
  uint32_t mask{}; for(unsigned i=0;i<4;++i) mask|=uint32_t(hooked[i])<<i;
  xess_mod::WriteLog("vk_ngx_hooks","\"mask\":"+std::to_string(mask));
}
}
