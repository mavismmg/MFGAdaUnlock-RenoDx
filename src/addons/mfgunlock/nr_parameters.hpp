// SPDX-License-Identifier: MIT
#pragma once
// 1.12.0 (DLSS 5): an NGX parameter block of MFG Unlock's own, for NVIDIA's neural rendering snippet.
//
// The snippet reads its settings and resources through NVSDK_NGX_Parameter's virtual Get methods, the
// interface the NGX SDK declares. A block of our own means the game's NGX (its DLSS Super Resolution,
// initialized by the game on the same device) is never touched: no Init of the driver's NGX core under
// MFG Unlock's identity, no parameter block allocated from it. Values keep the type they were written
// with; a Get of another numeric type converts, as NGX's own block does. Every name the snippet asks for
// is remembered, for diagnostics.
#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <variant>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

namespace mfgunlock::nr {

class Parameters final : public NVSDK_NGX_Parameter {
 public:
  using Value = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource*, ID3D12Resource*,
                             void*>;

  void Set(const char* name, unsigned long long v) override { Put(name, v); }
  void Set(const char* name, float v) override { Put(name, v); }
  void Set(const char* name, double v) override { Put(name, v); }
  void Set(const char* name, unsigned int v) override { Put(name, v); }
  void Set(const char* name, int v) override { Put(name, v); }
  void Set(const char* name, ID3D11Resource* v) override { Put(name, v); }
  void Set(const char* name, ID3D12Resource* v) override { Put(name, v); }
  void Set(const char* name, void* v) override { Put(name, v); }

  NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, float* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, double* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, int* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override { return Pointer(name, out); }
  NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override { return Pointer(name, out); }
  NVSDK_NGX_Result Get(const char* name, void** out) const override { return Pointer(name, out); }

  void Reset() override {
    std::lock_guard guard(lock_);
    values_.clear();
  }

  // Names the snippet asked for, and whether each was there.
  std::map<std::string, bool> Asked() const {
    std::lock_guard guard(lock_);
    return asked_;
  }

 private:
  template <class T>
  void Put(const char* name, T v) {
    if (name == nullptr) return;
    std::lock_guard guard(lock_);
    values_[name] = v;
  }
  const Value* Find(const char* name) const {
    if (name == nullptr) return nullptr;
    const auto found = values_.find(name);
    asked_[name] = found != values_.end();
    return found == values_.end() ? nullptr : &found->second;
  }
  template <class T>
  NVSDK_NGX_Result Number(const char* name, T* out) const {
    std::lock_guard guard(lock_);
    const Value* v = Find(name);
    if (v == nullptr || out == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    bool ok = true;
    std::visit(
        [&](auto&& x) {
          using X = std::decay_t<decltype(x)>;
          if constexpr (std::is_arithmetic_v<X>) *out = static_cast<T>(x);
          else if constexpr (std::is_same_v<T, unsigned long long>) *out = reinterpret_cast<unsigned long long>(x);
          else ok = false;
        },
        *v);
    return ok ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  template <class P>
  NVSDK_NGX_Result Pointer(const char* name, P** out) const {
    std::lock_guard guard(lock_);
    const Value* v = Find(name);
    if (v == nullptr || out == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    void* raw = nullptr;
    std::visit(
        [&](auto&& x) {
          using X = std::decay_t<decltype(x)>;
          if constexpr (std::is_pointer_v<X>) raw = static_cast<void*>(x);
          else if constexpr (std::is_same_v<X, unsigned long long>) raw = reinterpret_cast<void*>(x);
        },
        *v);
    *out = static_cast<P*>(raw);
    return NVSDK_NGX_Result_Success;
  }

  mutable std::mutex lock_;
  std::map<std::string, Value> values_;
  mutable std::map<std::string, bool> asked_;
};

}  // namespace mfgunlock::nr
