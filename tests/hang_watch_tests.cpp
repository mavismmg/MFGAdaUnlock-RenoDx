// SPDX-License-Identifier: MIT
// 1.11.0 (R1): a watched call that does not return gets MFGAdaUnlock-hang.txt with its stack.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/addons/mfgunlock/hang_watch.hpp"

#define CHECK(x)                                                 \
  do {                                                           \
    if (!(x)) {                                                  \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                              \
    }                                                            \
  } while (0)

namespace fs = std::filesystem;
using namespace mfgunlock::hangwatch;

__declspec(noinline) static void StuckCall(DWORD ms) {
  const Scope watch("test call");
  { const Scope nested("inner call"); }  // nested scopes keep the outermost
  Sleep(ms);
}

int main() {
  const fs::path folder = fs::temp_directory_path() / ("mfg-hang-test-" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(folder);
  const fs::path record = folder / L"MFGAdaUnlock-hang.txt";
  fs::remove(record);
  Configure(folder, false);
  g_hang_ms.store(500);

  // A short call: no record.
  StuckCall(100);
  Sleep(1500);
  CHECK(!fs::exists(record));

  // A call past the limit: one record, with the call, the thread and a stack through this test.
  const HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { StuckCall(3500); return 0; }, nullptr, 0, nullptr);
  CHECK(thread != nullptr);
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  CHECK(fs::exists(record));
  std::stringstream text;
  text << std::ifstream(record).rdbuf();
  const std::string content = text.str();
  std::printf("%s", content.c_str());
  CHECK(content.find("call=test call") != std::string::npos);
  CHECK(content.find("inner call") == std::string::npos);
  CHECK(content.find("dump=(not written") != std::string::npos);
  CHECK(content.find("hang_watch_tests.exe+0x") != std::string::npos);
  fs::remove_all(folder);
  std::printf("hang_watch_tests: ok\n");
  return 0;
}
