// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include "../src/addons/mfgunlock/thin_geometry.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED " << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while (false)
namespace tg = mfgunlock::thingeometry;
namespace aq = mfgunlock::adaptivequality;
size_t Count(const std::string& value, const std::string& key) {
  size_t count = 0, pos = 0;
  while ((pos = value.find(key, pos)) != std::string::npos) { ++count; pos += key.size(); }
  return count;
}
int main(int argc, char** argv) {
  tg::g_adaptive_quality_enabled = true;
  tg::g_adaptive_quality_profile = aq::Profile::kLuminanceDirectionalV3;
  tg::g_adaptive_quality_v3_photometric = true;
  tg::g_adaptive_quality_v3_directional_border = true;
  std::string why;
  if (argc == 4 && std::string(argv[1]) == "--emit") {
    std::ifstream input(argv[2], std::ios::binary);
    CHECK(input.good());
    std::string source{std::istreambuf_iterator<char>(input), {}};
    CHECK(tg::internal::RewriteValidatedWarpBlend(source, tg::g_adaptive_quality_profile, why));
    std::filesystem::create_directories(argv[3]);
    std::ofstream output(std::filesystem::path(argv[3]) / "warp-preset-0.ptx", std::ios::binary);
    output << source;
    CHECK(output.good());
    return EXIT_SUCCESS;
  }
  const std::string source = ".entry Kernel_BlendCandidatesFused(\n.maxntid 256, 1, 1\n.reg .pred %p<260>;\nld.param.u8 %rs8, [%rd6+220];\n";
  auto patched = source;
  CHECK(tg::internal::RewriteValidatedWarpBlend(patched, tg::g_adaptive_quality_profile, why));
#if defined(MFGUNLOCK_CONTINUOUS_BORDER_AXES)
  CHECK(Count(patched, "MFGUNLOCK_AXIS_BLEND_CANDIDATE") == 2);
  CHECK(Count(patched, "MFGUNLOCK_CONTINUOUS_BORDER_STABILITY") == 0);
#else
  CHECK(Count(patched, "MFGUNLOCK_CONTINUOUS_BORDER_STABILITY") == 2);
  CHECK(Count(patched, "MFGUNLOCK_AXIS_BLEND_CANDIDATE") == 0);
#endif
  CHECK(Count(patched, ".maxnreg 48") == 1);
  CHECK(Count(patched, "ld.") == Count(source, "ld."));
  for (const auto* forbidden : {"ld.global", "st.global", "tex.", "suld.", "sust.", ".local", "history", "WARP_LAB"})
    CHECK(patched.find(forbidden) == std::string::npos);
  auto malformed = source;
  malformed.erase(malformed.find(".maxntid"), std::string(".maxntid 256, 1, 1\n").size());
  CHECK(!tg::internal::RewriteValidatedWarpBlend(malformed, tg::g_adaptive_quality_profile, why));
  // BuildRedirectedFatbin owns a private PTX copy and restores its original
  // before trying V2/V1. The rejected helper must not insert the new program.
  CHECK(malformed.find("MFGUNLOCK_CONTINUOUS_BORDER_STABILITY") == std::string::npos);
  CHECK(malformed.find("MFGUNLOCK_AXIS_BLEND_CANDIDATE") == std::string::npos);
  CHECK(malformed.find(".maxnreg 48") == std::string::npos);
  tg::g_adaptive_quality_v3_directional_border = false;
  patched = source;
  CHECK(tg::internal::RewriteValidatedWarpBlend(patched, tg::g_adaptive_quality_profile, why));
  CHECK(patched.find("MFGUNLOCK_CONTINUOUS_BORDER_STABILITY") == std::string::npos);
  CHECK(patched.find(".maxnreg 48") == std::string::npos);
  tg::g_adaptive_quality_v3_directional_border = true;
  patched = source;
  CHECK(tg::internal::RewriteValidatedWarpBlend(patched, aq::Profile::kFlickerReducedV2, why));
  CHECK(patched.find("MFGUNLOCK_CONTINUOUS_BORDER_STABILITY") == std::string::npos);
  std::cout << "Local stability warp: continuity program, 48-register directive, rejected directive and A/B prerequisites passed\n";
  return EXIT_SUCCESS;
}
