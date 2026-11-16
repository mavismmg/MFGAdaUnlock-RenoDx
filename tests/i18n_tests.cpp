// SPDX-License-Identifier: MIT
// 1.10.0 (plan 5): the panel's language tables and the choice of language.
// Every translation must keep the English text's printf conversions in the
// same order (the panel passes them to ImGui::Text), the same NUL separators
// (ImGui::Combo lists), and the spaces at its edges (fragments joined at run
// time); a missing translation falls back to English, never to "".
#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "../src/addons/mfgunlock/i18n/strings.hpp"

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition \
                << '\n';                                                    \
      std::abort();                                                         \
    }                                                                       \
  } while (false)

using namespace mfgunlock::i18n;

namespace {
// The printf conversions of a string, in order ("%llu", "%.4g", "%%", ...).
std::vector<std::string> Conversions(const char* text) {
  std::vector<std::string> out;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p != '%') continue;
    const char* start = p++;
    while (*p != '\0' && std::string("-+ #0123456789.").find(*p) != std::string::npos) ++p;
    while (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'I' || *p == '6' || *p == '4') ++p;
    if (*p == '\0') break;
    // A literal percent sign in a label ("buffer (%)") is not a conversion.
    if (std::string("diuxXfFeEgGcsp%").find(*p) == std::string::npos) continue;
    out.emplace_back(start, p + 1);
  }
  return out;
}
// The ImGui::Combo item lists, the only entries with NUL separators. They end
// with an empty item ("a\0b\0" plus the literal's own terminator), so walking
// them stops inside the literal. Other entries are not walked past their end.
constexpr Str kComboLists[] = {Str::kGeneralQualityLatencyDiagnostics_b9c8, Str::kAutoRecommendedReflexStreamline_5561,
                               Str::kAutoXellXellReflex_7591};
size_t Items(Language language, Str key) {
  size_t count = 0;
  for (const char* p = TextIn(language, key); *p != '\0'; p += std::strlen(p) + 1) ++count;
  return count;
}
}  // namespace

int main() {
  const size_t count = static_cast<size_t>(Str::kCount);
  CHECK(count > 800);
  int translated = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto key = static_cast<Str>(i);
    const char* english = kEnglish[i];
    CHECK(english != nullptr && english[0] != '\0');
    for (const auto& info : kInfo) {
      const char* text = TextIn(info.language, key);
      CHECK(text != nullptr && text[0] != '\0');  // never an empty label
      if (Conversions(text) != Conversions(english)) {
        std::cerr << info.code << " key " << i << ": conversions differ from English\n";
        std::abort();
      }
      CHECK((text[0] == ' ') == (english[0] == ' '));
      const size_t a = std::strlen(text), b = std::strlen(english);
      CHECK((text[a - 1] == ' ') == (english[b - 1] == ' '));
      if (info.language != Language::kEnglish && kTables[static_cast<int>(info.language)][i] != nullptr) ++translated;
    }
  }
  CHECK(translated == static_cast<int>(count) * 5);  // 1.10.0: complete in every language
  for (const auto list : kComboLists) {
    const size_t items = Items(Language::kEnglish, list);
    CHECK(items >= 2);
    for (const auto& info : kInfo) CHECK(Items(info.language, list) == items);
  }

  // Choosing the language.
  CHECK(FromCode("pt-BR") == Language::kPortugueseBrazil && FromCode("pt") == Language::kPortugueseBrazil);
  CHECK(FromCode("es-419") == Language::kSpanish && FromCode("ja-JP") == Language::kJapanese);
  CHECK(FromCode("ko-KR") == Language::kKorean && FromCode("zh-TW") == Language::kChineseSimplified);
  CHECK(FromCode("de-DE") == Language::kEnglish && FromCode("") == Language::kEnglish && FromCode(nullptr) == Language::kEnglish);
  CHECK(Resolve("ja", "pt-BR", "ko-KR") == Language::kJapanese);           // the saved choice wins
  CHECK(Resolve("auto", "pt-BR", "ko-KR") == Language::kPortugueseBrazil); // then ReShade's
  CHECK(Resolve("", "", "ko-KR") == Language::kKorean);                    // then Windows'
  CHECK(Resolve("auto", nullptr, "fr-FR") == Language::kEnglish);
  for (const auto& info : kInfo) CHECK(FromCode(info.code) == info.language);

  // T() follows the frame's language; Select() sets both.
  Select(Language::kJapanese);
  CHECK(std::string(T(Str::kGeneral_fd0b)) == kJapanese[static_cast<size_t>(Str::kGeneral_fd0b)]);
  UseForThisFrame(Language::kEnglish);  // the font cannot draw Japanese this frame
  CHECK(std::string(T(Str::kGeneral_fd0b)) == "General" && Selected() == Language::kJapanese);
  Select(Language::kEnglish);

  // The CJK font: only over an empty font or the addon's own Cascadia Mono.
  const char* cascadia = "C:\\Windows\\Fonts\\CascadiaMono.ttf";
  const auto all = [](const char*) { return true; };
  const auto none = [](const char*) { return false; };
  const auto only_fallback = [](const char* path) { return std::string(path).find("msgothic") != std::string::npos; };
  CHECK(std::string(FontToSet(Language::kJapanese, "", cascadia, all)) == "C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc");
  CHECK(std::string(FontToSet(Language::kJapanese, cascadia, cascadia, only_fallback)) == "C:\\Windows\\Fonts\\msgothic.ttc");
  CHECK(std::string(FontToSet(Language::kKorean, nullptr, cascadia, all)) == "C:\\Windows\\Fonts\\malgun.ttf");
  CHECK(std::string(FontToSet(Language::kChineseSimplified, "c:\\windows\\fonts\\cascadiamono.ttf", cascadia, all)) ==
        "C:\\Windows\\Fonts\\msyh.ttc");
  CHECK(FontToSet(Language::kJapanese, "D:\\Fonts\\MyFont.ttf", cascadia, all) == nullptr);  // the player's choice
  CHECK(FontToSet(Language::kPortugueseBrazil, "", cascadia, all) == nullptr);                // Latin script
  CHECK(FontToSet(Language::kKorean, "", cascadia, none) == nullptr);                         // not on this Windows
  // 1.10.0: back to a Latin language from a CJK font the addon set, and from one CJK language to another.
  CHECK(std::string(FontToSet(Language::kEnglish, "C:\\Windows\\Fonts\\msgothic.ttc", cascadia, all)) == cascadia);
  CHECK(std::string(FontToSet(Language::kChineseSimplified, "C:\\Windows\\Fonts\\msgothic.ttc", cascadia, all)) ==
        "C:\\Windows\\Fonts\\msyh.ttc");
  CHECK(FontToSet(Language::kEnglish, "D:\\Fonts\\MyFont.ttf", cascadia, all) == nullptr);  // the player's choice
  CHECK(NeedsCjkFont(Language::kJapanese) && !NeedsCjkFont(Language::kSpanish));
  CHECK(ProbeGlyph(Language::kChineseSimplified) == 0x4E2D);
  std::cout << "i18n tests passed: " << count << " strings in " << std::size(kInfo) << " languages\n";
  return 0;
}
