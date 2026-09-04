/*
 * Load-time trigger for the DLSS-G snippet patches.
 * SPDX-License-Identifier: MIT
 *
 * ---------------------------------------------------------------------------
 * WHY
 *
 * NGX populates the snippet's capabilities within milliseconds of loading it --
 * PopulateParameters, the function carrying the arch gate, runs almost
 * immediately. Patching on the next present is therefore a race we only win by
 * accident:
 *
 *   Cyberpunk 2077 : nvngx_dlssg.dll already loaded when the addon initialised,
 *                    so DllMain patched it ~10ms in, long before the query.
 *   Deep Rock Gal. : the DLL loads seconds AFTER the addon, NGX populates
 *                    immediately, and our next-frame patch lands too late. The
 *                    gates get rewritten correctly, but the capability was
 *                    already computed and cached as 1.
 *
 * So catch the module as it is mapped. A single bootstrap scan covers providers
 * that predate the addon; this trigger covers later providers without polling
 * or enumerating modules from the presentation thread.
 *
 * LOADER LOCK: this callback runs inside LoadLibrary, holding the loader lock.
 * It must never call anything that takes that lock again --
 * CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) in particular would deadlock,
 * which is why the callback receives the HMODULE and patches it directly
 * instead of scanning for it.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <windows.h>

#include <atomic>
#include <cwctype>

#include <include/reshade.hpp>

#include "./ngx_hook.hpp"

namespace mfgunlock::loadhook {

// Called with the freshly loaded DLSS-G snippet. Set by the addon.
inline void (*g_on_dlssg_loaded)(HMODULE) = nullptr;
// slInit must be hooked before the game calls it, and it calls it early --
// so catch the interposer as it is mapped rather than hoping to beat it.
inline void (*g_on_interposer_loaded)() = nullptr;
inline std::atomic_bool g_hooked{false};
inline std::atomic<unsigned int> g_catches{0};

namespace internal {

constexpr wchar_t kNeedle[] = L"nvngx_dlssg";
constexpr wchar_t kOtaNeedle[] = L"\\models\\dlssg\\";
constexpr wchar_t kInterposerNeedle[] = L"sl.interposer";

inline bool NameContains(const wchar_t* path, const wchar_t* needle) {
  if (path == nullptr) return false;
  // Compare case-insensitively without touching the CRT locale machinery.
  for (const wchar_t* p = path; *p != L'\0'; ++p) {
    size_t i = 0;
    while (needle[i] != L'\0') {
      const wchar_t a = p[i];
      if (a == L'\0') break;
      const wchar_t lower = (a >= L'A' && a <= L'Z') ? static_cast<wchar_t>(a - L'A' + L'a') : a;
      if (lower != needle[i]) break;
      ++i;
    }
    if (needle[i] == L'\0') return true;
  }
  return false;
}

inline void Notify(HMODULE module, const wchar_t* path) {
  if (module == nullptr || path == nullptr) return;
  // Driver OTA snippets are commonly mapped from ...\models\dlssg\... under
  // opaque numeric .bin names. The loader call may receive only that basename,
  // so also inspect the resolved module path after the mapping completes.
  bool is_dlssg = NameContains(path, kNeedle) || NameContains(path, kOtaNeedle);
  if (!is_dlssg) {
    wchar_t resolved_path[32768] = {};
    const DWORD length = GetModuleFileNameW(module, resolved_path, ARRAYSIZE(resolved_path));
    if (length != 0 && length < ARRAYSIZE(resolved_path)) {
      is_dlssg = NameContains(resolved_path, kNeedle) || NameContains(resolved_path, kOtaNeedle);
    }
  }
  if (g_on_dlssg_loaded != nullptr && is_dlssg) {
    g_catches.fetch_add(1, std::memory_order_relaxed);
    g_on_dlssg_loaded(module);
  }
  if (g_on_interposer_loaded != nullptr && NameContains(path, kInterposerNeedle)) {
    g_on_interposer_loaded();
  }
}

using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);

inline LoadLibraryExWFn g_real_load_library_ex_w = nullptr;
inline LoadLibraryWFn g_real_load_library_w = nullptr;

inline HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR file_name, HANDLE file, DWORD flags) {
  HMODULE module = g_real_load_library_ex_w(file_name, file, flags);
  // Data-file mappings are not executable images; patching one would be
  // meaningless and the caller is not going to run code from it.
  constexpr DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                              LOAD_LIBRARY_AS_IMAGE_RESOURCE;
  if ((flags & kDataOnly) == 0) Notify(module, file_name);
  return module;
}

inline HMODULE WINAPI HookedLoadLibraryW(LPCWSTR file_name) {
  HMODULE module = g_real_load_library_w(file_name);
  Notify(module, file_name);
  return module;
}

inline const std::vector<hook::HookItem> kHooks = {
    {"LoadLibraryExW", reinterpret_cast<void**>(&g_real_load_library_ex_w),
     reinterpret_cast<void*>(&HookedLoadLibraryExW)},
    {"LoadLibraryW", reinterpret_cast<void**>(&g_real_load_library_w),
     reinterpret_cast<void*>(&HookedLoadLibraryW)},
};

}  // namespace internal

inline void TryInstall() {
  if (g_hooked.load(std::memory_order_acquire)) return;
  if (g_on_dlssg_loaded == nullptr) return;
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 == nullptr) return;
  if (!hook::Install(kernel32, internal::kHooks, "kernel32.dll")) return;
  g_hooked.store(true, std::memory_order_release);
}

inline void Uninstall() {
  if (!g_hooked.load(std::memory_order_acquire)) return;
  hook::Uninstall(internal::kHooks);
  g_hooked.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::loadhook
