// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (plan 5): the panel's text in six languages. Every user-visible
// literal of the panel became a key in string_keys.hpp and a line in
// strings_en.inl (tools/extract_panel_strings.py); the other tables hold the
// same lines in enum order, or nullptr where a translation is missing, and T()
// falls back to English. The log, the support report and the names of
// settings stay English: they are read by whoever gives support.
//
// The language: [Panel] Language in MFGAdaUnlock.ini (en, pt-BR, es, ja, ko,
// zh-CN, or auto). With auto, ReShade's own overlay language ([OVERLAY]
// Language in ReShade.ini), and without it the Windows display language.
// The selector in the panel's footer switches at once and saves for the next
// start. Pure: no ReShade or ImGui here, so tests/i18n_tests.cpp can run it.
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>

#include "string_keys.hpp"

namespace mfgunlock::i18n {
enum class Language : int { kEnglish = 0, kPortugueseBrazil, kSpanish, kJapanese, kKorean, kChineseSimplified, kCount };

inline constexpr const char* kEnglish[] = {
#include "strings_en.inl"
};
inline constexpr const char* kPortugueseBrazil[] = {
#include "strings_pt_br.inl"
};
inline constexpr const char* kSpanish[] = {
#include "strings_es.inl"
};
inline constexpr const char* kJapanese[] = {
#include "strings_ja.inl"
};
inline constexpr const char* kKorean[] = {
#include "strings_ko.inl"
};
inline constexpr const char* kChineseSimplified[] = {
#include "strings_zh_cn.inl"
};
static_assert(std::size(kEnglish) == static_cast<size_t>(Str::kCount), "strings_en.inl and string_keys.hpp differ");
static_assert(std::size(kPortugueseBrazil) == static_cast<size_t>(Str::kCount), "strings_pt_br.inl is out of step");
static_assert(std::size(kSpanish) == static_cast<size_t>(Str::kCount), "strings_es.inl is out of step");
static_assert(std::size(kJapanese) == static_cast<size_t>(Str::kCount), "strings_ja.inl is out of step");
static_assert(std::size(kKorean) == static_cast<size_t>(Str::kCount), "strings_ko.inl is out of step");
static_assert(std::size(kChineseSimplified) == static_cast<size_t>(Str::kCount), "strings_zh_cn.inl is out of step");

inline constexpr const char* const* kTables[] = {kEnglish, kPortugueseBrazil, kSpanish, kJapanese, kKorean, kChineseSimplified};
struct Info {
  Language language;
  const char* code;        // the INI value
  const char* name;        // in its own language, for the selector
  bool machine_translated; // 1.10.0: no community reviewer yet
};
inline constexpr Info kInfo[] = {
    {Language::kEnglish, "en", "English", false},
    {Language::kPortugueseBrazil, "pt-BR", "Português (Brasil)", true},
    {Language::kSpanish, "es", "Español", true},
    {Language::kJapanese, "ja", "日本語", true},
    {Language::kKorean, "ko", "한국어", true},
    {Language::kChineseSimplified, "zh-CN", "简体中文", true},
};
static_assert(std::size(kInfo) == static_cast<size_t>(Language::kCount));
static_assert(std::size(kTables) == static_cast<size_t>(Language::kCount));

// The language chosen, and the one T() reads this frame: the panel falls back
// to English for a frame when the font cannot draw the chosen language.
inline std::atomic<int> g_selected{static_cast<int>(Language::kEnglish)};
inline std::atomic<int> g_effective{static_cast<int>(Language::kEnglish)};
inline Language Selected() { return static_cast<Language>(g_selected.load(std::memory_order_relaxed)); }
inline Language Effective() { return static_cast<Language>(g_effective.load(std::memory_order_relaxed)); }
inline void Select(Language language) {
  g_selected.store(static_cast<int>(language), std::memory_order_relaxed);
  g_effective.store(static_cast<int>(language), std::memory_order_relaxed);
}
inline void UseForThisFrame(Language language) { g_effective.store(static_cast<int>(language), std::memory_order_relaxed); }

// The text for a key in the language of this frame, English when it is missing.
inline const char* TextIn(Language language, Str key) {
  const auto index = static_cast<size_t>(key);
  if (index >= static_cast<size_t>(Str::kCount)) return "";
  const auto table = static_cast<size_t>(language);
  const char* text = table < std::size(kTables) ? kTables[table][index] : nullptr;
  return text != nullptr && text[0] != '\0' ? text : kEnglish[index];
}
inline const char* T(Str key) { return TextIn(Effective(), key); }

// "pt-BR", "pt", "ja-JP", "zh-CN", "zh"... by prefix; anything else is English.
// Chinese: only the simplified table exists; a traditional setting gets it too.
inline Language FromCode(const char* code) {
  if (code == nullptr || code[0] == '\0') return Language::kEnglish;
  const auto prefix = [&](const char* p) { return _strnicmp(code, p, std::strlen(p)) == 0; };
  if (prefix("pt")) return Language::kPortugueseBrazil;
  if (prefix("es")) return Language::kSpanish;
  if (prefix("ja")) return Language::kJapanese;
  if (prefix("ko")) return Language::kKorean;
  if (prefix("zh")) return Language::kChineseSimplified;
  return Language::kEnglish;
}
inline bool IsAuto(const char* code) { return code == nullptr || code[0] == '\0' || _stricmp(code, "auto") == 0; }
// The panel's language from the saved choice, ReShade's language and Windows'.
inline Language Resolve(const char* saved, const char* reshade_language, const char* windows_language) {
  if (!IsAuto(saved)) return FromCode(saved);
  if (reshade_language != nullptr && reshade_language[0] != '\0') return FromCode(reshade_language);
  return FromCode(windows_language);
}

// Languages whose glyphs Cascadia Mono, the panel's default font, does not have.
inline bool NeedsCjkFont(Language language) {
  return language == Language::kJapanese || language == Language::kKorean || language == Language::kChineseSimplified;
}
// A glyph the language needs, to ask the font (ImFont::IsGlyphInFont).
inline unsigned ProbeGlyph(Language language) {
  switch (language) {
    case Language::kJapanese: return 0x3042;          // あ
    case Language::kKorean: return 0xD55C;            // 한
    case Language::kChineseSimplified: return 0x4E2D; // 中
    default: return 'A';
  }
}
// The Windows fonts ReShade itself picks for these languages (runtime_gui.cpp,
// build_font_atlas), first choice and fallback.
inline const char* SystemFontFor(Language language, bool fallback) {
  switch (language) {
    case Language::kJapanese: return fallback ? "C:\\Windows\\Fonts\\msgothic.ttc" : "C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc";
    case Language::kKorean: return fallback ? nullptr : "C:\\Windows\\Fonts\\malgun.ttf";
    case Language::kChineseSimplified: return fallback ? "C:\\Windows\\Fonts\\simsun.ttc" : "C:\\Windows\\Fonts\\msyh.ttc";
    default: return nullptr;
  }
}
// The font to write into ReShade's [STYLE] Font for this language, or nullptr
// to leave it: only when it is empty or is the Cascadia Mono the addon set
// itself (addon_font), never over a font the player chose.
// 1.10.0: a CJK font the addon set for an earlier language is the addon's too: leaving Japanese for
// English gave KCD2's panel MS Gothic for good, and Japanese to Chinese kept the Japanese font.
inline bool IsAddonCjkFont(const char* font) {
  if (font == nullptr || font[0] == '\0') return false;
  for (const Language language : {Language::kJapanese, Language::kKorean, Language::kChineseSimplified})
    for (const bool fallback : {false, true})
      if (const char* f = SystemFontFor(language, fallback); f != nullptr && _stricmp(font, f) == 0) return true;
  return false;
}
template <class Exists>
inline const char* FontToSet(Language language, const char* current_font, const char* addon_font, Exists exists) {
  if (!NeedsCjkFont(language)) {
    // Back from a CJK language: the addon's Latin font again.
    return IsAddonCjkFont(current_font) && addon_font != nullptr && exists(addon_font) ? addon_font : nullptr;
  }
  const bool ours = current_font == nullptr || current_font[0] == '\0' || IsAddonCjkFont(current_font) ||
                    (addon_font != nullptr && _stricmp(current_font, addon_font) == 0);
  if (!ours) return nullptr;
  for (const bool fallback : {false, true}) {
    const char* font = SystemFontFor(language, fallback);
    if (font != nullptr && exists(font)) return font;
  }
  return nullptr;
}
}  // namespace mfgunlock::i18n

// The panel's code calls T(Str::k...) unqualified, in mfgunlock::ui and the
// XeMFG panel's mfgunlock::xemfg::native.
namespace mfgunlock {
using i18n::Str;
using i18n::T;
}  // namespace mfgunlock
