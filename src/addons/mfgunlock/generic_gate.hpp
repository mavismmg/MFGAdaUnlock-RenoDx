// SPDX-License-Identifier: MIT
#pragma once
// 1.8.4: the architecture gate of a DLSS-G provider with no exact profile (a
// newer NVIDIA download, or a game's rebuilt copy such as the 310.6 of
// Assassin's Creed IV Black Flag Resynced). Every reviewed 310.x build gates
// Blackwell with exactly two compares against 0x1b0: one `cmp r32, imm32`
// (81 F8+r) and one `cmp eax, imm32` (3D). Only that pair is accepted; any other
// count or shape is refused, as an unknown provider was before. On the 20
// provider copies on the development PC, this byte scan found the same sites
// as the disassembler-checked profile tool (tools/build_compatibility_profiles.py).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mfgunlock::genericgate {
constexpr uint8_t kBlackwell[] = {0xb0, 0x01, 0x00, 0x00};
struct Hit {
  uint32_t rva;
  uint8_t size;  // 6: cmp r32, imm32 (81 F8+r); 5: cmp eax, imm32 (3D)
};
// Appends the compares against 0x1b0 in one executable section.
inline void Scan(const uint8_t* code, size_t size, uint32_t rva, std::vector<Hit>& hits) {
  for (size_t at = 0; at + 5 <= size; ++at) {
    if (code[at] == 0x3d && std::memcmp(code + at + 1, kBlackwell, 4) == 0) {
      hits.push_back({static_cast<uint32_t>(rva + at), 5});
      at += 4;
    } else if (at + 6 <= size && code[at] == 0x81 && code[at + 1] >= 0xf8 &&
               std::memcmp(code + at + 2, kBlackwell, 4) == 0) {
      hits.push_back({static_cast<uint32_t>(rva + at), 6});
      at += 5;
    }
  }
}
inline bool Accept(const std::vector<Hit>& hits) {
  if (hits.size() != 2) return false;
  return (hits[0].size == 6 && hits[1].size == 5) || (hits[0].size == 5 && hits[1].size == 6);
}
// VS_FIXEDFILEINFO's major file version in a mapped version resource, read
// without file I/O (this can run under the loader lock).
inline int MajorVersion(const uint8_t* resource, size_t size) {
  for (size_t at = 0; at + 52 <= size; at += 4) {
    uint32_t signature = 0;
    std::memcpy(&signature, resource + at, 4);
    if (signature != 0xfeef04bdu) continue;
    uint32_t ms = 0;
    std::memcpy(&ms, resource + at + 8, 4);
    return static_cast<int>(ms >> 16);
  }
  return -1;
}
}  // namespace mfgunlock::genericgate
