# How it works, validation and building

Part of the [MFG Unlock](../README.md) documentation. Sections: [How it works](#how-it-works), [Release Validation](#release-validation), [RTX 20 and 30 GPUs](#rtx-20-and-30-gpus), [Building](#building).

## How it works

Three gates decide whether multi-frame generation is available, and the addon
opens the two that matter:

1. `nvngx_dlssg.dll` exports `NVSDK_NGX_GetGPUArchitecture` as a hardcoded
   minimum architecture — `mov eax, 0x190` (Ada). A 40-series card already clears
   this, so it is left alone.
2. `DLSSGInstanceManager::PopulateParameters` compares the NVAPI arch id against
   `0x1b0` (Blackwell) to decide whether to advertise a max frame count of 5 or 1.
3. A second compare against the same constant feeds a runtime capability flag
   that drives generation itself.

Patching (2) without (3) makes the options appear and then render black. The
addon rewrites both compares, in both encodings, in memory only — NGX verifies
the snippet's Authenticode signature at load time, so the same bytes changed on
disk make frame generation disappear entirely.

Unlocking the count alone is not enough. The Ada interpolation kernel blends
with a compiled-in `0.5`, so every generated frame lands at the temporal
midpoint: 4x produces three identical half-way frames, the counter doubles and
the motion does not get smoother.

The experimental full-kernel path uses the Blackwell motion-vector, inpaint,
and inpaint-decision programs rebuilt for Ada. Each target is accepted only
when the original ELF fingerprint **and exact fatbin slot size** match the
locally generated compatibility table. The replacement cubin is then written
inside that original slot in mapped process memory; fatbin headers, entry
descriptors, registration metadata, surrounding provider data, and pacing code
are left untouched. The Blackwell motion-vector program consumes the generated
frame's temporal parameter natively, so it replaces rather than stacks with the
older midpoint rewrite.

The source repository does not store generated cubin tables. They are produced
locally from installed NVIDIA providers during release preparation and excluded
from source control. No complete NVIDIA DLL or provider package is included.

If the complete Blackwell path cannot be identified unambiguously, the addon
fails closed to the release-0.7 behavior: it decompresses the supported Ada
kernel's PTX, rewrites the blend weight to use the temporal parameter, and lets
the driver JIT the corrected version. The overlay reports which path applied.
Changing **Prefer full Blackwell framework kernels** requires a game restart.

When enabled, **Intermediate scatter retention** selects an exact-fingerprint
variant of the Blackwell intermediate motion-vector kernel. It relaxes only the
identified motion-consistency input and retains the separate depth test.
**Validated warp blend** operates later through a separately validated PTX
fatbin redirect. Its rebuilt fatbin preserves all original entries around the
modified program. Both paths modify mapped process memory only, validate the
provider and original payload exactly, and fall back without patching when any
identity or layout check fails.

DLSS-G owns frame generation and presentation pacing; this addon does not
implement a separate frame scheduler or issue generated-frame presents. With
current Streamline builds, pacing is normally left to the runtime. The optional
legacy compatibility setting disables the plugin's flip-metering path and
forces its existing software fallback only when higher multipliers otherwise
freeze presentation.

Each source file documents its own area in detail — start with the header comment
in [`addon.cpp`](../src/addons/mfgunlock/addon.cpp).

## Release Validation

Release 1.4.3 is checked with MSVC Release builds, the automated suite, CUDA
assembly/driver JIT, a synthetic GPU oracle and the final PE audit. Initial
in-game feedback was positive; comparative 1.4.2/1.4.3 performance measurements
remain pending. The field traces below document earlier versions and are not
measurements of the new border-axis path.

> **STALKER 2 frame-pacing validation (September 10, 2026):** one 45-second
> release-gate run used DLSS-G 310.9.1, Streamline 2.14.1, Dynamic MFG with
> VSync, and a 240 Hz display. PresentMon recorded Hardware: Independent Flip,
> 9,316 display intervals at 4.450 ms median, 5.637 ms p95 and 7.953 ms p99
> (224.48 FPS average). The generated/source cadence heuristic measured 3.986x,
> consistent with the active 4x mode. Of 2,337 source intervals, four exceeded
> the robust 31.662 ms outlier threshold.

This is a single release-gate trace, not a cross-version performance benchmark.
It validates the active 4x presentation path and does not claim zero game-side
stutter, universal compatibility, or an addon-overhead difference.

> **Onimusha: Way of the Sword thin-geometry validation (September 11, 2026):**
> a 45-second release-candidate run used an RTX 4070 SUPER, D3D12, Streamline
> 2.10.3, a mapped DLSS-G 310.9.1 provider, fixed 4x, Hardware: Independent
> Flip, and both Intermediate Scatter Retention and Validated Warp Blend.
> PresentMon recorded 9,312 display intervals at 4.446 ms median, 6.245 ms p95
> and 8.486 ms p99 (222.90 FPS average). The AnimationTime cadence heuristic
> measured 3.998x from 2,347 source-frame samples and 7,036 generated-frame
> candidates; two source intervals exceeded the robust 30.768 ms threshold.

This Onimusha result validates the tested 4x cadence with both experimental
quality mechanisms active. It remains one controlled run, not a universal
performance or artifact-free compatibility claim.

> **Onimusha Boundary Artifact Mitigation A/B (September 15, 2026):** matched
> manual camera runs compared Off with Balanced on an RTX 4070 SUPER using
> DLSS-G 310.9.1, Streamline 2.14.1, fixed 4x, and Hardware: Independent Flip.
> Both runs measured a 3.998x generated/source cadence. Off averaged 208.11 FPS
> with display-frame p95/p99 of 8.8895/9.6438 ms; Balanced averaged 208.44 FPS
> with p95/p99 of 8.8821/9.8289 ms. Instrumented mean latency was 22.0437 ms
> Off and 22.0099 ms Balanced. Generated-frame GPU p95 was 1.5508 ms Off and
> 1.5510 ms Balanced.

The matched A/B found no measurable throughput, cadence, mean-latency, or
generated-frame-cost regression from Balanced in this test. The small p99
differences remain within the variability of single manually repeated runs;
this is release-gate evidence, not a universal performance guarantee.

Use
[`Capture-STALKER2-FramePacing.ps1`](../src/addons/mfgdiagnostics/Capture-STALKER2-FramePacing.ps1)
for three repeated 45-second runs of each case: native 2x without the addon,
addon loaded at native 2x, addon 3x, and addon 4x. Keep the same warmed-up save,
camera route, resolution, cap, HDR, VSync/G-SYNC state, driver, and runtime DLLs.
Remove the diagnostic companion addon for these performance runs. Each capture
records PresentMon data, an NVAPI before/after snapshot, and a version/hash
inventory of relevant loaded modules; the analyzer reports display/application
interval distributions, robust outliers, Present API time, available GPU and
instrumented latency fields, and only clearly labeled heuristic generated-frame
cadence.

## RTX 20 and 30 GPUs

Since 1.9.0 the DLSS-G backend runs DLSS Frame Generation on RTX 20 and 30 GPUs,
with NVIDIA's own generator (**DLSS-G**) or with Intel's model (**XeMFG DLSS-G**). It is
experimental: XeMFG DLSS-G was confirmed on an RTX 3060 Ti; NVIDIA's generator on a real
RTX 30 and anything on RTX 20 are not confirmed yet.

NVIDIA's runtime refuses these GPUs on its own, and its frame generation file
carries GPU code for RTX 40 and 50 only. On a PC where every NVIDIA GPU is an
RTX 20 or 30, the addon:

- answers the runtime's GPU checks (Streamline, NGX and the file's minimum) so
  the game offers DLSS Frame Generation and 3x–6x;
- rewrites the file's GPU code for RTX 30 (sm_86) or RTX 20 (sm_75) before the
  runtime loads it, so the driver builds kernels the card can run. This comes
  from [nefh's MFGAmpereUnlock-RenoDx](https://github.com/nefh/MFGAmpereUnlock-RenoDx)
  (MIT). On RTX 20 it takes about 3 seconds per file at every game start;
- does not apply the quality patches made for RTX 40; nefh's temporal correction,
  applied to the rewritten code, keeps 3x and above smoother than 2x.

These GPUs need the addon loaded before the game starts Streamline. On the first
start the addon adds itself to `[ADDON] LoadFromDllMain` in `ReShade.ini` (when
the file can be written), so **start the game twice the first time**. If it
could not, the panel offers **Enable early addon loading**.

Settings in `MFGAdaUnlock.ini` (next to the game), under `[DLSSG]`:

| Key | Default | Effect |
|---|---|---|
| `PreAdaAdmission` | `1` | `0` turns the whole RTX 20/30 path off |
| `PreAdaRequirementsAnswer` | `1` | When NGX's frame generation check fails while the game starts Streamline (no DLSS-G file loaded yet, or the driver refuses the game's project ID), the addon answers "supported" if a DLSS-G file is present (the game's `nvngx_dlssg.dll` or the NVIDIA app's copy); otherwise Streamline turns DLSS-G off for the session. `0` leaves NGX's failure as it is. The support report's `chain_ngx_requirements` line names the failure (1.10.0) |
| `AutoEarlyLoad` | `1` | `0` stops the addon from adding itself to `LoadFromDllMain` |
| `SimulatePreAda` | `0` | Test only. `1` takes the RTX 20/30 path on an RTX 40/50; `2` also makes NVAPI answer Ampere there, so Streamline refuses DLSS-G as on an RTX 30 unless the admission works (1.10.0) |
| `SimulateRequirementsFailure` | `0` | Test only, with `SimulatePreAda=2`: `1`, `2` or `3` makes NGX's frame generation check fail as FeatureNotFound, InvalidParameter or FeatureNotSupported (1.10.0) |
| `StreamlineLog` | `0` | `0` leaves Streamline's log as NVIDIA configured it. `1` (verbose) or `2` (standard level) makes the addon set `SL_LOG_LEVEL`, `SL_LOG_PATH` (the game folder) and `SL_LOG_NAME=sl.log` in the game's environment before it starts Streamline, so Streamline writes its own `sl.log` beside the game; **Save support files** copies it. `3` sets only `SL_LOG_LEVEL=0`: no Streamline log for this game. Variables already set are kept, and the registry is never changed (1.10.0) |

Under `[Diagnostics]`, `CrashDump=1` (off by default) writes `MFGAdaUnlock-crash.txt` (exception code, the module and offset where it happened) and a small `MFGAdaUnlock-crash-<pid>.dmp` beside the game when the game crashes; **Save support files** copies both. It is an unhandled-exception filter: it runs only when the process is about to end, after the game's own handlers, and then calls the game's previous filter (1.10.0).

Nothing changes on RTX 40 or 50. When something fails, the panel's
**Frame generation chain** card (with its **NGX feature** line: whether NVIDIA's
runtime created frame generation after the game turned it on), the **System check**
section (GPU, driver, Windows, HAGS, NGX core, early loading, addon files) and
**Save support files** show where. Hardware-accelerated GPU scheduling (HAGS) must be
on: without it NVIDIA's runtime accepts the request and generates nothing.

When the panel says frame generation was requested but no frames are generated,
turn on **Streamline log (sl.log)** under **Support → All settings (advanced)**
(or `[DLSSG] StreamlineLog=1`), restart, reproduce, and save the support files:
`sl.log` holds Streamline's own reason. If the chain card says Streamline started
before the addon could hook it, the environment is read too early; then set, in
the registry under `HKCU\SOFTWARE\NVIDIA Corporation\Global\Streamline`, `LogLevel`
(DWORD) `2` and `LogPath` (string) to the game folder, and restart.

### Other options on RTX 20/30

RTX 30-series (and, since its 0.4 releases, RTX 20-series) support for NVIDIA's
own DLSS-G is also available through
[sdli1995's separate `dlssg_for_sm86` project](https://github.com/sdli1995/dlssg_for_sm86).
It is a proxy DLL that keeps NVIDIA's runtime and replaces its inference
kernels with its own CUDA kernels for SM86/SM75, instead of relying on the
Ada/Blackwell kernels shipped in the standard DLSS-G runtime.

Its documentation lists Direct3D 12 only (Vulkan is not supported) and a
recent NVIDIA driver (R580 or newer). Because it presents through NVIDIA's own
DLSS-G path, it is not limited to borderless or windowed display modes. Follow
that repository's installation, compatibility, and runtime-version
instructions.

When to use which on RTX 20/30:

- **This addon's DLSS-G or XeMFG DLSS-G** in Direct3D 12 games that ship DLSS Frame
  Generation, first.
- **`dlssg_for_sm86`** in those games when this addon's path does not work there.
- **The [XeMFG backend](xemfg.md#xemfg-backend)** in games without DLSS Frame
  Generation (it also reads FSR 3.1 and DLSS Super Resolution inputs), or when
  `dlssg_for_sm86` does not support the game.

`dlssg_for_sm86` is an independent implementation (GPLv3) and is not bundled
with or maintained by this fork.

## Building

Use an existing RenoDX dependency checkout and Python 3. The **official local
release target** is `mfgunlock_local_low_overhead`. The generic RenoDX addon target
and `mfgunlock_addon_compile` are research/compile checks and do not select the
release's local-only policy.

1. Generate private provider tables locally; never commit the derived headers.
2. Configure `tests/CMakeLists.txt` with `RENODX_SOURCE_DIR`.
3. Build `release_warp_tests` and generate the local table with
   `tools/build_local_stability.py --variant release-1.4.3`. Supply the exact provider,
   approved baseline table, emitter, ptxas, nvdisasm and an output directory.
4. Build `mfgunlock_local_low_overhead` and run
   `tests/audit_local_low_overhead.ps1 -AddonPath <addon> -Variant release-1.4.3`.

For the 1.4.2 control, use `mfgunlock_1_4_2_control`, `local_stability_warp_tests`
and the `control` generator/audit variant. Its binary has a distinct filename.

The separate `mfgunlock_border_axis_candidate` target defines
`MFGUNLOCK_CONTINUOUS_BORDER_AXES` in addition to the local release flags and outputs
`renodx-mfgunlock-border-axis-candidate.addon64`. Validate it with
`border_axis_warp_tests`, `build_local_stability.py --variant border-axis-candidate`
and the matching PE audit variant. Geometry/inpaint payload policy is identical
to the control; the candidate changes only the emitted border warp fragment.

Each addon build writes an adjacent JSON manifest containing the baseline,
variant, binary/source hashes and private payload-table identities. Assembly
reports can be attached using `tools/write_build_manifest.py --validation <report>`;
variant, source and generated-table hashes must match. Generate manifests after
final source changes. These reports do not certify in-game activation or latency.

Run CTest and see [the research workflow](research/post-1.4.2.md) for CUDA
correctness, SASS inspection and controlled game captures. No full NVIDIA DLL or
provider package is included. The diagnostic companion is optional and should
be removed during performance measurements.

### Building release 1.4.5

Use the locally validated 1.4.4 generated payload tables and the existing
RenoDX dependency checkout. The build script verifies their hashes; it does
not download, regenerate or redistribute provider packages.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Build-EngineeringExperimental.ps1 -RenoDxSource <RenoDX-checkout> -Variant release-1.4.5 -OfflineProviders "<semicolon-separated-local-provider-paths>"
```

The release target is `mfgunlock_release_1_4_5`, producing
`renodx-mfgunlock-1.4.5.addon64` and an adjacent provenance manifest. The
published download retains the standard name `renodx-mfgunlock.addon64`.
Without explicit local provider inputs, provider-dependent tests are skipped;
that does not reproduce the complete local release validation.

### Building the XeMFG backend

The XeMFG backend is part of the same addon. The test project builds it as the
`mfgunlock_native_xemfg` target (`renodx-mfgunlock-native.addon64`, published
as `renodx-mfgunlock.addon64`). Configure `tests/CMakeLists.txt` with:

- `RENODX_SOURCE_DIR`, the RenoDX dependency checkout;
- `XEMFG_SDK_ROOT`, the Intel XeSS SDK headers;
- the verified local inputs prepared by `prepare_xemfg_inputs.py` and
  `vendor/generate_xess_programs.py`.

The motion-vector dejitter and scRGB passes are compiled with `fxc` from the
Windows SDK during the build.

The Vulkan path (1.7) is built in only when two local inputs exist:
`XEMFG_VULKAN_HEADERS`, the Khronos Vulkan-Headers include directory (1.3 or
later; `vulkan-sdk-1.4.350.1` was used), and `XEMFG_XVK_PROGRAMS`, the folder
written by `python -I tools/extract_xvk_programs.py <MFGUnlocker.dll> <folder>`
from your own copy of dashdogy's MFG Unlocker 1.7.0 release DLL (its SHA-256 is
checked; the DLL is read, never loaded). The SPIR-V it extracts derives from
Intel's programs and is never committed. Without them the addon builds without
the Vulkan path.

The gate is `ctest -C Release`. It runs:

- the unit and numeric tests (dejitter and scRGB on a WARP device);
- the `xemfg_native_*` smoke scenarios and the `xemfg_conformance_*` matrix, on
  the real Intel runtime with Streamline, FidelityFX and NGX fixtures;
- the field-session replay (`tests/xemfg_replay_tests.py`).

Set `XEMFG_OFFLINE_FG`, `XEMFG_OFFLINE_LL` and `XEMFG_OFFLINE_RESHADE` to
verified copies of the Intel pair and ReShade outside a game folder, because
game updates can remove them. The optional Streamline sample tests run when
`XEMFG_SL_SAMPLES_ROOT` points at builds made with `tools/sl_sample.py`.
