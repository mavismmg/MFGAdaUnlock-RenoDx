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

#include <mutex>
#include <sstream>
#include "module_session.hpp"
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
inline std::mutex transaction_lock;
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
    resolved.emplace_back(real, replacement);
  }

  if (!modulesession::Acquire(module)) return false;
  for (const auto& [name, real, replacement] : hooks) *real = reinterpret_cast<void*>(GetProcAddress(module, name));
  std::lock_guard transaction(internal::transaction_lock);
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
  std::lock_guard transaction(internal::transaction_lock);
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

// Return success explicitly. Callers must retain trampoline/state on failure.
inline bool Uninstall(const std::vector<HookItem>& hooks) {
  // Not a blocking lock: at process exit its owner may already have been killed.
  std::unique_lock transaction(internal::transaction_lock, std::try_to_lock);
  if (!transaction.owns_lock()) return false;
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
