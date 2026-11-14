// SPDX-License-Identifier: MIT
#pragma once
// 1.10.0 (dashdogy item A1): what NGX's frame generation requirements query
// said while Streamline started, for the log and the support report.
//
// dashdogy/MFG-Unlocker (MIT, ampere_startup_requirements.h at 128d081) found
// two failures that make Streamline drop sl.dlss_g on RTX 20/30 before any
// device exists:
//   - 0xBAD00005 InvalidParameter: the driver (617.42, _nvngx.dll RVA 0x452B0)
//     refuses a project identifier whose ID is not a GUID, before any GPU
//     check. Streamline sends one when the game sets a project ID or an
//     engine version.
//   - FeatureNotFound / FeatureNotSupported: no DLSS-G provider is mapped yet.
// On a failure sl.dlss_g 2.14.16 logs "Failed to obtain DLSS-G min spec
// requirements from NGX, using SL defaults" and then "Disabling DLSS-G since it
// is not supported on current hardware" even though this addon answers Ada to
// its architecture query: the SL Sample oracle (SimulateRequirementsFailure)
// showed it on this PC. So, as dashdogy does, a failure while Streamline
// starts is answered "supported, minimum Turing" when a DLSS-G provider exists
// for NGX to load (Decide). Anything else stays NGX's own answer; creating the
// feature still goes through NGX, and the report keeps the original failure.
// ProjectIdPassesDriverCheck is dashdogy's reading of the driver's rule.
#include <cstddef>
#include <cstdint>
#include <string>

namespace mfgunlock::ngxreq {

constexpr uint32_t kSuccess = 0x1;
constexpr uint32_t kFeatureNotSupported = 0xBAD00001;
constexpr uint32_t kFeatureNotFound = 0xBAD00004;
constexpr uint32_t kInvalidParameter = 0xBAD00005;

enum class Failure : uint8_t { kNone, kFeatureMissing, kIdentifierRejected, kOther };

constexpr Failure Classify(uint32_t result) {
  if ((result & 0xFFF00000u) != 0xBAD00000u) return Failure::kNone;
  if (result == kFeatureNotSupported || result == kFeatureNotFound) return Failure::kFeatureMissing;
  if (result == kInvalidParameter) return Failure::kIdentifierRejected;
  return Failure::kOther;
}

constexpr const char* FailureText(Failure failure) {
  switch (failure) {
    case Failure::kNone: return "succeeded";
    case Failure::kFeatureMissing: return "no frame generation provider was available to NGX yet";
    case Failure::kIdentifierRejected: return "the driver refused the game's identifier";
    default: return "failed";
  }
}

// Whether to answer a failed query: a missing feature or a refused identifier,
// asked by Streamline's start (the admission scope or a runtime caller), with a
// DLSS-G provider present, and the answer not turned off.
constexpr bool Decide(Failure failure, bool streamline_start, bool provider_present, bool enabled) {
  return enabled && streamline_start && provider_present &&
         (failure == Failure::kFeatureMissing || failure == Failure::kIdentifierRejected);
}

// A DLSS-G provider NGX can load: a mapped nvngx_dlssg*, the game's own
// nvngx_dlssg.dll, or a file in NGX's dlssg model folder (the NVIDIA app's
// copy, ...\NGX\models\dlssg\versions\<n>\files\*). Paths are lower case.
inline bool ProviderPath(const std::wstring& lower) {
  const size_t slash = lower.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos ? lower : lower.substr(slash + 1);
  if (name.compare(0, 11, L"nvngx_dlssg") == 0) return true;
  return lower.find(L"\\ngx\\models\\dlssg\\versions\\") != std::wstring::npos &&
         lower.find(L"\\files\\") != std::wstring::npos && !name.empty();
}

// 1.10.0 (dashdogy item A2): the provider's own requirements record. Every
// DLSS-G provider's NVSDK_NGX_{D3D11,D3D12,VULKAN}_GetFeatureRequirements
// stores the frame generation feature (0x0B) and then its minimum
// architecture (0x190) with `mov dword [rsp+d8], imm32` (C7 44 24 d8 imm32),
// the second at d8+4. Checked in 310.x copies (the SL Sample's and two NVIDIA
// app versions): feature at +0x38, minimum at +0x3c, one of each. Returns the
// offset of the minimum's imm32 in [code, code+size), or SIZE_MAX when the
// shape is absent or ambiguous. dashdogy decodes instructions with hde64;
// this matches the two exact 8-byte stores instead.
constexpr size_t kRequirementsWindow = 0x600;
constexpr size_t FindRequirementsArchitecture(const uint8_t* code, size_t size) {
  if (code == nullptr || size < 8) return SIZE_MAX;
  size_t found = SIZE_MAX;
  unsigned architectures = 0;
  for (size_t i = 0; i + 8 <= size; ++i) {
    if (code[i] != 0xC7 || code[i + 1] != 0x44 || code[i + 2] != 0x24) continue;
    const uint32_t value = static_cast<uint32_t>(code[i + 4]) | static_cast<uint32_t>(code[i + 5]) << 8 |
                           static_cast<uint32_t>(code[i + 6]) << 16 | static_cast<uint32_t>(code[i + 7]) << 24;
    if (value != 0x190 && value != 0x170 && value != 0x160) continue;
    ++architectures;
    const uint8_t feature_disp = static_cast<uint8_t>(code[i + 3] - 4);
    for (size_t j = 0; j + 8 <= i; ++j)
      if (code[j] == 0xC7 && code[j + 1] == 0x44 && code[j + 2] == 0x24 && code[j + 3] == feature_disp &&
          code[j + 4] == 0x0B && code[j + 5] == 0 && code[j + 6] == 0 && code[j + 7] == 0)
        found = i + 4;
  }
  return architectures == 1 ? found : SIZE_MAX;
}

// The driver's project ID rule (617.42): {} [] () or "" wrappers are
// stripped, dashes are skipped, and the rest must be exactly 32 hex digits
// with no run of 6 identical characters (case-sensitive; a dash does not end
// a run).
constexpr bool ProjectIdPassesDriverCheck(const char* id, size_t length) {
  if (id == nullptr) return false;
  size_t begin = 0, end = length;
  while (end - begin > 1) {
    const char first = id[begin], last = id[end - 1];
    if (!((first == '{' && last == '}') || (first == '[' && last == ']') || (first == '(' && last == ')') ||
          (first == '"' && last == '"')))
      break;
    ++begin;
    --end;
  }
  unsigned digits = 0, run = 0;
  char previous = '\0';
  for (size_t i = begin; i < end; ++i) {
    const char c = id[i];
    if (c == '-') continue;
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    run = c == previous ? run + 1 : 0;
    previous = c;
    if (++digits > 32 || run > 4) return false;
  }
  return digits == 32;
}

inline std::string Hex(uint32_t value) {
  char text[12];
  constexpr char kDigits[] = "0123456789abcdef";
  text[0] = '0';
  text[1] = 'x';
  for (int i = 0; i < 8; ++i) text[2 + i] = kDigits[(value >> (28 - 4 * i)) & 0xF];
  return std::string(text, 10);
}

// One line for the log and the report. project_id is null for an
// application ID identifier.
inline std::string Describe(uint32_t result, bool project, const char* project_id, int engine_type) {
  std::string text = std::string(FailureText(Classify(result))) + " (" + Hex(result) + ")";
  if (!project) return text + "; identifier: application ID";
  std::string id = project_id ? std::string(project_id).substr(0, 64) : std::string("(none)");
  size_t length = 0;
  if (project_id) while (project_id[length] != '\0' && length < 256) ++length;
  return text + "; identifier: project ID \"" + id + "\", engine type " + std::to_string(engine_type) +
         ", " + (ProjectIdPassesDriverCheck(project_id, length) ? "passes" : "fails") + " the driver's GUID rule";
}

}  // namespace mfgunlock::ngxreq
