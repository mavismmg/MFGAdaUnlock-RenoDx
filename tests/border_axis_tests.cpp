// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include "../src/addons/mfgunlock/adaptive_quality_v3.hpp"
#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED " << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while (false)
namespace aq = mfgunlock::adaptivequalityv3;
struct Case { float w, h, u, v, fu, fv, iu, iv; };
float Evaluate(const Case& c, bool inverse = false) {
  return aq::ContinuousAxisBorderDistance(c.u, c.v, inverse ? c.iu : c.fu,
      inverse ? c.iv : c.fv, c.w, c.h);
}
Case Left(float angle, float w = 1920, float h = 1080) {
  const float radians = angle * 0.017453292519943295f;
  return {w, h, 1.75f / w, 0.5f, 0.75f / w,
      0.5f - std::tan(radians) / h, 2.75f / w,
      0.5f + std::tan(radians) / h};
}
Case Mirror(Case c, bool x, bool y) {
  if (x) { c.u = 1 - c.u; c.fu = 1 - c.fu; c.iu = 1 - c.iu; }
  if (y) { c.v = 1 - c.v; c.fv = 1 - c.fv; c.iv = 1 - c.iv; }
  return c;
}
Case Transpose(Case c) { return {c.h, c.w, c.v, c.u, c.fv, c.fu, c.iv, c.iu}; }
int main(int argc, char** argv) {
  // The wider pair survives FP32 quantization of normalized viewport coordinates.
  const Case before = Left(44.99f), after = Left(45.01f);
  const auto legacy = [](const Case& c) { return aq::BorderConfidence(
      aq::ContinuousDirectionalBorderDistance(c.u, c.v, c.fu, c.fv, c.w, c.h)); };
  CHECK(std::abs(legacy(before) - legacy(after)) > 0.5f);
  CHECK(std::abs(aq::BorderConfidence(Evaluate(before)) -
                 aq::BorderConfidence(Evaluate(after))) < 0.001f);
  std::vector<Case> cases;
  for (const auto dims : {std::pair{1920.f, 1080.f}, std::pair{2560.f, 1440.f},
                          std::pair{3840.f, 2160.f}, std::pair{1080.f, 1920.f}}) {
    for (int i = 0; i <= 800; ++i) {
      Case c = Left(41.f + i * 0.01f, dims.first, dims.second);
      for (bool mx : {false, true}) for (bool my : {false, true}) {
        Case m = Mirror(c, mx, my);
        cases.push_back(m);
        cases.push_back(Transpose(m));
        const float value = Evaluate(m);
        CHECK(std::isfinite(value) && value >= 0);
        CHECK(std::abs(value - Evaluate(Transpose(m))) < 0.003f);
        const float horizontal = aq::BorderAxisDistance(m.u, m.v, m.fu, m.fv, m.w, m.h, true);
        const float vertical = aq::BorderAxisDistance(m.u, m.v, m.fu, m.fv, m.w, m.h, false);
        const float symmetric = aq::SymmetricBorderDistance(m.fu, m.fv, m.w, m.h);
        CHECK(value >= std::min({horizontal, vertical, symmetric}) - 0.0001f);
        CHECK(value <= std::max({horizontal, vertical, symmetric}) + 0.0001f);
        CHECK(aq::TaperAddedWeight(0.3f, 0.9f, aq::BorderConfidence(value)) >= 0.3f);
      }
    }
  }
  // Cardinal motion keeps the 1.4.2 treatment; stationary motion is symmetric.
  for (const Case c : {Left(0), Transpose(Left(0)), Mirror(Left(0), true, true),
                       Case{1920,1080,.5f,.5f,.5f,.5f,.5f,.5f}}) {
    CHECK(std::abs(Evaluate(c) - aq::ContinuousDirectionalBorderDistance(
        c.u, c.v, c.fu, c.fv, c.w, c.h)) < 0.001f);
    cases.push_back(c);
  }
  // Candidate positions close to all corners and both magnitude endpoints.
  for (float length : {0.f, .4999f, .5f, .5001f, 1.4999f, 1.5f, 1.5001f, 8.f}) {
    for (float angle : {0.f, 44.9999f, 45.f, 45.0001f, 90.f}) {
      const float dx = length * std::cos(angle * .017453292519943295f);
      const float dy = length * std::sin(angle * .017453292519943295f);
      Case c{1920,1080,(.75f + dx)/1920,(.75f + dy)/1080,.75f/1920,.75f/1080,
          (.75f+2*dx)/1920,(.75f+2*dy)/1080};
      for (bool mx : {false,true}) for (bool my : {false,true}) {
        auto m = Mirror(c,mx,my);
        cases.push_back(m);
        CHECK(std::isfinite(Evaluate(m)) && Evaluate(m) >= 0);
        CHECK(Evaluate(m) < 0.76f);  // Both axes retain the opposite corner edge.
      }
    }
  }
  const std::string program = aq::ContinuousAxisBorderProgram();
  for (auto forbidden : {"ld.", "st.", "tex.", "suld.", "sust.", ".local", "CAND_", "DIRECTION"})
    CHECK(program.find(forbidden) == std::string::npos);
  CHECK(program.find("MFGUNLOCK_AXIS_FORWARD_DONE:") != std::string::npos);
  CHECK(program.find("MFGUNLOCK_AXIS_INVERSE_DONE:") != std::string::npos);
  if (argc == 3 && std::string(argv[1]) == "--emit") {
    const std::filesystem::path dir{argv[2]};
    std::filesystem::create_directories(dir);
    std::ofstream ptx(dir / "border-program.ptx"); ptx << program; CHECK(ptx.good());
    std::ofstream csv(dir / "border-cases.csv");
    csv << "width,height,u,v,forward_u,forward_v,inverse_u,inverse_v,expected_forward,expected_inverse\n";
    csv << std::setprecision(9);
    for (const auto& c : cases) csv << c.w << ',' << c.h << ',' << c.u << ',' << c.v << ','
        << c.fu << ',' << c.fv << ',' << c.iu << ',' << c.iv << ',' << Evaluate(c) << ',' << Evaluate(c,true) << '\n';
    CHECK(csv.good());
  }
  std::cout << "Axis border candidate: " << cases.size() << " oracle cases; continuity, corners, symmetry and native anchor passed\n";
  return EXIT_SUCCESS;
}
