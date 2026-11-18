/*
 * Thread-safe Detours installation.
 * SPDX-License-Identifier: MIT
 *
 * Detours rewrites the first bytes of the target function. If another thread's
 * instruction pointer is inside that function at the moment of the rewrite, it
 * resumes mid-instruction -- a hang or a crash, not an error code.
 *
 * DetourUpdateThread is the documented answer: every thread that might be
 * executing the patched code must be registered with the transaction so Detours
 * can suspend it and fix up its instruction pointer. Registering only
 * GetCurrentThread() is correct only when the target is idle, and _nvngx.dll is
 * not idle -- we patch it from the ReShade present callback while the render
 * thread is calling DLSS through it.
 */

#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <detours.h>

#include <atomic>
#include <mutex>
#include <sstream>
#include "module_session.hpp"
#include "hook_lock.hpp"
#include <tuple>
#include <vector>

#include <include/reshade.hpp>

namespace mfgunlock::hook {

// Function name, storage for the trampoline, replacement.
using HookItem = std::tuple<const char*, void**, void*>;

namespace internal {

struct ThreadOperations {
  decltype(&CreateToolhelp32Snapshot) snapshot = &CreateToolhelp32Snapshot;
  decltype(&Thread32First) first = &Thread32First;
  decltype(&Thread32Next) next = &Thread32Next;
  decltype(&OpenThread) open = &OpenThread;
  decltype(&CloseHandle) close = &CloseHandle;
};
inline ThreadOperations thread_operations{};
struct DetourOperations {
  decltype(&DetourTransactionBegin) begin = &DetourTransactionBegin;
  decltype(&DetourTransactionAbort) abort = &DetourTransactionAbort;
  decltype(&DetourTransactionCommit) commit = &DetourTransactionCommit;
  decltype(&DetourUpdateThread) update = &DetourUpdateThread;
  LONG (WINAPI *attach)(PVOID*, PVOID) = &DetourAttach;
  LONG (WINAPI *detach)(PVOID*, PVOID) = &DetourDetach;
};
inline DetourOperations detours{};
// Detours has one transaction at a time, across threads: a second Begin fails.
// Shared with the MinHook installs (hook_lock.hpp).
inline std::mutex& transaction_lock = HookTransactionLock();
struct ThreadCoverage {
  std::vector<HANDLE> handles;
  DWORD error = ERROR_SUCCESS;
  bool complete = false;
  void Close() { for (auto h : handles) thread_operations.close(h); handles.clear(); }
};
inline ThreadCoverage OpenOtherThreads() {
  ThreadCoverage result;
  const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  HANDLE snap = thread_operations.snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
  THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
  bool found = thread_operations.first(snap, &entry) != FALSE;
  if (!found) result.error = GetLastError();
  while (found) {
    if (entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(entry.th32OwnerProcessID)) {
      result.error = ERROR_BAD_LENGTH; break;
    }
    if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) {
      HANDLE handle = thread_operations.open(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                              FALSE, entry.th32ThreadID);
      if (handle) result.handles.push_back(handle);
      else if (GetLastError() != ERROR_INVALID_PARAMETER) {
        result.error = GetLastError(); break;  // Access denied is not an exited thread.
      }
    }
    entry.dwSize = sizeof(entry);
    found = thread_operations.next(snap, &entry) != FALSE;
    if (!found) result.error = GetLastError();
  }
  thread_operations.close(snap);
  result.complete = result.error == ERROR_NO_MORE_FILES;
  return result;
}

}  // namespace internal

// 1.8.3: where the jumps at `code` lead -- rel32, rel8 and RIP-relative
// indirect jumps, the forms Detours writes -- or `code` itself.
inline const uint8_t* FollowJumps(const uint8_t* code) {
  for (int hop = 0; hop < 4; ++hop) {
    if (!patching::Readable(code, 6)) break;
    if (code[0] == 0xE9) {
      int32_t offset = 0;
      std::memcpy(&offset, code + 1, sizeof(offset));
      code += 5 + offset;
    } else if (code[0] == 0xEB) {
      code += 2 + static_cast<int8_t>(code[1]);
    } else if (code[0] == 0xFF && code[1] == 0x25) {
      int32_t offset = 0;
      std::memcpy(&offset, code + 2, sizeof(offset));
      const uint8_t* slot = code + 6 + offset;
      if (!patching::Readable(slot, sizeof(void*))) break;
      std::memcpy(&code, slot, sizeof(code));
    } else {
      break;
    }
  }
  return code;
}

// True when `code` already jumps into this addon's image. ReShade loads,
// unloads and reloads add-ons around its temporary startup devices, and the
// next copy maps at the same address: a hook left by the unloaded copy lands
// in our own detour, and hooking over it again makes the trampoline call that
// detour forever (TLOU Part II, 1.8.2: stack overflow in HookedCreateFeature).
inline bool JumpsIntoSelf(const void* code) {
  const auto* start = static_cast<const uint8_t*>(code);
  const auto* end = FollowJumps(start);
  if (end == start) return false;
  HMODULE self = nullptr, target = nullptr;
  constexpr DWORD kFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  return GetModuleHandleExW(kFlags, reinterpret_cast<LPCWSTR>(&JumpsIntoSelf), &self) &&
         GetModuleHandleExW(kFlags, reinterpret_cast<LPCWSTR>(end), &target) && self == target;
}

// Installs `hooks` into `module`. Returns true only if EVERY hook attached and
// the transaction committed -- a partial install leaves some entry points
// patched and others not, which is worse than none.
inline bool Install(HMODULE module, const std::vector<HookItem>& hooks,
                    const char* module_label) {
  if (module == nullptr) return false;

  std::vector<std::pair<void**, void*>> resolved;
  resolved.reserve(hooks.size());
  const auto clear_resolved = [&resolved]() {
    for (auto& [real, unused] : resolved) *real = nullptr;
  };
  for (const auto& [name, real, replacement] : hooks) {
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) {
      clear_resolved();
      std::stringstream s;
      s << "mfgunlock::hook: " << module_label << " has no export " << name
        << " -- not hooking anything in this module.";
      reshade::log::message(reshade::log::level::error, s.str().c_str());
      return false;
    }
    if (*real != nullptr) {
      return false;  // already installed; preserve live trampolines
    }
    if (JumpsIntoSelf(reinterpret_cast<const void*>(proc))) {
      clear_resolved();
      std::stringstream s;
      s << "mfgunlock::hook: " << module_label << "!" << name
        << " still carries a hook from an earlier copy of this add-on that ReShade unloaded; "
           "not hooking it again. Restart the game.";
      reshade::log::message(reshade::log::level::error, s.str().c_str());
      return false;
    }
    resolved.emplace_back(real, replacement);
  }

  if (!modulesession::Acquire(module)) return false;
  for (const auto& [name, real, replacement] : hooks) *real = reinterpret_cast<void*>(GetProcAddress(module, name));
  std::lock_guard transaction(internal::transaction_lock);
  ProcessHeapGuard heap;
  if (internal::detours.begin() != NO_ERROR) {
    clear_resolved();
    return false;
  }

  auto threads = internal::OpenOtherThreads();
  bool threads_ok = threads.complete;
  for (HANDLE h : threads.handles) {
    if (internal::detours.update(h) != NO_ERROR) threads_ok = false;
  }
  if (internal::detours.update(GetCurrentThread()) != NO_ERROR) threads_ok = false;

  if (!threads_ok) {
    internal::detours.abort();
    threads.Close();
    clear_resolved();
    reshade::log::message(
        reshade::log::level::error,
        "mfgunlock::hook: could not register every thread with the Detours "
        "transaction -- refusing to patch, because patching code another thread "
        "is executing hangs the process.");
    return false;
  }

  bool ok = true;
  for (auto& [real, replacement] : resolved) {
    if (internal::detours.attach(real, replacement) != NO_ERROR) {
      ok = false;
      break;
    }
  }

  if (!ok || internal::detours.commit() != NO_ERROR) {
    if (!ok) internal::detours.abort();
    threads.Close();
    clear_resolved();
    std::stringstream s;
    s << "mfgunlock::hook: failed to install hooks in " << module_label << ".";
    reshade::log::message(reshade::log::level::error, s.str().c_str());
    return false;
  }

  const auto thread_count = threads.handles.size();
  threads.Close();
  std::stringstream s;
  s << "mfgunlock::hook: installed " << resolved.size() << " hook(s) in "
    << module_label << ", with " << thread_count
    << " other thread(s) registered with the transaction.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
  return true;
}

// 1.8.1: one hook whose trampoline is already resolved in `real`, under the same
// rules: every thread registered, or nothing patched.
inline bool InstallOne(void** real, void* replacement) {
  if (JumpsIntoSelf(*real)) return false;
  std::lock_guard transaction(internal::transaction_lock);
  ProcessHeapGuard heap;
  if (internal::detours.begin() != NO_ERROR) return false;
  auto threads = internal::OpenOtherThreads();
  bool ok = threads.complete;
  for (HANDLE h : threads.handles) if (internal::detours.update(h) != NO_ERROR) ok = false;
  if (internal::detours.update(GetCurrentThread()) != NO_ERROR) ok = false;
  if (ok) ok = internal::detours.attach(real, replacement) == NO_ERROR;
  if (ok) ok = internal::detours.commit() == NO_ERROR;
  else internal::detours.abort();
  threads.Close();
  return ok;
}

// 1.11.0 (F1): a hook another module took away. SOTTR with the "DLSS 5" addon
// (2026-11-18): it hooked _nvngx.dll's exports, we hooked over it, then ReShade
// reloaded the add-ons and that addon's unhook wrote back the bytes it had saved,
// which removed our jump with its own; this addon stayed loaded and never saw
// another DLSS SR evaluation.
//
// Such a hook is installed again over whatever is there now, as a new generation
// with its own trampoline. Whether the old one is really gone cannot be told from
// the bytes: a module that hooked over ours leaves our jump inside its own
// trampoline, so a call can still come back to us through the old generation.
// The detour therefore counts how deep it is on this thread: the outer call does
// the work and calls the newest trampoline; a call that comes back in passes
// straight to the next older one, and never loops.
inline constexpr int kMaxGenerations = 4;
struct Rehookable {
  const char* name;
  void* replacement;
  void* slots[kMaxGenerations]{};
  std::atomic<int> generations{0};
};
// Held exclusively from a generation's commit until it is published: a call
// that reaches the new jump in between waits instead of taking the previous
// trampoline, which may lead into the module that removed us (unloaded since).
inline SRWLOCK generation_lock = SRWLOCK_INIT;
// The trampoline for a call at `level` (0 = the outer call on this thread).
inline void* Trampoline(const Rehookable& hook, int level) {
  AcquireSRWLockShared(&generation_lock);
  const int count = hook.generations.load(std::memory_order_acquire);
  void* result = count <= 0 ? nullptr : hook.slots[level >= 0 && level < count ? count - 1 - level : 0];
  ReleaseSRWLockShared(&generation_lock);
  return result;
}
// One per detour, with a thread_local depth: Nesting nest(t_depth); nest.level.
struct Nesting {
  int& depth;
  const int level;
  explicit Nesting(int& d) : depth(d), level(d++) {}
  ~Nesting() { --depth; }
  Nesting(const Nesting&) = delete;
  Nesting& operator=(const Nesting&) = delete;
};
// Installed (at least once) and every export in the set still leads here.
inline bool StillHooked(HMODULE module, const std::vector<Rehookable*>& set) {
  if (module == nullptr || set.empty()) return false;
  for (const Rehookable* hook : set) {
    if (hook->generations.load(std::memory_order_acquire) == 0) return false;
    const void* proc = reinterpret_cast<const void*>(GetProcAddress(module, hook->name));
    if (proc == nullptr || !JumpsIntoSelf(proc)) return false;
  }
  return true;
}
// Installs the set's next generation, all of it or nothing (Install's rules).
inline bool InstallGeneration(HMODULE module, const std::vector<Rehookable*>& set, const char* module_label) {
  if (set.empty()) return false;
  const int generation = set.front()->generations.load(std::memory_order_acquire);
  if (generation >= kMaxGenerations) return false;
  // Detours writes each trampoline here at commit; published below, under the lock.
  std::vector<void*> fresh(set.size(), nullptr);
  std::vector<HookItem> items;
  for (size_t i = 0; i < set.size(); ++i) {
    if (set[i]->generations.load(std::memory_order_acquire) != generation) return false;
    items.emplace_back(set[i]->name, &fresh[i], set[i]->replacement);
  }
  AcquireSRWLockExclusive(&generation_lock);
  const bool installed = Install(module, items, module_label);
  if (installed)
    for (size_t i = 0; i < set.size(); ++i) {
      set[i]->slots[generation] = fresh[i];
      set[i]->generations.store(generation + 1, std::memory_order_release);
    }
  ReleaseSRWLockExclusive(&generation_lock);
  return installed;
}
// For a set installed before: reinstalls it when another module removed it.
// Returns true when it did. Bounded by kMaxGenerations, so two modules that both
// insist on being first cannot trade places forever.
inline bool Reinstall(HMODULE module, const std::vector<Rehookable*>& set, const char* module_label) {
  if (set.empty() || set.front()->generations.load(std::memory_order_acquire) == 0) return false;
  if (StillHooked(module, set)) return false;
  if (!InstallGeneration(module, set, module_label)) return false;
  std::stringstream s;
  s << "mfgunlock::hook: another module removed our hooks in " << module_label
    << "; installed them again (generation " << set.front()->generations.load() << " of " << kMaxGenerations
    << ").";
  reshade::log::message(reshade::log::level::warning, s.str().c_str());
  return true;
}

// Return success explicitly. Callers must retain trampoline/state on failure.
inline bool Uninstall(const std::vector<HookItem>& hooks) {
  // Not a blocking lock: at process exit its owner may already have been killed.
  std::unique_lock transaction(internal::transaction_lock, std::try_to_lock);
  if (!transaction.owns_lock()) return false;
  // No heap lock here: at process exit a killed thread may hold it, and HeapLock would wait forever.
  if (internal::detours.begin() != NO_ERROR) return false;
  auto threads = internal::OpenOtherThreads();
  bool ok = threads.complete;
  for (auto h : threads.handles) if (internal::detours.update(h) != NO_ERROR) ok = false;
  if (internal::detours.update(GetCurrentThread()) != NO_ERROR) ok = false;
  if (ok) for (const auto& [name, real, replacement] : hooks)
    if (*real && internal::detours.detach(real, replacement) != NO_ERROR) { ok = false; break; }
  if (ok) ok = internal::detours.commit() == NO_ERROR;
  else internal::detours.abort();
  threads.Close();
  return ok;
}

}  // namespace mfgunlock::hook
