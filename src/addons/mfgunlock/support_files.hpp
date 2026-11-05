// SPDX-License-Identifier: MIT
#pragma once
// 1.8.1: what a player needs when something goes wrong. A marker notices a session
// that never reached a clean exit, and Save gathers the logs into one folder. No
// exception handlers: catching faults inside a hooked game can break the game's own.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mfgunlock::support {
constexpr const wchar_t* kMarkerName = L"MFGAdaUnlock.session";
constexpr uint64_t kMaxCopiedFile = 64ull << 20;
inline std::atomic_bool g_previous_unclean{false};

// The panel's font is ReShade's own Font setting, the only way an addon can have
// one. Offered once, so a player who clears it later keeps ReShade's default, and
// never over a font the player chose.
constexpr const wchar_t* kPanelFontPath = L"C:\\Windows\\Fonts\\CascadiaMono.ttf";
constexpr const char* kPanelFontValue = "C:\\Windows\\Fonts\\CascadiaMono.ttf";
constexpr bool ShouldApplyPanelFont(bool alreadyOffered, bool playerHasFont, bool fontExists) {
  return !alreadyOffered && !playerHasFont && fontExists;
}
inline bool ProcessAlive(DWORD pid) {
  const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return alive;
}
inline DWORD ReadMarker(const std::filesystem::path& marker) {
  std::ifstream file(marker);
  DWORD pid = 0;
  return file >> pid ? pid : 0;
}
// 1.8.3: how many sessions in a row ended without a clean exit, kept after the
// pid. A setting that crashes the game at every start (TLOU Part II with a saved
// DLSS preset, 1.8.2) needs a way out before the panel can be reached.
inline std::atomic<uint32_t> g_unclean_streak{0};
constexpr uint32_t kSafeModeOffer = 3;
inline uint32_t ReadStreak(const std::filesystem::path& marker) {
  std::ifstream file(marker);
  DWORD pid = 0;
  uint32_t streak = 0;
  return file >> pid >> streak ? streak : 0;
}
// A marker left by a process that is gone: that session ended without a clean
// exit. A game that ends itself with TerminateProcess looks the same, which is why
// the panel says "may have".
inline void BeginSession(const std::filesystem::path& folder, DWORD pid = GetCurrentProcessId()) {
  std::error_code error;
  const auto marker = folder / kMarkerName;
  uint32_t streak = 0;
  if (std::filesystem::exists(marker, error)) {
    const DWORD previous = ReadMarker(marker);
    streak = ReadStreak(marker);
    if (previous != pid && !(previous != 0 && ProcessAlive(previous))) {
      g_previous_unclean.store(true);
      ++streak;
    }
  }
  g_unclean_streak.store(streak);
  std::ofstream(marker, std::ios::trunc) << pid << ' ' << streak;
}
// The player saw the notice and chose to go on: the next crash starts a new count.
inline void ResetStreak(const std::filesystem::path& folder, DWORD pid = GetCurrentProcessId()) {
  g_unclean_streak.store(0);
  const auto marker = folder / kMarkerName;
  if (ReadMarker(marker) == pid) std::ofstream(marker, std::ios::trunc) << pid << ' ' << 0;
}

// One session without the hooks a player can turn on (the DLSS preset, XMX, a
// forced multiplier), chosen in the notice after kSafeModeOffer unclean exits.
// The request is a file. The next process adopts it by writing its pid, so the
// copies ReShade loads and unloads at startup all see it; a later process
// removes it.
constexpr const wchar_t* kSafeModeName = L"MFGAdaUnlock.safe-mode";
inline std::atomic_bool g_safe_mode{false};
inline bool RequestSafeMode(const std::filesystem::path& folder) {
  std::ofstream file(folder / kSafeModeName, std::ios::trunc);
  file << 0;
  return bool(file);
}
inline bool ConsumeSafeMode(const std::filesystem::path& folder, DWORD pid = GetCurrentProcessId()) {
  std::error_code error;
  const auto request = folder / kSafeModeName;
  bool safe = false;
  if (std::filesystem::exists(request, error)) {
    const DWORD owner = ReadMarker(request);
    safe = owner == 0 || owner == pid;
    if (owner == 0) std::ofstream(request, std::ios::trunc) << pid;
    if (!safe) std::filesystem::remove(request, error);
  }
  g_safe_mode.store(safe);
  return safe;
}
// Only its own marker: another copy of the game may have written a newer one.
inline void EndSession(const std::filesystem::path& folder, DWORD pid = GetCurrentProcessId()) {
  const auto marker = folder / kMarkerName;
  if (ReadMarker(marker) == pid) DeleteFileW(marker.c_str());
}

inline bool IsSupportFile(std::wstring name) {
  std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
  const bool log = name.find(L".log") != std::wstring::npos;
  if (name.rfind(L"reshade", 0) == 0) return log;
  if (name.rfind(L"mfgadaunlock", 0) == 0) return log || name.ends_with(L".ini");
  return false;
}
// The game and ReShade hold their logs open for writing, which CopyFile can refuse.
inline bool CopyShared(const std::filesystem::path& from, const std::filesystem::path& to) {
  const HANDLE source = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (source == INVALID_HANDLE_VALUE) return false;
  std::ofstream target(to, std::ios::binary | std::ios::trunc);
  std::vector<char> buffer(1 << 16);
  DWORD count = 0;
  bool ok = bool(target);
  while (ok && ReadFile(source, buffer.data(), DWORD(buffer.size()), &count, nullptr) && count != 0)
    ok = bool(target.write(buffer.data(), count));
  CloseHandle(source);
  return ok;
}
struct SaveResult {
  std::filesystem::path folder;
  int copied{};
  bool ok{};
};
inline std::wstring TimeStamp() {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t text[32]{};
  swprintf_s(text, L"%04u-%02u-%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  return text;
}
inline SaveResult Save(const std::filesystem::path& folder, const std::string& report, const std::wstring& stamp = TimeStamp()) {
  SaveResult result;
  std::error_code error;
  result.folder = folder / (L"MFG-Support-" + stamp);
  std::filesystem::create_directories(result.folder, error);
  if (error) return result;
  std::ofstream(result.folder / L"report.txt", std::ios::binary | std::ios::trunc) << report;
  for (std::filesystem::directory_iterator entry(folder, error), end; !error && entry != end; entry.increment(error)) {
    std::error_code fileError;
    if (!entry->is_regular_file(fileError) || !IsSupportFile(entry->path().filename().wstring())) continue;
    if (entry->file_size(fileError) > kMaxCopiedFile || fileError) continue;
    if (CopyShared(entry->path(), result.folder / entry->path().filename())) ++result.copied;
  }
  result.ok = std::filesystem::exists(result.folder / L"report.txt", error);
  return result;
}
}  // namespace mfgunlock::support
