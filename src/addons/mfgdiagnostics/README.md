# MFG Streamline Diagnostics

This is a **separate, read-only diagnostic addon**, not an HDR/ghosting fix and
not a replacement for `renodx-mfgunlock.addon64`. The stable MFG implementation,
its temporal patch, multiplier behavior, and pacing code are unchanged.

It exists to compare the Streamline resources, constants, options and state seen
by different games or runtime configurations. Keep the game build, driver, DLLs,
resolution, frame cap, camera path and scene fixed between captures. No installed
game files are changed by the build process.

## Installation and capture

1. Place `renodx-mfgdiagnostics.addon64` beside the game's existing ReShade addon.
   It can observe native FG by itself or coexist with the stable MFG Unlock.
2. Open ReShade's **MFG HDR Diagnostics** panel, check **Enable observation hooks**,
   and restart the game. The setting is stored as `Enabled=1` under
   `[RenoDX.MFGDiagnostics]`. Hooks are disabled by default.
3. Verify the panel says hooks are installed. Some runtimes expose only legacy
   `slSetTag`; an unavailable `slSetTagForFrame` export is not by itself a failure.
4. Select a manual label matching the game's actual HDR/multiplier settings.
5. Press **Capture 10 seconds**, close the overlay and repeat the affected camera
   movement. Capture stops automatically after ten seconds or when the bounded
   buffer fills. Avoid judging performance with the overlay open.
6. After recording stops, press **Export stopped capture**. A separate worker
   writes a new JSON file under `%TEMP%\MFGUnlock-HDR-Diagnostics`; the full path
   appears in the panel. Existing captures are not overwritten.
7. Capture HDR OFF and ON at 2x first, then 3x/4x. For options/transition data,
   make an additional recording while switching the native FG/HDR setting.
   If the game only sends options during initialization, a gameplay capture
   may have no options events; this is not proof that FG is disabled.
8. Share the JSON captures and the corresponding ReShade log, plus which run had
   MFG Unlock installed. Review logs before sharing; ReShade logs contain local
   paths. The JSON does not export resource addresses or full module paths.

Restart between controlled comparisons and verify the loaded runtime versions.
Do not update DLLs, force OTA, or change legacy flip pacing during the baseline.
For actual frame-pacing/performance validation, remove the diagnostic companion
and restart. Even observational hooks and CPU descriptor reads add overhead.
Do not hot-unload either addon: the game can retain wrapped function pointers.

## What this does and does not observe

- Wraps existing `slDLSSGSetOptions` and `slDLSSGGetState` calls without changing
  arguments/results or issuing additional state queries.
- Observes `slSetConstants`, legacy `slSetTag`, and optional `slSetTagForFrame`.
  Only recognized structure versions are inspected; unknown layouts pass through.
- Captures jitter, matrices, MV scale/conventions, reset flags, tag types,
  resource lifecycle/state/extents, and the CPU-side D3D12 texture descriptor.
  For integrations whose `sl::Resource` layout does not validate against the
  observer's public header, a bounded pointer scan is matched against ReShade's
  live D3D12-resource registry. It is accepted only when exactly one live handle
  matches; no guessed pointer is invoked through COM.
  Vulkan/D3D11 native resources are not dereferenced as D3D12 resources.
- Samples the ReShade-observed swapchain format and color space while recording.
  These enum values are **ReShade values**, not DXGI `SetColorSpace1` values.
- Resource format does not prove pixel encoding. View formats, GPU pixel values,
  exposure textures, producer draws, barriers, and actual NGX evaluation inputs
  still require a targeted GPU/API capture. No buffers are copied or retagged.
- Resource IDs represent observed handle identities, not allocation generations;
  address reuse is possible and does not prove continuity of resource contents.
- Legacy tags have no explicit frame identity. Their `frame` stays null; do not
  associate them with the last constants call across threads as if proven.
- Function owners describe the next wrapper at the diagnostic boundary, which
  may belong to another addon. Options/state may be before or after its override.
  Module inventory is sampled at export, not proof of the executing NGX provider.
- Does not hook Reflex markers, NGX evaluation, or HDR metadata APIs in this first
  diagnostic build. Common-constant/tag order is not a complete GPU timing trace.
- At explicit export only, reads NVIDIA's public `NvAPI_D3D_GetSleepStatus`,
  `NvAPI_D3D_GetLatency`, and `NvAPI_NGX_GetNGXOverrideState` interfaces. This
  can report the driver-observed FG multiplier, Reflex/game-sleep and iFlip
  state, NGX override feedback, and `aiFrameTimeUs` when the game supplied valid
  latency markers. It does not call any NVAPI setter, add latency markers, hook
  `nvapi_QueryInterface`, or change the GPU architecture reported to the game.
- Does not request UI recomposition, reset history, zero jitter, change exposure,
  convert PQ/scRGB, select a model, modify the temporal kernel, or alter pacing.

Capture storage is bounded to 16,384 events and 64 tags per API call. Nonblocking
locks drop observations on contention; the export reports missing/truncated data.
Such captures must not be interpreted as proof of missing resources or broken
synchronization. No module enumeration, file writes, or GPU readbacks occur in
the recording callbacks. Export is explicit and runs after capture, on a worker.

## Offline summary

From PowerShell, use the adjacent script with one or two captures:

```powershell
.\Compare-HdrCaptures.ps1 -Baseline 'C:\captures\HDR_OFF_2x.json' -Candidate 'C:\captures\HDR_ON_2x.json'
```

The script summarizes observations, not a pass/fail judgment of visual quality.
Different resource IDs between sessions are expected. Different camera matrices
and temporal jitter samples are also expected during movement.

## Trace schema 1

Each event has QPC begin/end timestamps, thread, optional viewport/frame, API
result, original structure version, `recognized`, `values` and `floats` arrays.
Unknown integer values and unobserved floating fields are null. Streamline's
own `INVALID_FLOAT` sentinel remains a numeric value; Boolean 2 means invalid,
not false. Timestamps bracket the observer and downstream call, not GPU execution.

| Kind | Numeric ID | `values` indexes |
|---|---|---|
| Options | 0 | 0 mode; 1 generated count; 2 flags; 3-4 dynamic size; 5 buffers; 6-7 MV/depth size; 8-9 color size; 10-14 color/MV/depth/HUD-less/UI format hints; 15 queue mode; 16 UI recomposition; 23 chained extension present |
| Constants | 1 | 0 inverted depth; 1 camera motion included; 2 3D MVs; 3 reset; 4 orthographic; 5 dilated MVs; 6 jittered MVs; 23 extension present |
| Tag | 2 | 0 type; 1 lifecycle; 2 resource descriptor present; 3 native handle present; 4 session resource ID; 5-7 SL width/height/format; 8 state; 9-12 left/top/width/height extent; 13 command ID; 14-19 D3D12 width/height/format/mips/samples/flags; 20 tracked descriptor found; 21 resource extension present; 22 resource version; 23 tag extension present; 24 byte offset of the uniquely matched live handle; 25 number of distinct live-handle matches |
| State | 3 | 0 status; 1 presentations since previous application query; 2 maximum generated frames at this hook boundary |
| Output | 4 | 0 swapchain ID; 1 ReShade color space; 2 ReShade format; 3-4 dimensions; 5 backbuffer count |
| Null/truncated tag batch | 5 | 0 requested tag count; 1 tag-array pointer present; 2 number of inspected tags |

Options float 0 is the version-5 dynamic target rate. Constants floats 0-79 are
five row-major matrices (`cameraViewToClip`, `clipToCameraView`, `clipToLensClip`,
`clipToPrevClip`, `prevClipToClip`); 80-81 jitter; 82-83 MV scale; 84-85 pinhole;
86-97 position/up/right/forward; 98-102 near/far/FOV/aspect/invalid-MV sentinel;
103 the version-2 minimum relative linear-depth separation.

## Build and verification

Copy this folder beside `src/addons/mfgunlock` in the existing RenoDX tree. Build
the `mfgdiagnostics` target using the configured development toolchain. The
standard output is `renodx-mfgdiagnostics.addon64`. There are no shader changes.

The standalone tests include the actual observer wrappers, protected-page tests
for old/unknown structure versions, nonmutation and exact-once forwarding tests,
and bounded capture/timeout/contention checks. Configure `tests` with
`-DRENODX_SOURCE_DIR=<existing RenoDX checkout>`, build `mfgdiagnostics_tests`,
then run CTest. They do not validate in-game image quality or performance.

The offline comparison test uses PowerShell and requires permission to run local
scripts. On a machine that blocks scripts, that test is blocked by policy rather
than a failed capture comparison; do not change the machine-wide policy just to
run it. The packaged observer is an MSVC Release build, but it is still
diagnostic instrumentation and must be removed for performance benchmarks.

## Performance and latency capture

Use the diagnostic addon only to explain Streamline inputs. Remove it and restart
the game for performance comparisons. The production MFG addon may remain when
the test case requires it.

The adjacent `Capture-Performance.ps1` script records 30 seconds with the signed
Intel PresentMon console tool and samples the standalone read-only NVAPI probe
before and after the run. Start the game, load a repeatable save and then run:

```powershell
.\Capture-Performance.ps1 -Label native-2x-hdr -ProcessName GameExecutable
```

It also writes `capture-metadata.json` with the test label, notes, GPU/driver
inventory, and versions plus SHA-256 hashes for relevant Streamline, NGX,
ReShade and addon modules mapped at capture start. The inventory identifies
candidates and does not by itself prove which OTA/provider module executed.

By default the script looks below `Documents\Tooling\PresentMon` and
`Documents\Tooling\NVIDIA`. Pass `-PresentMonPath` and `-NvapiProbePath` when
the tools are installed elsewhere.

Accept the UAC prompt used for ETW capture. Keep the overlay closed and repeat
the same camera route. Run at least three passes for each case after a warm-up:

1. native 2x with MFG Unlock removed;
2. addon loaded, native 2x selected;
3. addon loaded, 3x selected;
4. addon loaded, 4x selected.

Do not change the save, route, resolution, cap, HDR mode, game build, driver,
Streamline files, or NGX DLLs between cases. `Analyze-PresentMon.ps1` selects the
main swap chain and its dominant presentation mode, excluding the common
UAC/alt-tab transition from the comparison. It reports application and display
intervals, average/1% low, large interval outliers, Present duration, GPU timing
and instrumented latency where available. For NVIDIA MFG, `FrameType` may label
generated output as `Application`. When available, the analyzer also reports a
clearly marked heuristic based on positive/zero `AnimationTime` samples. Treat
that split as driver-dependent corroboration, not an authoritative NVIDIA frame
label; cross-check display timing and the NVAPI-observed multiplier. PresentMon
GPU execution metrics have known limits with Hardware-Accelerated GPU
Scheduling, so use an Nsight Systems/Graphics GPU trace before drawing kernel
conclusions.

For kernel or queue-level conclusions, take a short Nsight Systems or Nsight
Graphics trace separately. Profiling overhead makes those traces unsuitable for
the PresentMon FPS comparison itself.

For the 0.8 release gate, use the dedicated STALKER 2 wrapper. It standardizes
case names, uses a 45-second default and displays the restart/configuration
checklist before capture:

```powershell
.\Capture-STALKER2-FramePacing.ps1 -Case native-no-addon-2x -Run 1
.\Capture-STALKER2-FramePacing.ps1 -Case addon-native-2x -Run 1
.\Capture-STALKER2-FramePacing.ps1 -Case addon-3x -Run 1
.\Capture-STALKER2-FramePacing.ps1 -Case addon-4x -Run 1
```

Repeat each case with `-Run 2` and `-Run 3`. Use `-ConfigurationNotes` to record
the game build, resolution, HDR, cap, VSync/G-SYNC state and save/route. Do not
publish a conclusion until all cases use the same driver/runtime hashes and the
three runs agree closely enough to rule out a one-off traversal or shader-cache
event.

References: [NVIDIA DLSS-G integration](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md),
[common constants](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_consts.h),
[NVIDIA NVAPI](https://github.com/NVIDIA/nvapi), and
[Intel PresentMon](https://github.com/GameTechDev/PresentMon).
The existing Dreamt/dashdogy and other project credits remain in the main README;
this companion reuses the repository's existing Detours installation helper.
