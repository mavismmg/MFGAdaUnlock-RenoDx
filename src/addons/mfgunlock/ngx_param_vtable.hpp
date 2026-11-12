/*
 * Slot replacement in the NGX parameter vtable (see "THE PARAMETER OVERRIDE"
 * in addon.cpp). Kept apart from the addon so it can be tested with a fake
 * vtable.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>

namespace mfgunlock::ngxparam {

// Slot 11 of NVSDK_NGX_Parameter == Get(const char*, unsigned int*).
inline constexpr size_t kGetUInt32Slot = 11;
// 1.10.0: slot 12 == Get(const char*, int*), which Streamline's DLSS-G plugin
// uses for FrameGeneration.Available and the driver-version answers.
inline constexpr size_t kGetInt32Slot = 12;

enum class PatchResult { kPatched, kAlreadyPatched, kNoObject, kProtectFailed, kForeignHook };

// NGX hands out parameter blocks on several threads, so the slot write is
// serialized: two unsynchronized patches could both pass the `patched` check,
// and the second would save the first one's hook as the original (the hook
// then calls itself until the stack overflows) or write the slot after the
// first had made the page read-only again.
struct VTablePatch {
  SRWLOCK lock = SRWLOCK_INIT;
  std::atomic_bool patched{false};
  void** slot = nullptr;
  // Read by the hook on any thread; published before the slot points to it.
  std::atomic<void*> original{nullptr};
};

// Every parameter object shares one vtable; replacing the slot once covers
// all of them. `original` receives the slot's previous value.
inline PatchResult Patch(VTablePatch& state, void* object, void* hook,
                         size_t index = kGetUInt32Slot) {
  if (object == nullptr) return PatchResult::kNoObject;
  if (state.patched.load(std::memory_order_acquire))
    return PatchResult::kAlreadyPatched;
  AcquireSRWLockExclusive(&state.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockExclusive(lock); }
  } unlock{&state.lock};
  if (state.patched.load(std::memory_order_acquire))
    return PatchResult::kAlreadyPatched;

  auto** vtable = *reinterpret_cast<void***>(object);
  if (vtable == nullptr) return PatchResult::kNoObject;
  void** slot = &vtable[index];
  // The slot already holds this hook without a recorded original: never adopt
  // the hook as the original.
  if (*slot == hook) return PatchResult::kForeignHook;

  DWORD old_protect = 0;
  if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect) == 0)
    return PatchResult::kProtectFailed;

  state.original.store(*slot, std::memory_order_release);
  *slot = hook;
  state.slot = slot;

  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void*), old_protect, &ignored);

  state.patched.store(true, std::memory_order_release);
  return PatchResult::kPatched;
}

inline void Restore(VTablePatch& state) {
  AcquireSRWLockExclusive(&state.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockExclusive(lock); }
  } unlock{&state.lock};
  if (!state.patched.load(std::memory_order_acquire)) return;
  void* original = state.original.load(std::memory_order_acquire);
  if (state.slot == nullptr || original == nullptr) return;

  DWORD old_protect = 0;
  if (VirtualProtect(state.slot, sizeof(void*), PAGE_READWRITE, &old_protect) != 0) {
    *state.slot = original;
    DWORD ignored = 0;
    VirtualProtect(state.slot, sizeof(void*), old_protect, &ignored);
  }
  state.patched.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::ngxparam
