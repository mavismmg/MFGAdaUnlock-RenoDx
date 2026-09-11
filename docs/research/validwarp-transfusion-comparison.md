# Published DLSSG-Transfusion comparison — Stage 3 supplement

Date: 2026-09-11. Addon baseline: `develop` at `6fac842`.

This supplements, and does not replace, the [independent Stage 3 investigation](validwarp-stage3.md). The original report and evidence file are preserved byte-for-byte. This stage analyzes the user-supplied Tony Joaca/DLSSG-Transfusion package; it does not implement Stage 4.

## Finding

The published `qualityValidWarp` has a concrete mechanism different from the scatter thresholds found independently. It changes **how existing warped color candidates are mixed with unwarped color** in `Kernel_BlendCandidatesFused`. Our candidates change **whether motion candidates survive scatter rejection** in `Kernel_EstimatePrev2CurrScatter` and `Kernel_EstimateIntermMvecsScatter`.

They can potentially complement each other, but their effects are coupled. Relaxing motion rejection and then giving the resulting candidate full weight can magnify a wrong correspondence. The evidence supports investigating both, with separate controls and measurements; it does not establish that enabling both is automatically better.

The published implementation also retargets Blackwell PTX for the blend kernel. This is an additional difference from our existing three-kernel port and must be isolated when comparing image quality.

## Material and provenance

Inspected all five supplied binaries, `README.txt`, and `DLSSG-Transfusion.json`. The README identifies v1.4.0; the ASI also contains the build label `v1.4.0-pure-warp`. Full package hashes and selected static evidence are recorded in [the comparison manifest](validwarp-transfusion-evidence.json).

Representative sample: `DLSSG-Transfusion.asi`, SHA-256 `835bfc97b1e3809ee67b5d9e2fbebed5dfa9cd46de8365e63002c1cc08379b94`.

The embedded quality PTX template is identical across the ASI and all four proxy DLLs. The quality-builder function bytes are identical in the ASI, `dinput8.dll`, `dxgi.dll`, and `version.dll`; `winmm.dll` has a different function-byte hash and was not separately disassembled. The detailed control-flow conclusions below refer to the ASI.

No binary was executed or installed into a game. Analysis used file reads, PE/x64 disassembly, original NVIDIA fatbin decompression, and finite-value algebra checks. No NVIDIA or third-party patch payload is added to this repository. Raw extracts and helper scripts remain in the local research workspace.

The supplied package contains no source tree or license file, and its README does not establish reuse terms. Credit for the published blend technique belongs to Tony Joaca, the DLSSG-Transfusion author; source provenance and permission/license should be established before directly incorporating its code. The independent scatter investigation remains separately documented. Downloadable binaries alone do not establish an open-source license.

## Verified application path

Static ASI references connect the option to the patch, rather than merely finding an unused string:

| Evidence | ASI location / observation |
| --- | --- |
| Config name | `qualityValidWarp` at RVA `0x56d90`; parser also recognizes `qualityFix` |
| Config-to-patch bridge | Setter at RVA `0x1d1a0` writes the byte at `0x754a0` |
| Named target and enabled check | Around `0x19150`–`0x19192`, checks `Kernel_BlendCandidatesFused` and the enabled byte before calling the quality builder |
| Quality builder | RVA `0x1aac0`, PE unwind end `0x1b2b5` |
| Exact input gate | CR-stripped, trailing-NUL-trimmed PTX length `39638`, FNV-1a64 `0x7a6f5f41105c6d85` |
| Insertion | Adds registers and inserts the quality operations before the native UIR flag load at parameter `+220` |
| Output path | Retargets Blackwell PTX to Ada, emits an uncompressed PTX entry and a shortened fatbin; the stock Ada cubin after that entry is not retained in the rebuilt payload |

The surrounding path allocates replacement data and redirects provider fatbin references in process memory. This is a PTX/memory-patching technique, not a public NGX/NVAPI quality parameter. These RVAs describe this ASI hash only and are not implementation instructions for our addon.

A key detail: the readable template initially contains a warp-weight floor of **0.85**. The x64 builder replaces both occurrences with **1.0** before inserting the code. An analysis based only on the embedded template would miss this change. On accepted finite candidates, the effective path uses full warped RGB weight. The accompanying log label describing 100% warp agrees with this transformation.

### Version evidence

Raw decompression reproduced the builder's exact length and FNV checks for our 310.9.0 and 310.9.1 Blackwell blend PTX. Their normalized source SHA-256 is `14d2c4b5e54e04c2865664b8019e53cdea0a847b6bdd5ef82265d5f567533e9d`.

The sampled 310.6.0, 310.7.0, and 310.7.129 blend PTX does not pass that exact gate. Those samples have length `39542` and FNV `0x1ddb45ec587d4411`, with older entry naming. Stage 3 found corresponding instruction text equivalent after entry-name normalization, but that is not the transformation performed by this exact quality gate. The inspected path also checks the newer descriptive kernel name.

Thus, the package's broad general MFG version claim must not be treated as proof that this quality patch activates on all those versions. No live activation was tested here, including on the two statically matching versions.

The original raw PTX was decompressed to reproduce this result. `cuobjdump`'s pretty-printed output changes source formatting and initially produced different hashes; it must not be used directly to emulate this gate.

## What the published patch actually does

In the original blend kernel, two input textures supply unwarped RGB values `U0/U1`. The same textures are sampled at motion-shifted coordinates to obtain `W0/W1`. Two sampled channels supply blend weights `c0/c1`. The native path uses those weights when mixing candidates. The semantic name “confidence” is convenient here; these are the observed blend-control values, not a proven calibrated probability.

For finite ordinary values, the published logic can be summarized with:

```text
E_static = sum_RGB(abs(U0 - U1))
E_motion = sum_RGB(abs(W0 - W1))

mutual = both candidates pass validity checks
         AND E_motion + 0.08 < E_static
         AND E_motion < 0.15

changing_pixel = E_static > 0.25

accept_i = candidate_i passes validity checks
           AND ((c_i >= 0.20 AND changing_pixel) OR mutual OR UIR_enabled)
```

Validity checks require the motion sentinel to be clear, warped coordinates to lie within a half-texel inset of the image, and finite warped RGB as checked by the sum of absolute components. These are useful checks, but they do not prove a motion vector points to the correct physical object.

For accepted candidates, the effective weight is clamped with a **1.0 floor and 1.0 ceiling**, and RGB is reconstructed from the warped sample. The native UIR operations subsequently run, because insertion is before their flag load.

Additional asymmetric rules copy one processed candidate to the other when its source weight is at least `0.25`, the opposite weight is below `0.08`, the source is accepted, and `E_static > 0.25`. This can fill an unreliable side of an occlusion transition. There is no added depth-ordering or foreground/background identity test in the injected snippet, so describing every such copy as a verified background recovery would overstate the evidence.

PTX ordered comparisons reject NaN operands; the report's simplified algebra assumes finite inputs and does not replace exact floating-point semantics. [NVIDIA PTX comparison instruction reference](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html#comparison-and-selection-instructions-setp)

### Important interpretation limits

- The `0.25` change test measures RGB difference at the same pixel between source images. It is not elapsed frame time or the MFG interpolation position.
- “Mutual consistency” here is agreement of sampled colors, not a forward/backward motion-vector round-trip test. Repeated fence patterns or similarly colored surfaces can agree while representing different objects.
- A stationary translucent HUD can have a changing composite pixel when the world behind it moves. `E_static > 0.25` is therefore a heuristic, not reliable semantic HUD detection.
- The UIR flag bypasses the confidence/change/mutual tests for the primary boost. Sentinel, coordinate and finite-RGB checks still apply. The snippet itself does not validate the supplied HUD-less/UI pair's color-space contract.
- The primary boost overwrites RGB while the candidate's fourth auxiliary component remains on the native path; asymmetric copying includes that component. Its downstream meaning and any consistency implications need verification before redesigning the payload. It must not be casually labeled UI alpha.
- The fixed color thresholds operate on the provider's input intermediates. Their precise scaling and transfer function in HDR are not established by these checks; they should not be assumed to represent nits or normalized display-linear values.
- Existing samples and registers are reused; the injected operations add arithmetic without extra texture-sampling instructions in this snippet. That does not prove zero GPU cost: register pressure, instruction count, occupancy and downstream work still matter.

The README's claims of completely artifact-free interpolation, zero HUD ghosting and a particular GPU cost are author claims, not results reproduced in this investigation.

## Comparison with our findings

| Mechanism | Point of intervention | Potential benefit | Main interaction/risk |
| --- | --- | --- | --- |
| Existing Blackwell/midpoint paths | Intermediate motion and selected fill kernels | Preserve corrected temporal placement and existing quality | Established baseline must remain separately identifiable |
| Our candidate A: previous scatter `+60` | Rejection before candidate use | Preserve locally inconsistent motion that may belong to thin geometry | Can also preserve incorrect motion across a real boundary |
| Our candidate B: intermediate scatter `+120` | Combined motion/depth rejection | Retain additional intermediate motion candidates | Same kernel as our temporal/Blackwell path; requires exact payload validation |
| Tony Joaca's blend technique | Weighting of surviving warped RGB samples | Reduce mixing of a moving fine feature with unrelated unwarped color | Incorrect accepted correspondence can receive full weight |
| Tony Joaca's asymmetric copy | Transfer between strong/weak candidate sides | Reduce broken gaps in some occlusion transitions | May stretch foreground or copy a wrong surface without depth validation |
| Our Quality Guard/UI handling | Optional resource selection and transition management | Avoid known invalid HUD/color separation inputs | Metadata validation does not prove every pixel is correct; Native remains useful |

The published code even emits a “stock scatter rejection” diagnostic for its intermediate scatter path. In the traced quality routine there is no change to our `+60`/`+120` divisors. This supports keeping the independent findings: the new evidence identifies another intervention point, rather than invalidating them.

If both motion candidates are rejected upstream, the blend-only acceptance checks cannot recover their missing motion from color agreement alone. That is a concrete reason to investigate our scatter candidate. Conversely, keeping more vectors does not ensure the blend kernel will give their warped colors useful weight. That is why the published approach is relevant to our work.

## Offline predicate checks

Six illustrative cases were evaluated as finite-value algebra. They are not game captures, GPU tests or visual-quality measurements. Inputs and results are included in the manifest.

| Case | Result |
| --- | --- |
| Warped RGB agrees, unwarped RGB differs, low original weights | Both candidates can receive full warp through the mutual test |
| Low source-image change without UIR | Primary boost remains inactive in the constructed case |
| Same low-change case with UIR and very low weights | Both geometrically eligible candidates receive the boost |
| Values consistent with a translucent HUD over changing background | Primary boost can activate even though mutual agreement fails |
| Strong source and invalid/weak destination | Asymmetric copy can activate from the accepted side |
| Both vectors invalid despite matching warped colors | Neither primary boost nor asymmetric copy activates |

These results identify limits of the rules. They neither disprove the author's visual improvements nor establish how often such inputs occur in a game.

## What could improve our image quality

The best next comparison is a blend-stage experiment with the existing scatter unchanged, followed by our independent scatter experiment, then their combination. This reveals whether remaining artifacts come mainly from missing motion candidates or from insufficient use of candidates that already exist.

Promising refinements to investigate, not yet validated or implemented:

1. **A gradual confidence increase.** Compare a bounded, continuous increase toward warped RGB against full 100% selection. A hard switch at a color/confidence threshold can change abruptly as a small feature moves. A continuous rule could reduce that instability without adding frame-history buffers; excessive blending can also reintroduce the original tearing, so it needs A/B evidence.
2. **Stronger checks before asymmetric copying.** Retain the option, but test it separately. Where existing resources allow it, verify depth/occlusion direction or vector consistency before copying across a boundary. Do not add new depth sampling blindly; mapping, units and GPU cost must first be established.
3. **Treat UIR as validated state.** Reuse our resource-pair/HDR findings when deciding whether a clean scene path is trustworthy. A UIR option bit alone is not evidence of compatible buffers. Preserve Native mode and the user's choice; this does not justify enabling Automatic Guard everywhere.
4. **Conservative border behavior.** Retain out-of-bounds protection and inspect transitions near the valid sampling footprint. A gradual reduction in trust near that boundary is a candidate to compare. No combination reconstructs reliable off-screen information that neither source frame contains.
5. **Bounded scatter relaxation combined with color evidence.** First measure A and B independently. If extra surviving candidates help, combine one with the blend rule and check whether newly retained wrong vectors are being overtrusted. Do not simultaneously change the shared host constant, both scatter fields and blend confidence.
6. **Verify the color domain before adapting thresholds.** Relative/local-contrast criteria might transfer better than fixed RGB limits, but only after identifying the relevant intermediate representation. Do not impose a generic HDR clamp or assume scene-linear values.

These refinements can use existing inputs where possible. Any new texture reads, masks or temporal buffers require a separate cost assessment. Better quality without changing Present/Reflex is plausible; unchanged frame pacing still has to be measured.

## Revised Stage 4/5 comparison proposal

Keep each effect independently identifiable:

| Variant | Purpose |
| --- | --- |
| Existing addon baseline | Preserve the known-good pacing and current quality modes |
| Blackwell blend kernel only | Separate the additional architecture port from the quality rule |
| Blend quality rule with stock scatter | Isolate the published mechanism, with proper provenance |
| Our candidate A only; then B only | Measure independent scatter findings |
| Best blend variant plus best bounded scatter variant | Test complementary benefit and compounded errors |
| Asymmetric copy off/on | Determine whether apparent improvement introduces stretching or ghost trails |

Use fixed 3x/4x first, identical scene/camera/resolution/HDR/runtime/settings, with Dynamic disabled for that controlled comparison. Preserve the working temporal program and source FPS cap interpretation. Measure presented intervals and source GPU time in addition to final FPS. Then test Dynamic and UIR/HDR combinations separately.

Suggested visual targets remain fences, wires, foliage, weapon/character silhouettes, screen borders, disocclusions, transparent HUD and bright/specular surfaces. A gain in vegetation accompanied by worse HUD or pacing is a mixed result, not a universal improvement.

Our exact three-cubin replacement infrastructure cannot automatically be extended to the blend kernel: replacement size, registration, parameter layout and fallback must be verified first. Tony Joaca's reconstructed-fatbin/JIT method and our current in-place cubin replacement have different integration risks. The Stage 4 implementation method remains undecided.

## Handoff

Static confidence is high for the named target, predicates, 100% effective floor, insertion point and 310.9.x gate matches. Actual game benefits, GPU cost, combined scatter/blend behavior, runtime activation and cross-backend compatibility remain untested.

The original research has been preserved. This comparison adds documentation and sanitized evidence only. No production code, installed addon, configuration, release, branch target or existing credits/license were changed. Stage 4 still requires its own authorization.
