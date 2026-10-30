// SPDX-License-Identifier: MIT
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/xemfg/native_gpu.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using Microsoft::WRL::ComPtr;
using namespace mfgunlock::xemfg::native;

int main() {
  ComPtr<IDXGIFactory4> factory;
  CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
  ComPtr<IDXGIAdapter> warp;
  CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
  ComPtr<ID3D12Device> device;
  CHECK(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
  ComPtr<ID3D12Fence> fence;
  CHECK(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));
  const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  CHECK(event);

  CHECK(!WaitFence(fence.Get(),1,event,20)); // not reached: times out
  // The timed-out wait's late completion sets the event; the wait for a later
  // value must not take that for its own completion.
  CHECK(SUCCEEDED(fence->Signal(1)));
  CHECK(!WaitFence(fence.Get(),2,event,20));
  CHECK(SUCCEEDED(fence->Signal(2)));
  CHECK(WaitFence(fence.Get(),2,event,20));
  CHECK(WaitFence(fence.Get(),1,event,0)); // already reached: no wait, even with no time left
  CHECK(!WaitFence(fence.Get(),3,nullptr,20)); // no event: refused, no hang
  CloseHandle(event);
  std::cout<<"PASS: fence wait rechecks the value after a stale event\n";
  return 0;
}
