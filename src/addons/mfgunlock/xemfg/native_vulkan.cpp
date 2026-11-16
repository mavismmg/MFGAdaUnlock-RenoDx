// SPDX-License-Identifier: MIT
#include "native_vulkan.hpp"
#include "sl_vulkan.hpp"
#include <deque>
#include <array>
#include "vendor/xvkhost/vk_host.h"
#include "vendor/xvkhost/vk_provider.h"
#include "vendor/xvkhost/xess/ngx_provider.h"
#include "backend_api.hpp"
#include "vulkan_policy.hpp"
#include "display_cap.hpp"
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

namespace mfgunlock::xemfg::native {
bool Hook(void* entry,void* replacement,void** trampoline);
uint32_t DisplayCapIntervalForWindow(HWND window) noexcept;
}

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
// 1.10.0 (item 3): XMX on Vulkan. The host never takes a chain over in this mode (HostPrepared answers false).
std::atomic<bool> providerMode{};
// 1.10.0 (prototype): Streamline hosted for a Vulkan game with DLSS SR only (sl_vulkan.hpp). The host stays off.
std::atomic<bool> streamlineMode{};
std::atomic<uint32_t> frames{4};
std::atomic<uint32_t> instanceApi{0}; // 0: the instance creation was not seen
thread_local int internalDepth{};
struct Internal { Internal() { ++internalDepth; } ~Internal() { --internalDepth; } };

// Calls from this module (the host and the xvk layer) pass straight on: they
// reach these hooks through the loader's exports, never to be taken over.
bool FromSelf(const void* caller) noexcept {
  static std::atomic<uintptr_t> selfBase{},selfEnd{};
  auto base=selfBase.load(std::memory_order_acquire);
  if(!base) {
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(xess_mod::Self());
    if(!dos) return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos)+dos->e_lfanew);
    selfEnd.store(reinterpret_cast<uintptr_t>(dos)+nt->OptionalHeader.SizeOfImage,std::memory_order_release);
    base=reinterpret_cast<uintptr_t>(dos);
    selfBase.store(base,std::memory_order_release);
  }
  const auto address=reinterpret_cast<uintptr_t>(caller);
  return address>=base && address<selfEnd.load(std::memory_order_acquire);
}
// Streamline's own calls (its proxies call the loader's exports) pass on too in its hosted mode.
bool Passed(const void* caller) noexcept {
  return internalDepth>0 || FromSelf(caller) || (streamlineMode.load(std::memory_order_relaxed) && slvk::IsStreamlineCaller(caller));
}
std::atomic<bool> takeoverSeen{};
std::atomic<uint64_t> presentsDone{}; // the game's presents through this hook (hang watch)
std::atomic<VkDevice> inputDevice{}; // the newest host device, for an input image created before the hooks
std::atomic<VkInstance> gameInstance{}; // for the surface's present modes
std::atomic<bool> exclusiveExtension{}; // the game's device enabled VK_EXT_full_screen_exclusive
// 1.7.1 (plan B.3): the game's Reflex limit and the display cap, as on D3D12: the
// longer interval wins, and XeLL holds it for every presented frame.
std::atomic<uint32_t> gameLimitUs{};
std::atomic<HWND> gameWindow{};
std::atomic<uint64_t> limitTick{};
void ApplyFpsLimit() noexcept {
  const auto cap=DisplayCapIntervalForWindow(gameWindow.load());
  xess_vulkan::SetStandaloneFpsLimit(policy::LimitFps(CombinedIntervalUs(gameLimitUs.load(),cap)));
}
void FlushLog();
void PollLatencyReports() noexcept;

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
  // XMX on Vulkan: the device carries the provider's features only; its chains, presents and extension
  // commands stay the game's and the driver's.
  if(providerMode.load() || streamlineMode.load()) return false;
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

VkResult VKAPI_CALL Hook_vkCreateWin32SurfaceKHR(VkInstance,const VkWin32SurfaceCreateInfoKHR*,const VkAllocationCallbacks*,VkSurfaceKHR*);
__declspec(noinline) PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance,const char* name) {
  // XMX provider mode sees the game's instance here (vkEnumeratePhysicalDevices is not hooked there).
  if((providerMode.load() || streamlineMode.load()) && instance && !Passed(_ReturnAddress())) SeeInstance(instance);
  // Streamline may resolve vkCreateWin32SurfaceKHR through the loader while it is shown the game's surface.
  if(slvk::ShowingSurface() && name && !strcmp(name,"vkCreateWin32SurfaceKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateWin32SurfaceKHR);
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
  if(streamlineMode.load() && slvk::Active()) {
    xess_vulkan::DeviceEditPtr edit;
    const VkDeviceCreateInfo* used=info;
    if(providerMode.load() && xess_vulkan::RuntimeFilesFound()) { edit=xess_vulkan::PrepareDevice(physical,info); used=xess_vulkan::EditedInfo(edit.get(),info); }
    auto sl=slvk::EditDevice(physical,used);
    VkResult result=original_vkCreateDevice(physical,slvk::Edited(sl.get(),used),allocator,output);
    slvk::Restore(sl.get());
    if(result!=VK_SUCCESS && sl) {
      xess_mod::WriteLog("sl_vulkan_device_refused","\"result\":"+std::to_string(int(result)));
      sl.reset();
      result=original_vkCreateDevice(physical,used,allocator,output);
    }
    if(result!=VK_SUCCESS && edit) { edit.reset(); result=original_vkCreateDevice(physical,info,allocator,output); }
    if(result==VK_SUCCESS && output && *output) {
      gameVulkan=true;
      if(edit) {
        { std::lock_guard guard(extensionLock); auto& e=deviceExtensions[*output]; e.physical=physical; }
        xess_vulkan::DeviceCreated(std::move(edit),*output);
      }
      if(sl) slvk::DeviceCreated(sl.get(),gameInstance.load(),physical,*output);
      inputDevice=*output;
    }
    return result;
  }
  // 1.8.1: without Intel's pair the host can never start, so the game's device and
  // chains stay exactly as the game asked (the host's own search, wider than the
  // D3D12 route's: a pair in RTXMFG-XeSS still works here).
  if(!xess_vulkan::RuntimeFilesFound()) {
    static std::atomic<bool> logged{};
    if(!logged.exchange(true)) xess_mod::WriteLog("vk_device_native","\"reason\":\"runtime_not_found\"");
    return original_vkCreateDevice(physical,info,allocator,output);
  }
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
  // Streamline being shown the game's existing surface (sl_vulkan.cpp, ShowSurface).
  if(const VkSurfaceKHR existing=slvk::ShowingSurface(); existing && output) { *output=existing; return VK_SUCCESS; }
  const auto result=original_vkCreateWin32SurfaceKHR(instance,info,allocator,output);
  if(!Passed(_ReturnAddress()) && result==VK_SUCCESS && info && output) {
    slvk::SurfaceCreated(*output,info->hwnd);
    SeeInstance(instance);
    gameWindow=info->hwnd;
    xess_vulkan::SurfaceCreated(instance,*output,info->hwnd);
  }
  return result;
}
__declspec(noinline) void VKAPI_CALL Hook_vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks* allocator) {
  if(!Passed(_ReturnAddress())) { xess_vulkan::SurfaceDestroyed(surface); slvk::SurfaceDestroyed(surface); }
  original_vkDestroySurfaceKHR(instance,surface,allocator);
}
__declspec(noinline) void VKAPI_CALL Hook_vkDestroyInstance(VkInstance instance,const VkAllocationCallbacks* allocator) {
  if(!Passed(_ReturnAddress())) { xess_vulkan::InstanceDestroyed(instance); ForgetInstance(instance); }
  original_vkDestroyInstance(instance,allocator);
}
void ForgetPrivateData(VkSwapchainKHR swapchain);
__declspec(noinline) VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator,VkSwapchainKHR* output) {
  if(!Passed(_ReturnAddress()) && streamlineMode.load()) { VkResult r{}; if(slvk::CreateSwapchain(device,info,allocator,output,r)) return r; }
  if(Passed(_ReturnAddress()) || !HostPrepared(device)) return original_vkCreateSwapchainKHR(device,info,allocator,output);
  Internal internal;
  // Full-screen exclusive is disallowed on every chain of the game while the
  // host is on: an exclusive native chain kept the display after the takeover
  // (RDR2 in focus showed its last picture). The mode commands are answered
  // by the hooks below. The game's structures stay untouched (policy::DisallowedChain).
  const VkSwapchainCreateInfoKHR* used=info;
  policy::DisallowedChain chain;
  const auto edit=chain.Edit(info?info->pNext:nullptr);
  VkSwapchainCreateInfoKHR named{};
  if(edit==policy::DisallowedChain::Outcome::Copied) { named=*info; named.pNext=chain.Head(); used=&named; }
  if(edit!=policy::DisallowedChain::Outcome::NoRequest) {
    const bool kept=edit==policy::DisallowedChain::Outcome::UnknownStructure;
    static std::atomic<int> logged{-1};
    if(logged.exchange(int(chain.Requested()))!=int(chain.Requested()))
      xess_mod::WriteLog("vk_swapchain_fullscreen_exclusive","\"requested\":"+std::to_string(int(chain.Requested()))
          +",\"used\":\""+(kept?"requested":"disallowed")+"\""+(kept?",\"unknown_structure\":"+std::to_string(int(chain.Unknown())):""));
  }
  // A game that enables the extension but names no mode (RDR2 in its fullscreen
  // mode) leaves the choice to the driver, which gave its own chain the display
  // for good: the chain is created disallowed explicitly.
  VkSurfaceFullScreenExclusiveInfoEXT disallowed{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,nullptr,
      VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT};
  if(info && edit==policy::DisallowedChain::Outcome::NoRequest && exclusiveExtension.load()) {
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
  if(!Passed(_ReturnAddress()) && streamlineMode.load() && slvk::DestroySwapchain(device,swapchain,allocator)) return;
  if(!Passed(_ReturnAddress())) {
    Internal internal;
    ForgetPrivateData(swapchain);
    if(xess_vulkan::DestroySwapchain(device,swapchain)) return;
  }
  original_vkDestroySwapchainKHR(device,swapchain,allocator);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkGetSwapchainImagesKHR(VkDevice device,VkSwapchainKHR swapchain,uint32_t* count,VkImage* images) {
  VkResult result=VK_SUCCESS;
  if(!Passed(_ReturnAddress()) && streamlineMode.load() && slvk::GetSwapchainImages(device,swapchain,count,images,result)) return result;
  if(!Passed(_ReturnAddress()) && xess_vulkan::GetSwapchainImages(device,swapchain,count,images,result)) return result;
  return original_vkGetSwapchainImagesKHR(device,swapchain,count,images);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR swapchain,uint64_t timeout,
    VkSemaphore semaphore,VkFence fence,uint32_t* index) {
  if(!Passed(_ReturnAddress())) {
    Internal internal;
    VkResult result=VK_SUCCESS;
    if(streamlineMode.load() && slvk::AcquireNextImage(device,swapchain,timeout,semaphore,fence,index,result)) return result;
    if(xess_vulkan::AcquireNextImage(device,swapchain,timeout,semaphore,fence,index,result)) return result;
  }
  return original_vkAcquireNextImageKHR(device,swapchain,timeout,semaphore,fence,index);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR* info,uint32_t* index) {
  if(!Passed(_ReturnAddress()) && info) {
    Internal internal;
    VkResult result=VK_SUCCESS;
    // Streamline has no vkAcquireNextImage2KHR proxy: its vkAcquireNextImageKHR one (single-GPU, device mask ignored).
    if(streamlineMode.load() && slvk::AcquireNextImage(device,info->swapchain,info->timeout,info->semaphore,info->fence,index,result)) return result;
    if(xess_vulkan::AcquireNextImage(device,info->swapchain,info->timeout,info->semaphore,info->fence,index,result)) return result;
  }
  return original_vkAcquireNextImage2KHR(device,info,index);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR* info) {
  if(Passed(_ReturnAddress())) return original_vkQueuePresentKHR(queue,info);
  Internal internal;
  if(streamlineMode.load()) {
    struct Count { ~Count() { presentsDone.fetch_add(1,std::memory_order_relaxed); } } count;
    VkResult result=VK_SUCCESS;
    if(slvk::QueuePresent(queue,info,result)) return result;
    return original_vkQueuePresentKHR(queue,info);
  }
  struct Count { ~Count() { presentsDone.fetch_add(1,std::memory_order_relaxed); } } count;
  if(providerMode.load()) PollLatencyReports();
  // The display's refresh and the panel's display cap, as on D3D12, every 2 s.
  if(const auto tick=GetTickCount64();tick-limitTick.load(std::memory_order_relaxed)>=2000) {
    limitTick.store(tick,std::memory_order_relaxed);
    ApplyFpsLimit();
  }
  VkResult result=VK_SUCCESS;
  if(xess_vulkan::QueuePresent(queue,info,result)) return result;
  result=original_vkQueuePresentKHR(queue,info);
  return xess_vulkan::AfterNativePresent(queue,info,result);
}
__declspec(noinline) VkResult VKAPI_CALL Hook_vkDeviceWaitIdle(VkDevice device) {
  VkResult result=VK_SUCCESS;
  if(!Passed(_ReturnAddress()) && streamlineMode.load() && slvk::DeviceWaitIdle(device,result)) return result;
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
// 1.10.0 (plan XMX Vulkan pacing, P1): the game's Reflex chain, for the driver's per-frame GPU timings.
std::atomic<VkDevice> latencyDevice{};
std::atomic<VkSwapchainKHR> latencyChain{};
void PollLatencyReports() noexcept;
VkResult VKAPI_CALL Hook_vkSetLatencySleepModeNV(VkDevice device,VkSwapchainKHR swapchain,const VkLatencySleepModeInfoNV* info) {
  if(!xess_vulkan::OwnsSwapchain(swapchain)) { latencyDevice=device; latencyChain=swapchain; }
  if(info && gameLimitUs.exchange(info->minimumIntervalUs)!=info->minimumIntervalUs) ApplyFpsLimit();
  if(xess_vulkan::OwnsSwapchain(swapchain)) return VK_SUCCESS;
  const auto original=Extension(device,&Extensions::vkSetLatencySleepModeNV);
  return original?original(device,swapchain,info):VK_ERROR_INITIALIZATION_FAILED;
}
VkResult VKAPI_CALL Hook_vkLatencySleepNV(VkDevice device,VkSwapchainKHR swapchain,const VkLatencySleepInfoNV* info) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) {
    { Internal internal; xess_vulkan::GameLatencySleep(swapchain); }
    const auto signal=Extension(device,&Extensions::vkSignalSemaphore);
    if(!info || !signal) return VK_SUCCESS;
    VkSemaphoreSignalInfo value{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,nullptr,info->signalSemaphore,info->value};
    return signal(device,&value);
  }
  const auto original=Extension(device,&Extensions::vkLatencySleepNV);
  return original?original(device,swapchain,info):VK_ERROR_INITIALIZATION_FAILED;
}
void VKAPI_CALL Hook_vkSetLatencyMarkerNV(VkDevice device,VkSwapchainKHR swapchain,const VkSetLatencyMarkerInfoNV* info) {
  if(xess_vulkan::OwnsSwapchain(swapchain)) {
    if(info) { Internal internal; xess_vulkan::GameLatencyMarker(swapchain,uint32_t(info->marker)); }
    return;
  }
  if(const auto original=Extension(device,&Extensions::vkSetLatencyMarkerNV)) original(device,swapchain,info);
}
// 1.10.0 (plan XMX Vulkan pacing): XMX's work makes each real frame cost more GPU than NVIDIA's generator, and the
// game's Reflex (through Streamline's DLSS-G) aims at the display's refresh / multiplier anyway. When the GPU cannot
// hold that, the queue filled until presentation stalled, every ~2 s in DOOM: The Dark Ages with path tracing (41 real
// frames per second, then 24). The GPU's cost per frame cannot be read back (the driver's GPU span stretches with the
// frame interval: DLSS-G's frames are metered across it), so the pacing watches the outcome instead: the interval
// between real frames from the driver's reports. A run of late frames is that stall; the limit then starts just under
// the rate the game did deliver, backs off 7% at each new stall and creeps up 3% after 8 s without one (AIMD).
namespace pacing_auto {
std::mutex lock;
std::deque<float> interval;           // ms between real frames, the newest last
uint64_t lastPresent = 0, lastStart = 0;
ULONGLONG lastPoll = 0, lastReport = 0, lastLog = 0, lastStall = 0, lastRaise = 0;
double limitFps = 0;                  // real frames per second to hold, 0 none
double stallFps = 0, stallUsual = 0;  // the limit where the last stall happened, and the usual interval then
uint32_t stalls = 0;
std::atomic<uint32_t> publishedUs{0}, costUs{0};
bool Enabled() {
  static const bool enabled =
      GetPrivateProfileIntW(L"DLSSG", L"XmxVulkanPacing", 1, (backend::Directory() / L"MFGAdaUnlock.ini").c_str()) != 0;
  return enabled;
}
double Percentile(std::vector<float> values, double p) {
  if (values.empty()) return 0;
  const size_t k = std::min(values.size() - 1, size_t(p * double(values.size() - 1) + 0.5));
  std::nth_element(values.begin(), values.begin() + k, values.end());
  return values[k];
}
}  // namespace pacing_auto
void PollLatencyReports() noexcept {
  using namespace pacing_auto;
  if (!Enabled()) return;
  const ULONGLONG now = GetTickCount64();
  if (now - lastPoll < 250) return;
  lastPoll = now;
  const VkDevice device = latencyDevice.load(); const VkSwapchainKHR chain = latencyChain.load();
  const auto get = device && chain ? Extension(device, &Extensions::vkGetLatencyTimingsNV) : nullptr;
  try {
    std::lock_guard guard(lock);
    if (get) {
      std::array<VkLatencyTimingsFrameReportNV, 64> reports{};
      for (auto& r : reports) r.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
      VkGetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV, nullptr, uint32_t(reports.size()), reports.data()};
      get(device, chain, &info);
      for (uint32_t i = 0; i < info.timingCount && i < reports.size(); ++i) {
        const auto& r = reports[i];
        if (!r.gpuRenderStartTimeUs || r.presentID <= lastPresent) continue;
        if (lastStart && r.gpuRenderStartTimeUs > lastStart) {
          interval.push_back(float(r.gpuRenderStartTimeUs - lastStart) / 1000.f);
          lastReport = now;
        }
        lastPresent = r.presentID;
        lastStart = r.gpuRenderStartTimeUs;
      }
      while (interval.size() > 160) interval.pop_front();
    }
    if (!lastReport || now - lastReport > 2000) {
      // No reports (no Reflex chain, a menu, a driver without them): no limit.
      limitFps = 0; interval.clear(); lastStart = 0;
    } else if (interval.size() >= 24) {
      // The last ~2 s: a stall is a run of real frames 1.4x longer than the usual one.
      std::vector<float> recent(interval.end() - (std::min<size_t>)(interval.size(), 64), interval.end());
      const double usual = Percentile(recent, 0.25);
      // A loading screen or a scene change (intervals over 250 ms or 3x the usual one) is not a stall and not a rate.
      uint32_t late = 0, counted = 0, outliers = 0; double sum = 0;
      for (float v : recent) {
        if (v > 250.f || v > usual * 3.0) { ++outliers; continue; }
        sum += v; ++counted;
        if (v > usual * 1.4) ++late;
      }
      const double delivered = sum > 0 ? 1000.0 * double(counted) / sum : 0;
      const bool stall = outliers == 0 && late >= 6 && usual > 0 && now - lastStall >= 1000;
      if (stall) {
        ++stalls;
        lastStall = lastRaise = now;
        stallFps = limitFps > 0 ? limitFps : delivered; stallUsual = usual;
        const double floor = (std::max)(15.0, 500.0 / usual);   // never under 15 real fps nor half the usual rate
        limitFps = limitFps == 0 ? (std::min)(delivered, 1000.0 / (usual * 1.15)) * 0.95 : limitFps * 0.93;
        limitFps = (std::max)(limitFps, floor);
        interval.clear();   // judge the new limit on its own frames
      } else if (limitFps > 0 && now - lastStall >= 4000 && now - lastRaise >= 4000) {
        lastRaise = now;
        // Below the last stall's rate for 60 s, unless the scene got clearly lighter (usual interval 10% shorter).
        const bool remembered = stallFps > 0 && now - lastStall < 60000 && !(usual > 0 && usual < stallUsual * 0.9);
        limitFps = remembered ? (std::min)(limitFps * 1.03, (std::max)(limitFps, stallFps * 0.97)) : limitFps * 1.03;
      }
      costUs = usual > 0 ? uint32_t(usual * 1000.0) : 0;
    }
    if (limitFps > 0 && limitFps < 15) limitFps = 15;
    publishedUs = limitFps > 0 ? uint32_t(1e6 / limitFps) : 0;
    if (now - lastLog >= 10000) {
      lastLog = now;
      xess_mod::WriteLog("vk_xmx_pacing", "\"limit_base_fps\":" + std::to_string(limitFps) + ",\"usual_interval_ms\":" +
          std::to_string(costUs.load() / 1000.0) + ",\"frames\":" + std::to_string(interval.size()) + ",\"stalls\":" + std::to_string(stalls));
      stalls = 0;
    }
  } catch (...) {}
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

// 1.10.0: the entries Install() hooked, removed again when ReShade unloads this
// copy (UninstallForUnload). ReShade loads the addon for a Vulkan instance and
// unloads it with that instance: DOOM: The Dark Ages' first instance did, the
// next copy hooked over the freed copy's hooks, and the game executed freed
// code in vkGetInstanceProcAddr (crash record, 2026-11-15).
std::vector<void*> hookedEntries;
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
  hookedEntries.clear();
  for(const auto& e:entries) {
    // 1.10.0: not in XMX provider mode. DOOM: The Dark Ages' Streamline (sl.common
    // 2.14.16) called the hooked export in an endless loop at its DLSS-G plugin
    // startup and the game never opened its window (one core busy, field test
    // 2026-11-15); the instance is seen from vkGetInstanceProcAddr instead.
    if(providerMode.load() && !strcmp(e.name,"vkEnumeratePhysicalDevices")) continue;
    void* target=reinterpret_cast<void*>(GetProcAddress(loader,e.name));
    if(!target || !Hook(target,e.hook,e.original)) missing+=(missing.empty()?"":",")+std::string(e.name);
    else hookedEntries.push_back(target);
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

// ---- 1.10.0 (plan XMX/Vulkan/VRAM, item 3): XMX on Vulkan through dashdogy's provider (native_vulkan.hpp).
// NVSDK_NGX_Handle is opaque to callers (Streamline only passes it back): the address of this record is the handle.
struct ProviderFeature { unsigned int id{}; void* feature{}; };
NVSDK_NGX_Handle* AsHandle(ProviderFeature* f) { return reinterpret_cast<NVSDK_NGX_Handle*>(f); }
std::map<const NVSDK_NGX_Handle*,ProviderFeature*> providerFeatures;  // under featureLock
std::atomic<unsigned int> providerIds{0x7f000000u};
std::atomic<uint64_t> providerEvaluations{};
std::atomic<int32_t> providerCreateResult{1};
std::mutex providerTextLock; std::string providerText;
void NoteProvider(const std::string& text) {
  { std::lock_guard guard(providerTextLock); providerText=text; }
  xess_mod::WriteLog("xmx_vulkan","\"state\":\""+text+"\"");
}
// The provider's feature for DLSS-G, or nullptr to let NVIDIA create its own (a refusal falls back).
NVSDK_NGX_Handle* ProviderCreate(VkDevice device,VkCommandBuffer command,NVSDK_NGX_Parameter* params,NVSDK_NGX_Result& result) {
  if(!providerMode.load() || !device) return nullptr;
  static std::once_flag init;
  std::call_once(init,[&] {
    xess_vulkan::ProviderDevice record{};
    if(!xess_vulkan::FindProviderDevice(device,record)) { NoteProvider("refused: the game's device was not prepared for it"); return; }
    xess_vulkan_provider::VulkanInit(record.instance,record.physical,device,record.gipa,nullptr);
  });
  void* feature{};
  const auto created=static_cast<NVSDK_NGX_Result>(xess_vulkan_provider::VulkanCreateFeature(device,command,params,&feature));
  providerCreateResult.store(int32_t(created));
  if(!NVSDK_NGX_SUCCEED(created) || !feature) {
    char reason[96]{};
    ngx_provider::Refused(reason,sizeof(reason));
    NoteProvider(std::string("refused (")+(reason[0]?reason:"no reason")+"); NVIDIA's frame generation runs instead");
    return nullptr;
  }
  auto* wrapped=new ProviderFeature;
  wrapped->id=providerIds.fetch_add(1);
  wrapped->feature=feature;
  { std::lock_guard guard(featureLock); providerFeatures[AsHandle(wrapped)]=wrapped; features[AsHandle(wrapped)]=NVSDK_NGX_Feature_FrameGeneration; }
  NoteProvider("created; XMX generates DLSS frame generation's frames");
  result=NVSDK_NGX_Result_Success;
  return AsHandle(wrapped);
}
ProviderFeature* FindProvider(const NVSDK_NGX_Handle* handle) {
  std::lock_guard guard(featureLock);
  const auto found=providerFeatures.find(handle);
  return found==providerFeatures.end()?nullptr:found->second;
}
bool InputFeature(NVSDK_NGX_Feature feature) {
  return feature==NVSDK_NGX_Feature_SuperSampling || feature==NVSDK_NGX_Feature_RayReconstruction;
}
void Remember(NVSDK_NGX_Result result,NVSDK_NGX_Feature feature,NVSDK_NGX_Handle** out) {
  if(!NVSDK_NGX_SUCCEED(result) || !out || !*out) return;
  { std::lock_guard guard(featureLock); features[*out]=feature; }
  xess_mod::WriteLog("vk_ngx_feature","\"feature\":"+std::to_string(int(feature))+",\"handle\":"+std::to_string(uintptr_t(*out)));
}
NVSDK_NGX_Result NVSDK_CONV NgxCreate(VkCommandBuffer command,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** out) {
  if(providerMode.load() && feature==NVSDK_NGX_Feature_FrameGeneration)
    NoteProvider("not used: the game created frame generation without naming its device");
  const auto result=ngxCreate(command,feature,params,out);
  Remember(result,feature,out);
  if(streamlineMode.load() && InputFeature(feature) && NVSDK_NGX_SUCCEED(result) && out) slvk::FeatureCreated(*out,params);
  return result;
}
NVSDK_NGX_Result NVSDK_CONV NgxCreate1(VkDevice device,VkCommandBuffer command,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** out) {
  if(feature==NVSDK_NGX_Feature_FrameGeneration && out) {
    NVSDK_NGX_Result provided=NVSDK_NGX_Result_Fail;
    if(auto* handle=ProviderCreate(device,command,params,provided)) { *out=handle; return provided; }
  }
  const auto result=ngxCreate1(device,command,feature,params,out);
  Remember(result,feature,out);
  if(streamlineMode.load() && InputFeature(feature) && NVSDK_NGX_SUCCEED(result) && out) slvk::FeatureCreated(*out,params);
  return result;
}
NVSDK_NGX_Result NVSDK_CONV NgxEvaluate(VkCommandBuffer command,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  if(auto* provided=handle?FindProvider(handle):nullptr) {
    providerEvaluations.fetch_add(1,std::memory_order_relaxed);
    return static_cast<NVSDK_NGX_Result>(xess_vulkan_provider::VulkanEvaluate(command,provided->feature,
        const_cast<NVSDK_NGX_Parameter*>(params)));
  }
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
      if(streamlineMode.load()) {
        // Streamline's hosted mode: the inputs are tagged after DLSS SR ran (they stay valid on this command buffer).
        const auto evaluated=ngxEvaluate(command,handle,params,callback);
        if(NVSDK_NGX_SUCCEED(evaluated)) slvk::AfterEvaluate(command,handle,params,depthImage.usage,motionImage.usage);
        return evaluated;
      }
      const bool captured=usable && xess_vulkan::CaptureDlssEvaluate(command,params);
      if(evaluateLog.fetch_add(1)<4)
        xess_mod::WriteLog("vk_ngx_evaluate","\"feature\":"+std::to_string(feature)+",\"captured\":"+(captured?"true":"false"));
    }
  }
  return ngxEvaluate(command,handle,params,callback);
}
NVSDK_NGX_Result NVSDK_CONV NgxRelease(NVSDK_NGX_Handle* handle) {
  if(auto* provided=handle?FindProvider(handle):nullptr) {
    { std::lock_guard guard(featureLock); providerFeatures.erase(handle); features.erase(handle); }
    const auto released=static_cast<NVSDK_NGX_Result>(xess_vulkan_provider::ReleaseFeature(provided->feature));
    delete provided;
    return released;
  }
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
  for(unsigned i=0,found=0;i<8192 && found<160;++i) {
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
    for(BOOL ok=Thread32First(threads,&entry);ok && logged<256;ok=Thread32Next(threads,&entry)) {
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
    // 1.7.1 (plans B.2, B.4): both off by default until measured in a game.
    const bool gameLatency=GetPrivateProfileIntW(L"XeFG",L"VulkanGameLatency",0,ini.c_str())!=0;
    xess_vulkan::SetGameLatency(gameLatency);
    if(gameLatency) xess_mod::WriteLog("vk_game_latency_config","\"enabled\":true");
    xess_vulkan::SetStandaloneDynamic(GetPrivateProfileIntW(L"XeFG",L"VulkanDynamicMFG",0,ini.c_str())!=0,
        GetPrivateProfileIntW(L"XeFG",L"VulkanDynamicTargetFps",0,ini.c_str()),
        int(GetPrivateProfileIntW(L"XeFG",L"VulkanDynamicRounding",1,ini.c_str())),
        GetPrivateProfileIntW(L"XeFG",L"VulkanDynamicAllow1x",1,ini.c_str())!=0,
        GetPrivateProfileIntW(L"XeFG",L"VulkanDynamicTargetAsCap",0,ini.c_str())!=0);
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

// Explicit unload only (DLL_PROCESS_DETACH with FreeLibrary): vulkan-1's entries
// get their own code back before this copy's memory goes away.
void UninstallForUnload() noexcept {
  if(!installed) return;
  for(void* target:hookedEntries) MH_DisableHook(target);
  hookedEntries.clear();
  installed=false;
}

// 1.10.0: on by default with XMX ([DLSSG] XmxVulkan=0 turns it off), and only in a process where ReShade
// was loaded as the Vulkan layer: in a D3D12 game this mode pinned ReShade and started XeMFG's Intel pair.
std::atomic_bool vulkanLayerHost{false};
void SetVulkanLayerHost(bool layer) noexcept { vulkanLayerHost.store(layer); }
bool VulkanLayerHost() noexcept { return vulkanLayerHost.load(); }
bool XmxProviderSelected() noexcept {
  static const bool selected=[] {
    if(backend::Read()!=backend::Kind::DlssG) return false;
    const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
    return GetPrivateProfileIntW(L"DLSSG",L"XmxGenerator",0,ini.c_str())!=0
        && GetPrivateProfileIntW(L"DLSSG",L"XmxVulkan",1,ini.c_str())!=0;
  }();
  return selected;
}
bool XmxProviderWanted() noexcept { return XmxProviderSelected() && VulkanLayerHost(); }

bool BeginXmxProvider(uint32_t& apiVersion) noexcept {
  if(!XmxProviderWanted()) return false;
  providerMode=true;
  // The host stays off (no chain is taken over); a wanted provider gets the features-only device edit.
  xess_vulkan::Configure(false,false,frames.load());
  xess_vulkan::SetProvider(true);
  // [DLSSG] XmxUiComposition=1: XeFG's own UI composition (off by default; it left frames uninterpolated).
  xess_vulkan_provider::SetUiComposition(
      GetPrivateProfileIntW(L"DLSSG",L"XmxUiComposition",0,(backend::Directory()/L"MFGAdaUnlock.ini").c_str())!=0);
  Install();
  const auto [major,minor,raised]=policy::ReadInstanceVersion(apiVersion);
  if(raised) apiVersion=0x1300;
  instanceApi=VK_MAKE_API_VERSION(0,raised?1:major,raised?3:minor,0);
  xess_mod::WriteLog("xmx_vulkan","\"state\":\"wanted\",\"instance_raised\":"+std::string(raised?"true":"false"));
  NoteProvider("waiting for the game's frame generation");
  return raised;
}

bool BeginStreamlineHost(uint32_t& apiVersion) noexcept {
  if(!slvk::Wanted()) return false;
  streamlineMode=true;
  // The XeMFG host stays off; the provider (XMX) may run inside Streamline's DLSS-G.
  if(!providerMode.load()) xess_vulkan::Configure(false,false,frames.load());
  Install();
  slvk::Start();
  const auto [major,minor,raised]=policy::ReadInstanceVersion(apiVersion);
  if(raised) apiVersion=0x1300;
  instanceApi=VK_MAKE_API_VERSION(0,raised?1:major,raised?3:minor,0);
  return raised;
}
bool StreamlineHostWanted() noexcept { return slvk::Wanted(); }
int StreamlineHostState() noexcept { return streamlineMode.load()?slvk::PanelState():0; }
uint32_t XmxPacingIntervalUs() noexcept { return providerMode.load() ? pacing_auto::publishedUs.load() : 0; }
uint32_t XmxPacingCostUs() noexcept { return providerMode.load() ? pacing_auto::costUs.load() : 0; }
bool XmxPacingEnabled() noexcept { return providerMode.load() && pacing_auto::Enabled(); }
std::string StreamlineHostReport() noexcept { try { return streamlineMode.load()?slvk::Describe():std::string(); } catch(...) { return {}; } }

std::string XmxProviderReport() noexcept {
  if(!providerMode.load()) return {};
  try {
    std::lock_guard guard(providerTextLock);
    return providerText+"; evaluations "+std::to_string(providerEvaluations.load());
  } catch(...) { return {}; }
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
    out.dynamic=snapshot.dynamic; out.dynamicTargetMilli=snapshot.dynamicTargetMilli;
    out.fpsLimit=snapshot.fpsLimit; out.gameLatency=snapshot.gameLatency;
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

}  // namespace mfgunlock::xemfg::native::vulkan

// 1.10.0 (item 3): the host-module calls dashdogy's provider makes (upstream's xess_mod, here on the XMX path).
namespace xess_mod {
namespace { std::atomic<bool> gProviderPort{}; std::atomic<uint64_t> gProviderBatches{}, gProviderMissed{}; }
HMODULE PrepareProvider(ID3D12Device*, std::string& reason) noexcept {
  try {
    std::string why;
    const HMODULE fg=xess_vulkan::ProviderRuntime(why);
    if(!fg) { reason="runtime_"+why; return nullptr; }
    wchar_t path[MAX_PATH]{};
    if(!GetModuleFileNameW(fg,path,MAX_PATH)) { reason="runtime_path"; return nullptr; }
    const auto xell=std::filesystem::path(path).parent_path()/L"libxell.dll";
    const HMODULE ll=GetModuleHandleW(xell.c_str());
    if(!ll) { reason="xell_not_loaded"; return nullptr; }
    // The same ported model and frame-count unlock as XMX on Direct3D 12 (backend.cpp).
    if(!mfgunlock::xemfg::PublishMultiFrameModel(fg,ll,Self(),why)) { reason="model_"+why; return nullptr; }
    gProviderPort=true;
    return fg;
  } catch(...) { reason="exception"; return nullptr; }
}
const char* ProviderModelName() noexcept { return gProviderPort.load()?"port":"stock"; }
void NoteProviderBatch(bool generated) noexcept { (generated?gProviderBatches:gProviderMissed).fetch_add(1); }
}

namespace mfgunlock::xemfg::native::vulkan {
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
