// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 diagnostic (XMX video memory): who allocates the video memory. With
// MFG_VRAM_TRACE=1 the addon detours the D3D12 runtime's own resource and heap
// creation entries (shared by every device object, wrapped or not), charges
// each local allocation to the first module on the caller's stack that is not
// the D3D12 runtime, DXGI or ReShade, and follows its lifetime through a
// private-data sentinel. MFGAdaUnlock-vramtrace.txt is rewritten every 5 s.
// Diagnostic only: nothing is installed without the variable.
#include <d3d12.h>
#include <intrin.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "ngx_hook.hpp"

namespace mfgunlock::vramtrace {

inline bool Wanted() {
  static const bool wanted = [] {
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"MFG_VRAM_TRACE", value, 4) != 0 && value[0] == L'1';
  }();
  return wanted;
}

namespace internal {
struct Bucket {
  HMODULE module = nullptr;
  std::string name;
  std::atomic<int64_t> created{0}, live{0}, created_count{0}, live_count{0};
  std::atomic<int64_t> nonlocal_created{0};
};
constexpr int kBuckets = 48;
inline Bucket buckets[kBuckets];
inline std::atomic<int> bucket_count{0};
inline SRWLOCK lock = SRWLOCK_INIT;
struct Big {
  uint64_t tick, bytes;
  std::string chain, what;
};
inline std::vector<Big> big;
inline thread_local int depth = 0;
inline uint64_t start_tick = GetTickCount64();

inline std::string ModuleName(HMODULE module) {
  wchar_t path[MAX_PATH]{};
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return "?";
  return std::filesystem::path(path).filename().string();
}
inline bool Skipped(HMODULE module) {
  static HMODULE self = [] {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleName), &m);
    return m;
  }();
  (void)self;
  if (module == nullptr) return true;
  if (GetProcAddress(module, "ReShadeRegisterAddon") != nullptr) return true;
  std::string name = ModuleName(module);
  std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(tolower(c)); });
  return name == "d3d12.dll" || name == "d3d12core.dll" || name == "d3d12sdklayers.dll" || name == "dxgi.dll" ||
         name == "kernelbase.dll" || name == "ntdll.dll";
}
inline HMODULE ModuleOf(void* address) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(address), &m);
  return m;
}
// owner: first non-skipped module from the hook's caller outwards; chain adds the next two distinct ones.
inline void Attribute(void* caller, HMODULE& owner, std::string& chain) {
  void* frames[48]{};
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  int first = 0;
  for (int i = 0; i < count; ++i)
    if (frames[i] == caller) { first = i; break; }
  owner = nullptr;
  HMODULE previous = nullptr;
  int named = 0;
  for (int i = first; i < count && named < 3; ++i) {
    const HMODULE m = ModuleOf(frames[i]);
    if (m == previous || Skipped(m)) continue;
    previous = m;
    if (owner == nullptr) owner = m;
    chain += (named ? " <- " : "") + ModuleName(m);
    ++named;
  }
  if (owner == nullptr) chain = "?";
}
inline Bucket* BucketFor(HMODULE module) {
  const int n = bucket_count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (buckets[i].module == module) return &buckets[i];
  AcquireSRWLockExclusive(&lock);
  Bucket* found = nullptr;
  const int m = bucket_count.load(std::memory_order_relaxed);
  for (int i = 0; i < m && !found; ++i)
    if (buckets[i].module == module) found = &buckets[i];
  if (!found && m < kBuckets) {
    found = &buckets[m];
    found->module = module;
    found->name = module ? ModuleName(module) : "?";
    bucket_count.store(m + 1, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&lock);
  return found;
}

// Released with the object it is attached to.
class Sentinel final : public IUnknown {
 public:
  Sentinel(Bucket* bucket, int64_t bytes) : bucket_(bucket), bytes_(bytes) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) {
      bucket_->live.fetch_sub(bytes_);
      bucket_->live_count.fetch_sub(1);
      delete this;
    }
    return left;
  }

 private:
  std::atomic<ULONG> refs_{1};
  Bucket* bucket_;
  int64_t bytes_;
};
// {6D3C0C51-2A44-4C8E-9F0B-5E1B7A90E3A1}
inline constexpr GUID kSentinelGuid = {0x6d3c0c51, 0x2a44, 0x4c8e, {0x9f, 0x0b, 0x5e, 0x1b, 0x7a, 0x90, 0xe3, 0xa1}};

inline bool Local(const D3D12_HEAP_PROPERTIES& p) {
  switch (p.Type) {
    case D3D12_HEAP_TYPE_DEFAULT: return true;
    case D3D12_HEAP_TYPE_CUSTOM: return p.MemoryPoolPreference == D3D12_MEMORY_POOL_L1;
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK: return false;
    default: return true;  // GPU_UPLOAD lives in video memory
  }
}

inline void Record(void* caller, REFIID riid, void** object, bool local, uint64_t bytes, const std::string& what) {
  if (object == nullptr || *object == nullptr) return;
  HMODULE owner = nullptr;
  std::string chain;
  Attribute(caller, owner, chain);
  Bucket* bucket = BucketFor(owner);
  if (bucket == nullptr) return;
  if (!local) {
    bucket->nonlocal_created.fetch_add(static_cast<int64_t>(bytes));
    return;
  }
  bucket->created.fetch_add(static_cast<int64_t>(bytes));
  bucket->created_count.fetch_add(1);
  ID3D12Object* tracked = nullptr;
  if (SUCCEEDED(static_cast<IUnknown*>(*object)->QueryInterface(IID_PPV_ARGS(&tracked)))) {
    auto* sentinel = new Sentinel(bucket, static_cast<int64_t>(bytes));
    bucket->live.fetch_add(static_cast<int64_t>(bytes));
    bucket->live_count.fetch_add(1);
    tracked->SetPrivateDataInterface(kSentinelGuid, sentinel);
    sentinel->Release();
    tracked->Release();
  }
  (void)riid;
  if (bytes >= (1u << 20)) {
    AcquireSRWLockExclusive(&lock);
    if (big.size() < 400) big.push_back({GetTickCount64() - start_tick, bytes, chain, what});
    ReleaseSRWLockExclusive(&lock);
  }
}
inline std::string Describe(const D3D12_RESOURCE_DESC& d) {
  char text[160];
  std::snprintf(text, sizeof(text), "res dim%d %llux%u x%u mips%u fmt%d flags0x%x", static_cast<int>(d.Dimension),
                static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, d.MipLevels,
                static_cast<int>(d.Format), static_cast<unsigned>(d.Flags));
  return text;
}
inline uint64_t SizeOf(ID3D12Device* device, const D3D12_RESOURCE_DESC& d) {
  const auto info = device->GetResourceAllocationInfo(0, 1, &d);
  return info.SizeInBytes == UINT64_MAX ? 0 : info.SizeInBytes;
}
inline D3D12_RESOURCE_DESC Narrow(const D3D12_RESOURCE_DESC1& d) {
  D3D12_RESOURCE_DESC r{};
  r.Dimension = d.Dimension;
  r.Alignment = d.Alignment;
  r.Width = d.Width;
  r.Height = d.Height;
  r.DepthOrArraySize = d.DepthOrArraySize;
  r.MipLevels = d.MipLevels;
  r.Format = d.Format;
  r.SampleDesc = d.SampleDesc;
  r.Layout = d.Layout;
  r.Flags = d.Flags;
  return r;
}

struct Depth {
  Depth() { ++depth; }
  ~Depth() { --depth; }
  bool Outer() const { return depth == 1; }
};

using CommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                const D3D12_CLEAR_VALUE*, REFIID, void**);
using HeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using Committed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID,
                                                 void**);
using Heap1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*,
                                            REFIID, void**);
using Committed2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID,
                                                 void**);
using Committed3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
                                                 const DXGI_FORMAT*, REFIID, void**);
inline CommittedFn real_committed = nullptr;
inline HeapFn real_heap = nullptr;
inline Committed1Fn real_committed1 = nullptr;
inline Heap1Fn real_heap1 = nullptr;
inline Committed2Fn real_committed2 = nullptr;
inline Committed3Fn real_committed3 = nullptr;

inline HRESULT STDMETHODCALLTYPE Committed(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                           const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
  Depth d;
  const HRESULT hr = real_committed(self, props, flags, desc, state, clear, riid, out);
  if (SUCCEEDED(hr) && d.Outer() && props && desc)
    Record(_ReturnAddress(), riid, out, Local(*props), SizeOf(self, *desc), Describe(*desc));
  return hr;
}
inline HRESULT STDMETHODCALLTYPE Committed1(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                            const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                            const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                            REFIID riid, void** out) {
  Depth d;
  const HRESULT hr = real_committed1(self, props, flags, desc, state, clear, session, riid, out);
  if (SUCCEEDED(hr) && d.Outer() && props && desc)
    Record(_ReturnAddress(), riid, out, Local(*props), SizeOf(self, *desc), Describe(*desc));
  return hr;
}
inline HRESULT STDMETHODCALLTYPE Committed2(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                            const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
                                            const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                            REFIID riid, void** out) {
  Depth d;
  const HRESULT hr = real_committed2(self, props, flags, desc, state, clear, session, riid, out);
  if (SUCCEEDED(hr) && d.Outer() && props && desc) {
    const auto narrow = Narrow(*desc);
    Record(_ReturnAddress(), riid, out, Local(*props), SizeOf(self, narrow), Describe(narrow));
  }
  return hr;
}
inline HRESULT STDMETHODCALLTYPE Committed3(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                            const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                            const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                            UINT32 casts, const DXGI_FORMAT* formats, REFIID riid, void** out) {
  Depth d;
  const HRESULT hr = real_committed3(self, props, flags, desc, layout, clear, session, casts, formats, riid, out);
  if (SUCCEEDED(hr) && d.Outer() && props && desc) {
    const auto narrow = Narrow(*desc);
    Record(_ReturnAddress(), riid, out, Local(*props), SizeOf(self, narrow), Describe(narrow));
  }
  return hr;
}
inline std::string DescribeHeap(const D3D12_HEAP_DESC& h) {
  char text[96];
  std::snprintf(text, sizeof(text), "heap type%d flags0x%x", static_cast<int>(h.Properties.Type),
                static_cast<unsigned>(h.Flags));
  return text;
}
inline HRESULT STDMETHODCALLTYPE Heap(ID3D12Device* self, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
  Depth d;
  const HRESULT hr = real_heap(self, desc, riid, out);
  if (SUCCEEDED(hr) && d.Outer() && desc)
    Record(_ReturnAddress(), riid, out, Local(desc->Properties), desc->SizeInBytes, DescribeHeap(*desc));
  return hr;
}
inline HRESULT STDMETHODCALLTYPE Heap1(ID3D12Device* self, const D3D12_HEAP_DESC* desc,
                                       ID3D12ProtectedResourceSession* session, REFIID riid, void** out) {
  Depth d;
  const HRESULT hr = real_heap1(self, desc, session, riid, out);
  if (SUCCEEDED(hr) && d.Outer() && desc)
    Record(_ReturnAddress(), riid, out, Local(desc->Properties), desc->SizeInBytes, DescribeHeap(*desc));
  return hr;
}

inline std::string installed_text;
}  // namespace internal

// From Present once the addon is resident: the runtime's entries, through any device object.
inline void Install(ID3D12Device* device) {
  using namespace internal;
  static std::atomic_bool done{false};
  if (!Wanted() || device == nullptr || done.exchange(true)) return;
  void** table = *reinterpret_cast<void***>(device);
  const auto hook = [&](int index, void** real, void* replacement, const char* name, REFIID iid) {
    IUnknown* probe = nullptr;
    if (FAILED(device->QueryInterface(iid, reinterpret_cast<void**>(&probe)))) return;
    void** t = *reinterpret_cast<void***>(probe);
    *real = t[index];
    const std::string where = ModuleName(ModuleOf(*real));
    const bool ok = hook::InstallOne(real, replacement);
    if (!ok) *real = nullptr;
    installed_text += std::string(name) + (ok ? " hooked in " : " FAILED in ") + where + "\n";
    probe->Release();
  };
  (void)table;
  hook(27, reinterpret_cast<void**>(&real_committed), reinterpret_cast<void*>(&Committed), "CreateCommittedResource",
       __uuidof(ID3D12Device));
  hook(28, reinterpret_cast<void**>(&real_heap), reinterpret_cast<void*>(&Heap), "CreateHeap", __uuidof(ID3D12Device));
  hook(53, reinterpret_cast<void**>(&real_committed1), reinterpret_cast<void*>(&Committed1), "CreateCommittedResource1",
       __uuidof(ID3D12Device4));
  hook(54, reinterpret_cast<void**>(&real_heap1), reinterpret_cast<void*>(&Heap1), "CreateHeap1",
       __uuidof(ID3D12Device4));
  hook(69, reinterpret_cast<void**>(&real_committed2), reinterpret_cast<void*>(&Committed2), "CreateCommittedResource2",
       __uuidof(ID3D12Device8));
  hook(76, reinterpret_cast<void**>(&real_committed3), reinterpret_cast<void*>(&Committed3), "CreateCommittedResource3",
       __uuidof(ID3D12Device10));
}

inline std::string Mib(int64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return text;
}

// Every 5 s from Present.
inline void Dump(const std::filesystem::path& directory) {
  using namespace internal;
  if (!Wanted()) return;
  static std::atomic<uint64_t> next{0};
  const uint64_t now = GetTickCount64();
  uint64_t due = next.load();
  if (now < due || !next.compare_exchange_strong(due, now + 5000)) return;
  std::string text = "vram trace at " + std::to_string((now - start_tick) / 1000) + " s\n" + installed_text +
                     "module | local live MiB (count) | local created MiB (count) | non-local created MiB\n";
  const int n = bucket_count.load(std::memory_order_acquire);
  int64_t total = 0;
  for (int i = 0; i < n; ++i) {
    const auto& b = buckets[i];
    total += b.live.load();
    text += b.name + " | " + Mib(b.live.load()) + " (" + std::to_string(b.live_count.load()) + ") | " +
            Mib(b.created.load()) + " (" + std::to_string(b.created_count.load()) + ") | " +
            Mib(b.nonlocal_created.load()) + "\n";
  }
  text += "total local live " + Mib(total) + " MiB\nallocations of 1 MiB or more (ms since load, MiB, caller chain, what):\n";
  AcquireSRWLockShared(&lock);
  for (const auto& e : big)
    text += std::to_string(e.tick) + " " + Mib(static_cast<int64_t>(e.bytes)) + " " + e.chain + " | " + e.what + "\n";
  ReleaseSRWLockShared(&lock);
  if (FILE* f = _wfopen((directory / L"MFGAdaUnlock-vramtrace.txt").c_str(), L"w")) {
    fputs(text.c_str(), f);
    fclose(f);
  }
}

}  // namespace mfgunlock::vramtrace
