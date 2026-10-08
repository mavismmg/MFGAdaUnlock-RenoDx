/* SPDX-License-Identifier: MIT
 * Exposure-relative, frame-local confidence and directional boundary helpers.
 *
 * V3 reuses values already live in Kernel_BlendCandidatesFused. It adds no
 * resource reads, history, storage or allocations. Structural validity stays
 * in the provider-specific V1 program and always runs before these fragments.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace mfgunlock::adaptivequalityv3 {

inline constexpr const char* kRelativePhotometricError = R"PTX(
// MFGUNLOCK_RELATIVE_PHOTOMETRIC_ERROR_V3
// Base-pair deltas and signal luminance. Values remain in the provider's
// sampled domain; the local scale makes the comparison exposure-relative.
// f174..f181 are future-defined at this exact, hash-validated insertion point
// and are overwritten by the provider immediately after the injected block.
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %f177, %f117, %f121;
mul.f32 %f178, %f115, 0f3E59B3D0;
fma.rn.f32 %f178, %f116, 0f3F371759, %f178;
fma.rn.f32 %f178, %f117, 0f3D93DD98, %f178;
mul.f32 %f179, %f119, 0f3E59B3D0;
fma.rn.f32 %f179, %f120, 0f3F371759, %f179;
fma.rn.f32 %f179, %f121, 0f3D93DD98, %f179;
sub.f32 %qf4, %f178, %f179;
abs.f32 %qf4, %qf4;
abs.f32 %f178, %f178;
abs.f32 %f179, %f179;
add.f32 %f178, %f178, %f179;
mul.f32 %f178, %f178, 0f3F000000;
max.f32 %f178, %f178, 0f3D800000;
rcp.approx.ftz.f32 %f178, %f178;
sub.f32 %f179, %qf6, %f177;
abs.f32 %f179, %f179;
add.f32 %qf5, %qf6, %f177;
fma.rn.f32 %qf5, %qf5, 0fBF000000, %qf7;
abs.f32 %qf5, %qf5;
max.f32 %f179, %f179, %qf5;
mul.f32 %f179, %f179, 0f3F000000;
max.f32 %qf4, %qf4, %f179;
mul.f32 %qf6, %qf4, %f178;

// Candidate-pair error. qf6 and qf9 are the normalized base and candidate
// errors consumed by the continuous confidence and arbitration stages.
sub.f32 %qf7, %f125, %f131;
sub.f32 %f177, %f126, %f132;
sub.f32 %f179, %f127, %f133;
mul.f32 %qf2, %f125, 0f3E59B3D0;
fma.rn.f32 %qf2, %f126, 0f3F371759, %qf2;
fma.rn.f32 %qf2, %f127, 0f3D93DD98, %qf2;
mul.f32 %qf3, %f131, 0f3E59B3D0;
fma.rn.f32 %qf3, %f132, 0f3F371759, %qf3;
fma.rn.f32 %qf3, %f133, 0f3D93DD98, %qf3;
sub.f32 %qf4, %qf2, %qf3;
abs.f32 %qf4, %qf4;
abs.f32 %qf2, %qf2;
abs.f32 %qf3, %qf3;
add.f32 %qf2, %qf2, %qf3;
mul.f32 %qf2, %qf2, 0f3F000000;
max.f32 %qf2, %qf2, 0f3D800000;
rcp.approx.ftz.f32 %qf2, %qf2;
sub.f32 %qf3, %qf7, %f179;
abs.f32 %qf3, %qf3;
add.f32 %qf5, %qf7, %f179;
fma.rn.f32 %qf5, %qf5, 0fBF000000, %f177;
abs.f32 %qf5, %qf5;
max.f32 %qf3, %qf3, %qf5;
mul.f32 %qf3, %qf3, 0f3F000000;
max.f32 %qf4, %qf4, %qf3;
mul.f32 %f176, %qf4, %qf2;
)PTX";

inline constexpr const char* kSmoothWarpConfidence = R"PTX(
// MFGUNLOCK_SMOOTH_WARP_CONFIDENCE_V3
add.sat.f32 %f174, %f148, 0f00000000;
add.sat.f32 %f175, %f149, 0f00000000;
// Candidate improvement: zero through 0.10 and full at 0.35.
sub.f32 %qf2, %qf6, %f176;
sub.f32 %qf2, %qf2, 0f3DCCCCCD;
mul.sat.f32 %qf2, %qf2, 0f40800000;
// Candidate agreement: full through 0.08 and zero at 0.30.
sub.f32 %qf3, 0f3E99999A, %f176;
mul.sat.f32 %qf3, %qf3, 0f4091745D;
min.f32 %qf2, %qf2, %qf3;
@!%qv3 mov.f32 %qf2, 0f00000000;
// Native evidence keeps the V2 0.20..0.50 weight interval while the relative
// base error transitions from 0.50 to 1.00.
sub.f32 %qf3, %qf6, 0f3F000000;
mul.sat.f32 %qf3, %qf3, 0f40000000;
sub.f32 %qf4, %f174, 0f3E4CCCCD;
mul.sat.f32 %qf4, %qf4, 0f40555555;
min.f32 %qf4, %qf4, %qf3;
max.f32 %qf4, %qf4, %qf2;
sub.f32 %qf5, %f175, 0f3E4CCCCD;
mul.sat.f32 %qf5, %qf5, 0f40555555;
min.f32 %qf5, %qf5, %qf3;
max.f32 %qf5, %qf5, %qf2;
// A lone structurally-valid candidate is useful only when NVIDIA's native
// evidence is already strong. This limits unstable addon-only boosts around
// disocclusions while retaining the native weight as the hard lower bound.
@%qv3 bra MFGUNLOCK_UNILATERAL_CONFIDENCE_DONE_V31;
sub.f32 %qf7, %f174, 0f3F000000;
mul.sat.f32 %qf7, %qf7, 0f40800000;
fma.rn.f32 %qf6, %qf7, 0fC0000000, 0f40400000;
mul.f32 %qf7, %qf7, %qf7;
mul.f32 %qf7, %qf7, %qf6;
mul.f32 %qf4, %qf4, %qf7;
sub.f32 %qf7, %f175, 0f3F000000;
mul.sat.f32 %qf7, %qf7, 0f40800000;
fma.rn.f32 %qf6, %qf7, 0fC0000000, 0f40400000;
mul.f32 %qf7, %qf7, %qf7;
mul.f32 %qf7, %qf7, %qf6;
mul.f32 %qf5, %qf5, %qf7;
MFGUNLOCK_UNILATERAL_CONFIDENCE_DONE_V31:
// Structural invalidity remains a hard zero. Semantic confidence zero is an
// exact no-op at the native anchor; confidence one keeps the V1 target.
@!%qv0 mov.f32 %qf0, 0f00000000;
@!%qv0 bra MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V3;
setp.le.f32 %qv5, %qf4, 0f00000000;
@%qv5 mov.f32 %qf0, %f174;
@%qv5 bra MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V3;
setp.ge.f32 %qv5, %qf4, 0f3F800000;
@%qv5 bra MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V3;
fma.rn.f32 %qf7, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf7;
sub.f32 %qf6, %qf0, %f174;
fma.rn.f32 %qf0, %qf4, %qf6, %f174;
MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V3:
@!%qv1 mov.f32 %qf1, 0f00000000;
@!%qv1 bra MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V3;
setp.le.f32 %qv5, %qf5, 0f00000000;
@%qv5 mov.f32 %qf1, %f175;
@%qv5 bra MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V3;
setp.ge.f32 %qv5, %qf5, 0f3F800000;
@%qv5 bra MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V3;
fma.rn.f32 %qf7, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf7;
sub.f32 %qf6, %qf1, %f175;
fma.rn.f32 %qf1, %qf5, %qf6, %f175;
MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V3:
)PTX";

inline constexpr const char* kDirectionalBorderDistances = R"PTX(
// MFGUNLOCK_DIRECTIONAL_BORDER_DISTANCES_V31
cvt.rn.f32.u32 %qf2, %r10;
cvt.rn.f32.u32 %qf3, %r11;
// Forward candidate displacement in pixel units.
sub.f32 %qf4, %f123, %f1;
mul.f32 %qf4, %qf4, %qf2;
mul.f32 %qf5, %qf4, %qf4;
sub.f32 %qf6, %f124, %f2;
mul.f32 %qf6, %qf6, %qf3;
fma.rn.f32 %qf7, %qf6, %qf6, %qf5;
setp.lt.f32 %qv5, %qf7, 0f3E800000;
@%qv5 bra MFGUNLOCK_FORWARD_BORDER_SYMMETRIC_V31;
abs.f32 %qf5, %qf4;
abs.f32 %qf7, %qf6;
setp.ge.f32 %qv4, %qf5, %qf7;
selp.f32 %qf5, %f1, %f2, %qv4;
selp.f32 %qf6, %f123, %f124, %qv4;
selp.f32 %qf7, %qf2, %qf3, %qv4;
add.f32 %qf4, %qf5, %qf6;
mul.f32 %qf4, %qf4, 0f3F000000;
setp.le.f32 %qv5, %qf4, 0f3F000000;
sub.f32 %qf4, 0f3F800000, %qf5;
selp.f32 %qf5, %qf5, %qf4, %qv5;
sub.f32 %qf4, 0f3F800000, %qf6;
selp.f32 %qf6, %qf6, %qf4, %qv5;
mul.f32 %qf5, %qf5, %qf7;
mul.f32 %qf6, %qf6, %qf7;
setp.gt.f32 %qv6, %qf5, %qf6;
min.f32 %qf4, %qf5, %qf6;
max.f32 %qf6, %qf5, %qf6;
add.f32 %qf5, %qf4, 0f3F800000;
min.f32 %qf6, %qf6, %qf5;
selp.f32 %qf4, %qf6, %qf4, %qv6;
// Preserve the perpendicular edge/corner protection discarded by V3.
selp.f32 %f178, %f2, %f1, %qv4;
selp.f32 %f179, %f124, %f123, %qv4;
selp.f32 %qf7, %qf3, %qf2, %qv4;
sub.f32 %qf5, 0f3F800000, %f178;
min.f32 %qf5, %qf5, %f178;
sub.f32 %qf6, 0f3F800000, %f179;
min.f32 %qf6, %qf6, %f179;
min.f32 %qf5, %qf5, %qf6;
mul.f32 %qf5, %qf5, %qf7;
min.f32 %qf4, %qf4, %qf5;
bra MFGUNLOCK_FORWARD_BORDER_DISTANCE_DONE_V31;
MFGUNLOCK_FORWARD_BORDER_SYMMETRIC_V31:
sub.f32 %qf4, 0f3F800000, %f123;
min.f32 %qf4, %qf4, %f123;
mul.f32 %qf4, %qf4, %qf2;
sub.f32 %qf6, 0f3F800000, %f124;
min.f32 %qf6, %qf6, %f124;
mul.f32 %qf6, %qf6, %qf3;
min.f32 %qf4, %qf4, %qf6;
MFGUNLOCK_FORWARD_BORDER_DISTANCE_DONE_V31:
mov.f32 %f180, %qf4;

// Inverse candidate, retaining f180 as the forward distance.
sub.f32 %qf4, %f129, %f1;
mul.f32 %qf4, %qf4, %qf2;
mul.f32 %qf5, %qf4, %qf4;
sub.f32 %qf6, %f130, %f2;
mul.f32 %qf6, %qf6, %qf3;
fma.rn.f32 %qf7, %qf6, %qf6, %qf5;
setp.lt.f32 %qv5, %qf7, 0f3E800000;
@%qv5 bra MFGUNLOCK_INVERSE_BORDER_SYMMETRIC_V31;
abs.f32 %qf5, %qf4;
abs.f32 %qf7, %qf6;
setp.ge.f32 %qv4, %qf5, %qf7;
selp.f32 %qf5, %f1, %f2, %qv4;
selp.f32 %qf6, %f129, %f130, %qv4;
selp.f32 %qf7, %qf2, %qf3, %qv4;
add.f32 %qf4, %qf5, %qf6;
mul.f32 %qf4, %qf4, 0f3F000000;
setp.le.f32 %qv5, %qf4, 0f3F000000;
sub.f32 %qf4, 0f3F800000, %qf5;
selp.f32 %qf5, %qf5, %qf4, %qv5;
sub.f32 %qf4, 0f3F800000, %qf6;
selp.f32 %qf6, %qf6, %qf4, %qv5;
mul.f32 %qf5, %qf5, %qf7;
mul.f32 %qf6, %qf6, %qf7;
setp.gt.f32 %qv6, %qf5, %qf6;
min.f32 %qf4, %qf5, %qf6;
max.f32 %qf6, %qf5, %qf6;
add.f32 %qf5, %qf4, 0f3F800000;
min.f32 %qf6, %qf6, %qf5;
selp.f32 %qf4, %qf6, %qf4, %qv6;
selp.f32 %f178, %f2, %f1, %qv4;
selp.f32 %f179, %f130, %f129, %qv4;
selp.f32 %qf7, %qf3, %qf2, %qv4;
sub.f32 %qf5, 0f3F800000, %f178;
min.f32 %qf5, %qf5, %f178;
sub.f32 %qf6, 0f3F800000, %f179;
min.f32 %qf6, %qf6, %f179;
min.f32 %qf5, %qf5, %qf6;
mul.f32 %qf5, %qf5, %qf7;
min.f32 %qf4, %qf4, %qf5;
bra MFGUNLOCK_INVERSE_BORDER_DISTANCE_DONE_V31;
MFGUNLOCK_INVERSE_BORDER_SYMMETRIC_V31:
sub.f32 %qf4, 0f3F800000, %f129;
min.f32 %qf4, %qf4, %f129;
mul.f32 %qf4, %qf4, %qf2;
sub.f32 %qf6, 0f3F800000, %f130;
min.f32 %qf6, %qf6, %f130;
mul.f32 %qf6, %qf6, %qf3;
min.f32 %qf4, %qf4, %qf6;
MFGUNLOCK_INVERSE_BORDER_DISTANCE_DONE_V31:
mov.f32 %f181, %qf4;
)PTX";

inline constexpr const char* kDirectionalBorderConfidence = R"PTX(
// MFGUNLOCK_DIRECTIONAL_BORDER_CONFIDENCE_V3
mov.f32 %qf4, %f180;
mov.f32 %qf5, %f181;
setp.ge.f32 %qv5, %qf4, 0f40200000;
setp.ge.f32 %qv6, %qf5, 0f40200000;
and.pred %qv2, %qv5, %qv6;
@%qv2 bra MFGUNLOCK_BORDER_CONFIDENCE_DONE_V3;
sub.f32 %qf4, %qf4, 0f3F000000;
mul.sat.f32 %qf4, %qf4, 0f3F000000;
fma.rn.f32 %qf6, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf6;
sub.f32 %qf6, %qf0, %f174;
fma.rn.f32 %qf0, %qf4, %qf6, %f174;
sub.f32 %qf5, %qf5, 0f3F000000;
mul.sat.f32 %qf5, %qf5, 0f3F000000;
fma.rn.f32 %qf6, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf6;
sub.f32 %qf6, %qf1, %f175;
fma.rn.f32 %qf1, %qf5, %qf6, %f175;
MFGUNLOCK_BORDER_CONFIDENCE_DONE_V3:
)PTX";

// Local-stability path. Blend the symmetric and directional distances in
// squared-motion space over 0.5..1.5 px. Reuse qf2..qf7; no new registers/loads.
inline std::string ContinuousBorderProgram() {
  std::string program = kDirectionalBorderDistances;
  for (int direction = 0; direction < 2; ++direction) {
    std::string fragment = R"PTX(
// MFGUNLOCK_CONTINUOUS_BORDER_STABILITY
sub.f32 %qf5, 0f3F800000, CAND_U;
min.f32 %qf5, %qf5, CAND_U;
mul.f32 %qf5, %qf5, %qf2;
sub.f32 %qf6, 0f3F800000, CAND_V;
min.f32 %qf6, %qf6, CAND_V;
mul.f32 %qf6, %qf6, %qf3;
min.f32 %qf7, %qf5, %qf6;
sub.f32 %qf5, CAND_U, %f1;
mul.f32 %qf5, %qf5, %qf2;
sub.f32 %qf6, CAND_V, %f2;
mul.f32 %qf6, %qf6, %qf3;
mul.f32 %qf5, %qf5, %qf5;
fma.rn.f32 %qf5, %qf6, %qf6, %qf5;
sub.f32 %qf5, %qf5, 0f3E800000;
mul.sat.f32 %qf5, %qf5, 0f3F000000;
fma.rn.f32 %qf6, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf6;
sub.f32 %qf4, %qf4, %qf7;
fma.rn.f32 %qf4, %qf5, %qf4, %qf7;
)PTX";
    for (const auto& replacement : {std::pair{"CAND_U", direction == 0 ? "%f123" : "%f129"},
                                    std::pair{"CAND_V", direction == 0 ? "%f124" : "%f130"}}) {
      size_t pos = 0;
      while ((pos = fragment.find(replacement.first, pos)) != std::string::npos) {
        fragment.replace(pos, 6, replacement.second);
        pos += 5;
      }
    }
    const std::string anchor = direction == 0 ? "mov.f32 %f180, %qf4;" : "mov.f32 %f181, %qf4;";
    const size_t pos = program.find(anchor);
    if (pos == std::string::npos) return kDirectionalBorderDistances;
    program.insert(pos, fragment);
  }
  return program;
}

// Continuous-axis path. Both treatments retain their perpendicular protection.
// Interpolation uses smoothstep(dx^2 / (dx^2 + dy^2)); the 1.4.2 magnitude
// transition stays intact. f177..f179 are dead at this validated insertion site.
inline std::string ContinuousAxisBorderProgram() {
  std::string program = "// MFGUNLOCK_CONTINUOUS_BORDER_AXES\n"
      "cvt.rn.f32.u32 %qf2, %r10;\n"
      "cvt.rn.f32.u32 %qf3, %r11;\n";
  for (int direction = 0; direction < 2; ++direction) {
    const std::string u = direction == 0 ? "%f123" : "%f129";
    const std::string v = direction == 0 ? "%f124" : "%f130";
    const std::string label = direction == 0 ? "FORWARD" : "INVERSE";
    // Apply replacements only to named tokens, never provider register names.
    auto emit = [&](std::string text) {
      for (const auto& item : {std::pair{"CAND_U", u}, std::pair{"CAND_V", v},
                               std::pair{"DIRECTION", label}}) {
        size_t pos = 0;
        while ((pos = text.find(item.first, pos)) != std::string::npos) {
          text.replace(pos, std::string(item.first).size(), item.second);
          pos += item.second.size();
        }
      }
      program += text;
    };
    emit(R"PTX(
// MFGUNLOCK_AXIS_BLEND_CANDIDATE
sub.f32 %qf5, 0f3F800000, CAND_U;
min.f32 %qf5, %qf5, CAND_U;
mul.f32 %qf5, %qf5, %qf2;
sub.f32 %qf6, 0f3F800000, CAND_V;
min.f32 %qf6, %qf6, CAND_V;
mul.f32 %qf6, %qf6, %qf3;
min.f32 %f177, %qf5, %qf6;
mov.f32 %qf4, %f177;
sub.f32 %qf5, CAND_U, %f1;
mul.f32 %qf5, %qf5, %qf2;
sub.f32 %qf6, CAND_V, %f2;
mul.f32 %qf6, %qf6, %qf3;
mul.f32 %qf5, %qf5, %qf5;
fma.rn.f32 %qf7, %qf6, %qf6, %qf5;
setp.le.f32 %qv5, %qf7, 0f3E800000;
@%qv5 bra MFGUNLOCK_AXIS_DIRECTION_DONE;
)PTX");
    for (int axis = 0; axis < 2; ++axis) {
      std::string text = R"PTX(
add.f32 %qf7, CURRENT, CANDIDATE;
setp.le.f32 %qv5, %qf7, 0f3F800000;
sub.f32 %qf5, 0f3F800000, CURRENT;
selp.f32 %qf5, CURRENT, %qf5, %qv5;
sub.f32 %qf6, 0f3F800000, CANDIDATE;
selp.f32 %qf6, CANDIDATE, %qf6, %qv5;
mul.f32 %qf5, %qf5, EXTENT;
mul.f32 %qf6, %qf6, EXTENT;
setp.gt.f32 %qv6, %qf5, %qf6;
min.f32 %qf4, %qf5, %qf6;
max.f32 %qf7, %qf5, %qf6;
add.f32 %qf6, %qf4, 0f3F800000;
min.f32 %qf7, %qf7, %qf6;
selp.f32 %qf4, %qf7, %qf4, %qv6;
sub.f32 %qf5, 0f3F800000, CROSS_CURRENT;
min.f32 %qf5, %qf5, CROSS_CURRENT;
sub.f32 %qf6, 0f3F800000, CROSS_CANDIDATE;
min.f32 %qf6, %qf6, CROSS_CANDIDATE;
min.f32 %qf5, %qf5, %qf6;
mul.f32 %qf5, %qf5, CROSS_EXTENT;
min.f32 %qf4, %qf4, %qf5;
mov.f32 RESULT, %qf4;
)PTX";
      // Longer tokens must be replaced before their suffixes.
      for (const auto& item : {
               std::pair{"CROSS_CURRENT", axis == 0 ? "%f2" : "%f1"},
               std::pair{"CROSS_CANDIDATE", axis == 0 ? "CAND_V" : "CAND_U"},
               std::pair{"CROSS_EXTENT", axis == 0 ? "%qf3" : "%qf2"},
               std::pair{"CURRENT", axis == 0 ? "%f1" : "%f2"},
               std::pair{"CANDIDATE", axis == 0 ? "CAND_U" : "CAND_V"},
               std::pair{"EXTENT", axis == 0 ? "%qf2" : "%qf3"},
               std::pair{"RESULT", axis == 0 ? "%f178" : "%f179"}}) {
        size_t pos = 0;
        while ((pos = text.find(item.first, pos)) != std::string::npos) {
          text.replace(pos, std::string(item.first).size(), item.second);
          pos += std::string(item.second).size();
        }
      }
      emit(text);
    }
    emit(R"PTX(
sub.f32 %qf5, CAND_U, %f1;
mul.f32 %qf5, %qf5, %qf2;
sub.f32 %qf6, CAND_V, %f2;
mul.f32 %qf6, %qf6, %qf3;
mul.f32 %qf5, %qf5, %qf5;
fma.rn.f32 %qf7, %qf6, %qf6, %qf5;
mov.f32 %qf6, %qf7;
rcp.approx.ftz.f32 %qf7, %qf7;
mul.sat.f32 %qf5, %qf5, %qf7;
fma.rn.f32 %qf7, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf7;
sub.f32 %qf4, %f178, %f179;
fma.rn.f32 %qf4, %qf5, %qf4, %f179;
sub.f32 %qf6, %qf6, 0f3E800000;
mul.sat.f32 %qf6, %qf6, 0f3F000000;
fma.rn.f32 %qf5, %qf6, 0fC0000000, 0f40400000;
mul.f32 %qf6, %qf6, %qf6;
mul.f32 %qf6, %qf6, %qf5;
sub.f32 %qf4, %qf4, %f177;
fma.rn.f32 %qf4, %qf6, %qf4, %f177;
MFGUNLOCK_AXIS_DIRECTION_DONE:
)PTX");
    program += direction == 0 ? "mov.f32 %f180, %qf4;\n"
                              : "mov.f32 %f181, %qf4;\n";
  }
  return program;
}

inline constexpr const char* kCandidateArbitration = R"PTX(
// MFGUNLOCK_RELATIVE_CANDIDATE_ARBITRATION_V3
sub.f32 %qf2, %f176, 0f3DCCCCCD;
mul.sat.f32 %qf2, %qf2, 0f40A00000;
@!%qv3 mov.f32 %qf2, 0f00000000;
setp.le.f32 %qv2, %qf2, 0f00000000;
@%qv2 bra MFGUNLOCK_RELATIVE_CANDIDATE_ARBITRATION_DONE_V3;
sub.f32 %qf3, %qf0, %qf1;
abs.f32 %qf4, %qf3;
mul.sat.f32 %qf2, %qf2, %qf4;
fma.rn.f32 %qf4, %qf2, 0fC0000000, 0f40400000;
mul.f32 %qf2, %qf2, %qf2;
mul.f32 %qf2, %qf2, %qf4;
fma.rn.f32 %qf4, %qf2, 0fBF400000, 0f3F800000;
setp.ge.f32 %qv2, %qf3, 0f00000000;
selp.f32 %qf2, %qf1, %qf0, %qv2;
selp.f32 %qf3, %f175, %f174, %qv2;
sub.f32 %qf2, %qf2, %qf3;
fma.rn.f32 %qf2, %qf4, %qf2, %qf3;
@%qv2 mov.f32 %qf1, %qf2;
@!%qv2 mov.f32 %qf0, %qf2;
MFGUNLOCK_RELATIVE_CANDIDATE_ARBITRATION_DONE_V3:
setp.gt.f32 %qv5, %qf0, 0f00000000;
and.pred %qv0, %qv0, %qv5;
setp.gt.f32 %qv6, %qf1, 0f00000000;
and.pred %qv1, %qv1, %qv6;
)PTX";

struct Rgb {
  float r;
  float g;
  float b;
};

inline float Clamp01(float value) {
  if (!(value > 0.0f)) return 0.0f;
  return value < 1.0f ? value : 1.0f;
}

inline float RelativePhotometricError(Rgb first, Rgb second) {
  if (!std::isfinite(first.r) || !std::isfinite(first.g) ||
      !std::isfinite(first.b) || !std::isfinite(second.r) ||
      !std::isfinite(second.g) || !std::isfinite(second.b)) {
    return std::numeric_limits<float>::infinity();
  }
  const float first_y =
      0.2126f * first.r + 0.7152f * first.g + 0.0722f * first.b;
  const float second_y =
      0.2126f * second.r + 0.7152f * second.g + 0.0722f * second.b;
  const float scale =
      std::max(0.0625f, 0.5f * (std::abs(first_y) + std::abs(second_y)));
  const float dr = first.r - second.r;
  const float dg = first.g - second.g;
  const float db = first.b - second.b;
  const float luma = std::abs(first_y - second_y) / scale;
  const float co = std::abs(dr - db);
  const float cg = std::abs(dg - 0.5f * (dr + db));
  return std::max(luma, 0.5f * std::max(co, cg) / scale);
}

inline float WarpConfidence(float base_error, float candidate_error,
                            float native_weight, bool both_valid) {
  float agreement = std::min(
      Clamp01((base_error - candidate_error - 0.10f) * 4.0f),
      Clamp01((0.30f - candidate_error) / 0.22f));
  if (!both_valid) agreement = 0.0f;
  const float native = std::min(
      Clamp01((base_error - 0.50f) * 2.0f),
      Clamp01((native_weight - 0.20f) / 0.30f));
  float confidence = std::max(agreement, native);
  if (!both_valid) {
    float unilateral = Clamp01((native_weight - 0.50f) * 4.0f);
    unilateral = unilateral * unilateral * (3.0f - 2.0f * unilateral);
    confidence *= unilateral;
  }
  return confidence;
}

inline float EffectiveWarpWeight(float native_weight, float target_weight,
                                 float confidence) {
  native_weight = Clamp01(native_weight);
  target_weight = std::max(native_weight, Clamp01(target_weight));
  confidence = Clamp01(confidence);
  confidence = confidence * confidence * (3.0f - 2.0f * confidence);
  return native_weight + confidence * (target_weight - native_weight);
}

inline float TaperAddedWeight(float native_weight, float adapted_weight,
                              float taper) {
  native_weight = Clamp01(native_weight);
  adapted_weight = std::max(native_weight, Clamp01(adapted_weight));
  taper = Clamp01(taper);
  return native_weight + taper * (adapted_weight - native_weight);
}

inline float ArbitrateExtraWeight(float native, float weaker,
                                  float candidate_error, float dominance) {
  native = Clamp01(native);
  weaker = Clamp01(weaker);
  float confidence =
      Clamp01((candidate_error - 0.10f) * 5.0f) * Clamp01(dominance);
  confidence = confidence * confidence * (3.0f - 2.0f * confidence);
  const float attenuation = 1.0f - 0.75f * confidence;
  return std::max(native, native + attenuation * (weaker - native));
}

inline float SymmetricBorderDistance(float candidate_u, float candidate_v,
                                     float width, float height) {
  return std::min(std::min(candidate_u, 1.0f - candidate_u) * width,
                  std::min(candidate_v, 1.0f - candidate_v) * height);
}

inline float DirectionalBorderDistance(float current_u, float current_v,
                                       float candidate_u, float candidate_v,
                                       float width, float height) {
  const float dx = (candidate_u - current_u) * width;
  const float dy = (candidate_v - current_v) * height;
  if (dx * dx + dy * dy < 0.25f) {
    return SymmetricBorderDistance(candidate_u, candidate_v, width, height);
  }
  const bool horizontal = std::abs(dx) >= std::abs(dy);
  const float candidate = horizontal ? candidate_u : candidate_v;
  const float current = horizontal ? current_u : current_v;
  const float extent = horizontal ? width : height;
  const bool low_edge = 0.5f * (candidate + current) <= 0.5f;
  const float candidate_distance =
      (low_edge ? candidate : 1.0f - candidate) * extent;
  const float current_distance =
      (low_edge ? current : 1.0f - current) * extent;
  const bool entering = current_distance > candidate_distance;
  const float nearest = std::min(current_distance, candidate_distance);
  const float dominant = entering
                             ? std::min(std::max(current_distance,
                                                 candidate_distance),
                                        nearest + 1.0f)
                             : nearest;
  const float cross_current = horizontal ? current_v : current_u;
  const float cross_candidate = horizontal ? candidate_v : candidate_u;
  const float cross_extent = horizontal ? height : width;
  const float cross_distance =
      std::min(std::min(cross_current, 1.0f - cross_current),
               std::min(cross_candidate, 1.0f - cross_candidate)) *
      cross_extent;
  return std::min(dominant, cross_distance);
}

inline float BorderConfidence(float distance_pixels) {
  const float value = Clamp01((distance_pixels - 0.5f) * 0.5f);
  return value * value * (3.0f - 2.0f * value);
}

inline float ContinuousDirectionalBorderDistance(float current_u, float current_v,
    float candidate_u, float candidate_v, float width, float height) {
  const float symmetric = SymmetricBorderDistance(candidate_u, candidate_v, width, height);
  const float directional = DirectionalBorderDistance(current_u, current_v,
      candidate_u, candidate_v, width, height);
  const float dx = (candidate_u - current_u) * width;
  const float dy = (candidate_v - current_v) * height;
  float weight = Clamp01((dx * dx + dy * dy - 0.25f) * 0.5f);
  weight = weight * weight * (3.0f - 2.0f * weight);
  return symmetric + weight * (directional - symmetric);
}

// Scalar oracle for the candidate's axis interpolation. No hard-validity gate
// is changed: this helper is used only after the provider's structural checks.
inline float BorderAxisDistance(float current_u, float current_v,
                               float candidate_u, float candidate_v,
                               float width, float height, bool horizontal) {
  const float current = horizontal ? current_u : current_v;
  const float candidate = horizontal ? candidate_u : candidate_v;
  const float extent = horizontal ? width : height;
  const bool low_edge = current + candidate <= 1.0f;
  const float current_distance = (low_edge ? current : 1.0f - current) * extent;
  const float candidate_distance = (low_edge ? candidate : 1.0f - candidate) * extent;
  const float nearest = std::min(current_distance, candidate_distance);
  const float along_axis = current_distance > candidate_distance
      ? std::min(std::max(current_distance, candidate_distance), nearest + 1.0f)
      : nearest;
  const float cross_current = horizontal ? current_v : current_u;
  const float cross_candidate = horizontal ? candidate_v : candidate_u;
  const float cross_extent = horizontal ? height : width;
  const float cross_distance = std::min(
      std::min(cross_current, 1.0f - cross_current),
      std::min(cross_candidate, 1.0f - cross_candidate)) * cross_extent;
  return std::min(along_axis, cross_distance);
}

inline float ContinuousAxisBorderDistance(float current_u, float current_v,
    float candidate_u, float candidate_v, float width, float height) {
  const float symmetric = SymmetricBorderDistance(candidate_u, candidate_v, width, height);
  const float dx = (candidate_u - current_u) * width;
  const float dy = (candidate_v - current_v) * height;
  const float motion_squared = dx * dx + dy * dy;
  if (motion_squared <= 0.25f) return symmetric;
  float horizontal_weight = Clamp01(dx * dx / motion_squared);
  horizontal_weight = horizontal_weight * horizontal_weight * (3.0f - 2.0f * horizontal_weight);
  const float horizontal = BorderAxisDistance(current_u, current_v, candidate_u,
      candidate_v, width, height, true);
  const float vertical = BorderAxisDistance(current_u, current_v, candidate_u,
      candidate_v, width, height, false);
  const float directional = vertical + horizontal_weight * (horizontal - vertical);
  float motion_weight = Clamp01((motion_squared - 0.25f) * 0.5f);
  motion_weight = motion_weight * motion_weight * (3.0f - 2.0f * motion_weight);
  return symmetric + motion_weight * (directional - symmetric);
}

inline float OrientationWeight(float motion_x, float motion_y,
                               float direction_x, float direction_y) {
  const float motion_length =
      std::sqrt(motion_x * motion_x + motion_y * motion_y);
  const float direction_length =
      std::sqrt(direction_x * direction_x + direction_y * direction_y);
  if (!(motion_length > 0.5f) || !(direction_length > 0.0f)) return 1.0f;
  const float alignment = std::min(
      1.0f, std::abs(motion_x * direction_x + motion_y * direction_y) /
                (motion_length * direction_length));
  float orientation = Clamp01(motion_length - 0.5f);
  orientation = orientation * orientation * (3.0f - 2.0f * orientation);
  return 1.0f - 0.25f * orientation * (1.0f - alignment);
}

inline float DiagonalAmbiguityWeight(float second_support) {
  float value = Clamp01((second_support - 0.35f) * 4.0f);
  value = value * value * (3.0f - 2.0f * value);
  return 1.0f - value;
}

inline float DiagonalDirectionWeight(float motion_x, float motion_y) {
  const float x = std::abs(motion_x);
  const float y = std::abs(motion_y);
  const float largest = std::max(x, y);
  const float length = std::sqrt(motion_x * motion_x + motion_y * motion_y);
  if (!(length > 0.5f) || !(largest > 0.0f)) return 0.0f;
  float value = Clamp01((std::min(x, y) / largest - 0.30f) / 0.30f);
  value = value * value * (3.0f - 2.0f * value);
  // Match qgf11 in the generated PTX: Euclidean length, not max component.
  float orientation = Clamp01(length - 0.5f);
  orientation = orientation * orientation * (3.0f - 2.0f * orientation);
  return value * orientation;
}

inline float StableGeometryRelaxation(float best_support,
                                      float second_support) {
  best_support = Clamp01(best_support);
  second_support = Clamp01(second_support);
  best_support = best_support * best_support * (3.0f - 2.0f * best_support);
  second_support =
      second_support * second_support * (3.0f - 2.0f * second_support);
  return best_support * (0.125f + 0.375f * second_support);
}

}  // namespace mfgunlock::adaptivequalityv3
