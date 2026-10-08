// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED " << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while (false)
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.good()) { std::cerr << "Cannot read " << path << '\n'; std::exit(EXIT_FAILURE); }
  return {std::istreambuf_iterator<char>(input), {}};
}
int main() {
  const std::filesystem::path root{RELEASE_SOURCE_ROOT};
  for (const auto* name : {"addon.cpp", "framecount.hpp", "input_diagnostics.hpp", "thin_geometry.hpp"}) {
    const auto source = Read(root / "src/addons/mfgunlock" / name);
    for (const auto* forbidden : {"MFGUNLOCK_QUALITY_PIPELINE_LAB", "warp_quality_lab.hpp", "Warp Quality Lab", "pipeline_ngx.hpp", "ngxprearm", "g_warp_preset"})
      CHECK(source.find(forbidden) == std::string::npos);
  }
  const auto addon = Read(root / "src/addons/mfgunlock/addon.cpp");
  CHECK(addon.find("Release 1.4.3 - Continuous Border Stability") != std::string::npos);
  CHECK(addon.find("Release 1.4.2 - Local Stability & CPU Overhead") != std::string::npos);
  CHECK(addon.find("Stability Lab experimental") == std::string::npos);
  const auto build = Read(root / "tests/CMakeLists.txt");
  CHECK(build.find("MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY") != std::string::npos);
  CHECK(build.find("MFGUNLOCK_NVAPI_TEMPORAL") == std::string::npos);
  CHECK(build.find("MFGUNLOCK_CONTINUOUS_BORDER_AXES MFGUNLOCK_RELEASE_1_4_3") != std::string::npos);
  CHECK(Read(root / ".gitignore").find("thin_geometry_stability.generated.hpp") != std::string::npos);
  CHECK(!std::filesystem::exists(root / "src/addons/mfgunlock/warp_quality_lab.hpp"));
  CHECK(!std::filesystem::exists(root / "src/addons/mfgunlock/pipeline_ngx.hpp"));
  std::cout << "Release isolation: no observer or unapproved warp presets; local-only production flags and release identity passed\n";
  return EXIT_SUCCESS;
}
