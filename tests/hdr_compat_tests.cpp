// SPDX-License-Identifier: MIT
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <iostream>

#include "../src/addons/mfgunlock/hdr_compat.hpp"

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
  std::abort(); } } while (false)

template <typename Callback>
void GuardedOptions(const sl::DLSSGOptions& source, size_t prefix, Callback callback) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  auto* pages = static_cast<unsigned char*>(VirtualAlloc(
      nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  CHECK(pages != nullptr);
  DWORD old = 0;
  CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
  auto* guarded = reinterpret_cast<sl::DLSSGOptions*>(pages + info.dwPageSize - prefix);
  std::memcpy(guarded, &source, prefix);
  callback(*guarded);
  CHECK(std::memcmp(guarded, &source, prefix) == 0);
  VirtualFree(pages, 0, MEM_RELEASE);
}

int main() {
  sl::DLSSGOptions source{};
  source.mode = sl::DLSSGMode::eOn;
  source.numFramesToGenerate = 1;
  source.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
  source.colorWidth = 2560;
  source.colorHeight = 1440;
  source.colorBufferFormat = 24;
  source.hudLessBufferFormat = 24;
  source.uiBufferFormat = 90;
  source.bReserved15 = sl::eInvalid;

  source.structVersion = sl::kStructVersion2;
  GuardedOptions(source, offsetof(sl::DLSSGOptions, queueParallelismMode),
                 [](const sl::DLSSGOptions& guarded) {
    sl::DLSSGOptions forwarded{};
    CHECK(mfgunlock::hdrcompat::BuildUiRecompositionOptions(
        guarded, forwarded, 3, true));
    CHECK(forwarded.structVersion == sl::kStructVersion4);
    CHECK(forwarded.mode == sl::DLSSGMode::eOn);
    CHECK(forwarded.numFramesToGenerate == 3);
    CHECK(forwarded.flags == sl::DLSSGFlags::eRetainResourcesWhenOff);
    CHECK(forwarded.colorWidth == 2560 && forwarded.colorHeight == 1440);
    CHECK(forwarded.colorBufferFormat == 24);
    CHECK(forwarded.hudLessBufferFormat == 24);
    CHECK(forwarded.uiBufferFormat == 90);
    CHECK(forwarded.enableUserInterfaceRecomposition == sl::eTrue);
  });

  source.structVersion = sl::kStructVersion1;
  GuardedOptions(source, offsetof(sl::DLSSGOptions, bReserved15),
                 [](const sl::DLSSGOptions& guarded) {
    sl::DLSSGOptions forwarded{};
    CHECK(mfgunlock::hdrcompat::BuildUiRecompositionOptions(
        guarded, forwarded, 0, false));
    CHECK(forwarded.structVersion == sl::kStructVersion4);
    CHECK(forwarded.numFramesToGenerate == 1);
    CHECK(forwarded.enableUserInterfaceRecomposition == sl::eTrue);
  });

  source.structVersion = sl::kStructVersion5;
  source.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
  source.dynamicTargetFrameRate = 165.0f;
  sl::DLSSGOptions forwarded{};
  CHECK(mfgunlock::hdrcompat::BuildUiRecompositionOptions(source, forwarded, 0, false));
  CHECK(forwarded.structVersion == sl::kStructVersion5);
  CHECK(forwarded.queueParallelismMode ==
        sl::DLSSGQueueParallelismMode::eBlockNoClientQueues);
  CHECK(forwarded.dynamicTargetFrameRate == 165.0f);

  source.structVersion = 0;
  CHECK(!mfgunlock::hdrcompat::BuildUiRecompositionOptions(source, forwarded, 0, false));
  source.structVersion = 6;
  CHECK(!mfgunlock::hdrcompat::BuildUiRecompositionOptions(source, forwarded, 0, false));

  auto* resource = reinterpret_cast<sl::Resource*>(uintptr_t{0x1000});
  sl::ResourceTag tags[] = {
      {resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow},
      {resource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow},
      {resource, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow},
      {resource, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eOnlyValidNow},
  };
  CHECK(mfgunlock::hdrcompat::HasHudSeparationResources(tags, 4));
  CHECK(mfgunlock::hdrcompat::SuppressHudSeparationResources(tags, 4) == 3);
  CHECK(!mfgunlock::hdrcompat::HasHudSeparationResources(tags, 4));
  CHECK(tags[0].resource == resource);
  CHECK(tags[1].resource == nullptr && tags[2].resource == nullptr &&
        tags[3].resource == nullptr);
}
