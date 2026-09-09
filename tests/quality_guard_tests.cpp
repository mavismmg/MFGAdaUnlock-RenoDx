// SPDX-License-Identifier: MIT
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>

#include "../src/addons/mfgunlock/quality_guard.hpp"

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
  std::abort(); } } while (false)

sl::Resource MakeResource(uint32_t width, uint32_t height, uint32_t format) {
  sl::Resource resource(sl::ResourceType::eTex2d, reinterpret_cast<void*>(uintptr_t{1}), 0);
  resource.width = width;
  resource.height = height;
  resource.nativeFormat = format;
  return resource;
}

template <typename Callback>
void GuardedConstants(const sl::Constants& source, size_t prefix, Callback callback) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  auto* pages = static_cast<unsigned char*>(VirtualAlloc(
      nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  CHECK(pages != nullptr);
  DWORD old = 0;
  CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
  auto* guarded = reinterpret_cast<sl::Constants*>(pages + info.dwPageSize - prefix);
  std::memcpy(guarded, &source, prefix);
  callback(*guarded);
  CHECK(std::memcmp(guarded, &source, prefix) == 0);
  VirtualFree(pages, 0, MEM_RELEASE);
}

int main() {
  using namespace mfgunlock::qualityguard;

  auto backbuffer = MakeResource(2560, 1440, 10);
  auto matching_hudless = MakeResource(2560, 1440, 10);
  auto matching_ui = MakeResource(2560, 1440, 28);
  sl::ResourceTag matching[] = {
      {&backbuffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&matching_hudless, sl::kBufferTypeHUDLessColor,
       sl::ResourceLifecycle::eValidUntilPresent},
      {&matching_ui, sl::kBufferTypeUIColorAndAlpha,
       sl::ResourceLifecycle::eValidUntilPresent},
  };

  const Assessment hdr = AssessTags(matching, 3, true);
  CHECK(hdr.suppress_hud_separation);
  CHECK((hdr.issues & kHdrFinalColorIsolation) != 0);
  CHECK(hdr.observed_backbuffer.width == 2560 && hdr.observed_backbuffer.height == 1440);
  CHECK(hdr.observed_backbuffer.format == 10);

  const Assessment clean_sdr = AssessTags(matching, 3, false);
  CHECK(clean_sdr.has_hud_separation);
  CHECK(!clean_sdr.suppress_hud_separation);
  CHECK(clean_sdr.issues == kNone);

  auto wrong_extent = MakeResource(1920, 1080, 10);
  sl::ResourceTag extent_tags[] = {
      {&backbuffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&wrong_extent, sl::kBufferTypeHUDLessColor,
       sl::ResourceLifecycle::eValidUntilPresent},
  };
  const Assessment extent = AssessTags(extent_tags, 2, false);
  CHECK(extent.suppress_hud_separation);
  CHECK((extent.issues & kHudlessExtentMismatch) != 0);

  auto wrong_format = MakeResource(2560, 1440, 24);
  sl::ResourceTag format_tags[] = {
      {&backbuffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&wrong_format, sl::kBufferTypeHUDLessColor,
       sl::ResourceLifecycle::eValidUntilPresent},
  };
  const Assessment format = AssessTags(format_tags, 2, false);
  CHECK(format.suppress_hud_separation);
  CHECK((format.issues & kHudlessFormatMismatch) != 0);

  sl::Extent cropped{0, 0, 1920, 1080};
  sl::ResourceTag ui_extent_tags[] = {
      {&backbuffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&matching_ui, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent,
       &cropped},
  };
  const Assessment ui_extent = AssessTags(ui_extent_tags, 2, false);
  CHECK(ui_extent.suppress_hud_separation);
  CHECK((ui_extent.issues & kUiExtentMismatch) != 0);

  auto low_alpha = MakeResource(2560, 1440, 24);
  sl::ResourceTag alpha_tags[] = {
      {&backbuffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&low_alpha, sl::kBufferTypeUIColorAndAlpha,
       sl::ResourceLifecycle::eValidUntilPresent},
  };
  const Assessment alpha = AssessTags(alpha_tags, 2, false);
  CHECK(alpha.suppress_hud_separation);
  CHECK((alpha.issues & kUiColorAlphaLowPrecision) != 0);

  auto invalid = MakeResource(2560, 1440, 10);
  invalid.structVersion = 99;
  sl::ResourceTag invalid_tags[] = {
      {&backbuffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&invalid, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
  };
  const Assessment invalid_assessment = AssessTags(invalid_tags, 2, false);
  CHECK(invalid_assessment.suppress_hud_separation);
  CHECK((invalid_assessment.issues & kInvalidOptionalResource) != 0);

  auto depth = MakeResource(2560, 1440, 40);
  auto motion = MakeResource(2560, 1440, 16);
  sl::ResourceTag required[] = {
      {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
      {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
  };
  CHECK(!AssessTags(required, 2, true).suppress_hud_separation);

  auto unknown = MakeResource(0, 0, 0);
  sl::ResourceTag unknown_tags[] = {
      {&unknown, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
  };
  CHECK(!AssessTags(unknown_tags, 1, false).suppress_hud_separation);

  sl::Constants constants{};
  constants.reset = sl::Boolean::eFalse;
  constants.jitterOffset = {0.25f, -0.25f};
  constants.structVersion = sl::kStructVersion1;
  const size_t v1_size = offsetof(sl::Constants, minRelativeLinearDepthObjectSeparation);
  GuardedConstants(constants, v1_size, [v1_size](const sl::Constants& guarded) {
    alignas(sl::Constants) std::array<std::byte, sizeof(sl::Constants)> copy{};
    CHECK(CopyConstantsWithReset(guarded, copy.data(), copy.size()));
    const auto* forwarded = reinterpret_cast<const sl::Constants*>(copy.data());
    CHECK(forwarded->reset == sl::Boolean::eTrue);
    CHECK(forwarded->jitterOffset.x == 0.25f && forwarded->jitterOffset.y == -0.25f);
    CHECK(ConstantsCopySize(guarded) == v1_size);
  });

  constants.structVersion = sl::kStructVersion2;
  constants.minRelativeLinearDepthObjectSeparation = 0.75f;
  alignas(sl::Constants) std::array<std::byte, sizeof(sl::Constants)> copy{};
  CHECK(CopyConstantsWithReset(constants, copy.data(), copy.size()));
  const auto* forwarded = reinterpret_cast<const sl::Constants*>(copy.data());
  CHECK(forwarded->reset == sl::Boolean::eTrue);
  CHECK(forwarded->minRelativeLinearDepthObjectSeparation == 0.75f);
  CHECK(forwarded->jitterOffset.x == constants.jitterOffset.x);

  CHECK(CopyConstantsWithQualityOverrides(constants, copy.data(), copy.size(), false, 1.0f));
  forwarded = reinterpret_cast<const sl::Constants*>(copy.data());
  CHECK(forwarded->reset == sl::Boolean::eFalse);
  CHECK(forwarded->minRelativeLinearDepthObjectSeparation == 1.0f);
  CHECK(forwarded->jitterOffset.x == constants.jitterOffset.x);

  CHECK(CopyConstantsWithQualityOverrides(constants, copy.data(), copy.size(), true, 4.0f));
  forwarded = reinterpret_cast<const sl::Constants*>(copy.data());
  CHECK(forwarded->reset == sl::Boolean::eTrue);
  CHECK(forwarded->minRelativeLinearDepthObjectSeparation == 4.0f);

  constants.structVersion = 99;
  CHECK(!CopyConstantsWithReset(constants, copy.data(), copy.size()));
}
