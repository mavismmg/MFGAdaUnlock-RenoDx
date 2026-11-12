// SPDX-License-Identifier: MIT
// 1.9.0: the multi-frame gate after the RTX 20/30 provider preparation. A
// tester's RTX 3080 (Onimusha, provider 310.5.2) and a simulated run (the
// Cyberpunk game copy) logged "architecture gate refused" right after the
// preparation, so 3x was refused. Each argument is a provider; the file is
// mapped as an image, the gate is planned before anything changes, then the
// addon's order is replayed (provider minimum, nefh's preparation with its code
// writes recorded as the addon's own) and the gate is planned again. A provider
// whose gate plans before must still plan after. Exit 77 with no inputs.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/addons/mfgunlock/ampere/provider.hpp"
#include "../src/addons/mfgunlock/binary_patch.hpp"

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}

namespace {
namespace bp = mfgunlock::binarypatch;
namespace ms = mfgunlock::modulesession;

// "exact", "generic", or why not.
std::string GatePlan(HMODULE module) {
  const auto* session = ms::Acquire(module);
  if (session == nullptr) return "no session";
  if (!ms::Validate(*session)) return "identity invalid";
  bp::Generic generic;
  const mfgunlock::binaryprofiles::Profile* profile = bp::ProfileFor(module, 0);
  const char* kind = "exact";
  if (profile == nullptr && bp::GenericArchProfileFor(module, generic)) {
    profile = &generic.profile;
    kind = "generic";
  }
  if (profile == nullptr) return "no profile";
  std::vector<mfgunlock::patching::Write> writes;
  if (!bp::Plan(module, *profile, writes)) return std::string(kind) + " profile, plan failed";
  return kind;
}

void LowerMinimum(HMODULE module) {
  auto* entry = reinterpret_cast<uint8_t*>(GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture"));
  if (entry == nullptr || entry[0] != 0xB8) return;
  const auto* session = ms::Acquire(module);
  const uint32_t lowered = 0x160;
  std::vector<mfgunlock::patching::Write> writes{mfgunlock::patching::Bytes(entry + 1, &lowered, sizeof(lowered))};
  if (session && mfgunlock::patching::Apply(writes, true)) ms::RecordCode(*session, writes);
}

// What preada::RetargetProvider records: nefh's writes in executable pages.
size_t RecordPreparation(HMODULE module) {
  namespace pr = mfgunlock::ampere::provider;
  const auto* session = ms::Acquire(module);
  std::vector<mfgunlock::patching::Write> code;
  AcquireSRWLockShared(&pr::internal::g_lock);
  for (const auto& provider : pr::internal::g_providers) {
    if (provider.module != module) continue;
    for (const auto& patch : provider.patches) {
      MEMORY_BASIC_INFORMATION memory{};
      if (!VirtualQuery(patch.address, &memory, sizeof(memory))) continue;
      const DWORD protection = memory.Protect & 0xFF;
      if (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
          protection == PAGE_EXECUTE_WRITECOPY)
        code.push_back({patch.address, {patch.before}, {patch.after}, true});
    }
  }
  ReleaseSRWLockShared(&pr::internal::g_lock);
  if (session && !code.empty()) ms::RecordCode(*session, code);
  return code.size();
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::puts("no provider inputs; skipped");
    return 77;
  }
  mfgunlock::ampere::architecture::Configure(mfgunlock::ampere::Architecture::kAmpere);
  int regressions = 0;
  for (int i = 1; i < argc; ++i) {
    std::wprintf(L"== %ls\n", argv[i]);
    if (GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) {
      std::puts("  missing on disk; not counted");  // 1.10.0: see ampere_prepare_tests
      continue;
    }
    HMODULE module = LoadLibraryExW(argv[i], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr) {
      std::printf("  cannot map (error %lu)\n", GetLastError());
      continue;
    }
    const std::string before = GatePlan(module);
    LowerMinimum(module);
    const bool prepared = mfgunlock::ampere::provider::PrepareProvider(module);
    const size_t recorded = prepared ? RecordPreparation(module) : 0;
    const std::string after = GatePlan(module);
    std::printf("  gate before: %s; prepared=%d (%zu code byte patch(es) recorded); gate after: %s\n",
                before.c_str(), prepared ? 1 : 0, recorded, after.c_str());
    const bool planned_before = before == "exact" || before == "generic";
    const bool planned_after = after == "exact" || after == "generic";
    if (planned_before && !planned_after) ++regressions;
  }
  std::printf("%d provider(s) lost the gate to the preparation\n", regressions);
  return regressions == 0 ? 0 : 1;
}
