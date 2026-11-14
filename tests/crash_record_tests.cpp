// SPDX-License-Identifier: MIT
// 1.10.0 (dashdogy B1): the crash record. A child copy of this test installs
// the filter and dereferences null; the parent checks the record and the dump.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/addons/mfgunlock/crash_record.hpp"
#include "../src/addons/mfgunlock/support_files.hpp"

static int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"crash") {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    mfgunlock::crash::Install(argv[2]);
    volatile int* null_pointer = nullptr;
    *null_pointer = 1;
    return 0;
  }
  CHECK(mfgunlock::crash::Location(L"game.exe", 0x140000000, 0x140001234) == "game.exe+0x1234");
  CHECK(mfgunlock::crash::Location(nullptr, 0, 0x1234) == "0x1234");
  CHECK(mfgunlock::support::IsSupportFile(L"MFGAdaUnlock-crash.txt"));
  CHECK(mfgunlock::support::IsSupportFile(L"MFGAdaUnlock-crash-1234.dmp"));
  CHECK(!mfgunlock::support::IsSupportFile(L"game-crash.dmp") && !mfgunlock::support::IsSupportFile(L"MFGAdaUnlock.dmp"));

  wchar_t self[MAX_PATH]{};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const auto dir = std::filesystem::temp_directory_path() / (L"mfg-crash-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::wstring command = L"\"" + std::wstring(self) + L"\" crash \"" + dir.wstring() + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
  DWORD code = 0;
  if (process.hProcess) {
    WaitForSingleObject(process.hProcess, 60000);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
  }
  CHECK(code == EXCEPTION_ACCESS_VIOLATION);
  std::ifstream record(dir / L"MFGAdaUnlock-crash.txt");
  std::stringstream text;
  text << record.rdbuf();
  const std::string body = text.str();
  std::printf("%s", body.c_str());
  CHECK(body.find("exception=0xc0000005") != std::string::npos);
  CHECK(body.find("access=write 0x0") != std::string::npos);
  CHECK(body.find("crash_record_tests.exe+0x") != std::string::npos);
  const auto dump = dir / (L"MFGAdaUnlock-crash-" + std::to_wstring(process.dwProcessId) + L".dmp");
  std::error_code error;
  const auto size = std::filesystem::file_size(dump, error);
  CHECK(!error && size > 1024 && size < (64ull << 20));
  record.close();
  std::filesystem::remove_all(dir, error);
  if (failures == 0) std::printf("crash_record: all checks passed (dump %llu bytes)\n", static_cast<unsigned long long>(size));
  return failures == 0 ? 0 : 1;
}
