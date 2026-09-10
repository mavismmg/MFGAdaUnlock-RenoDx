/*
 * Blackwell framework kernels rebuilt for Ada.
 *
 * The implementation follows the fail-closed method validated by MatiasLombo:
 * identify NVIDIA's original sm_89 cubins by their ELF fingerprint and exact
 * fatbin slot size, then replace only the payload in that existing slot. The
 * fatbin header, entry descriptors, registration metadata and surrounding
 * provider image remain untouched.
 *
 * Replacement cubins are generated locally from installed NVIDIA DLSS-G
 * providers with MatiasLombo's rebuild_cubins.py workflow. They are deliberately
 * excluded from source control. A source-only build simply reports that no
 * table is present and lets the addon use its stable midpoint fallback.
 */

#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#if __has_include("./blackwell_cubins.generated.hpp")
namespace mfgunlock::blackwell::generated {
#include "./blackwell_cubins.generated.hpp"
}
#define MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS 1
#else
#define MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS 0
#endif

namespace mfgunlock::blackwell {

enum class KernelRole {
  Unknown,
  MotionVector,
  Inpaint,
  InpaintDecision,
};

inline const char* RoleName(KernelRole role) {
  switch (role) {
    case KernelRole::MotionVector: return "motion-vector estimate";
    case KernelRole::Inpaint: return "inpaint";
    case KernelRole::InpaintDecision: return "inpaint decision";
    default: return "unknown";
  }
}

struct Patch {
  uint8_t* payload = nullptr;
  std::vector<uint8_t> original;
};

struct Result {
  bool motion_vector = false;
  bool inpaint = false;
  bool inpaint_decision = false;
  size_t kernels = 0;
};

namespace internal {

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr uint32_t kAdaArch = 89u;
constexpr size_t kMaxFatbinSize = 4u * 1024u * 1024u;

inline uint16_t ReadU16(const uint8_t* p) {
  uint16_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint32_t ReadU32(const uint8_t* p) {
  uint32_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

struct ElfFingerprint {
  uint32_t text = 0;
  uint32_t shared = 0;
  uint32_t registers = 0;
};

inline bool FingerprintElf(const uint8_t* bytes, size_t size, ElfFingerprint& out) {
  out = {};
  if (bytes == nullptr || size < 0x40 || bytes[0] != 0x7f || bytes[1] != 'E' ||
      bytes[2] != 'L' || bytes[3] != 'F') {
    return false;
  }

  const uint64_t section_offset = ReadU64(bytes + 0x28);
  const uint16_t section_entry_size = ReadU16(bytes + 0x3a);
  const uint16_t section_count = ReadU16(bytes + 0x3c);
  const uint16_t string_section = ReadU16(bytes + 0x3e);
  if (section_entry_size < 0x40 || section_count == 0 || string_section >= section_count ||
      section_offset > size ||
      static_cast<uint64_t>(section_entry_size) * section_count > size - section_offset) {
    return false;
  }

  const uint8_t* string_header =
      bytes + static_cast<size_t>(section_offset) + static_cast<size_t>(string_section) * section_entry_size;
  const uint64_t string_offset = ReadU64(string_header + 0x18);
  const uint64_t string_size = ReadU64(string_header + 0x20);
  if (string_offset >= size || string_size > size - string_offset) return false;

  for (uint16_t index = 0; index < section_count; ++index) {
    const uint8_t* section =
        bytes + static_cast<size_t>(section_offset) + static_cast<size_t>(index) * section_entry_size;
    const uint32_t name_offset = ReadU32(section);
    if (name_offset >= string_size) continue;

    const char* name = reinterpret_cast<const char*>(bytes + string_offset + name_offset);
    const size_t remaining = static_cast<size_t>(string_size - name_offset);
    const void* terminator = std::memchr(name, '\0', remaining);
    if (terminator == nullptr) continue;

    const uint64_t section_size = ReadU64(section + 0x20);
    const uint32_t info = ReadU32(section + 0x2c);
    if (std::strncmp(name, ".text.", 6) == 0) {
      if (section_size > UINT32_MAX) return false;
      out.text = static_cast<uint32_t>(section_size);
      out.registers = (info >> 24u) & 0xffu;
    } else if (std::strncmp(name, ".nv.shared", 10) == 0) {
      if (section_size > UINT32_MAX) return false;
      out.shared = static_cast<uint32_t>(section_size);
    }
  }
  return out.text != 0;
}

inline KernelRole RoleFromSharedMemory(uint32_t shared) {
  switch (shared) {
    case 7776u: return KernelRole::MotionVector;
    case 3920u: return KernelRole::Inpaint;
    case 784u: return KernelRole::InpaintDecision;
    default: return KernelRole::Unknown;
  }
}

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
inline const generated::CubinPatch* MatchReplacement(const ElfFingerprint& fingerprint,
                                                      size_t slot_size) {
  for (const auto& replacement : generated::kCubinPatches) {
    if (replacement.text == fingerprint.text && replacement.shared == fingerprint.shared &&
        replacement.regs == fingerprint.registers && replacement.orig_size == slot_size &&
        replacement.data != nullptr && replacement.size != 0 && replacement.size <= slot_size) {
      return &replacement;
    }
  }
  return nullptr;
}
#endif

struct Candidate {
  uint8_t* payload = nullptr;
  size_t slot_size = 0;
#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  const generated::CubinPatch* replacement = nullptr;
#endif
  KernelRole role = KernelRole::Unknown;
};

inline bool CollectCandidates(HMODULE module, std::vector<Candidate>& candidates, std::string& why) {
#if !MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  (void)module;
  (void)candidates;
  why = "this build contains no locally generated Blackwell cubin table";
  return false;
#else
  auto* base = reinterpret_cast<uint8_t*>(module);
  if (base == nullptr) {
    why = "provider module is null";
    return false;
  }

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    why = "provider has no valid DOS header";
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    why = "provider has no valid PE header";
    return false;
  }

  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (uint16_t section_index = 0; section_index < nt->FileHeader.NumberOfSections;
       ++section_index) {
    if ((section[section_index].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
    const size_t section_offset = section[section_index].VirtualAddress;
    const size_t section_size = section[section_index].Misc.VirtualSize;
    if (section_offset >= image_size || section_size > image_size - section_offset) continue;

    uint8_t* bytes = base + section_offset;
    for (size_t offset = 0; offset + 16 <= section_size; ++offset) {
      if (ReadU32(bytes + offset) != kFatbinMagic) continue;

      const uint16_t header_size = ReadU16(bytes + offset + 6);
      const uint64_t fatbin_size = ReadU64(bytes + offset + 8);
      if (header_size != 16 || fatbin_size == 0 || fatbin_size > kMaxFatbinSize ||
          fatbin_size > section_size - offset - 16) {
        continue;
      }

      size_t entry_offset = offset + 16;
      const size_t fatbin_end = entry_offset + static_cast<size_t>(fatbin_size);
      while (entry_offset + 32 <= fatbin_end) {
        const uint8_t* entry = bytes + entry_offset;
        const uint16_t kind = ReadU16(entry);
        const uint32_t entry_header_size = ReadU32(entry + 4);
        const uint64_t payload_size = ReadU64(entry + 8);
        const uint64_t compressed_size = ReadU64(entry + 16);
        const uint32_t arch = ReadU32(entry + 28);
        if (entry_header_size < 64 || entry_header_size > 256 ||
            entry_header_size > fatbin_end - entry_offset ||
            payload_size > fatbin_end - entry_offset - entry_header_size) {
          break;
        }

        if (kind == 2 && arch == kAdaArch && compressed_size == 0 && payload_size > 0x40) {
          uint8_t* payload = bytes + entry_offset + entry_header_size;
          ElfFingerprint fingerprint;
          if (FingerprintElf(payload, static_cast<size_t>(payload_size), fingerprint)) {
            if (const auto* replacement =
                    MatchReplacement(fingerprint, static_cast<size_t>(payload_size))) {
              const KernelRole role = RoleFromSharedMemory(fingerprint.shared);
              if (role != KernelRole::Unknown &&
                  std::none_of(candidates.begin(), candidates.end(),
                               [payload](const Candidate& item) { return item.payload == payload; })) {
                candidates.push_back({payload, static_cast<size_t>(payload_size), replacement, role});
              }
            }
          }
        }
        entry_offset += entry_header_size + static_cast<size_t>(payload_size);
      }
      offset = fatbin_end - 1;
    }
  }

  if (candidates.empty()) {
    why = std::string("no exact Ada cubin slot matched the generated table for ") +
          generated::kCubinsBuiltFor;
    return false;
  }
  return true;
#endif
}

}  // namespace internal

inline constexpr bool HasGeneratedCubins() {
  return MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS != 0;
}

inline void Restore(std::vector<Patch>& patches, std::vector<void*>& allocations) {
  for (auto patch = patches.rbegin(); patch != patches.rend(); ++patch) {
    if (patch->payload == nullptr || patch->original.empty()) continue;
    DWORD old_protection = 0;
    if (VirtualProtect(patch->payload, patch->original.size(), PAGE_READWRITE, &old_protection)) {
      std::memcpy(patch->payload, patch->original.data(), patch->original.size());
      DWORD ignored = 0;
      VirtualProtect(patch->payload, patch->original.size(), old_protection, &ignored);
    }
  }
  patches.clear();
  // Kept in the API so addon state remains compatible with the earlier
  // experiment. In-place cubin replacement allocates no executable memory.
  allocations.clear();
}

inline bool Apply(HMODULE module, std::vector<Patch>& patches, std::vector<void*>& allocations,
                  Result& result, std::string& detail) {
  patches.clear();
  allocations.clear();
  result = {};
  detail.clear();

  std::vector<internal::Candidate> candidates;
  if (!internal::CollectCandidates(module, candidates, detail)) return false;

  const auto count_role = [&candidates](KernelRole role) {
    return std::count_if(candidates.begin(), candidates.end(),
                         [role](const internal::Candidate& item) { return item.role == role; });
  };
  const size_t motion_vectors = count_role(KernelRole::MotionVector);
  const size_t inpaints = count_role(KernelRole::Inpaint);
  const size_t decisions = count_role(KernelRole::InpaintDecision);
  if (motion_vectors != 1 || inpaints > 1 || decisions > 1) {
    std::ostringstream stream;
    stream << "ambiguous cubin set (motion-vector=" << motion_vectors << ", inpaint=" << inpaints
           << ", decision=" << decisions << ')';
    detail = stream.str();
    return false;
  }

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  for (const auto& candidate : candidates) {
    Patch patch;
    patch.payload = candidate.payload;
    patch.original.assign(candidate.payload, candidate.payload + candidate.slot_size);

    DWORD old_protection = 0;
    if (!VirtualProtect(candidate.payload, candidate.slot_size, PAGE_READWRITE, &old_protection)) {
      detail = std::string("cubin slot was not writable for ") + RoleName(candidate.role);
      Restore(patches, allocations);
      return false;
    }
    std::memcpy(candidate.payload, candidate.replacement->data, candidate.replacement->size);
    std::memset(candidate.payload + candidate.replacement->size, 0,
                candidate.slot_size - candidate.replacement->size);
    DWORD ignored = 0;
    VirtualProtect(candidate.payload, candidate.slot_size, old_protection, &ignored);
    patches.push_back(std::move(patch));

    result.motion_vector |= candidate.role == KernelRole::MotionVector;
    result.inpaint |= candidate.role == KernelRole::Inpaint;
    result.inpaint_decision |= candidate.role == KernelRole::InpaintDecision;
    ++result.kernels;
  }
#endif

  std::ostringstream stream;
  stream << "precompiled Blackwell cubins in original Ada slots: motion-vector="
         << (result.motion_vector ? "yes" : "no") << ", inpaint="
         << (result.inpaint ? "yes" : "no") << ", decision="
         << (result.inpaint_decision ? "yes" : "no") << ", kernels=" << result.kernels;
  detail = stream.str();
  return result.motion_vector;
}

}  // namespace mfgunlock::blackwell
