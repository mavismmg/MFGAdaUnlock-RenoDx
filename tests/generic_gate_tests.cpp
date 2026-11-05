// 1.8.4: the generic architecture gate accepts only the reviewed pair of
// compares against 0x1b0 (one `cmp r32, imm32`, one `cmp eax, imm32`).
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>
#include "../src/addons/mfgunlock/generic_gate.hpp"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace g = mfgunlock::genericgate;

static std::vector<g::Hit> Scan(const std::vector<uint8_t>& code, uint32_t rva = 0x1000) {
  std::vector<g::Hit> hits;
  g::Scan(code.data(), code.size(), rva, hits);
  return hits;
}
static std::vector<uint8_t> Filler(size_t n) { return std::vector<uint8_t>(n, 0xcc); }
static void Put(std::vector<uint8_t>& code, size_t at, std::vector<uint8_t> bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) code[at + i] = bytes[i];
}

int main() {
  // The shape of every reviewed 310.x provider (310.6 of AC Black Flag: cmp esi at
  // 0x1a026, cmp eax at 0x33eff).
  auto code = Filler(256);
  Put(code, 16, {0x81, 0xfe, 0xb0, 0x01, 0x00, 0x00});
  Put(code, 100, {0x3d, 0xb0, 0x01, 0x00, 0x00});
  auto hits = Scan(code);
  CHECK(hits.size() == 2 && g::Accept(hits));
  CHECK(hits[0].rva == 0x1010 && hits[0].size == 6 && hits[1].rva == 0x1064 && hits[1].size == 5);

  // Order does not matter.
  code = Filler(256);
  Put(code, 10, {0x3d, 0xb0, 0x01, 0x00, 0x00});
  Put(code, 200, {0x81, 0xfd, 0xb0, 0x01, 0x00, 0x00});
  CHECK(g::Accept(Scan(code)));

  // Nothing, one, three or four compares: refused (TLOU Part II's 310.1 has 3,
  // RE Requiem's 310.3 has 4, the driver's 310.2.1 has none).
  CHECK(!g::Accept(Scan(Filler(256))));
  code = Filler(256);
  Put(code, 10, {0x3d, 0xb0, 0x01, 0x00, 0x00});
  CHECK(!g::Accept(Scan(code)));
  Put(code, 60, {0x81, 0xfd, 0xb0, 0x01, 0x00, 0x00});
  Put(code, 120, {0x3d, 0xb0, 0x01, 0x00, 0x00});
  CHECK(Scan(code).size() == 3 && !g::Accept(Scan(code)));
  Put(code, 180, {0x81, 0xfe, 0xb0, 0x01, 0x00, 0x00});
  CHECK(Scan(code).size() == 4 && !g::Accept(Scan(code)));

  // Two of the same shape: refused.
  code = Filler(256);
  Put(code, 10, {0x3d, 0xb0, 0x01, 0x00, 0x00});
  Put(code, 100, {0x3d, 0xb0, 0x01, 0x00, 0x00});
  CHECK(!g::Accept(Scan(code)));

  // Not the gate: another opcode extension (81 E8 = sub), another constant (Ada's
  // 0x190, already lowered), or a compare cut by the section's end.
  code = Filler(64);
  Put(code, 4, {0x81, 0xe8, 0xb0, 0x01, 0x00, 0x00});
  Put(code, 20, {0x3d, 0x90, 0x01, 0x00, 0x00});
  Put(code, 60, {0x3d, 0xb0, 0x01, 0x00});
  CHECK(Scan(code).empty());

  // VS_FIXEDFILEINFO: the major version is the high word of dwFileVersionMS.
  std::vector<uint8_t> resource(128, 0);
  const uint32_t signature = 0xfeef04bd, version = (310u << 16) | 6u;
  std::memcpy(resource.data() + 40, &signature, 4);
  std::memcpy(resource.data() + 48, &version, 4);
  CHECK(g::MajorVersion(resource.data(), resource.size()) == 310);
  CHECK(g::MajorVersion(resource.data(), 40) == -1);

  std::cout << "PASS: generic architecture gate" << std::endl;
  return 0;
}
