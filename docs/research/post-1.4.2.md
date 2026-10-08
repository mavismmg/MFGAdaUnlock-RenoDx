# Post-1.4.2 quality and performance research

Baseline: `1.4.2` / `3a2c021d5882ffd62e2872d84ed752398be58f58`, 6 October 2026.
The continuous-axis path was promoted to release 1.4.3 following initial positive
in-game feedback. This work retains the 1.4.2 control, an isolated candidate,
reproducible assembly reports, experiments and a capture matrix. Comparative
visual/performance measurements remain pending.

## Border-axis candidate

The control switches its dominant axis at `abs(dx) >= abs(dy)`. Near an edge,
this can change the border distance abruptly. The candidate evaluates both axis
treatments, each retaining the opposite-edge/corner restriction, and uses
`smoothstep(dx² / (dx² + dy²))` to interpolate them. It keeps the control's
0.5–1.5 px squared-motion ramp, the one-pixel entry allowance, native warp anchor
and structural invalidity gates. Stationary and cardinal endpoints are preserved.

The official `mfgunlock_local_low_overhead` target enables
`MFGUNLOCK_CONTINUOUS_BORDER_AXES` and `MFGUNLOCK_RELEASE_1_4_3`. The axis path
requires both local release flags and adds no public INI setting. The control
target is `mfgunlock_1_4_2_control`; the separately identified candidate remains
`mfgunlock_border_axis_candidate`. Existing saved choices remain unchanged. No history, buffers, texture reads, inpaint, scatter, motion-vector
processing, Reflex, pacing or provider-profile changes are introduced.

Tests cover four edges/corners, axis transposition, portrait/landscape resolutions,
subpixel/magnitude endpoints, direction sweeps, forward/inverse movement and
native-anchor preservation. The regression uses 44.99°/45.01° because normalized
FP32 viewport coordinates can quantize the earlier double-precision 44.9999° pair.

## Build and provenance

Configure `tests/CMakeLists.txt` against an existing RenoDX dependency checkout.
Build the emitters `local_stability_warp_tests`, `border_axis_warp_tests` and
`border_axis_tests` before generating the private table. Use `release_warp_tests`
for the promoted release and the `release-1.4.3` generator/audit variant. Do not commit any
`*.generated.hpp` or locally extracted provider PTX/cubin/SASS.

For each variant, run:

```text
python tools/build_local_stability.py --variant <control|border-axis-candidate|release-1.4.3>
  --provider <exact-provider> --baseline <approved-original-table>
  --header src/addons/mfgunlock/thin_geometry_stability.generated.hpp
  --emitter <matching-warp-emitter.exe> --ptxas <ptxas.exe>
  --nvdisasm <nvdisasm.exe> --output <variant-validation-directory>
```

Arguments above are shown on separate lines for readability; use the shell's
line continuation when running them. The script rejects the wrong emitter
identity, verifies unchanged resource-access instructions and records provider,
table, emitter, warp/rewrite source and compiler identities. Control/candidate
geometry tables are identical; warp is rewritten from the selected C++ source.

Build the selected addon target with `--config Release`, then audit it using
`tests/audit_local_low_overhead.ps1 -AddonPath <file> -Variant <variant>`.
The binary name, overlay, copied diagnostics and log distinguish the candidate.
Only one MFG Unlock addon should be installed during a test. Restart between
replacements; never install this entire build directory into a game.

The adjacent `.manifest.json` records binary and source hashes plus identities
of private payload inputs. Attach a matching assembly report with:

```text
python tools/write_build_manifest.py --source-root . --addon <addon-file>
  --variant <variant> --validation <variant-validation-directory>/validation.json
```

A wrong variant, stale warp/rewrite source or different table is rejected.
Generation does not prove that a game installed/activated a kernel; retain the
runtime diagnostics alongside the manifest. Source changes after a build require
rebuilding it and regenerating its manifest.

## Offline experiments and measured checks

`tools/validate_border_axis.py --emitter <border_axis_tests.exe> --output <directory>`
executes the actual emitted fragment on CUDA against its C++ FP32 oracle. It
writes the inputs, harness and a machine-readable correctness report. These
GPU launches occur only in this developer tool, not in the addon.

`tools/prepare_quality_experiments.py --control <control-validation-directory>
--output <directory> --ptxas <ptxas.exe> --nvdisasm <nvdisasm.exe>` prepares:

- A stricter candidate-agreement interval (0.06–0.24 instead of 0.08–0.30),
  leaving other eligibility ramps intact.
- Half of the addon-added warp contribution, gated by structural validity and
  retaining the native anchor.
- Individual constant-divisor experiments for 1, 4 and 3; FTZ is retained.

They start from the validated control, never the border candidate. Outputs are
**offline research artifacts**, not installable addons or promoted settings.
The script requires exact unique anchors, preserves resource accesses, assembles
with a 48-register zero-spill gate and reports SASS/cubin equality. An installable
quality candidate needs a separately reviewed compile-time path and actual game
captures; these weights are not silently enabled.

Local validation on the installed 310.9.1 provider and CUDA ptxas 13.3:

| Check | Control | Border candidate |
|---|---:|---:|
| Warp registers | 48 | 48 |
| Warp .text bytes | 17,024 | 17,152 |
| Warp shared bytes | 288 | 288 |
| Stack / spills / local | 0 / 0 / 0 | 0 / 0 / 0 |
| Geometry registers / shared bytes | 40 / 7,776 | 40 / 7,776 |

The border GPU/oracle check passed 25,796 cases in two directions with maximum
error about 3.62e-7 px. The new warp adds 128 bytes of code; this is not a frame-time
benchmark. Assembly and driver JIT both respected 48 registers. The agreement
experiment assembled at the control's instruction count; halving extra weight
added eight instructions. Visual benefit is unmeasured.

All three divisor experiments produced the **same complete cubin as the control**
on this toolchain: the compiler already simplified those constants. Therefore
there is no demonstrated extra throughput benefit, and no arithmetic rewrite
enters the addon. Equal cubins establish equal generated operations here,
including special-value handling; this is not a claim about other compilers.

## HDR, motion inputs and game validation

Generate `tools/quality_capture_matrix.py --output <matrix.json>`. Its 42 core
runs cover Onimusha, STALKER 2, Cyberpunk 2077, Hogwarts Legacy and Indiana Jones,
three repetitions per control/candidate condition. `--tier all` expands fixed
2x/4x/6x, HDR and Dynamic cases and includes exploratory Witcher 3. Skip and record
unsupported modes instead of forcing them. Dynamic must be evaluated separately.

Use the existing `Capture-Performance.ps1` with `-DurationSeconds 45` and the
matrix job ID as label; collect PresentMon summaries, NVAPI snapshots and loaded
module inventories. Freeze a warmed save/camera route, resolution, SR preset,
cap, VSync/VRR, driver, DLL hashes and quality/UI modes. Close the overlay and
remove the diagnostic companion for performance runs. Match visual footage for
45° direction crossings, slow pans, corners, wires, vegetation/hair, shadows,
particles, HUD and newly revealed background.

For input investigation, capture a **separate** bounded diagnostic run and use
`Compare-HdrCaptures.ps1`. Inspect actual subrects/extents, tag lifetimes, alpha,
premultiplication, exposure, motion scale/jitter/dilation and camera matrices.
Identify whether HDR uses PQ or scRGB. Metadata/format cannot prove pixel encoding
or alpha content: use a targeted GPU/API capture where needed. Do not tune a
photometric threshold or relax optional-resource admission from metadata alone.

Temporal history and reconstruction remain research only. For the Witcher 3
vegetation/hair case, first distinguish absent deformation motion, rejected
candidates and native fallback; generic kernel tuning cannot recover missing
inputs. A future temporal candidate must prove activation, phase/resource
identity, reset behavior, bounded allocation and synchronization cost, then show
repeatable benefit against the local candidate. Prior activation failures are
not resolved by re-enabling the old backend.

Report display/source p95/p99, cadence, generated-frame GPU cost, VRAM and available
Reflex telemetry. Keep missing metrics missing; application telemetry is not
input-to-photon latency. The matrix's `--capture-root` inventory reports evidence
presence only and never promotes a candidate. Promotion requires repeatable
visual improvement with performance inside repeated-control variability; expensive
paths additionally require an explicit gain/cost comparison.
