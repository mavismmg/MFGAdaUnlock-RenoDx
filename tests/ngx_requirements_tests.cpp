// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "../src/addons/mfgunlock/ngx_requirements.hpp"

using namespace mfgunlock::ngxreq;

static int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

static bool Passes(const char* id) { return ProjectIdPassesDriverCheck(id, std::strlen(id)); }

int main() {
  CHECK(Classify(kSuccess) == Failure::kNone);
  CHECK(Classify(kFeatureNotFound) == Failure::kFeatureMissing);
  CHECK(Classify(kFeatureNotSupported) == Failure::kFeatureMissing);
  CHECK(Classify(kInvalidParameter) == Failure::kIdentifierRejected);
  CHECK(Classify(0xBAD00002) == Failure::kOther);

  CHECK(Passes("a0f57b54-1daf-4934-90ae-c4035c19df04"));
  CHECK(Passes("{a0f57b54-1daf-4934-90ae-c4035c19df04}"));
  CHECK(Passes("\"[a0f57b541daf493490aec4035c19df04]\""));
  CHECK(!Passes("MyGame"));                                   // not hex
  CHECK(!Passes("a0f57b54-1daf-4934-90ae-c4035c19df0"));      // 31 digits
  CHECK(!Passes("a0f57b54-1daf-4934-90ae-c4035c19df045"));    // 33 digits
  CHECK(!Passes("00000000-0000-0000-0000-000000000000"));     // run of 6
  CHECK(Passes("a0f57b54-1daf-4934-90ae-c4035c11111f"));      // run of 5 is fine
  CHECK(!Passes("a0f57b54-1daf-4934-90ae-c4035c111-111"));    // a dash does not end a run
  CHECK(!ProjectIdPassesDriverCheck(nullptr, 0));

  CHECK(Decide(Failure::kFeatureMissing, true, true, true));
  CHECK(Decide(Failure::kIdentifierRejected, true, true, true));
  CHECK(!Decide(Failure::kOther, true, true, true));
  CHECK(!Decide(Failure::kNone, true, true, true));
  CHECK(!Decide(Failure::kFeatureMissing, false, true, true));  // the game's own query
  CHECK(!Decide(Failure::kFeatureMissing, true, false, true));  // nothing NGX could load
  CHECK(!Decide(Failure::kFeatureMissing, true, true, false));  // PreAdaRequirementsAnswer=0

  CHECK(ProviderPath(L"c:\\games\\x\\nvngx_dlssg.dll"));
  CHECK(ProviderPath(L"c:\\programdata\\nvidia\\ngx\\models\\dlssg\\versions\\20318480\\files\\160_e658700.bin"));
  CHECK(!ProviderPath(L"c:\\programdata\\nvidia\\ngx\\models\\dlss\\versions\\1\\files\\160_e658700.bin"));
  CHECK(!ProviderPath(L"c:\\programdata\\nvidia\\ngx\\models\\sl_dlss_g_0\\versions\\1\\files\\190_e658700.dll"));
  CHECK(!ProviderPath(L"c:\\games\\x\\nvngx_dlss.dll"));

  {
    // The 310.x shape: feature at [rsp+0x38], minimum 0x190 at [rsp+0x3c].
    const uint8_t body[] = {0x48, 0x83, 0xEC, 0x58, 0xC7, 0x44, 0x24, 0x38, 0x0B, 0x00, 0x00, 0x00,
                            0x90, 0xC7, 0x44, 0x24, 0x3C, 0x90, 0x01, 0x00, 0x00, 0xC3};
    CHECK(FindRequirementsArchitecture(body, sizeof(body)) == 17);
    uint8_t lowered[sizeof(body)];
    std::memcpy(lowered, body, sizeof(body));
    lowered[17] = 0x60;  // already Turing: still found, so a second pass is a no-op
    CHECK(FindRequirementsArchitecture(lowered, sizeof(lowered)) == 17);
    uint8_t wrong_disp[sizeof(body)];
    std::memcpy(wrong_disp, body, sizeof(body));
    wrong_disp[16] = 0x40;  // minimum not right after the feature
    CHECK(FindRequirementsArchitecture(wrong_disp, sizeof(wrong_disp)) == SIZE_MAX);
    const uint8_t two[] = {0xC7, 0x44, 0x24, 0x38, 0x0B, 0, 0, 0, 0xC7, 0x44, 0x24, 0x3C, 0x90, 0x01, 0, 0,
                           0xC7, 0x44, 0x24, 0x40, 0x90, 0x01, 0, 0};
    CHECK(FindRequirementsArchitecture(two, sizeof(two)) == SIZE_MAX);  // ambiguous
    const uint8_t none[] = {0xC7, 0x44, 0x24, 0x3C, 0x90, 0x01, 0, 0};
    CHECK(FindRequirementsArchitecture(none, sizeof(none)) == SIZE_MAX);  // no feature store
    CHECK(FindRequirementsArchitecture(nullptr, 0) == SIZE_MAX);
  }

  const auto app = Describe(kFeatureNotFound, false, nullptr, 0);
  CHECK(app.find("0xbad00004") != std::string::npos && app.find("application ID") != std::string::npos);
  const auto project = Describe(kInvalidParameter, true, "MyGame", 1);
  CHECK(project.find("refused the game's identifier") != std::string::npos);
  CHECK(project.find("fails the driver's GUID rule") != std::string::npos);

  if (failures == 0) std::printf("ngx_requirements: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
