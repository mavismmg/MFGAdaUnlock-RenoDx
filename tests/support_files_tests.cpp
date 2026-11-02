// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "../src/addons/mfgunlock/support_files.hpp"
#include "../src/addons/mfgunlock/system_info.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using namespace mfgunlock::support;
namespace fs = std::filesystem;

int main() {
  {
    using namespace mfgunlock::system;
    constexpr int kRgba8 = 28;
    CHECK(std::string(OutputName(kFormatRgb10a2, true)) == "HDR10");
    CHECK(std::string(OutputName(kFormatRgb10a2, false)) == "SDR (10-bit)");
    CHECK(std::string(OutputName(kRgba8, true)) == "SDR");   // SDR game on an HDR desktop
    CHECK(std::string(OutputName(kFormatRgba16Float, true)) == "scRGB");
    CHECK(OutputIsScrgb(kFormatRgba16Float) && !OutputIsScrgb(kFormatRgb10a2));
    CHECK(DriverName(0x10DE, 32, 0, 16, 2101) == "621.01");
    CHECK(DriverName(0x10DE, 32, 0, 15, 9505) == "595.05");
    CHECK(DriverName(0x1002, 32, 0, 21, 1000) == "32.0.21.1000");
    CHECK(WindowsName(10, 26300) == "11 (26300)" && WindowsName(10, 19045) == "10 (19045)");
    CHECK(!ReadMachine().windows.empty());
  }

  CHECK(ShouldApplyPanelFont(false, false, true));
  CHECK(!ShouldApplyPanelFont(true, false, true));   // cleared by the player later
  CHECK(!ShouldApplyPanelFont(false, true, true));   // the player's own font
  CHECK(!ShouldApplyPanelFont(false, false, false)); // not on this Windows

  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const fs::path folder = fs::path(temp) / (L"mfg-support-test-" + std::to_wstring(GetCurrentProcessId()));
  fs::remove_all(folder);
  fs::create_directories(folder);
  const auto marker = folder / kMarkerName;

  // A first run and a clean exit leave nothing to report.
  BeginSession(folder);
  CHECK(fs::exists(marker) && !g_previous_unclean.load());
  EndSession(folder);
  CHECK(!fs::exists(marker));
  BeginSession(folder);
  CHECK(!g_previous_unclean.load());

  // Another live process owns the marker: not a crash, and it is not ours to delete.
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  wchar_t command[] = L"cmd.exe /c exit 0";
  CHECK(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
  { std::ofstream(marker, std::ios::trunc) << child.dwProcessId; }
  BeginSession(folder);
  CHECK(!g_previous_unclean.load());
  { std::ofstream(marker, std::ios::trunc) << child.dwProcessId; }
  EndSession(folder);
  CHECK(fs::exists(marker));
  // The same process gone with its marker left behind: an unclean end.
  ResumeThread(child.hThread);
  WaitForSingleObject(child.hProcess, 10000);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  BeginSession(folder);
  CHECK(g_previous_unclean.exchange(false));
  EndSession(folder);

  // An empty or damaged marker still means the last session did not finish.
  { std::ofstream(marker, std::ios::trunc) << "x"; }
  BeginSession(folder);
  CHECK(g_previous_unclean.exchange(false));
  EndSession(folder);

  CHECK(IsSupportFile(L"ReShade.log") && IsSupportFile(L"reshade.log1") && IsSupportFile(L"MFGAdaUnlock-XeMFG.log"));
  CHECK(IsSupportFile(L"MFGAdaUnlock.ini"));
  CHECK(!IsSupportFile(L"ReShade.ini") && !IsSupportFile(L"game.log") && !IsSupportFile(L"save.dat"));

  // A log the game still holds open for writing is copied too.
  const auto held = folder / L"ReShade.log";
  const HANDLE writer = CreateFileW(held.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(writer != INVALID_HANDLE_VALUE);
  DWORD written = 0;
  CHECK(WriteFile(writer, "log text", 8, &written, nullptr));
  { std::ofstream(folder / L"MFGAdaUnlock.ini") << "[DLSSG]\n"; }
  { std::ofstream(folder / L"save.dat") << "private"; }
  // Too large to attach: skipped, the rest still saved.
  { std::ofstream big(folder / L"ReShade.log1", std::ios::binary); big.seekp(std::streamoff(kMaxCopiedFile + 1)); big.put('x'); }
  const auto saved = Save(folder, "report text", L"stamp");
  CHECK(saved.ok && saved.copied == 2);
  CloseHandle(writer);
  CHECK(fs::file_size(saved.folder / L"ReShade.log") == 8);
  CHECK(fs::exists(saved.folder / L"MFGAdaUnlock.ini") && fs::exists(saved.folder / L"report.txt"));
  CHECK(!fs::exists(saved.folder / L"save.dat") && !fs::exists(saved.folder / L"ReShade.log1"));

  fs::remove_all(folder);
  std::cout << "PASS: support files\n";
  return 0;
}
