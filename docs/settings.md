# Settings and runtime versions

Part of the [MFG Unlock](../README.md) documentation. Sections: [Version Matrix and Advanced Runtime Setup](#version-matrix-and-advanced-runtime-setup), [Settings](#settings).

## Version Matrix and Advanced Runtime Setup

The table separates general fixed-MFG compatibility from features that depend
on the new Dynamic ABI. “Validated” means the listed path has been exercised;
it is not a universal claim for every game or presentation setup.

| DLSS-G | Streamline | General addon / fixed MFG | Automatic Guard | UI Composition | Dynamic MFG | VSync / G-SYNC notes |
|---|---|---|---|---|---|---|
| 310.9.0 | 2.12.x | Validated legacy/compatibility path | Validated | Not release-validated on this older wrapper | Not supported by this release | Keep the game's established sync path; do not assume the 2.14.1 Dynamic/VSync behavior |
| 310.9.0 | 2.14.0 / internal RC builds | Fixed MFG may work, including NVIDIA App override builds | Not release-validated as a complete combination | Not release-validated | The underlying runtime may expose Dynamic without VSync when its capability bit is true, but this addon does not advertise this combination as supported | Do not assume 2.14.1 VSync/frame-limiter behavior; use the exact current stack below for release testing |
| 310.9.1 | 2.14.1 | Previously validated path | Validated | Validated where the game supplies a correct pair; slight provider cost is expected | Supported on D3D12 when the runtime capability bit is true; final in-game release retest pending | VSync optional; with VSync, target follows refresh. G-SYNC remains a driver/display choice; capability and Independent Flip still matter |

| 310.9.16 | 2.14.16 | Played with XeMFG DLSS-G in several games (1.8.3–1.8.5) | In-game validation pending | In-game validation pending | Tested in Cyberpunk 2077 with XeMFG DLSS-G, including the NVIDIA app override copy and the Dynamic ceiling (1.8.5) | Same public Dynamic/VSync interface as 2.14.1; tested with driver VSync + G-SYNC |
| 310.9.1 or newer | 2.14 or newer | Fixed MFG follows the provider gate (exact or generic profile) | Not release-validated | Not release-validated | Accepted by the 1.8.5 version minimum on D3D12; the runtime's support bit decides | VSync with Dynamic needs 2.14.1 or newer |

### Manually using the current NVIDIA runtime

> [!IMPORTANT]
> **Do not assume that updating the NVIDIA App or display driver installs
> DLSS-G 310.9.16 and Streamline 2.14.16 into a game.** Games normally keep the
> DLLs they ship, while NVIDIA App driver-profile overrides may load a different
> module from an NVIDIA cache. For the supported Dynamic configuration, the
> files currently have to be installed manually and the driver overrides must
> be returned to application-controlled/default behavior.

Dynamic MFG in this release requires the game process to **actually load** both:

- `nvngx_dlssg.dll` **310.9.16**
- the complete matching Streamline **2.14.16** runtime, including at least the
  game's corresponding `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`,
  Reflex/PCL components, and any other Streamline plugins that game requires

Replacing only `nvngx_dlssg.dll` is not sufficient. Likewise, seeing a 2.14.16
DLL in the game directory does not prove that the process selected it.

- Normal users can obtain the current game DLL from the established
  [TechPowerUp DLSS Frame Generation archive](https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/).
- Developers and advanced users can obtain
  [Streamline 2.14.16 from NVIDIA](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16),
  inspect NVIDIA's official
  [Windows DLSS library directory](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel),
  and consult the
  [DLSS 310.9.16 release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.16).

#### Required installation and override checklist

1. Back up the original game DLLs.
2. Install `nvngx_dlssg.dll` 310.9.16 manually.
3. Install the **complete, version-matched** Streamline 2.14.16 set. Never mix
   `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, or
   other Streamline components from different packages.
4. Open the NVIDIA App, check both **Global Settings** and the game's own
   profile, and set **DLSS Override - Frame Generation** plus the Frame
   Generation entry under **DLSS Override - Model Presets** to **Use the 3D
   application setting**. Apply the changes.
5. If NVIDIA Profile Inspector (NVPI) has been used, open the same game profile
   and return every DLSS Frame Generation/NGX override changed there to its
   NVIDIA default or application-controlled value, then apply the profile.
   NVIDIA App and NVPI edit driver-profile state; leaving an override active in
   either place can make the manually installed DLL lose selection again.
6. In MFG Unlock choose **Streamline runtime selection -> Prefer local runtime
   - disable OTA**. If the panel says this policy did not reach `slInit`, enable
   **early addon loading for next restart**.
7. Exit the game completely and start it again. Do not rely on an in-game reload
   after changing DLLs, driver overrides, runtime selection, or early loading.
8. Verify the **loaded** versions and paths in the MFG Unlock panel and
   `ReShade.log` before enabling Dynamic MFG.

For example, a path containing
`ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_g_override_0` means the driver/NVIDIA
App override is active. In that situation a 2.14.16 file beside the executable
can be completely ignored. Do not delete NVIDIA cache directories as a normal
installation step; correct the driver profile instead.

Success means the log reports all of the following from the same launch:

```text
observed Streamline DLSS-G wrapper version 2.14.16.0 from ...
verified mapped DLSS-G provider candidate version 310.9.16.0
slDLSSGGetState confirms NVIDIA Dynamic MFG support
NVIDIA Dynamic MFG accepted
```

If VSync is enabled, the last line can still report Dynamic as accepted, but
the numeric `DynamicTargetFPS` is intentionally ignored and the provider aims
near the active display refresh. Streamline 2.14.1 introduced supported VSync and
frame-limiter behavior to Dynamic mode; it does not make a custom Dynamic target
override the monitor-refresh target while VSync is active.

The addon does not write NVIDIA driver profiles, NVPI settings, or NVIDIA App
settings. Public
`NvAPI_NGX_GetNGXOverrideState` is used only by the optional diagnostic tool to
observe override feedback; there is no verified public NVAPI call here that can
safely force this per-game policy after Streamline/NGX initialization. To stop
Streamline itself from choosing downloaded OTA plugins during a controlled test,
select **Prefer local runtime**, restart, and verify the loaded paths/versions in
the log.

## Settings

Written to your `ReShade.ini` under `[RenoDX.MFGUnlock]`:

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Enables the addon for the next launch; changing this requires a restart so live memory patches cannot be left in a partial state |
| `MaxCount` | `4` | The `DLSSG.MultiFrameCountMax` value reported to the runtime |
| `ForceFlipMeteringOff` | `0` | Normally leave off. Enable only if 3x/4x freezes; this forces Streamline's legacy software pacing fallback and requires a game restart |
| `TemporalFix` | `1` | The interpolation correction. Leave on; changing it requires a restart |
| `BlackwellFrameworkKernels` | `1` | Uses the exact-fingerprint Blackwell motion-vector/inpaint/inpaint-decision replacements when the installed provider matches; otherwise falls back to the 0.7 temporal correction. Changing it requires a restart |
| `ExperimentalAdaptiveQuality` | `1` | Enables the coordinated geometry, warp-confidence, border and inpaint quality path; restart required |
| `AdaptiveQualityProfile` | `3` | `1` preserves Stable V1, `2` selects Flicker-Reduced V2, and `3` selects the default Luminance + Directional V3. Existing saved V1/V2 values are preserved; missing keys use V3 and invalid values fail closed to V1 |
| `AdaptiveQualityV3Photometric` | `1` | Developer A/B control for local-luminance-normalized luma/chroma confidence. Off retains V2 absolute-RGB confidence inside the V3 profile; restart required |
| `AdaptiveQualityV3DirectionalBorder` | `1` | Developer A/B control for motion-directional border tapering. Off retains V2's symmetric taper; restart required |
| `AdaptiveQualityV3OrientedGeometry` | `1` | Developer A/B control for motion-oriented cardinal/diagonal support. Off deliberately requests geometry V2; restart required |
| `AdaptiveQualityV3StabilityMode` | `1` effective | Local Stable in the official release and border-axis candidate. Saved temporal selections are ignored without rewriting them; research builds are separate. |
| `AdaptiveQualityV3InpaintMode` | `0` effective | V2 Compatibility in the official release and border-axis candidate. Saved V3/temporal selections are ignored without rewriting them. |
| `ThinGeometryIntermediateScatter` | `1` | Experimental recommended default: retains more motion information while constructing intermediate generated frames; keeps the separate depth test and requires the validated full Blackwell path. Disable per game if it adds ghosting or disocclusion artifacts |
| `ThinGeometryValidatedWarpBlend` | `1` | Experimental recommended default paired with Intermediate scatter retention: validates warped candidates before gradually increasing their blend weight; may reduce thin-detail flicker but can increase temporal persistence. Requires a restart |
| `ThinGeometryPreviousScatter` | `0` | Unstable advanced research control for a separate previous-to-current motion-rejection path; not recommended for normal use |
| `BoundaryArtifactMitigationMode` | `1` | `0` restores the 0.9 intermediate-retention behavior, `1` is **Balanced** (recommended default for fresh configurations), and `2` is Aggressive. Existing saved choices are preserved; changing it requires a restart |
| `ForceMultiplier` | `0` | `0` respects the game's own choice; `2`–`6` requests that exact multiplier, whether it is higher or lower than the game's choice |
| `DynamicMFG` | `0` | Requests native NVIDIA Dynamic MFG on D3D12 with DLSS-G 310.9.1 or newer and Streamline 2.14 or newer (1.8.5; before, exactly 310.9.1/310.9.16 + 2.14.1/2.14.16), after the runtime reports support; takes priority over `ForceMultiplier` while active |
| `DynamicTargetFPS` | `0` | Dynamic output target; `0` follows display refresh. With VSync active, Streamline ignores a nonzero value and follows refresh instead |
| `DynamicMaxMultiplier` | `0` | 1.8.5. The highest multiplier Dynamic MFG may pick: `0` keeps the driver's or NVIDIA app's value, `2`–`6` is "Up to 2x"–"Up to 6x". Given to Streamline in this game only; the driver profile is not changed. Read when the game starts, so a change applies after a restart |
| `ReflexSourceFpsCap` | `0` | Compatibility key for **Output FPS Cap (Reflex)**. Despite the historical key name, the number is the final/output FPS ceiling; `120` targets up to approximately 120 displayed FPS |
| `ReflexModeOverride` | `0` | `0` preserves the game setting; `1` uses FG-safe Off by keeping Streamline at Low Latency and bypassing only `slReflexSleep` (the NVIDIA overlay still shows Reflex On); `2` forces On; `3` forces On + Boost. With Frame Generation on, DLSS-G paces its own queue, so these usually measure the same latency; Boost helps only when the CPU is the bottleneck. A change in the panel applies on the game's next Reflex sleep |
| `ReflexPacingMethod` | `0` | `0` uses native `slReflexSleep`; `1` requests the experimental DXGI Waitable path after its 120-token safety probe |
| `ReflexVrrHeadroomEnabled` | `0` | Enables the final/output Reflex cap derived from verified VRR refresh and the configured headroom |
| `ReflexVrrHeadroomBasisPoints` | `100` | VRR headroom from `50` to `300` basis points (0.5%-3.0%); invalid values normalize to 1.0% |
| `DynamicReflexSourceCap` | `0` | Legacy migration flag. New configurations should use `ReflexSourceFpsCap` through the UI |
| `FixedOutputFpsCap` | `0` | Legacy fixed-output setting migrated in memory when the current cap key is absent |

| `RaiseFrameCeiling` | `0` | Raises an old Streamline plugin's compiled hard limit to 6x. Off by default because that breaks some games; the stale device-limit bypass needed by STALKER 2 is always applied |
| `RuntimeSelectionMode` | `0` | `0` preserves the game's runtime policy, `1` disables OTA/downloaded plugins to prefer local files, and `2` forces the NVIDIA OTA flags; restart required |
| `HDRCompatibilityMode` | `0` | `0` is **Native** (default for new configurations), `1` forces UI Composition, `2` enables **Automatic Guard + UI Composition (HDR compatibility)**, and `3` enables Final Color Fallback; existing saved values remain unchanged |
| `DlssSuperResolutionPreset` | `0` | DLSS Super Resolution preset applied during play: `0` keeps the game's presets; `10`-`13` select J, K, L or M. Streamline games apply it at once; games that create DLSS through NGX use it when they recreate DLSS. An NVIDIA App preset override takes precedence |
| `DlssRayReconstructionPreset` | `0` | DLSS Ray Reconstruction preset applied during play: `0` keeps the game's presets; `4`-`6` select D, E or F (F is the DLSS 4.5 model). Only games that pass Ray Reconstruction presets to Streamline |
| `DepthEdgeGuardLevel` | `0` | Optional depth-edge tuning: `0` keeps the game value; `1`-`4` select progressively lower separation thresholds |
| `Rtx50QualityFixes` | `0` | RTX 50 only, experimental: `1` applies the validated warp blend (the thin-geometry and HUD-edge quality fix) to the provider's sm_120 kernel. RTX 50 cards otherwise leave the DLSS-G provider untouched, because the other quality fixes target Ada kernels the RTX 50 does not run. Restart required |

`[DLSSG] ForceBorderless=1` in `MFGAdaUnlock.ini` beside the game still serves
the DLSS-G backend's window as borderless. Since 1.7 it is no longer in the
panel; it is read once when the game starts.

The full geometry V3 ptxas payload is larger than the provider's original Ada
cubin slot. Builds that deliberately include this research payload never write
past that slot: after exact provider, fatbin, cubin fingerprint, slot-size and
descriptor validation, the addon copies the fatbin into a bounded allocation,
replaces only the matching cubin entry, and redirects the provider's exact
fatbin descriptor references. If that redirect cannot be established safely,
geometry falls back independently to V2, then V1, then native. The V3.2 Local
artifact has 39,552 bytes of `.text`; the Temporal artifact has 41,216 bytes of
`.text` and a 45,360-byte cubin. Both use 40 registers and 7,776 bytes of shared
memory, with zero stack, local memory and spills. `nvdisasm` confirms that Local
contains no global memory instruction and Temporal contains eight global loads
(including exactly two confidence-byte reads) plus two confidence-byte writes.
The V3.2 warp program is bit-identical to V3.1, uses 48 registers and has zero spills;
profile GPU and Reflex latency before treating the temporal mode as validated
for a game.

If a game has its own multiplier selector, leave `ForceMultiplier` at `0` and use
the game's setting. A fixed value is an absolute override: for example, if the
game requests 4x and the addon is set to 2x, the downstream request becomes 2x.
Dynamic MFG retains priority while it is active. After changing the fixed value,
the panel reports it as pending until the game submits its next enabled
`slDLSSGSetOptions` call; toggling Frame Generation off/on forces most games to
submit one. The panel lists the game's request, the addon's fixed request, and
the effective downstream request separately.

> **First launch after installation or update:** After installing or updating
> the addon, the first launch may perform DLSS-G kernel compilation and exhibit
> temporary stutter or uneven pacing. Restart the game once before evaluating
> performance or image quality.

## Performance overlay and diagnostics (MFGAdaUnlock.ini)

1.11.0. Written to `MFGAdaUnlock.ini`; the `[Overlay]` keys apply at once and have controls under **General → Performance overlay**.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Overlay]` | `PerfOverlay` | `0` | Shows the performance overlay over the game, also with the ReShade menu closed |
| `[Overlay]` | `PerfOverlayLevel` | `1` | `0` Minimal (frame rate), `1` Standard (adds frame times, lows, hitches and the frame generation status), `2` Full (adds a frame time graph, latency, GPU time and video memory) |
| `[Overlay]` | `PerfOverlayCorner` | `0` | `0` top left, `1` top right, `2` bottom left, `3` bottom right |
| `[Overlay]` | `PerfOverlayScale` | `100` | Text size in percent of ReShade's font, `50`–`200` |
| `[Overlay]` | `PerfOverlayOpacity` | `70` | Background opacity in percent; `0` draws the text only |
| `[Overlay]` | `PerfOverlayKey` | `121` | Windows virtual-key code of the shortcut (`121` = F10); `0` turns the shortcut off |
| `[Overlay]` | `PerfOverlayModifier` | `1` | Key held with the shortcut: `0` none, `1` Shift, `2` Ctrl, `3` Alt |
| `[DLSSG]` | `HostGameReflex` | `1` | Add frame generation: `0` holds back the game's own NVAPI Reflex calls while MFG Unlock's Streamline runs Reflex. Leave unset unless asked |
| `[Diagnostics]` | `CrashDump` | `0` | Writes `MFGAdaUnlock-crash.txt` and a dump on a crash, and a dump with `MFGAdaUnlock-hang.txt` when the game stops responding inside a call MFG Unlock watches. The hang record's text file is written either way |

## DLSS 5 neural rendering (MFGAdaUnlock.ini)

1.12.0. The **DLSS 5** tab writes these keys. `Enabled` needs a game restart; the others apply at once.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[DLSS5]` | `Enabled` | `0` | Runs NVIDIA's DLSS 5 neural rendering on the game's frames. Needs `nvngx_dlssnr.dll` beside the game's exe (or `RuntimePath`) |
| `[DLSS5]` | `RuntimePath` | empty | Full path of `nvngx_dlssnr.dll` when it is not beside the game's exe |
| `[DLSS5]` | `Network` | `0` | `0` the current network, `1` the original one |
| `[DLSS5]` | `Style` | `0` | `0` Default, `1` Natural, `2` Cinematic |
| `[DLSS5]` | `Intensity`, `LocalTone`, `LocalStructure`, `Skin` | `100` | Strength of the effect and of its parts, `0`–`200` percent |
| `[DLSS5]` | `ProtectHud` | `1` | Lets NR mask the HUD it recognizes |
| `[DLSS5]` | `Scale` | `100` | NR's resolution in percent of the output, `25`–`100`; `50` costs about a quarter |
| `[DLSS5]` | `Passes` | `1` | NR again on its own result, `1`–`3`; each pass costs about as much as the first |
| `[DLSS5]` | `HookPoint` | `0` | Where NR runs: `0` Auto, `1` after the game's upscaler only, `2` Present |
| `[DLSS5]` | `Motion` | `0` | Motion for NR: `0` Auto (the game's vectors, else estimated from the image), `1` the game's only, `2` off |
| `[DLSS5]` | `TaaFinder` | `0` | Before Present: looks for the game's own TAA motion vectors and depth (experimental; restart) |
| `[DLSS5]` | `Stability` | `60` | Keeps part of earlier frames' result where nothing moved, against flicker, when NR has no motion, `0`–`100` |
| `[DLSS5]` | `PaperWhite` | `0` | HDR: the brightness of SDR white in nits; `0` follows Windows' SDR content brightness |
| `[DLSS5]` | `ToggleKey`, `CompareKey` | `119`, `118` | Virtual-key codes of the on/off and compare keys (F8, F7); `0` turns a key off |
| `[DLSS5]` | `KeyModifier` | `2` | Key held with them: `0` none, `1` Shift, `2` Ctrl, `3` Alt |
| `[DLSS5]` | `VramMargin` | `512` | MiB of video memory kept free; below it NR pauses and frees its memory |
| `[DLSS5]` | `Coexist` | `0` | With another DLSS 5 add-on loaded: `0` MFG Unlock's NR stands aside, `1` runs anyway |
| `[DLSS5.Looks]` | look name | | Looks saved from the tab |
| `[Diagnostics]` | `ForceDRED` | `0` | Also: on a device loss, the unfinished command lists and the page fault in the log, and the process' exceptions with their stacks |
