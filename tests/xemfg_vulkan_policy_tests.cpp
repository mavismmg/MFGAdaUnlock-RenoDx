// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/xemfg/vulkan_policy.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using namespace mfgunlock::xemfg::native::vulkan::policy;

int main() {
  // A D32S8 depth (RDR2): depth aspect alone, the image's format.
  auto view=ChooseInputView(true,VK_FORMAT_D32_SFLOAT_S8_UINT,0,VK_FORMAT_D32_SFLOAT_S8_UINT);
  CHECK(view.format==VK_FORMAT_D32_SFLOAT_S8_UINT && view.aspect==VK_IMAGE_ASPECT_DEPTH_BIT);
  // A D24S8 depth declared to NGX as a color view (Indiana Jones).
  view=ChooseInputView(true,VK_FORMAT_D24_UNORM_S8_UINT,0,VK_FORMAT_R32_UINT);
  CHECK(view.format==VK_FORMAT_D24_UNORM_S8_UINT && view.aspect==VK_IMAGE_ASPECT_DEPTH_BIT);
  // A mutable depth still keeps its own format; only color follows NGX.
  view=ChooseInputView(true,VK_FORMAT_D32_SFLOAT,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,VK_FORMAT_R32_SFLOAT);
  CHECK(view.format==VK_FORMAT_D32_SFLOAT && view.aspect==VK_IMAGE_ASPECT_DEPTH_BIT);
  view=ChooseInputView(true,VK_FORMAT_R16G16_UNORM,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,VK_FORMAT_R16G16_SFLOAT);
  CHECK(view.format==VK_FORMAT_R16G16_SFLOAT && view.aspect==VK_IMAGE_ASPECT_COLOR_BIT);
  view=ChooseInputView(true,VK_FORMAT_R16G16_SFLOAT,0,VK_FORMAT_R16G16_UNORM);
  CHECK(view.format==VK_FORMAT_R16G16_SFLOAT && view.aspect==VK_IMAGE_ASPECT_COLOR_BIT);
  // An image created before the hooks: only NGX's description exists.
  view=ChooseInputView(false,VK_FORMAT_UNDEFINED,0,VK_FORMAT_R16G16_SFLOAT);
  CHECK(view.format==VK_FORMAT_R16G16_SFLOAT && view.aspect==VK_IMAGE_ASPECT_COLOR_BIT);

  const VkPresentModeKHR both[]={VK_PRESENT_MODE_FIFO_KHR,VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_IMMEDIATE_KHR};
  const VkPresentModeKHR noImmediate[]={VK_PRESENT_MODE_FIFO_KHR,VK_PRESENT_MODE_MAILBOX_KHR};
  CHECK(HostPresentMode(VK_PRESENT_MODE_MAILBOX_KHR,both,3)==VK_PRESENT_MODE_IMMEDIATE_KHR);
  CHECK(HostPresentMode(VK_PRESENT_MODE_MAILBOX_KHR,noImmediate,2)==VK_PRESENT_MODE_MAILBOX_KHR);
  CHECK(HostPresentMode(VK_PRESENT_MODE_MAILBOX_KHR,nullptr,0)==VK_PRESENT_MODE_MAILBOX_KHR);
  CHECK(HostPresentMode(VK_PRESENT_MODE_FIFO_KHR,both,3)==VK_PRESENT_MODE_FIFO_KHR);

  CHECK(ReadInstanceVersion(0).raised && ReadInstanceVersion(0).major==1 && ReadInstanceVersion(0).minor==0);
  CHECK(ReadInstanceVersion(0x1200).raised);
  CHECK(!ReadInstanceVersion(0x1300).raised && ReadInstanceVersion(0x1300).minor==3);
  CHECK(!ReadInstanceVersion(0x1400).raised);

  CHECK(LimitFps(0)==0);
  CHECK(LimitFps(16667)==59);       // never above the interval's rate
  CHECK(LimitFps(16666)==60);
  CHECK(LimitFps(4300)==232);
  CHECK(LimitFps(2000000)==1);      // a very long interval still limits

  PrivateDataStore store;
  CHECK(store.Get(1,10)==0);
  CHECK(store.Set(1,10,111) && store.Set(1,11,112) && store.Set(2,10,222));
  CHECK(store.Get(1,10)==111 && store.Get(2,10)==222);
  // A chain's entries go with it; a later chain at the same address starts clean.
  store.Forget(1);
  CHECK(store.Get(1,10)==0 && store.Get(1,11)==0 && store.Get(2,10)==222 && store.Size()==1);
  for(uint64_t i=0;store.Size()<PrivateDataStore::kCapacity;++i) CHECK(store.Set(100+i,1,i));
  CHECK(!store.Set(1,1,1));       // full: refused, not lost silently
  CHECK(store.Set(2,10,223));     // an existing entry still updates
  CHECK(store.Get(2,10)==223);

  const void* tableA=&store; const void* tableB=&view;
  struct Dispatchable { const void* table; } device{tableA},command{tableA},other{tableB};
  CHECK(DispatchKey(&device)==DispatchKey(&command));
  CHECK(DispatchKey(&device)!=DispatchKey(&other));
  std::cout<<"PASS: Vulkan adapter decisions\n";
  return 0;
}
