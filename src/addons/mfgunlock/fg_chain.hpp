// SPDX-License-Identifier: MIT
#pragma once
// 1.9.0: where each link of the frame-generation chain came from, for the
// panel's "Frame generation chain" card and the support report. Pure path
// classification; the addon collects the paths. "Unknown" whenever the link
// has not been seen yet: nothing here guesses.
#include <cwchar>
#include <cwctype>
#include <string>

namespace mfgunlock::fgchain {

enum class Source { kUnknown, kGame, kNvidiaAppOverride, kNvidiaAppUpdate, kNvidiaApp, kDriver, kMfgUnlock };

// 1.10.1: Add frame generation's own Streamline, beside the addon (the Metro Exodus
// report called those files the game's).
inline bool InMfgUnlockFolder(const std::wstring& lower) {
  return lower.find(L"\\mfgunlock-streamline\\") != std::wstring::npos;
}

inline std::wstring Lower(const wchar_t* path) {
  std::wstring out = path != nullptr ? path : L"";
  for (auto& c : out) c = static_cast<wchar_t>(towlower(c));
  return out;
}

// sl.dlss_g.dll (Streamline's DLSS-G plugin). The NVIDIA app puts its copies in
// ...\models\sl_dlss_g_override_0 (the "DLSS Override" setting) or
// ...\models\sl_dlss_g_0 (its automatic update); anything else came with the game.
inline Source ClassifyStreamline(const wchar_t* path) {
  const std::wstring p = Lower(path);
  if (p.empty()) return Source::kUnknown;
  if (InMfgUnlockFolder(p)) return Source::kMfgUnlock;
  if (p.find(L"\\sl_dlss_g_override_") != std::wstring::npos) return Source::kNvidiaAppOverride;
  if (p.find(L"\\sl_dlss_g_") != std::wstring::npos) return Source::kNvidiaAppUpdate;
  return Source::kGame;
}

// nvngx_dlssg.dll (or its .bin). The driver's built-in copy lives in the
// DriverStore; the NVIDIA app's in its NGX models folder.
inline Source ClassifyProvider(const wchar_t* path) {
  const std::wstring p = Lower(path);
  if (p.empty()) return Source::kUnknown;
  if (InMfgUnlockFolder(p)) return Source::kMfgUnlock;
  if (p.find(L"\\driverstore\\") != std::wstring::npos) return Source::kDriver;
  if (p.find(L"\\nvidia\\ngx\\") != std::wstring::npos || p.find(L"\\ngx\\models\\") != std::wstring::npos)
    return Source::kNvidiaApp;
  return Source::kGame;
}

inline const char* Name(Source source) {
  switch (source) {
    case Source::kGame: return "the game's files";
    case Source::kNvidiaAppOverride: return "NVIDIA app (DLSS Override)";
    case Source::kNvidiaAppUpdate: return "NVIDIA app (automatic update)";
    case Source::kNvidiaApp: return "NVIDIA app";
    case Source::kDriver: return "NVIDIA driver (built-in, 2x only)";
    case Source::kMfgUnlock: return "MFG Unlock's Streamline (MFGUnlock-Streamline)";
    default: return "unknown";
  }
}

}  // namespace mfgunlock::fgchain
