// SPDX-License-Identifier: MIT
#pragma once
// 1.11.0 (R1): a record of a call of ours that never returns.
//
// Monster Hunter Wilds (2026-11-20) stopped responding inside the game's slInit: the log ended at
// "slInit entered" and said nothing about where. The long calls MFG Unlock intercepts (slInit,
// swap chain creation, the hosted Present, NGX feature creation) now mark their start and end on
// a few slots. A watchdog thread checks them once a second; a call still running after 15 s gets
// MFGAdaUnlock-hang.txt: the call, the thread and its stack as module+offset, and with
// [Diagnostics] CrashDump=1 a minidump. It only observes: the stuck thread is suspended for the
// time of GetThreadContext, nothing else.
//
// The watchdog is a thread running this module's code, so the first watched call pins the module
// (as XeMFG does at load, and the DLSS-G route after the first Present).
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

#include "crash_record.hpp"

namespace mfgunlock::hangwatch {

constexpr uint64_t kHangMs = 15000;
inline std::atomic<uint64_t> g_hang_ms{kHangMs};  // the test lowers it
constexpr int kSlots = 8;
constexpr int kMaxFrames = 32;

namespace internal {
struct Slot {
  std::atomic<const char*> name{nullptr};
  std::atomic<DWORD> thread{0};
  std::atomic<uint64_t> start{0};
  std::atomic<uint64_t> serial{0};  // which call; a report is written once per call
};
inline Slot g_slots[kSlots];
inline std::atomic<uint64_t> g_serial{0};
inline std::atomic_bool g_started{false};
inline std::filesystem::path g_directory;
inline std::atomic_bool g_dump{false};
inline thread_local int t_depth = 0;
inline thread_local int t_slot = -1;

inline std::string Frame(DWORD64 address) {
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH]{};
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module))
    GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* name = std::wcsrchr(path, L'\\');
  return crash::Location(name ? name + 1 : path, reinterpret_cast<uintptr_t>(module), static_cast<uintptr_t>(address));
}

// The thread's stack, from its context: suspended only while the context is read. A hung thread does
// not move, so walking its stack afterwards reads what it was doing.
inline int Stack(DWORD thread_id, DWORD64 (&frames)[kMaxFrames]) {
  const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   thread_id);
  if (thread == nullptr) return 0;
  CONTEXT context{};
  context.ContextFlags = CONTEXT_FULL;
  bool ok = false;
  if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
    ok = GetThreadContext(thread, &context) != FALSE;
    ResumeThread(thread);
  }
  CloseHandle(thread);
  if (!ok) return 0;
  int count = 0;
  for (; count < kMaxFrames && context.Rip != 0; ++count) {
    frames[count] = context.Rip;
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
    if (entry == nullptr) {  // a leaf function: the return address is at the top of the stack
      if (IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), sizeof(DWORD64))) break;
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += sizeof(DWORD64);
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, const_cast<PRUNTIME_FUNCTION>(entry), &context,
                     &handler_data, &establisher, nullptr);
  }
  return count;
}

inline void Report(const char* name, DWORD thread_id, uint64_t seconds) {
  DWORD64 frames[kMaxFrames]{};
  const int count = Stack(thread_id, frames);
  const std::wstring dump_name = L"MFGAdaUnlock-hang-" + std::to_wstring(GetCurrentProcessId()) + L".dmp";
  bool dumped = false;
  if (g_dump.load()) {
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      const auto write = reinterpret_cast<crash::MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
      const HANDLE file = CreateFileW((g_directory / dump_name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
      if (write != nullptr && file != INVALID_HANDLE_VALUE)
        dumped = write(GetCurrentProcess(), GetCurrentProcessId(), file, crash::kDumpType, nullptr, nullptr, nullptr) !=
                 FALSE;
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
  }
  if (FILE* f = _wfopen((g_directory / L"MFGAdaUnlock-hang.txt").c_str(), L"w")) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(f, "time=%04u-%02u-%02u %02u:%02u:%02u\ncall=%s\nthread=%lu\nseconds=%llu\ndump=%ls\nstack:\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, name, thread_id,
                 static_cast<unsigned long long>(seconds), dumped ? dump_name.c_str() : L"(not written; CrashDump=1)");
    for (int i = 0; i < count; ++i) std::fprintf(f, "  %s\n", Frame(frames[i]).c_str());
    std::fclose(f);
  }
}

inline DWORD WINAPI Watch(void*) {
  uint64_t reported[kSlots]{};
  for (;;) {
    Sleep(1000);
    const uint64_t now = GetTickCount64();
    for (int i = 0; i < kSlots; ++i) {
      Slot& slot = g_slots[i];
      const char* name = slot.name.load(std::memory_order_acquire);
      const uint64_t start = slot.start.load(std::memory_order_acquire);
      const uint64_t serial = slot.serial.load(std::memory_order_acquire);
      if (name == nullptr || start == 0 || now - start < g_hang_ms.load() || reported[i] == serial) continue;
      reported[i] = serial;
      Report(name, slot.thread.load(), (now - start) / 1000);
    }
  }
}

inline void Start() {
  if (g_started.exchange(true)) return;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&Watch), &self))
    return;  // not pinned: no thread of ours may outlive this copy
  if (const HANDLE thread = CreateThread(nullptr, 64 * 1024, &Watch, nullptr, 0, nullptr)) CloseHandle(thread);
}
}  // namespace internal

// At load (DllMain): where the record goes, and whether a dump goes with it.
inline void Configure(const std::filesystem::path& directory, bool dump) {
  internal::g_directory = directory;
  internal::g_dump.store(dump);
}

// Around a watched call; nested calls on the same thread keep the outermost.
class Scope {
 public:
  explicit Scope(const char* name) {
    using namespace internal;
    if (t_depth++ != 0 || g_directory.empty()) return;
    Start();
    for (int i = 0; i < kSlots; ++i) {
      const char* expected = nullptr;
      if (g_slots[i].name.compare_exchange_strong(expected, "")) {
        g_slots[i].thread.store(GetCurrentThreadId());
        g_slots[i].serial.store(g_serial.fetch_add(1) + 1);
        g_slots[i].start.store(GetTickCount64(), std::memory_order_release);
        g_slots[i].name.store(name, std::memory_order_release);
        t_slot = i;
        return;
      }
    }
  }
  ~Scope() {
    using namespace internal;
    if (--t_depth != 0 || t_slot < 0) return;
    g_slots[t_slot].start.store(0, std::memory_order_release);
    g_slots[t_slot].name.store(nullptr, std::memory_order_release);
    t_slot = -1;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

}  // namespace mfgunlock::hangwatch
