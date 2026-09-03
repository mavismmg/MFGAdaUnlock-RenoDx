/*
 * Temporal (midpoint) correction for DLSS-G frame interpolation on Ada.
 * SPDX-License-Identifier: MIT
 *
 * ---------------------------------------------------------------------------
 * THE PROBLEM
 *
 * With multi-frame generation unlocked, 3x/4x produce the right NUMBER of
 * frames but not the right CONTENT. The interpolation kernel shipped in
 * nvngx_dlssg.dll blends the two source frames using a compiled-in constant of
 * 0.5 -- 104 `mul.ftz.f32 ..., 0f3F000000;` instructions in its PTX. Every
 * generated frame therefore lands at the temporal midpoint, so 4x shows three
 * identical half-way frames instead of frames at 1/4, 2/4, 3/4. The frame
 * counter doubles while the motion does not get smoother.
 *
 * THE FIX
 *
 * Rewrite that kernel's PTX so the blend weight comes from the kernel's own
 * temporal parameter instead of a constant:
 *
 *     ld.param.f32 %f134, [<temporal-kernel>_param_0+32]; // t for this frame
 *     mov.f32      %f135, 0f3F800000;                 // 1.0
 *     sub.ftz.f32  %f136, %f135, %f134;               // 1.0 - t
 *
 * injected after the unique `$L__BB0_3:` label, then the 104 midpoint constants
 * replaced with %f136 (first half, current->previous) and %f134 (second half,
 * previous->current).
 *
 * The catch: the fatbin also carries a precompiled sm_89 cubin, and the driver
 * would load that in preference to JIT-ing our edited PTX. So the rebuilt
 * fatbin is TRUNCATED after the sm_89 PTX entry, dropping the cubin and forcing
 * the JIT path. The PTX entry is re-emitted uncompressed (flags 0x41).
 *
 * Credit: the technique -- the midpoint diagnosis, the injected PTX, and the
 * truncation trick -- is from dashdogy's RTX40MFG-Unlock. That project ships no
 * licence, so nothing here is copied from it; this is an independent
 * implementation of the same idea, verified by reproducing its published output
 * digest byte-for-byte offline (19FB3CD5...104B) before being written.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#include <include/reshade.hpp>

namespace mfgunlock::midpoint {

namespace internal {

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr size_t kOuterHeader = 16;
constexpr uint32_t kPtxKind = 1;
constexpr uint32_t kAdaArch = 89;
constexpr uint64_t kUncompressedFlags = 0x41;

// Structural expectations. NVIDIA renamed every kernel in 310.9, but the D157
// temporal program itself is otherwise unchanged: the PTX grew by exactly the
// accumulated symbol-name delta. Keep separate, exact profiles so an unrelated
// provider cannot be admitted merely because it happens to contain a similar
// midpoint sequence.
struct TemporalProfile {
  size_t ptx_bytes;
  const char* entry_name;
  const char* descriptor_name;
  ptrdiff_t entry_name_offset;
  ptrdiff_t descriptor_name_offset;
};

constexpr TemporalProfile kTemporalProfiles[] = {
    {99362, "main_kernel", "dlfg_kernel", 0x10, 0x28},
    {99626, "Kernel_EstimateIntermMvecsScatter", "EstimateIntermMvecsScatter", 0x10, -0x08},
};

constexpr size_t kExpectedMidpoints = 104;

constexpr char kJoinLabel[] = "$L__BB0_3:";
constexpr char kMidpointBits[] = "0f3F000000";
constexpr char kMulPrefix[] = "mul.ftz.f32 ";
constexpr char kCurrToPrev[] = "%f136";
constexpr char kPrevToCurr[] = "%f134";
inline uint16_t ReadU16(const uint8_t* p) {
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint32_t ReadU32(const uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Plain LZ4 block format: token high nibble = literal run, low nibble = match
// length - 4, 16-bit little-endian back offset, 0xFF continuation bytes.
inline bool Lz4BlockDecompress(const uint8_t* src, size_t src_size, uint8_t* dst,
                               size_t dst_size) {
  size_t in = 0;
  size_t out = 0;
  while (in < src_size) {
    const uint8_t token = src[in++];
    size_t literals = token >> 4;
    if (literals == 15) {
      uint8_t ext = 0;
      do {
        if (in >= src_size) return false;
        ext = src[in++];
        literals += ext;
      } while (ext == 0xFF);
    }
    if (literals > src_size - in || literals > dst_size - out) return false;
    std::memcpy(dst + out, src + in, literals);
    in += literals;
    out += literals;
    if (in == src_size) break;
    if (src_size - in < 2) return false;
    const size_t back = static_cast<size_t>(src[in]) | (static_cast<size_t>(src[in + 1]) << 8);
    in += 2;
    if (back == 0 || back > out) return false;
    size_t match = 4 + (token & 0x0F);
    if ((token & 0x0F) == 15) {
      uint8_t ext = 0;
      do {
        if (in >= src_size) return false;
        ext = src[in++];
        match += ext;
      } while (ext == 0xFF);
    }
    if (match > dst_size - out) return false;
    for (size_t i = 0; i < match; ++i) dst[out + i] = dst[out + i - back];
    out += match;
  }
  return in == src_size && out == dst_size;
}

// Locates the sm_89 PTX entry inside a fatbin by walking its entry list rather
// than trusting fixed offsets.
inline bool FindAdaPtxEntry(const uint8_t* fat, size_t fat_size, size_t& entry_offset) {
  if (fat_size < kOuterHeader || ReadU32(fat) != kFatbinMagic) return false;
  if (ReadU16(fat + 6) != kOuterHeader) return false;
  const uint64_t declared = ReadU64(fat + 8);
  if (declared + kOuterHeader != fat_size) return false;

  size_t p = kOuterHeader;
  while (p + 64 <= fat_size) {
    const uint32_t kind = ReadU16(fat + p);
    const uint32_t hdr = ReadU32(fat + p + 4);
    const uint64_t payload = ReadU64(fat + p + 8);
    if (hdr < 64 || payload == 0) return false;
    if (p + hdr + payload > fat_size) return false;
    if (kind == kPtxKind && ReadU32(fat + p + 28) == kAdaArch) {
      entry_offset = p;
      return true;
    }
    p += hdr + payload;
  }
  return false;
}

// Decompress the Ada PTX, rewrite the blend weights, and re-emit a truncated
// fatbin that ends after it.
inline bool BuildTemporalFatbin(const uint8_t* fat, size_t fat_size,
                                const TemporalProfile& profile,
                                std::vector<uint8_t>& out, std::string& why) {
  size_t entry = 0;
  if (!FindAdaPtxEntry(fat, fat_size, entry)) {
    why = "no sm_89 PTX entry";
    return false;
  }

  const uint32_t hdr = ReadU32(fat + entry + 4);
  const uint32_t compressed = ReadU32(fat + entry + 16);
  const uint64_t raw = ReadU64(fat + entry + 56);
  if (compressed == 0 || raw == 0 || raw > (8u << 20)) {
    why = "PTX entry is not compressed as expected";
    return false;
  }
  if (raw != profile.ptx_bytes) {
    std::stringstream s;
    s << "PTX is " << raw << " bytes, expected " << profile.ptx_bytes;
    why = s.str();
    return false;
  }

  std::vector<uint8_t> ptx(static_cast<size_t>(raw));
  if (!Lz4BlockDecompress(fat + entry + hdr, compressed, ptx.data(), ptx.size())) {
    why = "LZ4 decompression failed";
    return false;
  }

  const std::string entry_signature = std::string(".entry ") + profile.entry_name + "(";
  const std::string parameter_name = std::string(profile.entry_name) + "_param_0";
  const std::string parameter_signature =
      std::string(".param .align 8 .b8 ") + parameter_name + "[144]";
  const std::string ptx_text(reinterpret_cast<const char*>(ptx.data()), ptx.size());
  if (ptx_text.find(entry_signature) == std::string::npos ||
      ptx_text.find(parameter_signature) == std::string::npos ||
      ptx_text.find(".reg .f32 %f<1362>;") == std::string::npos) {
    why = "temporal kernel signature changed";
    return false;
  }

  // The join label must be unique, or we are not looking at the kernel we think.
  const char* begin = reinterpret_cast<const char*>(ptx.data());
  const size_t n = ptx.size();
  const size_t label_len = sizeof(kJoinLabel) - 1;
  size_t label = SIZE_MAX;
  for (size_t i = 0; i + label_len <= n; ++i) {
    if (std::memcmp(begin + i, kJoinLabel, label_len) != 0) continue;
    if (label != SIZE_MAX) {
      why = "join label is not unique";
      return false;
    }
    label = i;
  }
  if (label == SIZE_MAX) {
    why = "join label not found";
    return false;
  }
  size_t insertion = label + label_len;
  while (insertion < n && begin[insertion] != '\n') ++insertion;
  if (insertion >= n) {
    why = "join label has no line end";
    return false;
  }
  ++insertion;

  // Every midpoint constant that terminates a mul.ftz.f32 line.
  const size_t mid_len = sizeof(kMidpointBits) - 1;
  const size_t mul_len = sizeof(kMulPrefix) - 1;
  std::vector<size_t> marks;
  marks.reserve(kExpectedMidpoints);
  for (size_t i = 0; i + mid_len < n; ++i) {
    if (std::memcmp(begin + i, kMidpointBits, mid_len) != 0) continue;
    if (begin[i + mid_len] != ';') continue;
    size_t line = i;
    while (line > 0 && begin[line - 1] != '\n') --line;
    if (i - line < mul_len) continue;
    if (std::memcmp(begin + line, kMulPrefix, mul_len) != 0) continue;
    marks.push_back(i);
  }
  if (marks.size() != kExpectedMidpoints) {
    std::stringstream s;
    s << "found " << marks.size() << " midpoint multiplies, expected " << kExpectedMidpoints;
    why = s.str();
    return false;
  }
  if (marks.front() <= insertion) {
    why = "first midpoint precedes the injection point";
    return false;
  }

  const std::string temporal_input =
      "ld.param.f32 %f134, [" + parameter_name + "+32];\r\n"
      "mov.f32 %f135, 0f3F800000;\r\n"
      "sub.ftz.f32 %f136, %f135, %f134;\r\n";

  std::vector<uint8_t> patched;
  patched.reserve(n + temporal_input.size());
  auto append = [&patched](const void* p, size_t bytes) {
    const auto* b = static_cast<const uint8_t*>(p);
    patched.insert(patched.end(), b, b + bytes);
  };
  append(ptx.data(), insertion);
  append(temporal_input.data(), temporal_input.size());
  size_t src = insertion;
  const size_t half = kExpectedMidpoints / 2;
  for (size_t i = 0; i < marks.size(); ++i) {
    append(ptx.data() + src, marks[i] - src);
    const char* scale = (i < half) ? kCurrToPrev : kPrevToCurr;
    append(scale, 5);
    src = marks[i] + mid_len;
  }
  append(ptx.data() + src, n - src);

  const size_t padded = (patched.size() + 7) & ~size_t{7};
  const size_t final_size = entry + hdr + padded;

  out.assign(fat, fat + entry + hdr);
  out.resize(final_size, 0);
  std::memcpy(out.data() + entry + hdr, patched.data(), patched.size());

  const uint64_t payload64 = padded;
  const uint32_t zero32 = 0;
  const uint64_t zero64 = 0;
  std::memcpy(out.data() + entry + 8, &payload64, sizeof(payload64));
  std::memcpy(out.data() + entry + 16, &zero32, sizeof(zero32));
  std::memcpy(out.data() + entry + 40, &kUncompressedFlags, sizeof(kUncompressedFlags));
  std::memcpy(out.data() + entry + 56, &zero64, sizeof(zero64));
  const uint64_t outer = final_size - kOuterHeader;
  std::memcpy(out.data() + 8, &outer, sizeof(outer));
  return true;
}

// Kernel names confirm that a slot belongs to the expected descriptor family,
// but do not uniquely identify its temporal program. Identify that program by
// the exact sm_89 PTX size as well, then validate its internal signatures before
// rebuilding it.
inline const TemporalProfile* FindTemporalProfile(const uint8_t* fat, size_t fat_size) {
  size_t entry = 0;
  if (!FindAdaPtxEntry(fat, fat_size, entry)) return nullptr;
  const uint64_t raw = ReadU64(fat + entry + 56);
  for (const auto& profile : kTemporalProfiles) {
    if (raw == profile.ptx_bytes) return &profile;
  }
  return nullptr;
}

inline bool PointsToCString(const uint8_t* base, size_t image_size, uint64_t value,
                            const char* expected) {
  const auto start = reinterpret_cast<uintptr_t>(base);
  if (value < start || value >= start + image_size) return false;
  const char* s = reinterpret_cast<const char*>(value);
  const size_t len = std::strlen(expected);
  if (value + len + 1 > start + image_size) return false;
  return std::memcmp(s, expected, len + 1) == 0;
}

inline bool ReadRelativePointer(const uint8_t* base, size_t image_size, const uint8_t* slot,
                                ptrdiff_t displacement, uint64_t& value) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  const uintptr_t end = start + image_size;
  const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
  uintptr_t field = address;
  if (displacement >= 0) {
    const auto distance = static_cast<uintptr_t>(displacement);
    if (distance > UINTPTR_MAX - address) return false;
    field = address + distance;
  } else {
    const auto distance = static_cast<uintptr_t>(-(displacement + 1)) + 1;
    if (distance > address) return false;
    field = address - distance;
  }
  if (field < start || field > end || sizeof(value) > end - field) return false;
  std::memcpy(&value, reinterpret_cast<const void*>(field), sizeof(value));
  return true;
}

}  // namespace internal

struct Patch {
  uint64_t* slot;
  uint64_t original;
};

// Replaces the dlfg kernel fatbin in every descriptor that references it. The
// module carries eight identical descriptors in separate tables and we cannot
// tell which one the runtime will pick, so all of them are redirected.
inline bool Apply(HMODULE module, std::vector<Patch>& patches, void*& allocation,
                  std::string& detail) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const auto start = reinterpret_cast<uintptr_t>(base);

  // Find descriptor slots: an 8-byte pointer to a fatbin, with the profile's
  // exact entry and descriptor names at its version-specific relative offsets.
  std::vector<uint64_t*> slots;
  const uint8_t* fat = nullptr;
  size_t fat_size = 0;
  const internal::TemporalProfile* selected_profile = nullptr;

  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
    uint8_t* sec = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    for (size_t off = 0; off + sizeof(uint64_t) <= size; off += sizeof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, sec + off, sizeof(value));
      if (value < start || value >= start + image_size) continue;
      const auto* candidate = reinterpret_cast<const uint8_t*>(value);
      if (internal::ReadU32(candidate) != internal::kFatbinMagic) continue;

      const internal::TemporalProfile* name_profile = nullptr;
      for (const auto& profile : internal::kTemporalProfiles) {
        uint64_t entry_name = 0;
        uint64_t desc_name = 0;
        if (!internal::ReadRelativePointer(base, image_size, sec + off,
                                           profile.entry_name_offset, entry_name) ||
            !internal::ReadRelativePointer(base, image_size, sec + off,
                                           profile.descriptor_name_offset, desc_name)) {
          continue;
        }
        if (internal::PointsToCString(base, image_size, entry_name, profile.entry_name) &&
            internal::PointsToCString(base, image_size, desc_name, profile.descriptor_name)) {
          name_profile = &profile;
          break;
        }
      }
      if (name_profile == nullptr) continue;

      const uint64_t declared = internal::ReadU64(candidate + 8);
      const size_t total = static_cast<size_t>(declared) + internal::kOuterHeader;
      if (total < 1024 || total > (16u << 20)) continue;
      if (value + total > start + image_size) continue;
      const auto* fat_profile = internal::FindTemporalProfile(candidate, total);
      if (fat_profile == nullptr || fat_profile != name_profile) continue;
      if (fat == nullptr) {
        fat = candidate;
        fat_size = total;
        selected_profile = fat_profile;
      } else if (candidate != fat) {
        continue;  // a second temporal-looking kernel; leave it alone
      }
      slots.push_back(reinterpret_cast<uint64_t*>(sec + off));
    }
  }

  if (fat == nullptr || slots.empty() || selected_profile == nullptr) {
    detail = "no supported temporal-kernel descriptor found";
    return false;
  }

  std::vector<uint8_t> rebuilt;
  std::string why;
  if (!internal::BuildTemporalFatbin(fat, fat_size, *selected_profile, rebuilt, why)) {
    detail = why;
    return false;
  }

  void* mem = VirtualAlloc(nullptr, rebuilt.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (mem == nullptr) {
    detail = "allocation failed";
    return false;
  }
  std::memcpy(mem, rebuilt.data(), rebuilt.size());

  for (uint64_t* slot : slots) {
    DWORD old_protect = 0;
    if (VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &old_protect) == 0) continue;
    patches.push_back({slot, *slot});
    *slot = reinterpret_cast<uint64_t>(mem);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(uint64_t), old_protect, &ignored);
  }

  if (patches.empty()) {
    VirtualFree(mem, 0, MEM_RELEASE);
    detail = "no descriptor slot was writable";
    return false;
  }

  allocation = mem;
  std::stringstream s;
  s << "redirected " << patches.size() << " " << selected_profile->descriptor_name
    << " descriptor(s) from a " << fat_size << "-byte fatbin to a " << rebuilt.size()
    << "-byte temporal-corrected rebuild";
  detail = s.str();
  return true;
}

inline void Restore(std::vector<Patch>& patches, void*& allocation) {
  for (const auto& patch : patches) {
    DWORD old_protect = 0;
    if (VirtualProtect(patch.slot, sizeof(uint64_t), PAGE_READWRITE, &old_protect) == 0) continue;
    *patch.slot = patch.original;
    DWORD ignored = 0;
    VirtualProtect(patch.slot, sizeof(uint64_t), old_protect, &ignored);
  }
  patches.clear();
  if (allocation != nullptr) {
    VirtualFree(allocation, 0, MEM_RELEASE);
    allocation = nullptr;
  }
}

}  // namespace mfgunlock::midpoint
