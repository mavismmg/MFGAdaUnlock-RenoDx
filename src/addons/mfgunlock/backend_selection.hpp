#pragma once
#include <windows.h>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include "config_store.hpp"

namespace mfgunlock::backend {
enum class Kind { DlssG, XeMfg, Invalid };
inline Kind Parse(std::wstring_view value) {
  // Hand-edited files may differ in case or padding (1.5.2).
  while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
  while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
  const std::wstring text(value);
  if (!_wcsicmp(text.c_str(), L"DLSSG")) return Kind::DlssG;
  if (!_wcsicmp(text.c_str(), L"XeMFG")) return Kind::XeMfg;
  return Kind::Invalid;
}
inline std::filesystem::path Directory() {
  std::wstring path(32768, L'\0');
  const auto n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (!n || n >= path.size()) return {};
  path.resize(n);
  return std::filesystem::path(path).parent_path();
}
inline Kind Read() {
  const auto dir = Directory();
  if (dir.empty()) return Kind::Invalid;
  wchar_t value[32]{};
  GetPrivateProfileStringW(L"Backend", L"Selected", L"DLSSG", value, 32,
                          (dir/L"MFGAdaUnlock.ini").c_str());
  return Parse(value);
}
// The backend a process starts with. An unreadable or unknown preference keeps
// the DLSS-G backend running, as before the selector existed (1.4.5); the panel
// reports the invalid value (1.5.2).
inline Kind Effective(Kind value) { return value == Kind::Invalid ? Kind::DlssG : value; }
inline bool Select(Kind value) {
  if ((value != Kind::DlssG && value != Kind::XeMfg) || Directory().empty()) return false;
  return config::Save(Directory()/L"MFGAdaUnlock.ini",{{L"Backend",L"Selected",value==Kind::XeMfg?L"XeMFG":L"DLSSG"}});
}
// 1.8: what the user chooses. XMX keeps the DLSS-G backend, whose presentation it
// uses, and replaces the generator.
enum class Mode { DlssG, XeMfg, Xmx };
inline Mode ReadMode() {
  if (Read() == Kind::XeMfg) return Mode::XeMfg;
  return GetPrivateProfileIntW(L"DLSSG", L"XmxGenerator", 0, (Directory()/L"MFGAdaUnlock.ini").c_str()) == 1 ? Mode::Xmx : Mode::DlssG;
}
inline bool SelectMode(Mode value) {
  if (Directory().empty()) return false;
  return config::Save(Directory()/L"MFGAdaUnlock.ini",{{L"Backend",L"Selected",value==Mode::XeMfg?L"XeMFG":L"DLSSG"},
      {L"DLSSG",L"XmxGenerator",value==Mode::Xmx?L"1":L"0"}});
}
// Latched at process startup. A pending setting never changes a live owner.
inline Kind active = Kind::Invalid;
}
