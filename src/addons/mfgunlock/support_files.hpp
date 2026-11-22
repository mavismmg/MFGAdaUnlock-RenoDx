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
// 1.11.0 (S1): how far the session got. Unreal games, launchers, anti-cheats and Steam's Stop end
// the process with TerminateProcess, which skips DLL_PROCESS_DETACH and so EndSession: the marker
// stayed and the next start said "the last session may have closed unexpectedly" after a normal
// exit. A session that started closing (its last swap chain gone, or TerminateProcess on itself)
// counts as clean, unless the crash record was written after it started.
enum class Stage : uint32_t { kRunning = 0, kClosing = 1 };
struct Marker {
  DWORD pid = 0;
  uint32_t streak = 0;
  Stage stage = Stage::kRunning;
  uint64_t start = 0;  // FILETIME of BeginSession; 0 in markers older than 1.11.0
};
inline Marker ReadFullMarker(const std::filesystem::path& marker) {
  std::ifstream file(marker);
  Marker m;
  uint32_t stage = 0;
  if (!(file >> m.pid)) return {};
  if (file >> m.streak && file >> stage) {
    m.stage = stage == 1 ? Stage::kClosing : Stage::kRunning;
    file >> m.start;
  }
  return m;
}
inline void WriteMarker(const std::filesystem::path& marker, const Marker& m) {
  std::ofstream(marker, std::ios::trunc) << m.pid << ' ' << m.streak << ' ' << static_cast<uint32_t>(m.stage) << ' '
                                         << m.start;
}
// Pure, for the test: whether the session a marker was left by ended without a clean exit.
constexpr bool EndedUnclean(bool same_process, bool owner_alive, Stage stage, bool crash_after_start) {
  if (same_process || owner_alive) return false;
  if (crash_after_start) return true;
  return stage != Stage::kClosing;
}
inline uint64_t NowFileTime() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}
// The crash record (crash_record.hpp, CrashDump=1) written after that session started.
inline bool CrashRecordAfter(const std::filesystem::path& folder, uint64_t start) {
  if (start == 0) return false;
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW((folder / L"MFGAdaUnlock-crash.txt").c_str(), GetFileExInfoStandard, &data)) return false;
  const uint64_t written = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
  return written > start;
}
// A marker left by a process that is gone: that session ended without a clean
// exit, unless it was closing (S1). A crash the game swallows after closing looks
// clean too, which is why the panel still says "may have".
inline void BeginSession(const std::filesystem::path& folder, DWORD pid = GetCurrentProcessId()) {
  std::error_code error;
  const auto marker = folder / kMarkerName;
  uint32_t streak = 0;
  if (std::filesystem::exists(marker, error)) {
    const Marker previous = ReadFullMarker(marker);
    streak = previous.streak;
    if (EndedUnclean(previous.pid == pid, previous.pid != 0 && previous.pid != pid && ProcessAlive(previous.pid),
                     previous.stage, CrashRecordAfter(folder, previous.start))) {
      g_previous_unclean.store(true);
      ++streak;
    } else if (previous.pid != pid && !(previous.pid != 0 && ProcessAlive(previous.pid))) {
      streak = 0;  // closed cleanly: a new count
    }
  }
  g_unclean_streak.store(streak);
  const Marker current = ReadFullMarker(marker);
  // A second copy of the addon in the same process keeps the first copy's start.
  WriteMarker(marker, {pid, streak, Stage::kRunning, current.pid == pid && current.start ? current.start : NowFileTime()});
}
// S1: the session reached (or left) its closing stage. Only its own marker.
inline void MarkStage(const std::filesystem::path& folder, Stage stage, DWORD pid = GetCurrentProcessId()) {
  const auto marker = folder / kMarkerName;
  Marker m = ReadFullMarker(marker);
  if (m.pid != pid || m.stage == stage) return;
  m.stage = stage;
  WriteMarker(marker, m);
}
// The player saw the notice and chose to go on: the next crash starts a new count.
inline void ResetStreak(const std::filesystem::path& folder, DWORD pid = GetCurrentProcessId()) {
  g_unclean_streak.store(0);
  const auto marker = folder / kMarkerName;
  Marker m = ReadFullMarker(marker);
  if (m.pid != pid) return;
  m.streak = 0;
  WriteMarker(marker, m);
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
  // 1.10.0: also the crash record and its dump ([Diagnostics] CrashDump=1).
  if (name.rfind(L"mfgadaunlock", 0) == 0)
    return log || name.ends_with(L".ini") || name == L"mfgadaunlock-crash.txt" ||
           (name.rfind(L"mfgadaunlock-crash-", 0) == 0 && name.ends_with(L".dmp"));
  // 1.10.0: Streamline's own log (sl.log), written when [DLSSG] StreamlineLog=1.
  if (name.rfind(L"sl.", 0) == 0) return log;
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
