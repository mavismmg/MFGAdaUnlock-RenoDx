/*
 * Experimental, independently selectable thin-geometry patches.
 * SPDX-License-Identifier: MIT
 *
 * The validated-warp blend experiment is informed by the public behavior of
 * Tony Joaca's DLSSG-Transfusion qualityValidWarp option. This implementation is a
 * conservative, independently authored variant: it validates two warped color
 * candidates, checks their mutual color agreement, and raises accepted blend
 * weights gradually to at least 0.85.  It deliberately omits the published
 * asymmetric candidate-copy rule and does not key behavior from UI/HUD state.
 *
 * The previous-scatter experiment comes from this project's independent Stage
 * 3 provider analysis.  It relaxes only the local motion-consistency divisor
 * consumed at parameter +60.  The separate intermediate-scatter variant is
 * assembled offline and selected by blackwell.hpp because that kernel already
 * participates in the existing temporal/Blackwell replacement path. The
 * intermediate variant and its +120 intervention were independently identified
 * and implemented in this fork; the NVIDIA kernel itself remains NVIDIA code.
 */

#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace mfgunlock::thingeometry {

enum class Mechanism {
  ValidatedWarpBlend,
  PreviousScatter,
};

inline const char* MechanismName(Mechanism mechanism) {
  switch (mechanism) {
    case Mechanism::ValidatedWarpBlend:
      return "validated warp blend";
    case Mechanism::PreviousScatter:
      return "previous-to-current scatter retention";
    default:
      return "unknown";
  }
}

struct Options {
  bool validated_warp_blend = false;
  bool previous_scatter = false;
};

struct DescriptorPatch {
  uint64_t* slot = nullptr;
  uint64_t original = 0;
};

struct Redirect {
  Mechanism mechanism = Mechanism::ValidatedWarpBlend;
  std::vector<DescriptorPatch> descriptors;
  void* allocation = nullptr;
};

struct MechanismResult {
  bool requested = false;
  bool detected = false;
  bool applied = false;
  std::string detail;
};

struct Result {
  MechanismResult validated_warp_blend;
  MechanismResult previous_scatter;
};

namespace internal {

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr size_t kOuterHeader = 16;
constexpr uint32_t kPtxKind = 1;
constexpr uint32_t kAdaArch = 89;
constexpr uint32_t kBlackwellArch = 120;
constexpr uint64_t kUncompressedFlags = 0x41;
constexpr size_t kMaxFatbinSize = 4u * 1024u * 1024u;

struct ProviderProfile {
  uint32_t timestamp;
  uint32_t image_size;
  const char* version;
};

// Exact PE metadata for the inspected 310.9.0 OTA and 310.9.1 release
// providers. PTX identity is validated independently below, so metadata alone
// can never authorize a patch.
constexpr ProviderProfile kProviderProfiles[] = {
    {0x6A8745ADu, 7565312u, "310.9.0"},
    {0x6A986031u, 7565312u, "310.9.1"},
};

struct PtxProfile {
  Mechanism mechanism;
  uint32_t arch;
  size_t declared_raw_size;
  size_t normalized_size;
  uint64_t raw_fnv1a64;
  const char* entry_name;
  const char* parameter_signature;
};

constexpr PtxProfile kPtxProfiles[] = {
    {Mechanism::ValidatedWarpBlend, kBlackwellArch, 39639u, 39638u,
     0x7a6f5f41105c6d85ull, "Kernel_BlendCandidatesFused",
     ".param .align 8 .b8 Kernel_BlendCandidatesFused_param_0[240]"},
    {Mechanism::PreviousScatter, kAdaArch, 8933u, 8607u,
     0xf9a5baf8aff7d9caull, "Kernel_EstimatePrev2CurrScatter",
     ".param .align 16 .b8 Kernel_EstimatePrev2CurrScatter_param_0[144]"},
};

inline uint16_t ReadU16(const uint8_t* bytes) {
  uint16_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

inline uint32_t ReadU32(const uint8_t* bytes) {
  uint32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

inline uint64_t ReadU64(const uint8_t* bytes) {
  uint64_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

inline uint64_t Fnv1a64(const uint8_t* bytes, size_t size) {
  uint64_t value = 0xcbf29ce484222325ull;
  for (size_t index = 0; index < size; ++index) {
    value = (value ^ bytes[index]) * 0x100000001b3ull;
  }
  return value;
}

inline bool Lz4BlockDecompress(const uint8_t* source, size_t source_size,
                               uint8_t* destination, size_t destination_size) {
  size_t input = 0;
  size_t output = 0;
  while (input < source_size) {
    const uint8_t token = source[input++];
    size_t literals = token >> 4;
    if (literals == 15) {
      uint8_t extra = 0;
      do {
        if (input >= source_size) return false;
        extra = source[input++];
        literals += extra;
      } while (extra == 0xff);
    }
    if (literals > source_size - input || literals > destination_size - output) return false;
    std::memcpy(destination + output, source + input, literals);
    input += literals;
    output += literals;
    if (input == source_size) break;
    if (source_size - input < 2) return false;
    const size_t distance = source[input] | (static_cast<size_t>(source[input + 1]) << 8u);
    input += 2;
    if (distance == 0 || distance > output) return false;
    size_t match = 4 + (token & 0x0f);
    if ((token & 0x0f) == 15) {
      uint8_t extra = 0;
      do {
        if (input >= source_size) return false;
        extra = source[input++];
        match += extra;
      } while (extra == 0xff);
    }
    if (match > destination_size - output) return false;
    for (size_t index = 0; index < match; ++index) {
      destination[output + index] = destination[output + index - distance];
    }
    output += match;
  }
  return input == source_size && output == destination_size;
}

inline bool ReplaceOnce(std::string& text, const std::string& needle,
                        const std::string& replacement, std::string& why) {
  const size_t first = text.find(needle);
  if (first == std::string::npos) {
    why = "required PTX anchor is missing";
    return false;
  }
  if (text.find(needle, first + needle.size()) != std::string::npos) {
    why = "required PTX anchor is ambiguous";
    return false;
  }
  text.replace(first, needle.size(), replacement);
  return true;
}

inline bool RewritePreviousScatter(std::string& ptx, std::string& why) {
  if (ptx.find("MFGUNLOCK_PREVIOUS_SCATTER_V1") != std::string::npos) {
    why = "previous-scatter PTX is already modified";
    return false;
  }
  constexpr char kAnchor[] = "ld.param.f32 %f24, [%rd6+60];\n";
  return ReplaceOnce(
      ptx, kAnchor,
      std::string(kAnchor) +
          "mul.ftz.f32 %f24, %f24, 0f3F000000; // MFGUNLOCK_PREVIOUS_SCATTER_V1\n",
      why);
}

inline bool RewriteValidatedWarpBlend(std::string& ptx, std::string& why) {
  if (ptx.find("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != std::string::npos) {
    why = "validated-warp PTX is already modified";
    return false;
  }
  constexpr char kRegisters[] = ".reg .pred %p<260>;\n";
  if (!ReplaceOnce(ptx, kRegisters,
                   std::string(kRegisters) + ".reg .pred %qv<7>;\n"
                                             ".reg .f32 %qf<12>;\n",
                   why)) {
    return false;
  }

  constexpr char kInsertion[] = "ld.param.u8 %rs8, [%rd6+220];\n";
  constexpr char kProgram[] = R"ptx(// MFGUNLOCK_VALIDATED_WARP_BLEND_V1
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
div.approx.ftz.f32 %qf0, 0f3F000000, %qf0;
div.approx.ftz.f32 %qf1, 0f3F000000, %qf1;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;
and.pred %qv3, %qv0, %qv1;
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %qf8, %f117, %f121;
abs.f32 %qf6, %qf6;
abs.f32 %qf7, %qf7;
abs.f32 %qf8, %qf8;
add.f32 %qf6, %qf6, %qf7;
add.f32 %qf6, %qf6, %qf8;
sub.f32 %qf9, %f125, %f131;
sub.f32 %qf10, %f126, %f132;
sub.f32 %qf11, %f127, %f133;
abs.f32 %qf9, %qf9;
abs.f32 %qf10, %qf10;
abs.f32 %qf11, %qf11;
add.f32 %qf9, %qf9, %qf10;
add.f32 %qf9, %qf9, %qf11;
add.f32 %qf10, %qf9, 0f3DA3D70A;
setp.lt.f32 %qv4, %qf10, %qf6;
setp.lt.f32 %qv2, %qf9, 0f3E19999A;
and.pred %qv4, %qv4, %qv2;
and.pred %qv4, %qv4, %qv3;
setp.gt.f32 %qv2, %qf6, 0f3E800000;
setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;
and.pred %qv5, %qv5, %qv2;
or.pred %qv5, %qv5, %qv4;
and.pred %qv0, %qv0, %qv5;
setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;
and.pred %qv6, %qv6, %qv2;
or.pred %qv6, %qv6, %qv4;
and.pred %qv1, %qv1, %qv6;
max.f32 %qf0, %f148, 0f3F59999A;
min.f32 %qf0, %qf0, 0f3F800000;
max.f32 %qf1, %f149, 0f3F59999A;
min.f32 %qf1, %qf1, 0f3F800000;
sub.f32 %qf2, %f125, %f115;
sub.f32 %qf3, %f126, %f116;
sub.f32 %qf4, %f127, %f117;
@%qv0 fma.rn.f32 %f39, %qf0, %qf2, %f115;
@%qv0 fma.rn.f32 %f38, %qf0, %qf3, %f116;
@%qv0 fma.rn.f32 %f37, %qf0, %qf4, %f117;
sub.f32 %qf2, %f131, %f119;
sub.f32 %qf3, %f132, %f120;
sub.f32 %qf4, %f133, %f121;
@%qv1 fma.rn.f32 %f43, %qf1, %qf2, %f119;
@%qv1 fma.rn.f32 %f42, %qf1, %qf3, %f120;
@%qv1 fma.rn.f32 %f41, %qf1, %qf4, %f121;
)ptx";
  return ReplaceOnce(ptx, kInsertion, std::string(kProgram) + kInsertion, why);
}

inline const ProviderProfile* MatchProvider(const IMAGE_NT_HEADERS64* nt) {
  for (const auto& profile : kProviderProfiles) {
    if (nt->FileHeader.TimeDateStamp == profile.timestamp &&
        nt->OptionalHeader.SizeOfImage == profile.image_size) {
      return &profile;
    }
  }
  return nullptr;
}

inline const PtxProfile* ProfileFor(Mechanism mechanism) {
  for (const auto& profile : kPtxProfiles) {
    if (profile.mechanism == mechanism) return &profile;
  }
  return nullptr;
}

inline bool FindPtxEntry(const uint8_t* fatbin, size_t fatbin_size,
                         const PtxProfile& profile, size_t& entry_offset,
                         std::vector<uint8_t>& ptx, std::string& why) {
  if (fatbin_size < kOuterHeader || ReadU32(fatbin) != kFatbinMagic ||
      ReadU16(fatbin + 6) != kOuterHeader ||
      ReadU64(fatbin + 8) + kOuterHeader != fatbin_size) {
    why = "invalid fatbin header";
    return false;
  }
  size_t cursor = kOuterHeader;
  while (cursor + 64 <= fatbin_size) {
    const uint32_t header = ReadU32(fatbin + cursor + 4);
    const uint64_t payload = ReadU64(fatbin + cursor + 8);
    if (header < 64 || payload == 0 || cursor + header + payload > fatbin_size) {
      why = "invalid fatbin entry";
      return false;
    }
    if (ReadU16(fatbin + cursor) == kPtxKind &&
        ReadU32(fatbin + cursor + 28) == profile.arch) {
      const uint32_t compressed = ReadU32(fatbin + cursor + 16);
      const uint64_t raw = ReadU64(fatbin + cursor + 56);
      if (raw != profile.declared_raw_size || compressed == 0 ||
          compressed > payload || raw > (8u << 20)) {
        why = "PTX size/compression does not match the supported profile";
        return false;
      }
      ptx.resize(static_cast<size_t>(raw));
      if (!Lz4BlockDecompress(fatbin + cursor + header, compressed,
                              ptx.data(), ptx.size())) {
        why = "PTX LZ4 decompression failed";
        return false;
      }
      ptx.erase(std::remove(ptx.begin(), ptx.end(), '\r'), ptx.end());
      while (!ptx.empty() && ptx.back() == 0) ptx.pop_back();
      if (ptx.size() != profile.normalized_size ||
          Fnv1a64(ptx.data(), ptx.size()) != profile.raw_fnv1a64) {
        why = "PTX identity hash does not match the supported profile";
        return false;
      }
      const std::string text(reinterpret_cast<const char*>(ptx.data()), ptx.size());
      const std::string entry = std::string(".entry ") + profile.entry_name + "(";
      if (text.find(entry) == std::string::npos ||
          text.find(profile.parameter_signature) == std::string::npos) {
        why = "PTX entry or parameter layout changed";
        return false;
      }
      entry_offset = cursor;
      return true;
    }
    cursor += header + static_cast<size_t>(payload);
  }
  why = "required PTX architecture entry is missing";
  return false;
}

inline bool BuildRedirectedFatbin(const uint8_t* fatbin, size_t fatbin_size,
                                  const PtxProfile& profile,
                                  std::vector<uint8_t>& rebuilt,
                                  std::string& why) {
  size_t entry = 0;
  std::vector<uint8_t> source;
  if (!FindPtxEntry(fatbin, fatbin_size, profile, entry, source, why)) return false;
  std::string ptx(reinterpret_cast<const char*>(source.data()), source.size());
  if (profile.arch == kBlackwellArch &&
      !ReplaceOnce(ptx, ".target sm_120", ".target sm_89", why)) {
    return false;
  }
  if (profile.mechanism == Mechanism::ValidatedWarpBlend) {
    if (!RewriteValidatedWarpBlend(ptx, why)) return false;
  } else if (profile.mechanism == Mechanism::PreviousScatter) {
    if (!RewritePreviousScatter(ptx, why)) return false;
  } else {
    why = "unknown PTX rewrite";
    return false;
  }

  const uint32_t header = ReadU32(fatbin + entry + 4);
  const uint64_t original_payload64 = ReadU64(fatbin + entry + 8);
  if (original_payload64 > fatbin_size - entry - header) {
    why = "target PTX entry extends beyond its fatbin";
    return false;
  }
  const size_t original_payload = static_cast<size_t>(original_payload64);
  const size_t padded = (ptx.size() + 7u) & ~size_t{7u};
  const size_t suffix_offset = entry + header + original_payload;
  if (padded > std::numeric_limits<size_t>::max() -
                   (fatbin_size - original_payload)) {
    why = "replacement fatbin size overflow";
    return false;
  }
  const size_t final_size = fatbin_size - original_payload + padded;
  rebuilt.assign(fatbin, fatbin + entry + header);
  rebuilt.resize(final_size, 0);
  std::memcpy(rebuilt.data() + entry + header, ptx.data(), ptx.size());
  // Preserve every entry after the replaced PTX payload. Some fatbins place
  // architecture-specific cubins or metadata after the source entry; dropping
  // that suffix can make a provider fail only when CUDA consumes it at startup.
  std::memcpy(rebuilt.data() + entry + header + padded,
              fatbin + suffix_offset, fatbin_size - suffix_offset);
  const uint64_t payload = padded;
  const uint32_t zero32 = 0;
  const uint64_t zero64 = 0;
  std::memcpy(rebuilt.data() + entry + 8, &payload, sizeof(payload));
  std::memcpy(rebuilt.data() + entry + 16, &zero32, sizeof(zero32));
  std::memcpy(rebuilt.data() + entry + 28, &kAdaArch, sizeof(kAdaArch));
  std::memcpy(rebuilt.data() + entry + 40, &kUncompressedFlags,
              sizeof(kUncompressedFlags));
  std::memcpy(rebuilt.data() + entry + 56, &zero64, sizeof(zero64));
  const uint64_t outer = final_size - kOuterHeader;
  std::memcpy(rebuilt.data() + 8, &outer, sizeof(outer));
  return true;
}

struct LocatedFatbin {
  const uint8_t* address = nullptr;
  size_t size = 0;
};

inline bool LocateUniqueFatbin(HMODULE module, const PtxProfile& profile,
                               LocatedFatbin& located, std::string& why) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  size_t matches = 0;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
        (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
        section->VirtualAddress >= image_size) {
      continue;
    }
    const size_t section_size = std::min<size_t>(
        section->Misc.VirtualSize, image_size - section->VirtualAddress);
    const uint8_t* bytes = base + section->VirtualAddress;
    for (size_t offset = 0; offset + kOuterHeader <= section_size; ++offset) {
      if (ReadU32(bytes + offset) != kFatbinMagic ||
          ReadU16(bytes + offset + 6) != kOuterHeader) {
        continue;
      }
      const uint64_t declared = ReadU64(bytes + offset + 8);
      if (declared == 0 || declared > kMaxFatbinSize ||
          declared > section_size - offset - kOuterHeader) {
        continue;
      }
      const size_t total = static_cast<size_t>(declared) + kOuterHeader;
      size_t ignored_entry = 0;
      std::vector<uint8_t> ignored_ptx;
      std::string ignored_reason;
      if (!FindPtxEntry(bytes + offset, total, profile, ignored_entry,
                        ignored_ptx, ignored_reason)) {
        continue;
      }
      located = {bytes + offset, total};
      ++matches;
      offset += total - 1;
    }
  }
  if (matches != 1) {
    std::ostringstream stream;
    stream << "found " << matches << " exact PTX fatbins, expected one";
    why = stream.str();
    located = {};
    return false;
  }
  return true;
}

inline bool RedirectFatbin(HMODULE module, const PtxProfile& profile,
                           Redirect& redirect, std::string& detail) {
  LocatedFatbin located;
  if (!LocateUniqueFatbin(module, profile, located, detail)) return false;
  std::vector<uint8_t> rebuilt;
  if (!BuildRedirectedFatbin(located.address, located.size, profile,
                             rebuilt, detail)) {
    return false;
  }
  void* allocation = VirtualAlloc(nullptr, rebuilt.size(),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (allocation == nullptr) {
    detail = "replacement fatbin allocation failed";
    return false;
  }
  std::memcpy(allocation, rebuilt.data(), rebuilt.size());

  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const uint64_t expected = reinterpret_cast<uint64_t>(located.address);
  const auto* section = IMAGE_FIRST_SECTION(nt);
  redirect.mechanism = profile.mechanism;
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
        (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
        section->VirtualAddress >= image_size) {
      continue;
    }
    const size_t section_size = std::min<size_t>(
        section->Misc.VirtualSize, image_size - section->VirtualAddress);
    uint8_t* bytes = base + section->VirtualAddress;
    for (size_t offset = 0; offset + sizeof(uint64_t) <= section_size;
         offset += alignof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, bytes + offset, sizeof(value));
      if (value != expected) continue;
      auto* slot = reinterpret_cast<uint64_t*>(bytes + offset);
      DWORD old_protection = 0;
      if (!VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &old_protection)) continue;
      redirect.descriptors.push_back({slot, *slot});
      *slot = reinterpret_cast<uint64_t>(allocation);
      DWORD ignored = 0;
      VirtualProtect(slot, sizeof(uint64_t), old_protection, &ignored);
    }
  }
  if (redirect.descriptors.empty() || redirect.descriptors.size() > 32) {
    for (const auto& patch : redirect.descriptors) {
      DWORD old_protection = 0;
      if (VirtualProtect(patch.slot, sizeof(uint64_t), PAGE_READWRITE, &old_protection)) {
        *patch.slot = patch.original;
        DWORD ignored = 0;
        VirtualProtect(patch.slot, sizeof(uint64_t), old_protection, &ignored);
      }
    }
    redirect.descriptors.clear();
    VirtualFree(allocation, 0, MEM_RELEASE);
    detail = "descriptor reference count is outside the safe 1..32 range";
    return false;
  }
  redirect.allocation = allocation;
  std::ostringstream stream;
  stream << "redirected " << redirect.descriptors.size()
         << " exact descriptor reference(s) from a " << located.size
         << "-byte provider fatbin to a " << rebuilt.size() << "-byte PTX rebuild";
  detail = stream.str();
  return true;
}

}  // namespace internal

inline bool IsSupportedProvider(HMODULE module, std::string& version,
                                std::string& why) {
  if (module == nullptr) {
    why = "provider module is null";
    return false;
  }
  MEMORY_BASIC_INFORMATION mapping{};
  if (VirtualQuery(module, &mapping, sizeof(mapping)) != sizeof(mapping) ||
      mapping.AllocationBase != module || mapping.Type != MEM_IMAGE ||
      mapping.State != MEM_COMMIT) {
    why = "provider module is no longer a mapped image";
    return false;
  }
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    why = "provider has no valid DOS header";
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    why = "provider has no valid x64 PE header";
    return false;
  }
  const auto* profile = internal::MatchProvider(nt);
  if (profile == nullptr) {
    std::ostringstream stream;
    stream << "unsupported provider metadata (timestamp=0x" << std::hex
           << nt->FileHeader.TimeDateStamp << std::dec
           << ", image-size=" << nt->OptionalHeader.SizeOfImage << ')';
    why = stream.str();
    return false;
  }
  version = profile->version;
  return true;
}

inline void Restore(std::vector<Redirect>& redirects) {
  for (auto redirect = redirects.rbegin(); redirect != redirects.rend(); ++redirect) {
    bool restored_all = true;
    for (const auto& patch : redirect->descriptors) {
      DWORD old_protection = 0;
      if (VirtualProtect(patch.slot, sizeof(uint64_t), PAGE_READWRITE, &old_protection)) {
        *patch.slot = patch.original;
        DWORD ignored = 0;
        VirtualProtect(patch.slot, sizeof(uint64_t), old_protection, &ignored);
      } else {
        restored_all = false;
      }
    }
    redirect->descriptors.clear();
    // Never free a replacement while a live descriptor may still point at it.
    // A small leak during an abnormal unload is safer than a dangling pointer.
    if (restored_all && redirect->allocation != nullptr) {
      VirtualFree(redirect->allocation, 0, MEM_RELEASE);
      redirect->allocation = nullptr;
    }
  }
  redirects.clear();
}

inline bool Apply(HMODULE module, const Options& options,
                  std::vector<Redirect>& redirects, Result& result,
                  std::string& provider_version) {
  redirects.clear();
  result = {};
  result.validated_warp_blend.requested = options.validated_warp_blend;
  result.previous_scatter.requested = options.previous_scatter;

  std::string provider_reason;
  if (!IsSupportedProvider(module, provider_version, provider_reason)) {
    if (options.validated_warp_blend) result.validated_warp_blend.detail = provider_reason;
    if (options.previous_scatter) result.previous_scatter.detail = provider_reason;
    return false;
  }

  const auto apply_one = [&](Mechanism mechanism, bool requested,
                             MechanismResult* mechanism_result) {
    if (!requested) return;
    const auto* profile = internal::ProfileFor(mechanism);
    if (profile == nullptr) {
      mechanism_result->detail = "internal profile is missing";
      return;
    }
    internal::LocatedFatbin located;
    std::string detection_reason;
    mechanism_result->detected =
        internal::LocateUniqueFatbin(module, *profile, located, detection_reason);
    if (!mechanism_result->detected) {
      mechanism_result->detail = detection_reason;
      return;
    }
    Redirect redirect;
    if (!internal::RedirectFatbin(module, *profile, redirect,
                                  mechanism_result->detail)) {
      return;
    }
    mechanism_result->applied = true;
    redirects.push_back(std::move(redirect));
  };

  apply_one(Mechanism::ValidatedWarpBlend, options.validated_warp_blend,
            &result.validated_warp_blend);
  apply_one(Mechanism::PreviousScatter, options.previous_scatter,
            &result.previous_scatter);
  return result.validated_warp_blend.applied || result.previous_scatter.applied;
}

}  // namespace mfgunlock::thingeometry
