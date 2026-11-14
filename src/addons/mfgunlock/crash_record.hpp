// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (dashdogy item B1, our own implementation): [Diagnostics] CrashDump=1
// writes MFGAdaUnlock-crash.txt (exception code, address, module and offset)
// and MFGAdaUnlock-crash-<pid>.dmp beside the game when the process is about
// to die, for crashes that end in silence (Dying Light 2 at start).
//
// Only an unhandled-exception filter: it runs after every handler the game
// has, so a fault the game catches is never seen here (support_files.hpp's rule
// against exception handlers stands). The previous filter is called afterwards,
// so the game's own crash reporter still runs; a game that installs its own
// filter later simply replaces this one. dbghelp.dll is loaded from System32
// only when a crash happens: no static import (xemfg_addon_imports).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

namespace mfgunlock::crash {

// Pure, for the test: "module+0xoffset" or the raw address.
inline std::string Location(const wchar_t* module_name, uintptr_t base, uintptr_t address) {
  char text[320];
  if (module_name != nullptr && module_name[0] != L'\0' && base != 0 && address >= base)
    std::snprintf(text, sizeof(text), "%ls+0x%llx", module_name, static_cast<unsigned long long>(address - base));
  else
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
  return text;
}

inline std::filesystem::path g_directory;
inline LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
inline std::atomic_bool g_installed{false};
inline std::atomic_bool g_written{false};

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, const void*, const void*, const void*);
#pragma pack(push, 4)  // as dbghelp.h declares it: the pointer sits at offset 4
struct ExceptionInformation {  // MINIDUMP_EXCEPTION_INFORMATION, without dbghelp.h
  DWORD ThreadId;
  EXCEPTION_POINTERS* ExceptionPointers;
  BOOL ClientPointers;
};
#pragma pack(pop)
static_assert(sizeof(ExceptionInformation) == 16);
// Stacks, modules and threads only: a few MB, under the support copy's 64 MB.
constexpr int kDumpType = 0x00000020 /*WithUnloadedModules*/ | 0x00001000 /*WithThreadInfo*/;

inline LONG WINAPI Filter(EXCEPTION_POINTERS* pointers) {
  if (!g_written.exchange(true) && pointers != nullptr && pointers->ExceptionRecord != nullptr) {
    const auto* record = pointers->ExceptionRecord;
    const auto address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    HMODULE module = nullptr;
    wchar_t module_path[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(record->ExceptionAddress), &module))
      GetModuleFileNameW(module, module_path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(module_path, L'\\');
    name = name ? name + 1 : module_path;
    const std::wstring dump_name = L"MFGAdaUnlock-crash-" + std::to_wstring(GetCurrentProcessId()) + L".dmp";
    bool dumped = false;
    DWORD dump_error = 0;
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      const auto write = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
      const HANDLE file = CreateFileW((g_directory / dump_name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
      if (write != nullptr && file != INVALID_HANDLE_VALUE) {
        ExceptionInformation info{GetCurrentThreadId(), pointers, FALSE};
        dumped = write(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, &info, nullptr, nullptr) != FALSE;
        if (!dumped) dump_error = GetLastError();
      } else {
        dump_error = GetLastError();
      }
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    if (FILE* f = _wfopen((g_directory / L"MFGAdaUnlock-crash.txt").c_str(), L"w")) {
      SYSTEMTIME now{};
      GetLocalTime(&now);
      std::fprintf(f,
                   "time=%04u-%02u-%02u %02u:%02u:%02u\nexception=0x%08lx\naddress=%s\nthread=%lu\ndump=%ls\n",
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, record->ExceptionCode,
                   Location(name, reinterpret_cast<uintptr_t>(module), address).c_str(), GetCurrentThreadId(),
                   dumped ? dump_name.c_str() : L"(not written)");
      if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        std::fprintf(f, "access=%s 0x%llx\n", record->ExceptionInformation[0] == 0   ? "read"
                                              : record->ExceptionInformation[0] == 1 ? "write"
                                                                                     : "execute",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
      if (!dumped) std::fprintf(f, "dump_error=0x%08lx\n", dump_error);
      std::fclose(f);
    }
  }
  return g_previous != nullptr ? g_previous(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

inline bool Enabled(const std::filesystem::path& ini) {
  return GetPrivateProfileIntW(L"Diagnostics", L"CrashDump", 0, ini.c_str()) != 0;
}
inline void Install(const std::filesystem::path& directory) {
  if (g_installed.exchange(true)) return;
  g_directory = directory;
  g_previous = SetUnhandledExceptionFilter(&Filter);
}
// ReShade loads and unloads startup copies of the addon: an unloading copy
// puts the previous filter back if this one is still the process's filter,
// so it never points into an unloaded module. (A filter installed after this
// one and chaining to it is the remaining case; CrashDump is opt-in.)
inline void Uninstall() {
  if (!g_installed.exchange(false)) return;
  const auto current = SetUnhandledExceptionFilter(g_previous);
  if (current != &Filter) SetUnhandledExceptionFilter(current);  // someone else's now: leave it
}

}  // namespace mfgunlock::crash
