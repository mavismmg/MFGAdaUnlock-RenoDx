// SPDX-License-Identifier: MIT
// 1.10.0 (plan 1.1): the Streamline log environment the addon sets for support.
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "../src/addons/mfgunlock/streamline_log.hpp"

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition \
                << '\n';                                                    \
      std::abort();                                                         \
    }                                                                       \
  } while (false)

using namespace mfgunlock::streamlinelog;

int main() {
  const std::filesystem::path folder = L"C:\\Games\\Example";
  // Nothing set: all three variables, verbose level, the game folder, sl.log.
  {
    const auto plan = Plan(folder, [](const wchar_t*) { return false; });
    CHECK(plan.size() == 3);
    CHECK(std::wstring(plan[0].name) == L"SL_LOG_LEVEL" && plan[0].value == L"2");
    CHECK(std::wstring(plan[1].name) == L"SL_LOG_PATH" && plan[1].value == folder.wstring());
    CHECK(std::wstring(plan[2].name) == L"SL_LOG_NAME" && plan[2].value == L"sl.log");
  }
  // 1.10.0 (D4): the modes. Inherit sets nothing; default is level 1; off is
  // only SL_LOG_LEVEL=0; unknown values inherit.
  {
    auto none = [](const wchar_t*) { return false; };
    CHECK(Plan(folder, none, Mode::kInherit).empty());
    const auto standard = Plan(folder, none, Mode::kDefault);
    CHECK(standard.size() == 3 && standard[0].value == L"1");
    const auto off = Plan(folder, none, Mode::kOff);
    CHECK(off.size() == 1 && std::wstring(off[0].name) == L"SL_LOG_LEVEL" && off[0].value == L"0");
    CHECK(ModeFromValue(0) == Mode::kInherit && ModeFromValue(1) == Mode::kVerbose &&
          ModeFromValue(2) == Mode::kDefault && ModeFromValue(3) == Mode::kOff && ModeFromValue(9) == Mode::kInherit);
  }
  // The player's own level is kept; the others are still set.
  {
    const auto plan = Plan(folder, [](const wchar_t* name) { return std::wstring(name) == L"SL_LOG_LEVEL"; });
    CHECK(plan.size() == 2);
    CHECK(std::wstring(plan[0].name) == L"SL_LOG_PATH" && std::wstring(plan[1].name) == L"SL_LOG_NAME");
  }
  // Apply writes the process environment and reports it.
  {
    for (const wchar_t* name : {kLevelName, kPathName, kFileName}) SetEnvironmentVariableW(name, nullptr);
    CHECK(!EnvironmentHas(kLevelName));
    CHECK(Apply(folder, Mode::kVerbose) == State::kSet && Current() == State::kSet);
    wchar_t value[512]{};
    CHECK(GetEnvironmentVariableW(kLevelName, value, 512) != 0 && std::wstring(value) == L"2");
    CHECK(GetEnvironmentVariableW(kPathName, value, 512) != 0 && std::wstring(value) == folder.wstring());
    CHECK(GetEnvironmentVariableW(kFileName, value, 512) != 0 && std::wstring(value) == L"sl.log");
    // A second start (ReShade's temporary copies) finds them and keeps them.
    CHECK(Apply(folder, Mode::kVerbose) == State::kKept);
    for (const wchar_t* name : {kLevelName, kPathName, kFileName}) SetEnvironmentVariableW(name, nullptr);
  }
  CHECK(Describe(State::kOff) == "off");
  CHECK(Describe(State::kSet).find("sl.log") != std::string::npos);
  CHECK(!Describe(State::kKept).empty() && !Describe(State::kFailed).empty());
  std::cout << "streamline_log_tests passed\n";
  return 0;
}
