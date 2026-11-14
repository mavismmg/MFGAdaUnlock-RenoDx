// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (plan 1.1): Streamline's own log, for support. The production
// sl.interposer.dll reads SL_LOG_LEVEL, SL_LOG_PATH and SL_LOG_NAME from the
// environment when the game calls slInit ("ConfigureLogOverridesFromEnvironment"
// in the 2.14.16 binary; the public guide does not document it) and writes
// sl.log there. With [DLSSG] StreamlineLog=1 the addon sets those variables in
// its own process before the game starts Streamline, so the plugin's reasons
// ("NGX indicates DLSS-G is not available", "not supported on current
// hardware", "window not focused") land in a file the support report can copy.
//
// A game that starts Streamline before the addon loads (Rift Apart) reads the
// environment too early; there the registry route the panel shows is the only
// way: HKCU\SOFTWARE\NVIDIA Corporation\Global\Streamline, LogLevel and LogPath.
// Variables the player set already are kept.
//
// 1.10.0 (dashdogy item D4, after JavieEm's streamlineLogMode): the value is a
// mode. 0 leaves NVIDIA's own settings alone (the default), 1 is the verbose
// log (the original StreamlineLog=1), 2 the default level, 3 turns
// Streamline's log off for this game (SL_LOG_LEVEL=0 only). The registry is
// never changed by the addon.
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "backend_selection.hpp"

namespace mfgunlock::streamlinelog {
constexpr const wchar_t* kLevelName = L"SL_LOG_LEVEL";
constexpr const wchar_t* kPathName = L"SL_LOG_PATH";
constexpr const wchar_t* kFileName = L"SL_LOG_NAME";
constexpr const wchar_t* kVerbose = L"2";
constexpr const wchar_t* kLogFile = L"sl.log";
constexpr const wchar_t* kRegistryKey = L"HKCU\\SOFTWARE\\NVIDIA Corporation\\Global\\Streamline";

enum class Mode : int { kInherit = 0, kVerbose = 1, kDefault = 2, kOff = 3 };
constexpr Mode ModeFromValue(int value) {
  return value >= 1 && value <= 3 ? static_cast<Mode>(value) : Mode::kInherit;
}
constexpr const wchar_t* LevelFor(Mode mode) {
  return mode == Mode::kOff ? L"0" : mode == Mode::kDefault ? L"1" : kVerbose;
}
inline Mode Configured() {
  return ModeFromValue(GetPrivateProfileIntW(L"DLSSG", L"StreamlineLog", 0,
                                             (backend::Directory() / L"MFGAdaUnlock.ini").c_str()));
}
inline bool Requested() { return Configured() != Mode::kInherit; }

struct Variable {
  const wchar_t* name;
  std::wstring value;
};
// Which variables to set for a log in `folder`, skipping the ones `already_set`
// says exist. Pure, so the test needs no environment.
template <class AlreadySet>
inline std::vector<Variable> Plan(const std::filesystem::path& folder, AlreadySet already_set,
                                  Mode mode = Mode::kVerbose) {
  std::vector<Variable> plan;
  if (mode == Mode::kInherit) return plan;
  if (!already_set(kLevelName)) plan.push_back({kLevelName, LevelFor(mode)});
  if (mode == Mode::kOff) return plan;  // no file to name
  if (!already_set(kPathName)) plan.push_back({kPathName, folder.wstring()});
  if (!already_set(kFileName)) plan.push_back({kFileName, kLogFile});
  return plan;
}

enum class State : int { kOff = 0, kSet = 1, kKept = 2, kFailed = 3 };
inline std::atomic<int> g_state{static_cast<int>(State::kOff)};
inline State Current() { return static_cast<State>(g_state.load(std::memory_order_acquire)); }

inline bool EnvironmentHas(const wchar_t* name) {
  return GetEnvironmentVariableW(name, nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

// Sets the variables for this process. Returns the state for the log line.
inline State Apply(const std::filesystem::path& folder, Mode mode) {
  const auto plan = Plan(folder, EnvironmentHas, mode);
  State state = plan.size() == (mode == Mode::kOff ? 1u : 3u) ? State::kSet : State::kKept;
  for (const auto& variable : plan)
    if (!SetEnvironmentVariableW(variable.name, variable.value.c_str())) state = State::kFailed;
  g_state.store(static_cast<int>(state), std::memory_order_release);
  return state;
}

// One line for the log and the support report.
inline std::string Describe(State state) {
  switch (state) {
    case State::kSet:
      return Configured() == Mode::kOff
                 ? "turned off for this game (SL_LOG_LEVEL=0)"
                 : "environment set for this process; sl.log goes to the game folder once the game starts Streamline";
    case State::kKept: return "some SL_LOG_* variables were already set; they were kept";
    case State::kFailed: return "could not set the environment variables";
    default: return "off";
  }
}
}  // namespace mfgunlock::streamlinelog
