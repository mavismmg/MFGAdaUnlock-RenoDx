// SPDX-License-Identifier: MIT
// 1.8.7: offline check of the vendored nefh provider preparation (RTX 20/30).
// Each argument is a DLSS-G provider (nvngx_dlssg.dll or an NGX model .bin).
// The file is mapped as an image in this process and prepared for the given
// architecture; nothing on disk changes. Exit 77 (skipped) with no inputs.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/addons/mfgunlock/ampere/provider.hpp"
#include "../src/addons/mfgunlock/ampere/midpoint.hpp"

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int level, const char* message) {
  std::printf("  [log %d] %s\n", level, message ? message : "");
}

namespace {
uint32_t MinimumArchitecture(HMODULE module) {
  const auto* entry = reinterpret_cast<const unsigned char*>(GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture"));
  if (entry == nullptr || entry[0] != 0xB8) return 0;
  uint32_t value = 0;
  std::memcpy(&value, entry + 1, sizeof(value));
  return value;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  namespace a = mfgunlock::ampere;
  // Default Ampere; "--turing" as the first argument selects the sm_75 path.
  int first = 1;
  auto architecture = a::Architecture::kAmpere;
  if (argc > 1 && std::wcscmp(argv[1], L"--turing") == 0) {
    architecture = a::Architecture::kTuring;
    first = 2;
  }
  if (argc <= first) {
    std::puts("no provider inputs; skipped");
    return 77;
  }
  a::architecture::Configure(architecture);
  int failures = 0;
  int temporal_applied = 0;
  int missing = 0;
  for (int i = first; i < argc; ++i) {
    std::wprintf(L"== %ls\n", argv[i]);
    // 1.10.0: the list is a configure-time cache value; a game update or an
    // uninstall removes a file from it. An absent input is not a provider the
    // preparation failed on.
    if (GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) {
      std::puts("  missing on disk; not counted");
      ++missing;
      continue;
    }
    HMODULE module = LoadLibraryExW(argv[i], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr) {
      std::printf("  cannot map (error %lu)\n", GetLastError());
      ++failures;
      continue;
    }
    const uint32_t before = MinimumArchitecture(module);
    const bool prepared = a::provider::PrepareProvider(module);
    const uint32_t after = MinimumArchitecture(module);
    std::printf("  prepared=%d minimum 0x%x -> 0x%x\n", prepared ? 1 : 0, before, after);
    if (!prepared) ++failures;
    // 1.9.0: the temporal (midpoint) correction on the prepared provider, as
    // the addon applies it on RTX 20/30. Older providers without the
    // multi-frame temporal kernel report why and do not count as failures.
    std::vector<a::midpoint::Patch> patches;
    void* allocation = nullptr;
    std::string detail;
    const bool temporal = prepared && a::midpoint::Apply(module, patches, allocation, detail);
    std::printf("  temporal=%d %s\n", temporal ? 1 : 0, detail.c_str());
    if (temporal) {
      ++temporal_applied;
      // The PTX entry for this GPU (the one the driver compiles on an RTX
      // 20/30) must carry the correction; none may target Ada (sm_89).
      const auto* fat = static_cast<const unsigned char*>(allocation);
      const size_t bytes = static_cast<size_t>(a::fatbin::ReadU64(fat + 8)) + a::fatbin::kHeaderBytes;
      std::vector<a::fatbin::Entry> entries;
      size_t end = 0;
      const uint32_t want = architecture == a::Architecture::kTuring ? 75u : 86u;
      bool corrected = false, ada = false;
      if (a::fatbin::Parse({fat, bytes}, entries, end)) {
        for (const auto& entry : entries) {
          std::vector<unsigned char> raw;
          if (entry.kind != 1 || !a::fatbin::DecodePtx({fat, bytes}, entry, raw)) continue;
          const std::string text(raw.begin(), raw.end());
          if (entry.architecture == 89) ada = true;
          if (entry.architecture == want && text.find("MFGUNLOCK_MIDPOINT_R6") != std::string::npos) corrected = true;
        }
      }
      const bool ok = corrected && !ada;
      std::printf("  corrected sm_%u PTX: %s\n", want, ok ? "yes" : "NO");
      if (!ok) ++failures;
    }
  }
  std::printf("%d of %d provider(s) not prepared; temporal correction on %d; %d missing on disk\n", failures,
              argc - first - missing, temporal_applied, missing);
  if (missing == argc - first) {
    std::puts("every provider input is missing; skipped");
    return 77;
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
