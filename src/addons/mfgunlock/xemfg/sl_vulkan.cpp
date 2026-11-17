// SPDX-License-Identifier: MIT
#include "sl_vulkan.hpp"
#include "vendor/xess_mod.h"
#include "../backend_selection.hpp"
#include "../force_fg.hpp"
#include "../sl_module.hpp"
#include "../swapchain_format_guard.hpp"
#include <reshade.hpp>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_params.h>
#include <DirectXMath.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mfgunlock::xemfg::native::vulkan {
bool VulkanLayerHost() noexcept;
}

namespace mfgunlock::xemfg::native::slvk {
namespace {
// The XeMFG log only runs with the XeMFG backend: the host's one-time events also go to ReShade.log
// (the periodic status does not).
void Log(const std::string& event, const std::string& fields, bool reshadeLog = true) {
  xess_mod::WriteLog(event.c_str(), fields);
  if (reshadeLog) reshade::log::message(reshade::log::level::info, ("mfgunlock: " + event + " " + fields).c_str());
}
std::string Quote(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) { if (c == '"' || c == '\\') out += '\\'; if (static_cast<unsigned char>(c) >= 0x20) out += c; }
  return out + "\"";
}

std::atomic<bool> active{};
std::mutex reportLock;
std::string report = "not started";
void SetReport(const std::string& text) {
  { std::lock_guard guard(reportLock); report = text; }
  Log("sl_vulkan", "\"state\":" + Quote(text));
  // The log is buffered; a hang or crash right after a state change would lose it.
  using Flush = bool (*)(uint32_t);
  if (const auto flush = reinterpret_cast<Flush>(GetProcAddress(xess_mod::Self(), "MFGAXeLogFlush"))) flush(200);
}

HMODULE module{};
PFun_slInit* init{};
PFun_slSetVulkanInfo* setVulkanInfo{};
PFun_slGetFeatureRequirements* requirements{};
PFun_slGetNewFrameToken* newToken{};
PFun_slSetTagForFrame* setTags{};
PFun_slSetConstants* setConstants{};
PFun_slGetFeatureFunction* featureFunction{};
PFun_slIsFeatureLoaded* featureLoaded{};
PFun_slDLSSGSetOptions* dlssgOptions{};
PFun_slDLSSGGetState* dlssgState{};
PFun_slReflexSetOptions* reflexOptions{};
PFun_slReflexSleep* reflexSleep{};
PFun_slPCLSetMarker* marker{};
PFN_vkGetDeviceProcAddr slGdpa{};

// Streamline's proxies for the device it was given (sl_hooks.h, the mandatory Vulkan list).
VkDevice slDevice{};
VkInstance slInstance{};
// The game's surfaces and their windows (Streamline finds a swap chain's window through the surfaces it saw created).
std::mutex surfaceLock;
std::map<VkSurfaceKHR, HWND> surfaceWindows;
std::map<VkSurfaceKHR, bool> surfacesShown;
thread_local VkSurfaceKHR showing = VK_NULL_HANDLE;
PFN_vkCreateSwapchainKHR proxyCreateSwapchain{};
PFN_vkDestroySwapchainKHR proxyDestroySwapchain{};
PFN_vkGetSwapchainImagesKHR proxyGetSwapchainImages{};
PFN_vkAcquireNextImageKHR proxyAcquireNextImage{};
PFN_vkQueuePresentKHR proxyQueuePresent{};
PFN_vkDeviceWaitIdle proxyDeviceWaitIdle{};
std::atomic<bool> deviceReady{};
// 1.10.1 (plan item 7): Streamline steps aside for the session (swap chain format changed under it).
std::atomic<bool> bypassed{};
bool Routing() { return deviceReady.load() && !bypassed.load(); }
std::mutex queueLock;
std::map<VkQueue, VkDevice> queueDevice;  // filled lazily: Streamline's present proxy takes any queue of its device

// ------------------------------------------------------------- the callers
std::mutex callerLock;
std::map<HMODULE, bool> callerModules;
bool StreamlineModule(HMODULE m) {
  wchar_t path[MAX_PATH]{};
  if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
  std::wstring text = path;
  for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
  const auto leaf = text.substr(text.find_last_of(L'\\') + 1);
  return text.find(L"\\mfgunlock-streamline\\") != std::wstring::npos || leaf.rfind(L"sl.", 0) == 0;
}
}  // namespace

bool IsStreamlineCaller(const void* address) noexcept {
  if (!module || !address) return false;
  HMODULE m{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &m) || !m)
    return false;
  try {
    std::lock_guard guard(callerLock);
    const auto found = callerModules.find(m);
    if (found != callerModules.end()) return found->second;
    const bool sl = StreamlineModule(m);
    callerModules[m] = sl;
    return sl;
  } catch (...) { return false; }
}

bool Wanted() noexcept {
  static const bool wanted = [] {
    if (backend::Read() != backend::Kind::DlssG || !vulkan::VulkanLayerHost()) return false;
    const auto ini = backend::Directory() / L"MFGAdaUnlock.ini";
    const bool add = GetPrivateProfileIntW(L"DLSSG", L"AddFrameGeneration", 0, ini.c_str()) != 0 ||
                     GetPrivateProfileIntW(L"DLSSG", L"HostStreamline", 0, ini.c_str()) != 0;
    // [DLSSG] HostVulkan=0 keeps the Vulkan route off while the D3D12 one stays on.
    return add && GetPrivateProfileIntW(L"DLSSG", L"HostVulkan", 1, ini.c_str()) != 0;
  }();
  return wanted;
}
bool Active() noexcept { return active.load(std::memory_order_acquire); }

bool Start() noexcept {
  // Once, and never waiting: slInit's DLSS-G plugin creates its own Vulkan instance to probe the driver, ReShade's
  // layer runs the addon's create_device again inside it, and a std::call_once here deadlocked RDR2 at startup.
  static std::atomic<int> phase{0};  // 0 not started, 1 starting, 2 done
  int expected = 0;
  if (!phase.compare_exchange_strong(expected, 1)) return Active();
  struct Done { ~Done() { phase = 2; } } done;
  [] {
    try {
      if (!Wanted()) return;
      // 1.10.1 (plan item 1): only a started Streamline (sl.common.dll mapped) is the game's.
      if (slmodule::StartedByAnother()) { SetReport("the game has its own Streamline; not hosting"); return; }
      // ReShade's layer loads before a Vulkan game maps its own Streamline (DOOM: The Dark Ages ships it in
      // streamline\production\): a game that ships sl.interposer.dll is never hosted.
      {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto gameDir = std::filesystem::path(exe).parent_path();
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(gameDir, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
          if (ec) break;
          if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
          const auto& path = it->path();
          if (it->is_directory(ec) && _wcsicmp(path.filename().c_str(), L"MFGUnlock-Streamline") == 0) { it.disable_recursion_pending(); continue; }
          if (_wcsicmp(path.filename().c_str(), L"sl.interposer.dll") == 0) {
            SetReport("the game ships its own Streamline (" + path.parent_path().filename().string() + "); not hosting");
            return;
          }
        }
      }
      const auto folder = backend::Directory() / L"MFGUnlock-Streamline";
      if (!std::filesystem::exists(folder / L"sl.interposer.dll") || !std::filesystem::exists(folder / L"sl.dlss_g.dll")) {
        SetReport("MFGUnlock-Streamline\\sl.interposer.dll or sl.dlss_g.dll is missing beside the addon");
        return;
      }
      module = LoadLibraryExW((folder / L"sl.interposer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!module) { SetReport("sl.interposer.dll did not load (" + std::to_string(GetLastError()) + ")"); return; }
      slmodule::HostLoaded(module);
      const auto get = [](const char* name) { return GetProcAddress(module, name); };
      init = reinterpret_cast<PFun_slInit*>(get("slInit"));
      setVulkanInfo = reinterpret_cast<PFun_slSetVulkanInfo*>(get("slSetVulkanInfo"));
      requirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(get("slGetFeatureRequirements"));
      newToken = reinterpret_cast<PFun_slGetNewFrameToken*>(get("slGetNewFrameToken"));
      setTags = reinterpret_cast<PFun_slSetTagForFrame*>(get("slSetTagForFrame"));
      setConstants = reinterpret_cast<PFun_slSetConstants*>(get("slSetConstants"));
      featureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(get("slGetFeatureFunction"));
      featureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(get("slIsFeatureLoaded"));
      slGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get("vkGetDeviceProcAddr"));
      if (!init || !setVulkanInfo || !requirements || !newToken || !setTags || !setConstants || !featureFunction || !slGdpa) {
        SetReport("sl.interposer.dll lacks the Streamline 2 Vulkan entry points");
        return;
      }
      const std::wstring folderText = folder.wstring(), logs = backend::Directory().wstring();
      const wchar_t* paths[] = {folderText.c_str()};
      const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
      sl::Preferences p{};
      p.pathsToPlugins = paths;
      p.numPathsToPlugins = 1;
      p.pathToLogsAndData = logs.c_str();
      p.featuresToLoad = features;
      p.numFeaturesToLoad = 3;
      p.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
      p.engine = sl::EngineType::eCustom;
      p.engineVersion = "1.10.0";
      p.projectId = forcefg::kHostProjectId;
      p.renderAPI = sl::RenderAPI::eVulkan;
      const sl::Result result = init(p, sl::kSDKVersion);
      if (result != sl::Result::eOk) { SetReport("slInit (Vulkan) failed (" + std::to_string(int(result)) + ")"); return; }
      active = true;
      SetReport("Streamline 2 started for Vulkan; waiting for the game's device");
    } catch (...) { SetReport("exception while starting Streamline"); }
  }();
  return Active();
}

// ------------------------------------------------------------- the device
struct DeviceEdit {
  VkDeviceCreateInfo info{};
  std::vector<const char*> extensions;
  std::vector<VkDeviceQueueCreateInfo> queues;
  std::vector<std::vector<float>> priorities;
  VkPhysicalDeviceVulkan12Features own12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features own13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  // The game's own 1.2/1.3 feature structures, raised in place for the call and restored after it.
  VkPhysicalDeviceVulkan12Features* game12{};
  VkPhysicalDeviceVulkan13Features* game13{};
  VkPhysicalDeviceVulkan12Features saved12{};
  VkPhysicalDeviceVulkan13Features saved13{};
  uint32_t computeFamily{}, computeIndex{}, graphicsFamily{}, graphicsIndex{};
};
void DeviceEditDeleter::operator()(DeviceEdit* edit) const noexcept { delete edit; }

namespace {
// ORs the VkBool32 members of a Vulkan 1.x features structure (everything after sType and pNext).
template <class T> void Raise(T& target, const T& wanted) {
  constexpr size_t offset = offsetof(T, pNext) + sizeof(void*);
  auto* t = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&target) + offset);
  const auto* w = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&wanted) + offset);
  for (size_t i = 0; i < (sizeof(T) - offset) / sizeof(VkBool32); ++i) t[i] = t[i] || w[i];
}
template <class T> bool Any(const T& value) {
  constexpr size_t offset = offsetof(T, pNext) + sizeof(void*);
  const auto* v = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&value) + offset);
  for (size_t i = 0; i < (sizeof(T) - offset) / sizeof(VkBool32); ++i) if (v[i]) return true;
  return false;
}
}  // namespace

DeviceEditPtr EditDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept {
  if (!Active() || !info || !physical) return {};
  try {
    const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        GetProcAddress(loader, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        GetProcAddress(loader, "vkEnumerateDeviceExtensionProperties"));
    if (!families || !enumerate) { SetReport("vulkan-1 lacks the queue or extension queries"); return {}; }
    DeviceEditPtr edit(new DeviceEdit);
    edit->info = *info;
    edit->extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    uint32_t available = 0;
    enumerate(physical, nullptr, &available, nullptr);
    std::vector<VkExtensionProperties> supported(available);
    enumerate(physical, nullptr, &available, supported.data());
    const auto isSupported = [&](const char* name) {
      return std::any_of(supported.begin(), supported.end(), [&](const auto& e) { return !strcmp(e.extensionName, name); });
    };
    uint32_t extraCompute = 0, extraGraphics = 0;
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    std::string added, skipped;
    for (const sl::Feature feature : {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL}) {
      sl::FeatureRequirements r{};
      if (requirements(feature, r) != sl::Result::eOk) continue;
      extraCompute = (std::max)(extraCompute, r.vkNumComputeQueuesRequired);
      extraGraphics = (std::max)(extraGraphics, r.vkNumGraphicsQueuesRequired);
      for (uint32_t i = 0; i < r.vkNumDeviceExtensions; ++i) {
        const char* name = r.vkDeviceExtensions[i];
        if (std::any_of(edit->extensions.begin(), edit->extensions.end(), [&](const char* e) { return !strcmp(e, name); })) continue;
        if (!isSupported(name)) { skipped += std::string(skipped.empty() ? "" : ",") + name; continue; }
        edit->extensions.push_back(name);
        added += std::string(added.empty() ? "" : ",") + name;
      }
      Raise(want12, sl::getVkPhysicalDeviceVulkan12Features(r.vkNumFeatures12, r.vkFeatures12));
      Raise(want13, sl::getVkPhysicalDeviceVulkan13Features(r.vkNumFeatures13, r.vkFeatures13));
    }
    // Features: raised in the game's own structures when it has them, else added at the head of its chain.
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext) {
      if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        edit->game12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(const_cast<VkBaseInStructure*>(node));
      if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
        edit->game13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(const_cast<VkBaseInStructure*>(node));
    }
    const void* head = info->pNext;
    if (Any(want12)) {
      if (edit->game12) { edit->saved12 = *edit->game12; Raise(*edit->game12, want12); }
      else { edit->own12 = want12; edit->own12.pNext = const_cast<void*>(head); head = &edit->own12; }
    }
    if (Any(want13)) {
      if (edit->game13) { edit->saved13 = *edit->game13; Raise(*edit->game13, want13); }
      else { edit->own13 = want13; edit->own13.pNext = const_cast<void*>(head); head = &edit->own13; }
    }
    edit->info.pNext = head;
    // Queues: Streamline's come after the game's in the same family.
    uint32_t count = 0;
    families(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    families(physical, &count, props.data());
    uint32_t graphics = UINT32_MAX, compute = UINT32_MAX;
    for (uint32_t f = 0; f < count; ++f) {
      if (graphics == UINT32_MAX && (props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) graphics = f;
      if (compute == UINT32_MAX && (props[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) compute = f;
    }
    if (compute == UINT32_MAX) compute = graphics;
    if (graphics == UINT32_MAX) { SetReport("no graphics queue family"); return {}; }
    edit->queues.assign(info->pQueueCreateInfos, info->pQueueCreateInfos + info->queueCreateInfoCount);
    edit->priorities.reserve(edit->queues.size() + 2);
    const auto addQueues = [&](uint32_t family, uint32_t extra, uint32_t& firstIndex) -> bool {
      if (extra == 0) return true;
      for (auto& q : edit->queues) {
        if (q.queueFamilyIndex != family || q.flags) continue;
        if (q.queueCount + extra > props[family].queueCount) return false;
        firstIndex = q.queueCount;
        auto& p = edit->priorities.emplace_back(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);
        p.resize(q.queueCount + extra, 1.0f);
        q.queueCount += extra;
        q.pQueuePriorities = p.data();
        return true;
      }
      if (extra > props[family].queueCount) return false;
      firstIndex = 0;
      auto& p = edit->priorities.emplace_back(extra, 1.0f);
      VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      q.queueFamilyIndex = family;
      q.queueCount = extra;
      q.pQueuePriorities = p.data();
      edit->queues.push_back(q);
      return true;
    };
    // The game's queues, for the log (RDR2 asks for every queue of a family).
    std::string gameQueues;
    for (const auto& q : edit->queues)
      gameQueues += std::string(gameQueues.empty() ? "" : ",") + std::to_string(q.queueFamilyIndex) + ":" +
                    std::to_string(q.queueCount) + "/" + std::to_string(props[q.queueFamilyIndex].queueCount);
    // Each queue Streamline asks for: its preferred family, then any family with the capability; when every one
    // is full, the game's last queue in the preferred family is shared (Streamline submits on it).
    std::string shared;
    const auto place = [&](VkQueueFlags need, uint32_t preferred, uint32_t extra, uint32_t& family, uint32_t& index,
                           const char* what) {
      family = preferred;
      if (extra == 0) return;
      std::vector<uint32_t> order{preferred};
      for (uint32_t f = 0; f < count; ++f)
        if (f != preferred && (props[f].queueFlags & need) == need) order.push_back(f);
      for (const uint32_t f : order)
        if (addQueues(f, extra, index)) { family = f; return; }
      uint32_t used = 0;
      for (const auto& q : edit->queues) if (q.queueFamilyIndex == preferred && !q.flags) used = (std::max)(used, q.queueCount);
      index = used ? used - 1 : 0;
      shared += std::string(shared.empty() ? "" : ",") + what;
    };
    place(VK_QUEUE_COMPUTE_BIT, compute, extraCompute, edit->computeFamily, edit->computeIndex, "compute");
    place(VK_QUEUE_GRAPHICS_BIT, graphics, extraGraphics, edit->graphicsFamily, edit->graphicsIndex, "graphics");
    compute = edit->computeFamily;
    graphics = edit->graphicsFamily;
    edit->info.queueCreateInfoCount = static_cast<uint32_t>(edit->queues.size());
    edit->info.pQueueCreateInfos = edit->queues.data();
    edit->info.enabledExtensionCount = static_cast<uint32_t>(edit->extensions.size());
    edit->info.ppEnabledExtensionNames = edit->extensions.data();
    Log("sl_vulkan_device", "\"added_extensions\":" + Quote(added) + ",\"unsupported\":" + Quote(skipped) +
        ",\"compute\":[" + std::to_string(compute) + "," + std::to_string(extraCompute) + "],\"graphics\":[" +
        std::to_string(graphics) + "," + std::to_string(extraGraphics) + "],\"indices\":[" + std::to_string(edit->computeIndex) +
        "," + std::to_string(edit->graphicsIndex) + "],\"game_queues\":" + Quote(gameQueues) + ",\"shared\":" + Quote(shared) +
        ",\"features12\":" + (Any(want12) ? "true" : "false") +
        ",\"features13\":" + (Any(want13) ? "true" : "false"));
    return edit;
  } catch (...) { SetReport("exception while editing the device"); return {}; }
}
const VkDeviceCreateInfo* Edited(DeviceEdit* edit, const VkDeviceCreateInfo* fallback) noexcept {
  return edit ? &edit->info : fallback;
}
void Restore(DeviceEdit* edit) noexcept {
  if (!edit) return;
  if (edit->game12) { const void* next = edit->game12->pNext; *edit->game12 = edit->saved12; edit->game12->pNext = const_cast<void*>(next); edit->game12 = nullptr; }
  if (edit->game13) { const void* next = edit->game13->pNext; *edit->game13 = edit->saved13; edit->game13->pNext = const_cast<void*>(next); edit->game13 = nullptr; }
}

void DeviceCreated(DeviceEdit* edit, VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept {
  if (!edit || !Active() || deviceReady.load()) return;
  try {
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueIndex = edit->computeIndex;
    info.computeQueueFamily = edit->computeFamily;
    info.graphicsQueueIndex = edit->graphicsIndex;
    info.graphicsQueueFamily = edit->graphicsFamily;
    const sl::Result result = setVulkanInfo(info);
    if (result != sl::Result::eOk) { SetReport("slSetVulkanInfo failed (" + std::to_string(int(result)) + ")"); return; }
    slDevice = device;
    slInstance = instance;
    const auto proxy = [&](const char* name) { return slGdpa(device, name); };
    proxyCreateSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(proxy("vkCreateSwapchainKHR"));
    proxyDestroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(proxy("vkDestroySwapchainKHR"));
    proxyGetSwapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(proxy("vkGetSwapchainImagesKHR"));
    proxyAcquireNextImage = reinterpret_cast<PFN_vkAcquireNextImageKHR>(proxy("vkAcquireNextImageKHR"));
    proxyQueuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(proxy("vkQueuePresentKHR"));
    proxyDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(proxy("vkDeviceWaitIdle"));
    if (!proxyCreateSwapchain || !proxyQueuePresent || !proxyAcquireNextImage || !proxyGetSwapchainImages) {
      SetReport("sl.interposer gave no swap chain proxies");
      return;
    }
    bool loaded = false;
    if (featureLoaded) featureLoaded(sl::kFeatureDLSS_G, loaded);
    void* fn = nullptr;
    if (featureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn) == sl::Result::eOk) dlssgOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(fn);
    fn = nullptr;
    if (featureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn) == sl::Result::eOk) dlssgState = reinterpret_cast<PFun_slDLSSGGetState*>(fn);
    fn = nullptr;
    if (featureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk) reflexOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fn);
    fn = nullptr;
    if (featureFunction(sl::kFeatureReflex, "slReflexSleep", fn) == sl::Result::eOk) reflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fn);
    fn = nullptr;
    if (featureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk) marker = reinterpret_cast<PFun_slPCLSetMarker*>(fn);
    if (!loaded || !dlssgOptions) { SetReport("Streamline started, but DLSS-G did not load on this GPU or driver"); return; }
    if (reflexOptions) {
      sl::ReflexOptions reflex{};
      reflex.mode = sl::ReflexMode::eLowLatency;
      reflexOptions(reflex);
    }
    deviceReady = true;
    SetReport("the game's Vulkan device is Streamline's; waiting for its swap chain");
  } catch (...) { SetReport("exception at slSetVulkanInfo"); }
}

// ------------------------------------------------------------- frames (as sl_host.hpp)
namespace {
std::mutex framesLock;
uint32_t nextIndex = 1, pending[8]{}, pendingCount = 0, presenting = 0, slept = 0;
std::atomic<uint64_t> evaluations{}, tagged{}, presents{};
std::atomic<bool> optionsOn{}, swapchainSeen{};
sl::FrameToken* TokenFor(uint32_t index) {
  sl::FrameToken* t = nullptr;
  if (!newToken || newToken(t, &index) != sl::Result::eOk) return nullptr;
  return t;
}
void Mark(sl::PCLMarker which, uint32_t index) {
  if (!marker || !index) return;
  if (sl::FrameToken* t = TokenFor(index)) marker(which, *t);
}
uint32_t BeginFrame() {
  uint32_t index;
  bool markStart;
  {
    std::lock_guard guard(framesLock);
    index = nextIndex++;
    if (pendingCount == std::size(pending)) { for (uint32_t i = 1; i < pendingCount; ++i) pending[i - 1] = pending[i]; --pendingCount; }
    pending[pendingCount++] = index;
    markStart = index != slept;
  }
  if (markStart) Mark(sl::PCLMarker::eSimulationStart, index);
  Mark(sl::PCLMarker::eSimulationEnd, index);
  Mark(sl::PCLMarker::eRenderSubmitStart, index);
  return index;
}
uint32_t PresentFrame() {
  std::lock_guard guard(framesLock);
  if (pendingCount == 0) return presenting = 0;
  while (pendingCount > 2) { for (uint32_t i = 1; i < pendingCount; ++i) pending[i - 1] = pending[i]; --pendingCount; }
  presenting = pending[0];
  for (uint32_t i = 1; i < pendingCount; ++i) pending[i - 1] = pending[i];
  --pendingCount;
  return presenting;
}
std::mutex featuresLock;
std::map<const NVSDK_NGX_Handle*, int> createFlags;
}  // namespace

// ------------------------------------------------------------- proxies
void SurfaceCreated(VkSurfaceKHR surface, HWND window) noexcept {
  try { std::lock_guard guard(surfaceLock); surfaceWindows[surface] = window; surfacesShown.erase(surface); } catch (...) {}
}
void SurfaceDestroyed(VkSurfaceKHR surface) noexcept {
  try { std::lock_guard guard(surfaceLock); surfaceWindows.erase(surface); surfacesShown.erase(surface); } catch (...) {}
}
VkSurfaceKHR ShowingSurface() noexcept { return showing; }
// The game's surface predates Streamline's setup: Streamline's vkCreateWin32SurfaceKHR is called with its window,
// and native_vulkan.cpp's hook answers with the existing surface instead of creating one.
void ShowSurface(VkSurfaceKHR surface) noexcept {
  HWND window{};
  {
    std::lock_guard guard(surfaceLock);
    if (surfacesShown[surface]) return;
    surfacesShown[surface] = true;
    if (const auto it = surfaceWindows.find(surface); it != surfaceWindows.end()) window = it->second;
  }
  const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(GetProcAddress(module, "vkCreateWin32SurfaceKHR"));
  if (!create || !window || !slInstance) {
    Log("sl_vulkan_surface", std::string("\"shown\":false,\"proxy\":") + (create ? "true" : "false") + ",\"window\":" +
        (window ? "true" : "false"));
    return;
  }
  VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  ci.hinstance = GetModuleHandleW(nullptr);
  ci.hwnd = window;
  VkSurfaceKHR shown = VK_NULL_HANDLE;
  showing = surface;
  const VkResult r = create(slInstance, &ci, nullptr, &shown);
  showing = VK_NULL_HANDLE;
  Log("sl_vulkan_surface", "\"result\":" + std::to_string(int(r)) + ",\"same\":" + (shown == surface ? "true" : "false"));
  if (r == VK_SUCCESS && shown != surface && shown != VK_NULL_HANDLE) {
    // Streamline did not go through the hooks: it made its own surface, which is not the swap chain's.
    if (const auto destroy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkDestroySurfaceKHR")))
      destroy(slInstance, shown, nullptr);
  }
}
bool CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator,
                     VkSwapchainKHR* output, VkResult& result) noexcept {
  if (!Routing() || device != slDevice) return false;
  if (info) ShowSurface(info->surface);
  const scguard::Watch watch;
  result = proxyCreateSwapchain(device, info, allocator, output);
  // 1.10.1 (plan item 7): another addon (RenoDX's HDR upgrade) changed the real swap chain's
  // format under Streamline, whose buffers keep the game's: the game would draw in one format
  // and present in another (RDR2 crashed). Streamline steps aside; the game runs without it.
  if (result == VK_SUCCESS && watch.Changed()) {
    if (proxyDestroySwapchain) proxyDestroySwapchain(device, *output, allocator);
    *output = VK_NULL_HANDLE;
    bypassed = true;
    SetReport("not used: another addon changed the swap chain's format under Streamline (" + watch.Text() +
              "; RenoDX's HDR upgrade?). Use the game's own HDR, or turn the other addon's swap chain upgrade off");
    return false;
  }
  if (result == VK_SUCCESS && !swapchainSeen.exchange(true))
    SetReport("the game's swap chain is Streamline's (" + std::to_string(info ? info->imageExtent.width : 0) + "x" +
              std::to_string(info ? info->imageExtent.height : 0) + ")");
  return true;
}
bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) noexcept {
  if (!Routing() || device != slDevice || !proxyDestroySwapchain) return false;
  proxyDestroySwapchain(device, swapchain, allocator);
  return true;
}
bool GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images, VkResult& result) noexcept {
  if (!Routing() || device != slDevice) return false;
  result = proxyGetSwapchainImages(device, swapchain, count, images);
  return true;
}
bool AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                      uint32_t* index, VkResult& result) noexcept {
  if (!Routing() || device != slDevice) return false;
  result = proxyAcquireNextImage(device, swapchain, timeout, semaphore, fence, index);
  return true;
}
bool DeviceWaitIdle(VkDevice device, VkResult& result) noexcept {
  if (!Routing() || device != slDevice || !proxyDeviceWaitIdle) return false;
  result = proxyDeviceWaitIdle(device);
  return true;
}
bool QueuePresent(VkQueue queue, const VkPresentInfoKHR* info, VkResult& result) noexcept {
  if (!Routing() || !swapchainSeen.load()) return false;
  const uint32_t index = PresentFrame();
  if (index) {
    Mark(sl::PCLMarker::eRenderSubmitEnd, index);
    Mark(sl::PCLMarker::ePresentStart, index);
    presents.fetch_add(1, std::memory_order_relaxed);
    // Every game frame, as on D3D12: the panel's multiplier replaces the count in the addon's wrapper.
    if (optionsOn.load() || tagged.load() > 2) {
      sl::DLSSGOptions options{};
      options.mode = sl::DLSSGMode::eOn;
      options.numFramesToGenerate = 1;
      const sl::Result r = dlssgOptions(sl::ViewportHandle{0}, options);
      if (!optionsOn.exchange(true))
        SetReport(r == sl::Result::eOk ? "DLSS-G turned on from the game's DLSS Super Resolution (Vulkan)"
                                       : "slDLSSGSetOptions failed (" + std::to_string(int(r)) + ")");
    }
    static uint64_t lastTick = 0;
    if (dlssgState && GetTickCount64() - lastTick >= 2000) {
      lastTick = GetTickCount64();
      sl::DLSSGState st{};
      if (dlssgState(sl::ViewportHandle{0}, st, nullptr) == sl::Result::eOk)
        Log("sl_vulkan_status", "\"status\":" + std::to_string(unsigned(st.status)) + ",\"presented_per_frame\":" +
            std::to_string(st.numFramesActuallyPresented) + ",\"presents\":" + std::to_string(presents.load()) +
            ",\"evaluations\":" + std::to_string(evaluations.load()), false);
    }
  }
  result = proxyQueuePresent(queue, info);
  if (index) {
    Mark(sl::PCLMarker::ePresentEnd, index);
    uint32_t next;
    { std::lock_guard guard(framesLock); next = nextIndex; }
    if (reflexSleep) {
      if (sl::FrameToken* t = TokenFor(next)) {
        reflexSleep(*t);
        Mark(sl::PCLMarker::eSimulationStart, next);
        std::lock_guard guard(framesLock);
        slept = next;
      }
    }
  }
  return true;
}

// ------------------------------------------------------------- DLSS SR
void FeatureCreated(const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params) noexcept {
  if (!Active() || !handle || !params) return;
  int flags = 0;
  params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
  std::lock_guard guard(featuresLock);
  createFlags[handle] = flags;
}

void AfterEvaluate(VkCommandBuffer command, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p,
                   uint32_t depthUsage, uint32_t motionUsage) noexcept {
  if (!Routing() || !swapchainSeen.load() || !command || !p) return;
  try {
    int flags = -1;
    {
      std::lock_guard guard(featuresLock);
      if (const auto found = createFlags.find(handle); found != createFlags.end()) flags = found->second;
    }
    void* depthValue{}; void* motionValue{}; void* outputValue{};
    p->Get(NVSDK_NGX_Parameter_Depth, &depthValue);
    p->Get(NVSDK_NGX_Parameter_MotionVectors, &motionValue);
    p->Get(NVSDK_NGX_Parameter_Output, &outputValue);
    const auto* depth = static_cast<const NVSDK_NGX_Resource_VK*>(depthValue);
    const auto* motion = static_cast<const NVSDK_NGX_Resource_VK*>(motionValue);
    const auto* output = static_cast<const NVSDK_NGX_Resource_VK*>(outputValue);
    if (!depth || !motion || !output || depth->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
        motion->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || output->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW)
      return;
    const auto& d = depth->Resource.ImageViewInfo;
    const auto& m = motion->Resource.ImageViewInfo;
    const auto& o = output->Resource.ImageViewInfo;
    if (flags < 0) flags = m.Width < o.Width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
    const auto getUint = [&](const char* name, unsigned fallback) { unsigned v = fallback; p->Get(name, &v); return v; };
    const auto getFloat = [&](const char* name, float fallback) { float v = fallback; p->Get(name, &v); return v; };
    const uint32_t frame = BeginFrame();
    sl::FrameToken* current = TokenFor(frame);
    if (!current) return;
    evaluations.fetch_add(1, std::memory_order_relaxed);
    unsigned width = getUint(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0);
    unsigned height = getUint(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0);
    if (!width || !height) { width = d.Width; height = d.Height; }
    const bool lowRes = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    const bool inverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    const bool jittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
    sl::Extent depthExtent{getUint(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0),
                           getUint(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0), width, height};
    sl::Extent motionExtent{getUint(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0),
                            getUint(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0), lowRes ? width : o.Width,
                            lowRes ? height : o.Height};
    // NGX Vulkan reads its inputs as sampled images; they stay in that layout after the evaluation.
    const auto describe = [](const NVSDK_NGX_ImageViewInfo_VK& v, uint32_t usage) {
      sl::Resource r(sl::ResourceType::eTex2d, v.Image, nullptr, v.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      r.width = v.Width;
      r.height = v.Height;
      r.nativeFormat = static_cast<uint32_t>(v.Format);
      r.mipLevels = v.SubresourceRange.levelCount;
      r.arrayLayers = v.SubresourceRange.layerCount;
      r.usage = usage;
      return r;
    };
    sl::Resource depthResource = describe(d, depthUsage);
    sl::Resource motionResource = describe(m, motionUsage);
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &depthExtent),
        sl::ResourceTag(&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &motionExtent)};
    const sl::Result tagResult = setTags(*current, sl::ViewportHandle{0}, tags, 2, reinterpret_cast<sl::CommandBuffer*>(command));
    static std::atomic<int> logged{};
    if (logged.fetch_add(1) < 3)
      Log("sl_vulkan_tags", "\"result\":" + std::to_string(int(tagResult)) + ",\"depth\":\"" + std::to_string(d.Width) + "x" +
          std::to_string(d.Height) + "/" + std::to_string(int(d.Format)) + "\",\"motion\":\"" + std::to_string(m.Width) + "x" +
          std::to_string(m.Height) + "/" + std::to_string(int(m.Format)) + "\",\"flags\":" + std::to_string(flags));
    if (tagResult != sl::Result::eOk) return;
    // No camera in NGX: as sl_host.hpp (a 60 degree projection, identity view, camera motion in the vectors,
    // zero orientation vectors so DLSS-G's automatic scene change detection stays off).
    constexpr float kFov = 1.0471976f, kNear = 0.1f, kFar = 10000.f;
    const float aspect = float(width) / float(height);
    DirectX::XMFLOAT4X4 projection, inverse, identity;
    const auto proj = DirectX::XMMatrixPerspectiveFovLH(kFov, aspect, inverted ? kFar : kNear, inverted ? kNear : kFar);
    DirectX::XMStoreFloat4x4(&projection, proj);
    DirectX::XMStoreFloat4x4(&inverse, DirectX::XMMatrixInverse(nullptr, proj));
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
    sl::Constants c{};
    std::memcpy(&c.cameraViewToClip, &projection, sizeof(projection));
    std::memcpy(&c.clipToCameraView, &inverse, sizeof(inverse));
    std::memcpy(&c.clipToLensClip, &identity, sizeof(identity));
    std::memcpy(&c.clipToPrevClip, &identity, sizeof(identity));
    std::memcpy(&c.prevClipToClip, &identity, sizeof(identity));
    c.jitterOffset = {getFloat(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f), getFloat(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f)};
    c.mvecScale = {getFloat(NVSDK_NGX_Parameter_MV_Scale_X, 1.f) / float(motionExtent.width),
                   getFloat(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f) / float(motionExtent.height)};
    c.cameraPinholeOffset = {0.f, 0.f};
    c.cameraPos = {0.f, 0.f, 0.f};
    c.cameraUp = {0.f, 0.f, 0.f};
    c.cameraRight = {0.f, 0.f, 0.f};
    c.cameraFwd = {0.f, 0.f, 0.f};
    c.cameraNear = kNear;
    c.cameraFar = kFar;
    c.cameraFOV = kFov;
    c.cameraAspectRatio = aspect;
    c.depthInverted = inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    int reset = 0;
    p->Get(NVSDK_NGX_Parameter_Reset, &reset);
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.motionVectorsJittered = jittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (setConstants(c, *current, sl::ViewportHandle{0}) == sl::Result::eOk) tagged.fetch_add(1, std::memory_order_relaxed);
  } catch (...) {}
}

int PanelState() noexcept {
  // sl_host.hpp's State values, so the panel shows both hosts alike.
  enum { kOff, kWaiting, kGameStreamline, kMissingFiles, kFailed, kReady, kSwapChain, kGenerating };
  if (!Wanted()) return kOff;
  std::string text;
  try { std::lock_guard guard(reportLock); text = report; } catch (...) { return kWaiting; }
  if (text.find("own Streamline") != std::string::npos) return kGameStreamline;
  if (text.find("missing") != std::string::npos) return kMissingFiles;
  for (const char* bad : {"failed", "did not", "lacks", "exception", "no room", "no graphics", "gave no"})
    if (text.find(bad) != std::string::npos) return kFailed;
  if (!Active()) return text == "not started" ? kWaiting : kFailed;
  if (!Routing() || !swapchainSeen.load()) return kReady;
  return optionsOn.load() && tagged.load() > 0 ? kGenerating : kSwapChain;
}
std::string Describe() {
  std::lock_guard guard(reportLock);
  return report + "; evaluations " + std::to_string(evaluations.load()) + ", tagged " + std::to_string(tagged.load()) +
         ", presents " + std::to_string(presents.load());
}
}  // namespace mfgunlock::xemfg::native::slvk
