# MFG Unlock

A [ReShade](https://reshade.me/) addon that enables **DLSS multi-frame generation
(3x / 4x and above) on GeForce RTX 40-series** cards, which NVIDIA ships gated to
RTX 50-series only — and corrects the frame interpolation so the extra frames
carry new motion instead of repeats.

The default for a new installation is **Native**, which preserves the game's
existing HUD/UI tags and is recommended for most games. **Automatic Guard + UI
Composition (HDR compatibility)** remains available for known HDR-related
issues and keeps the game's required color, depth and motion-vector inputs
intact.

Nothing in the game installation is modified. Every patch is applied to the
mapped image at runtime and reverted when the addon unloads. **No complete
NVIDIA DLL or provider package is redistributed here.**

> [!NOTE]
> Vulkan support is new in version 0.6 and remains experimental while its game
> compatibility matrix grows. Direct3D 12 behavior remains the established
> path.

---

## Attribution

This repository is a fork of the
[original ReShade/RenoDX addon project](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx)
created by [Dreamt](https://github.com/ImDreamt). Full credit for that original
addon implementation goes to Dreamt and the contributors already credited in
this repository.

The underlying technical approach originates from
[dashdogy's RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock).
Dashdogy identified Ada's higher-multiplier midpoint compaction problem and
developed the original ASI implementation that verifies the active Streamline
wrapper and NGX provider, intercepts `slGetFeatureFunction`, adjusts
`slDLSSGSetOptions`, observes real presentation counts through
`slDLSSGGetState`, and applies the corrected slot-9 temporal program entirely
in mapped process memory. Dreamt then adapted this work into the ReShade/RenoDX
addon on which this fork is based.

Work implemented and maintained by [mavismmg](https://github.com/mavismmg) in
this fork expands the original project with broader game and runtime
compatibility. The repository history and current code attribute these additions:

- Support and fixes for **S.T.A.L.K.E.R. 2: Heart of Chornobyl**, including its
  native 3x/4x selector and bundled/OTA provider handling.
- Temporal-patch compatibility with newer 310.9 DLSS-G providers.
- An exact-fingerprint Blackwell framework-kernel path for Ada, covering the
  motion-vector, inpaint, and inpaint-decision stages, with the established
  temporal correction retained as a safe fallback. This path follows the
  Blackwell-kernel research and rebuild workflow published by Matias Lombo.
- **Intermediate Scatter Retention**, independently investigated and implemented
  in this fork. It conservatively relaxes one motion-consistency rejection while
  intermediate-frame motion vectors are constructed, while retaining the
  separate depth-mismatch test.
- **Validated Warp Blend**, a separate conservative candidate-validation and
  blending path informed by Tony Joaca's public `qualityValidWarp` research in
  DLSSG-Transfusion and independently implemented for this addon.
- The two complementary thin-geometry quality mechanisms enabled together as
  experimental defaults while remaining independently selectable per game.
- Exact DLSS-G 310.9.0/310.9.1 provider and payload validation for the
  thin-geometry paths, with unknown providers failing closed instead of being
  patched speculatively.
- Safer reconstructed-fatbin handling that preserves kernels and metadata after
  the modified PTX entry, plus hardened restoration that avoids leaving a live
  provider descriptor pointing to released replacement memory.
- Safer ReShade addon lifecycle handling across temporary device probing and
  addon reloads.
- Bounded background provider discovery, removing continuous module enumeration
  from the active `Present` path.
- Additional compatibility controls for Streamline flip metering and software
  pacing fallback behavior.
- Runtime diagnostics using `slDLSSGSetOptions` and `slDLSSGGetState`, including
  requested multipliers, DLSS-G status, and actual presentation telemetry.
- A concise normal telemetry display that hides the cumulative sample counter
  without removing presentation counts, multiplier validation, or diagnostic
  logging.
- A conservative Streamline input Quality Guard that avoids incompatible
  optional HUD/UI separation resources, including the HDR mismatch confirmed
  during Hogwarts Legacy testing.
- **Automatic Guard + UI Composition**, including HUD-less/UI validation,
  conservative HDR final-color fallback, transition-safe split-tag handling,
  and one-shot temporal-history synchronization.
- HDR + Frame Generation investigation and compatibility work based on captures
  from Hogwarts Legacy, with related diagnostic tooling intended for comparison
  against other integrations such as Jedi Survivor.
- Native NVIDIA Dynamic MFG integration through `DLSSGMode::eDynamic`, including
  exact release-stack checks, proactive capability validation for older games,
  correct VSync target semantics, bounded retries, and fixed-MFG fallback.
- Absolute fixed-multiplier control from 2x through 6x, allowing the addon to
  lower or raise the game's request while preserving Dynamic MFG priority and
  distinguishing game, addon, effective, observed, and pending states.
- **Native** quality mode as the least-invasive default for new configurations,
  while preserving every explicitly saved choice from existing users.
- Optional depth-edge tuning for integrations whose native linear-depth
  separation produces visible disocclusion artifacts. It is off by default.
- Experimental Vulkan renderer and NGX provider discovery.
- Reusable PresentMon/NVAPI validation tooling and controlled frame-pacing
  evidence, including an observed 3.998x cadence in Onimusha: Way of the Sword
  with Intermediate Scatter Retention and Validated Warp Blend active at 4x.
- Compatibility testing and documentation across the games and runtime
  combinations listed below.

These additions extend Dreamt's addon and dashdogy's research; they do not
replace or claim authorship of either original contribution.

## Contents

- [Tested Games](#tested-games)
- [Requirements](#requirements)
- [Usage](#usage)
- [Using with RenoDX DLSS5](#using-with-renodx-dlss5)
- [Selecting the Streamline Runtime](#selecting-the-streamline-runtime)
- [Verifying Operation](#verifying-operation)
- [Experimental Vulkan Support](#experimental-vulkan-support)
- [Frame-generation Input Quality](#frame-generation-input-quality)
- [Experimental Thin-geometry Interpolation](#experimental-thin-geometry-interpolation)
- [Dynamic Multi Frame Generation](#dynamic-multi-frame-generation)
- [Version Matrix and Advanced Runtime Setup](#version-matrix-and-advanced-runtime-setup)
- [Settings](#settings)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
- [Release Validation](#release-validation)
- [Building](#building)
- [Credits](#credits)

## Tested Games

| Game | Status |
|---|---|
| S.T.A.L.K.E.R. 2: Heart of Chornobyl | Working |
| God of War Ragnarök | Working |
| Death Stranding 2: On the Beach | Working |
| Clair Obscur: Expedition 33 | Working |
| The Last of Us Part II Remastered | Working |
| Resident Evil Requiem | Working |
| Assassin's Creed IV: Black Flag | Working |
| PRAGMATA | Working |
| Cyberpunk 2077 | Working |
| Alan Wake 2 | Working |
| Dragon's Dogma 2 | Working |
| The Blood of Dawnwalker | Maybe |
| Starfield | Working |
| Star Wars Outlaws | Working |
| Marvel's Spider-Man 2 | Working |
| Mortal Shell II | Working |
| Resonance: A Plague Tale Legacy | Working |
| Black Myth: Wukong | Working |
| Assetto Corsa Rally | Working |
| Indiana Jones and the Great Circle | Working — launch with `+r_allowBlackListedLayers 1` so ReShade can load through Vulkan |
| Hell Is Us | Working |
| Silent Hill 2 | Working |
| Forza Horizon 6 | Working |
| Assassin's Creed Shadows | Working |
| Stellar Blade | Working |
| Doom the Dark Ages | Working — launch with `+r_allowBlackListedLayers 1` so ReShade can load through Vulkan |
| Horizon Forbidden West | Working |
| 007 The First Light | Working |
| Avatar: Frontiers of Pandora | Working |
| Borderlands 4 | Working |
| Payday 3 | Working |
| Ghost of Tsushima | Working |
| Crimson Desert | Maybe |
| Gothic 1 Remake | Working |
| Jusant | Working with HDR fix |
| Hogwarts Legacy | Working with HDR fix |
| Mafia: The Old Country | Working with HDR fix |
| Dying Light: The Beast | Working |
| Onimusha: Way of the Sword | Working |

These are the games personally tested with this fork; this is not a claim of
universal compatibility. Results may vary with the game version, DLSS and
Streamline versions, GPU, drivers, and configuration.

**Working with HDR fix** means selecting **Automatic Guard + UI Composition
(HDR compatibility)** if the HDR issue occurs. If that mode introduces an
artifact on a HUD/UI element, switch back to **Native**.

## Known Multiplier Behavior

| Game | Reaches | Notes |
|---|---|---|
| Cyberpunk 2077 | 6x | Has its own 2x/3x/4x selector; the addon can force beyond it |
| Deep Rock Galactic | 6x | FG is on/off only, so the addon drives the count entirely. Needs a modern `nvngx_dlssg.dll` (see below) |
| Grand Theft Auto V Enhanced | 4x | Genuine ceiling — its bundled `sl.dlss_g` 2.9.1.0 clamps to 3 generated frames |
| S.T.A.L.K.E.R. 2: Heart of Chornobyl | 4x | Uses both a bundled snippet and an opaque NVIDIA OTA provider. The addon patches both, bypasses Streamline's stale Ada limit, and exposes 3x/4x through the native menu |

Other titles may work, but compatibility should be evaluated per game and
runtime version.

## Requirements

- **GeForce RTX 40-series.** For the separate RTX 30-series solution, see
  [RTX 30-series support](#rtx-30-series-support) below.
- ReShade with addon support (this is an `.addon64`, not an effect).
- A game shipping DLSS frame generation via Streamline, with a reasonably modern
  `nvngx_dlssg.dll` (310.x). Games still on the DLSS 3 snippet (3.5.x) contain no
  multi-frame code at all and need a newer one dropped in beside the executable.
  When an update is needed, use the latest
  [`nvngx_dlssg.dll` available from TechPowerUp](https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/).

## Usage

1. Install [ReShade](https://reshade.me/) with addon support, or install the
   appropriate [RenoDX](https://github.com/clshortfuse/renodx) mod for the game.
2. Download the [latest release from this fork](../../releases/latest).
3. Place `renodx-mfgunlock.addon64` in the ReShade addon location used by the
   game. This is commonly the directory containing the game executable, but a
   game-specific RenoDX package may use its own addon folder.
4. Use the latest
   [`nvngx_dlssg.dll` available from TechPowerUp](https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/).
   Back up the DLL bundled with the game before replacing it.
5. Launch the game and select the desired Multi Frame Generation multiplier
   directly from the game's graphics settings. If the game only provides an
   on/off Frame Generation option, use **Force frame multiplier** in the
   ReShade **MFG Unlock** addon panel instead.

The latest `nvngx_dlssg.dll` is the normal recommendation when this addon is
used by itself. When using it together with RenoDX DLSS5, first read the
version-specific guidance below instead of mixing individual DLLs from
different packages.

For NVIDIA Dynamic MFG, replacing only `nvngx_dlssg.dll` is not sufficient.
The game must load a complete Streamline runtime that implements Dynamic MFG;
VSync and frame-limiter support for Dynamic MFG was added in Streamline 2.14.1.

## Using with RenoDX DLSS5

MFG Unlock and the RenoDX DLSS5 addon can work together, but compatibility may
depend on the complete Streamline and NVIDIA NGX/DLSS runtime combination.
Their coexistence should not yet be treated as universal across runtime
versions, games, or load orders.

### Currently reported combinations

| Configuration | Result | Confidence |
|---|---|---|
| Streamline 2.12.129 with the corresponding 310.7.129 NVIDIA DLLs | Working together and individually | Known-good user report |
| Streamline 2.14.0 with 310.9 NVIDIA DLLs | Severe menu slowdown reported in STALKER 2 and Cyberpunk 2077 when both addons were loaded | Under investigation; not confirmed universal |

Special thanks to [mugensc](https://next.nexusmods.com/profile/mugensc) for
reproducing the combined-addon issue, testing both addons separately, and
identifying the 2.12.129 / 310.7.129 combination as a working solution. That
careful isolation is the basis for the compatibility guidance in this section.

The report above establishes a useful workaround, but it does not prove that
Streamline 2.14.0 or the 310.9 provider is independently defective. The cause
may involve runtime changes, hook/load order, a local-versus-OTA provider
selection, or an interaction that only occurs when both addons are active.

### Recommended combined setup

1. Follow the RenoDX DLSS5 installation and early-loading instructions. The
   RenoDX documentation may require its DLSS addon to be listed under
   `[ADDON] LoadFromDllMain` in `ReShade.ini`.
2. Keep all Streamline files from one package together. Keep the NVIDIA
   NGX/DLSS DLLs on the matching build; do not update only one DLL in the set.
3. Restart the game after every addon, Streamline, or NVIDIA DLL change.
4. Test MFG Unlock alone, RenoDX DLSS5 alone, and then both together.
5. If the combined configuration falls into single-digit framerates, use the
   known-good 2.12.129 / 310.7.129 set while the newer combination is being
   investigated.

The default runtime-selection mode preserves the game's own OTA policy. Check
the ReShade and Streamline logs to confirm the actual loaded module paths and
versions rather than assuming the DLL beside the executable was selected.

Useful information for a compatibility report:

- Game name and version.
- GPU and driver version.
- MFG Unlock and RenoDX DLSS5 addon versions.
- Versions and paths of `sl.interposer.dll`, `sl.dlss_g.dll`,
  `nvngx_dlssg.dll`, and `nvngx_dlssnr.dll` actually loaded by the process.
- Selected multiplier and whether the problem also occurs at native 2x.
- ReShade and Streamline/DLSS-G logs from the same run.

See the current
[RenoDX installation notes](https://github.com/clshortfuse/renodx/wiki/Mods)
for its addon-specific loading requirements.

## Selecting the Streamline Runtime

The **Streamline runtime selection** control changes only NVIDIA's documented
`slInit` OTA flags and requires a full game restart:

- **Game default** preserves the game's request and is recommended for normal
  use.
- **Prefer local runtime** disables OTA download and downloaded-plugin loading,
  allowing a complete game-folder runtime to be tested.
- **Force NVIDIA OTA runtime** enables both OTA flags.

Some games, including Cyberpunk 2077, can call `slInit` before ReShade performs
its normal addon scan. If the panel says the selected policy did not reach
`slInit`, use **Enable early addon loading for next restart** and restart the
game. This adds the currently loaded addon filename to
`[ADDON] LoadFromDllMain`; it does not replace any DLL. The log must confirm
that the policy reached `slInit` and must report the version actually loaded.

Use a complete, version-matched Streamline package. A lone
`nvngx_dlssg.dll` does not update `sl.interposer.dll`, `sl.common.dll`, or
`sl.dlss_g.dll`, and mixing those versions can cause missing capabilities,
startup failures, severe slowdowns, or misleading test results. The addon does
not bypass Streamline's signature or compatibility validation.

## Verifying Operation

Open ReShade, select the **Add-ons** tab, and open **MFG Unlock**. With DLSS
Frame Generation enabled in the game, check the following:

- The expected renderer is detected.
- The DLSS-G provider was found and its architecture gates were rewritten.
- The temporal fix was applied.
- `slDLSSGSetOptions` and `slDLSSGGetState` were intercepted.
- The DLSS-G runtime status is OK.
- `slDLSSGGetState` telemetry is being sampled without an error. Its
  `numFramesActuallyPresented` value is the count since the game's previous
  state query, not a direct multiplier readout.

The presentation count comes from NVIDIA Streamline's `slDLSSGGetState`. Use the
read-only diagnostic NVAPI snapshot or an external presentation trace to
corroborate the active multiplier. For frame-pacing analysis, inspect actual
display intervals; ordinary application-Present counters may not represent the
final display timing used by DLSS-G.

## Experimental Vulkan Support

Version 0.6 introduces an experimental Vulkan compatibility path. It recognizes
Vulkan NGX providers while keeping the existing renderer-independent Streamline
`slDLSSGSetOptions` and `slDLSSGGetState` integration.

This does **not** add Frame Generation to games that do not already integrate
Streamline DLSS-G. Vulkan support has not yet completed the same game matrix as
Direct3D 12 and should not be considered stable or universal. When testing,
confirm that the overlay reports **Vulkan (experimental)** and use the
verification checklist above.

### Indiana Jones and the Great Circle

Vulkan MFG Unlock support has been tested successfully. ReShade requires the
following game launch option so its Vulkan layer is allowed to load:

```text
+r_allowBlackListedLayers 1
```

Without this option, ReShade—and therefore the addon—may not initialize in the
game.

For Xbox Game Pass / UWP-style installations, see the community installation guide by u/amart565 below.

### UWP / Xbox Game Pass Installation Guide

Installing ReShade addons in some Xbox Game Pass / UWP-style game packages can require additional steps
compared to standard Steam or standalone installations. 

For a detailed walkthrough covering ReShade installation, `gamelaunchhelper.exe`, 
Vulkan setup, launch arguments, addon loading, and MFG Unlock setup in games such as
**Indiana Jones and the Great Circle** and **DOOM: The Dark Ages**, 
see the community guide by [u/amart565](https://www.reddit.com/user/amart565/): > **[Guide to installing ReShade on UWP (Xbox Game Pass) games and getting MFG Unlock working](https://www.reddit.com/r/ReShade/comments/1wd6dyr/guide_to_installing_reshade_on_uwpxbox_game_pass/)**
The guide covers Xbox Game Pass-specific installation steps that are outside the core 
scope of MFG Unlock and may be especially useful when ReShade cannot be installed through the usual executable-selection workflow. 

**Credit:** Huge thanks to [u/amart565](https://www.reddit.com/user/amart565/) 
for testing the Xbox Game Pass / UWP installation path and putting together the detailed community guide.

## Frame-generation Input Quality

The default for new configurations is **Native**, which passes the game's
optional HUD-less and UI tags through unchanged. It is the least-invasive mode
and is recommended for most games. Existing saved selections are preserved.

DLSS-G can receive an optional HUD-less scene plus a UI color/alpha mask so it
does not interpolate the interface as ordinary world geometry. This works only
when those resources obey Streamline's contract: matching output extents,
compatible formats, sufficient alpha precision, premultiplied UI color, and the
same color space/post-processing as final color.

**Automatic Guard + UI Composition (HDR compatibility)** is intended for
HDR-related artifacts in known affected games such as Hogwarts Legacy, Jusant,
and Mafia: The Old Country. If HUD/UI elements show artifacts while it is
selected, switch back to **Native**.

The compatibility mode validates the metadata the addon can observe. Once the
primary swapchain positively reports SDR, the addon requests Streamline's
UI-capable path early because many games call `SetOptions` before their first
resource tags. That request only allocates the capable path: the guard still
forwards optional HUD-less/UI inputs after it has observed a complete,
structurally valid pair. When a pair is invalid, incomplete, or arrives in an
unsafe split transition, the addon clears only those optional tags and lets
DLSS-G use final color. In HDR it uses final color automatically because
Streamline resource tags do not expose enough color-space information to prove
that the HUD-less buffer matches a PQ/scRGB final buffer. The explicit **Force
UI Composition (Advanced)** mode remains available for a game whose HDR
integration has been independently verified.

This can mitigate UI/HUD ghosting, flicker, bright halos, invalid masking, and
composition mismatches in affected integrations. It does not claim that every
artifact has that cause, and it cannot repair incorrect motion vectors, depth,
exposure, camera matrices, distortion data, or pixels produced by the game.

> **UI Composition example:** This community video shows the type of UI/HUD-related
> Frame Generation artifact that UI Composition is intended to mitigate in
> affected integrations: https://www.youtube.com/watch?v=xV_E-cvyu8Q

The guard requests one Streamline temporal reset after an actual HDR,
swapchain, resolution, option, multiplier, or quality-mode transition. It does
not inject continuous resets. Required color, depth and motion-vector tags, the
selected multiplier, the temporal kernel, and presentation pacing are not
rewritten by the guard.

The optional **depth-edge guard** changes Streamline's existing minimum relative
linear-depth separation value. Lower values may improve disocclusion around
nearby objects or screen edges in some games, but the best value is
integration-specific. It is disabled by default and does not add camera-turn
resets or a separate pacing path.

## Experimental Thin-geometry Interpolation

These controls modify separate stages of the DLSS-G kernel pipeline. They do
not change the selected multiplier, presentation pacing, Reflex, Dynamic MFG,
HUD/UI tags, or HDR color handling. They currently require an exactly validated
DLSS-G 310.9.0 or 310.9.1 provider; unknown or changed providers fail closed and
keep the normal kernel path. Changes take effect after restarting the game.

**Intermediate scatter retention (Experimental — Recommended)** and
**Validated warp blend (Experimental — Recommended)** are enabled together by
default when no saved settings exist. They remain independently selectable,
and existing explicitly saved choices are preserved.

Intermediate scatter retention relaxes one motion-consistency
rejection while DLSS-G constructs motion vectors for intermediate generated
frames. The kernel's separate depth-mismatch test remains active. This can
preserve more useful motion for fences, wires, foliage, small objects, character
outlines, and weapon edges. Because retaining additional motion can also retain
an incorrect vector, disable it if a particular game develops trails, ghosting,
stretched pixels, or worse disocclusion artifacts.

Validated warp blend is a separate later-stage experiment. It checks
warped-coordinate bounds, invalid-vector sentinels,
finite color values, and agreement between two candidates before gradually
increasing how strongly accepted warped color is used. It may reduce flicker or
the breakup of thin moving detail, but can increase temporal persistence or
ghosting in some scenes. This implementation was informed by Tony Joaca's public
DLSSG-Transfusion `qualityValidWarp` work, but is independently implemented and
intentionally uses additional conservative validation rather than copying its
complete behavior.

The two options are deliberately independent: **Intermediate scatter
retention** changes which motion information survives during intermediate-frame
construction, while **Validated warp blend** changes how accepted candidates
are blended later. For troubleshooting, test one option at a time and restart
between changes.

**Previous-to-current scatter retention** remains available only as an advanced
research control. It changes a different rejection path between real frames,
was unstable in initial game testing, and is disabled by default. It is not
recommended for normal use.

## Dynamic Multi Frame Generation

**Use NVIDIA Dynamic MFG** requests Streamline's native
[`DLSSGMode::eDynamic`](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#63-enabling-dynamic-multi-frame-generation).
Dynamic MFG in this release requires the exact validated stack:

- **DLSS-G 310.9.1**
- **Streamline 2.14.1**
- NVIDIA display driver **595.41 or newer**, as required by NVIDIA's current
  Dynamic MFG integration guide
- A compatible NVIDIA driver/runtime that reports
  `DLSSGState::bIsDynamicMFGSupported = eTrue`
- Direct3D 12; NVIDIA currently does not expose Dynamic MFG for Vulkan

The active DLSS-G provider—not an addon frame scheduler—selects the multiplier
and owns its pacing and hysteresis. In Dynamic mode, `numFramesToGenerate` is
ignored. A target of `0` follows the refresh rate of the display containing the
game window; with VSync off, a nonzero `dynamicTargetFrameRate` requests that
output target.

VSync is **not** a universal requirement for Dynamic MFG. When VSync is active,
Streamline ignores the numeric Dynamic target and instead aims near the active
display refresh for tear-free presentation. Users who are not using G-SYNC
through the NVIDIA driver do not need to enable VSync solely because Dynamic
MFG is enabled. If VSync is used with Dynamic MFG, use the validated 310.9.1 +
2.14.1 stack, check that the addon reports VSync capability, and avoid a path
that cannot reach Independent Flip; NVIDIA warns that such a path can add high
latency.

The optional **Advanced: cap application-rendered FPS with Reflex** setting is
off by default. It changes `ReflexOptions::frameLimitUs`, which limits source
frames rendered by the game; it is not a final displayed-FPS control and must
not be treated as a workaround for VSync ignoring the Dynamic target. When the
advanced cap is disabled, the game-owned Reflex options pass through unchanged.

The addon queries `DLSSGState::bIsDynamicMFGSupported` on D3D12 whether or not
the option is already enabled, using addon-owned v4 storage so games compiled
against older state structures are not written past their ABI. Providers that
reject the newer state ABI are retried only a bounded number of times. Dynamic
is attempted only after the loaded versions and capability bit are confirmed.
Transient initialization failures are retried on later game-side SetOptions
calls; structural rejection fails safely to the game's fixed mode. The panel
reports pending, active, rejected, version-mismatch and VSync-capability states
separately. Renderer eligibility follows the selected primary swapchain rather
than whichever temporary or auxiliary ReShade device initialized last. After
changing Dynamic settings, toggle Frame Generation off/on in the game so it
submits a fresh SetOptions call.

## Version Matrix and Advanced Runtime Setup

The table separates general fixed-MFG compatibility from features that depend
on the new Dynamic ABI. “Validated” means the listed path has been exercised;
it is not a universal claim for every game or presentation setup.

| DLSS-G | Streamline | General addon / fixed MFG | Automatic Guard | UI Composition | Dynamic MFG | VSync / G-SYNC notes |
|---|---|---|---|---|---|---|
| 310.9.0 | 2.12.x | Validated legacy/compatibility path | Validated | Not release-validated on this older wrapper | Not supported by this release | Keep the game's established sync path; do not assume the 2.14.1 Dynamic/VSync behavior |
| 310.9.0 | 2.14.0 / internal RC builds | Fixed MFG may work, including NVIDIA App override builds | Not release-validated as a complete combination | Not release-validated | The underlying runtime may expose Dynamic without VSync when its capability bit is true, but this addon does not advertise this combination as supported | Do not assume 2.14.1 VSync/frame-limiter behavior; use the exact current stack below for release testing |
| 310.9.1 | 2.14.1 | Validated current path | Validated | Validated where the game supplies a correct pair; slight provider cost is expected | Supported on D3D12 when the runtime capability bit is true; final in-game release retest pending | VSync optional; with VSync, target follows refresh. G-SYNC remains a driver/display choice; capability and Independent Flip still matter |

### Manually using the current NVIDIA runtime

> [!IMPORTANT]
> **Do not assume that updating the NVIDIA App or display driver installs
> DLSS-G 310.9.1 and Streamline 2.14.1 into a game.** Games normally keep the
> DLLs they ship, while NVIDIA App driver-profile overrides may load a different
> module from an NVIDIA cache. For the supported Dynamic configuration, the
> files currently have to be installed manually and the driver overrides must
> be returned to application-controlled/default behavior.

Dynamic MFG in this release requires the game process to **actually load** both:

- `nvngx_dlssg.dll` **310.9.1**
- the complete matching Streamline **2.14.1** runtime, including at least the
  game's corresponding `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`,
  Reflex/PCL components, and any other Streamline plugins that game requires

Replacing only `nvngx_dlssg.dll` is not sufficient. Likewise, seeing a 2.14.1
DLL in the game directory does not prove that the process selected it.

- Normal users can obtain the current game DLL from the established
  [TechPowerUp DLSS Frame Generation archive](https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/).
- Developers and advanced users can obtain
  [Streamline 2.14.1 from NVIDIA](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1),
  inspect NVIDIA's official
  [Windows DLSS library directory](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel),
  and consult the
  [DLSS 310.9.1 release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1).

#### Required installation and override checklist

1. Back up the original game DLLs.
2. Install `nvngx_dlssg.dll` 310.9.1 manually.
3. Install the **complete, version-matched** Streamline 2.14.1 set. Never mix
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
App override is active. In that situation a 2.14.1 file beside the executable
can be completely ignored. Do not delete NVIDIA cache directories as a normal
installation step; correct the driver profile instead.

Success means the log reports all of the following from the same launch:

```text
observed Streamline DLSS-G wrapper version 2.14.1.0 from ...
verified mapped DLSS-G provider candidate version 310.9.1.0
slDLSSGGetState confirms NVIDIA Dynamic MFG support
NVIDIA Dynamic MFG accepted
```

If VSync is enabled, the last line can still report Dynamic as accepted, but
the numeric `DynamicTargetFPS` is intentionally ignored and the provider aims
near the active display refresh. Streamline 2.14.1 adds supported VSync and
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
| `ThinGeometryIntermediateScatter` | `1` | Experimental recommended default: retains more motion information while constructing intermediate generated frames; keeps the separate depth test and requires the validated full Blackwell path. Disable per game if it adds ghosting or disocclusion artifacts |
| `ThinGeometryValidatedWarpBlend` | `1` | Experimental recommended default paired with Intermediate scatter retention: validates warped candidates before gradually increasing their blend weight; may reduce thin-detail flicker but can increase temporal persistence. Requires a restart |
| `ThinGeometryPreviousScatter` | `0` | Unstable advanced research control for a separate previous-to-current motion-rejection path; not recommended for normal use |
| `ForceMultiplier` | `0` | `0` respects the game's own choice; `2`–`6` requests that exact multiplier, whether it is higher or lower than the game's choice |
| `DynamicMFG` | `0` | Requests native NVIDIA Dynamic MFG only on the validated 310.9.1 + 2.14.1 D3D12 stack after the provider reports support; takes priority over `ForceMultiplier` while active |
| `DynamicTargetFPS` | `0` | Dynamic output target; `0` follows display refresh. With VSync active, Streamline ignores a nonzero value and follows refresh instead |
| `DynamicReflexSourceCap` | `0` | Advanced opt-in source/application frame cap through Reflex; not a final-output target |
| `RaiseFrameCeiling` | `0` | Raises an old Streamline plugin's compiled hard limit to 6x. Off by default because that breaks some games; the stale device-limit bypass needed by STALKER 2 is always applied |
| `RuntimeSelectionMode` | `0` | `0` preserves the game's runtime policy, `1` disables OTA/downloaded plugins to prefer local files, and `2` forces the NVIDIA OTA flags; restart required |
| `HDRCompatibilityMode` | `0` | `0` is **Native** (default for new configurations), `1` forces UI Composition, `2` enables **Automatic Guard + UI Composition (HDR compatibility)**, and `3` enables Final Color Fallback; existing saved values remain unchanged |
| `DepthEdgeGuardLevel` | `0` | Optional depth-edge tuning: `0` keeps the game value; `1`-`4` select progressively lower separation thresholds |

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

## Troubleshooting

### The addon does not appear in ReShade

- Confirm that ReShade was installed with full addon support.
- Confirm that the `.addon64` file is in the addon location used by that game.
- Check the ReShade log for an addon loading or API-version error.

### Only Automatic or 2x appears

- Toggle Frame Generation off and on after the game reaches its graphics menu.
- Confirm that the DLSS-G provider and Streamline hooks are shown as active in
  the MFG Unlock panel.
- Confirm that the game is loading the expected `nvngx_dlssg.dll`, including
  its full path and version in the log.

### 3x/4x appears but generated frames are not confirmed

- Check the `slDLSSGGetState` result and DLSS-G status displayed in the addon.
- Check whether actual presentation telemetry exceeds one.
- Look for Streamline or NGX errors before changing the forced multiplier.

### The image freezes or pacing becomes unusable at 3x/4x

- First leave **Force legacy software flip pacing** disabled with current
  Streamline builds.
- If the image freezes specifically at higher multipliers, enable the
  compatibility option and fully restart the game.
- Measure final presentation pacing with FrameView rather than relying only on
  a Present-based overlay graph.

### Dynamic MFG is not recognized or ignores my target

- Confirm the panel observes **DLSS-G 310.9.1**, **Streamline 2.14.1**, D3D12,
  a driver version of at least **595.41**, and
  `bIsDynamicMFGSupported = true`. A DLL merely present beside the game is not
  proof that Streamline selected it.
- Toggle Frame Generation off/on after changing the addon setting. The addon
  waits for the game's next `slDLSSGSetOptions` call instead of injecting one
  from an unrelated UI thread. The panel keeps the change visibly marked as
  pending until a successful call applies it.
- If VSync is active, a target such as 100 FPS is intentionally ignored by
  Streamline and Dynamic follows display refresh. Disable VSync for a custom
  Dynamic output target; do not use the advanced Reflex source cap as though it
  were a final-output limiter.
- Check NVIDIA App global and per-game DLSS overrides, reset any matching NVPI
  Frame Generation/NGX overrides, and check the addon's Streamline
  runtime-selection mode. Apply the profile and restart fully after changes.
- If a 2.14.1 file is present but the panel reports 2.14.0, select **Prefer
  local runtime**. If the policy did not reach `slInit`, enable the panel's
  early-addon-loading option and restart again. Presence on disk is not proof
  that Streamline selected that module.
- Dynamic remains unavailable on Vulkan. Fixed MFG continues to work there.

### Performance collapses when RenoDX DLSS5 is also installed

- Follow the [Using with RenoDX DLSS5](#using-with-renodx-dlss5) section.
- Test each addon individually and restart between tests.
- Use a complete matched runtime set; do not replace only one Streamline or
  NVIDIA DLL.
- Record the actual loaded module paths because an OTA provider may override a
  local DLL.

### Ghosting, flicker, or edge artifacts remain

- First compare native 2x with the addon completely removed and restart the
  game. Artifacts that remain are part of the game's native DLSS-G integration.
- Start with **Native**. For known HDR-related issues, try **Automatic Guard +
  UI Composition (HDR compatibility)**; if HUD/UI elements then show artifacts,
  switch back to **Native**. Use **Force UI Composition** and **Final Color
  Fallback** only as controlled A/B comparisons.
- Restart and compare **Prefer full Blackwell framework kernels** on and off.
  Off uses the release-0.7 midpoint correction as the control path.
- Test the optional depth-edge levels one at a time and fully recheck pacing;
  leave the setting off if it does not produce a repeatable visual improvement.
- The addon cannot reconstruct missing or incorrect motion vectors, depth,
  exposure, distortion data, or camera matrices supplied by the game.

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
in [`addon.cpp`](src/addons/mfgunlock/addon.cpp).

## Release Validation

Code-side release checks use MSVC Release builds, native static analysis, and
the tests in `tests/`. The project source is warning-clean in the latest check;
the analyzer reports only existing warnings in external ReShade/Streamline
headers. The final manual gate used a controlled STALKER 2 presentation trace.

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

Use
[`Capture-STALKER2-FramePacing.ps1`](src/addons/mfgdiagnostics/Capture-STALKER2-FramePacing.ps1)
for three repeated 45-second runs of each case: native 2x without the addon,
addon loaded at native 2x, addon 3x, and addon 4x. Keep the same warmed-up save,
camera route, resolution, cap, HDR, VSync/G-SYNC state, driver, and runtime DLLs.
Remove the diagnostic companion addon for these performance runs. Each capture
records PresentMon data, an NVAPI before/after snapshot, and a version/hash
inventory of relevant loaded modules; the analyzer reports display/application
interval distributions, robust outliers, Present API time, available GPU and
instrumented latency fields, and only clearly labeled heuristic generated-frame
cadence.

## RTX 30-series support

RTX 30-series support is now available through
[sdli1995's separate `dlssg_for_sm86` project](https://github.com/sdli1995/dlssg_for_sm86).
It provides a dedicated SM86 backend and proxy runtime instead of relying on the
Ada/Blackwell kernels shipped in the standard DLSS-G runtime.

The project's first milestone documents validation on an RTX 3080 Ti with
Direct3D 12, including native 2x/4x operation in Black Myth: Wukong and
Cyberpunk 2077. Follow that repository's installation, compatibility, and
runtime-version instructions for RTX 30-series use.

`dlssg_for_sm86` is an independent implementation and is not bundled with or
maintained by this fork. MFG Unlock itself remains targeted at RTX 40-series
GPUs.

## Building

The addon is built as part of a [RenoDX](https://github.com/clshortfuse/renodx)
tree, which supplies ReShade, ImGui, Detours, and the NGX/Streamline headers.

```bash
git clone --recursive https://github.com/clshortfuse/renodx
cp -r src/addons/mfgunlock <renodx>/src/addons/
cd <renodx>
cmake --preset vs-x64
cmake --build build.vs --config Release --target mfgunlock
```

The build globs `src/**/**/addon.cpp`, so no CMake changes are needed. The output
is `build.vs/Release/renodx-mfgunlock.addon64`.

Prebuilt binaries are attached to [Releases](../../releases).

The separate, read-only diagnostic addon and capture tools used for Streamline
input and PresentMon analysis are documented in
[`src/addons/mfgdiagnostics/README.md`](src/addons/mfgdiagnostics/README.md).
They are developer tools and are not required for normal use.

## Credits

- [dashdogy/RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock)
  provided the foundational reverse engineering and original working ASI
  implementation. Dashdogy diagnosed the midpoint compaction bug, demonstrated
  the corrected slot-9 temporal program, established the verified
  Streamline/NGX interception strategy, and showed how to apply the fix only to
  mapped process memory without modifying NVIDIA DLLs on disk.
- Dashdogy's project is published under the
  [MIT License](https://github.com/dashdogy/RTX40MFG-Unlock/blob/main/LICENSE).
  The implementation in `midpoint.hpp` remains independently written for the
  ReShade-addon format and was verified by reproducing the original patcher's
  output digest byte-for-byte.
- [Dreamt](https://github.com/ImDreamt) created the original ReShade/RenoDX addon
  adaptation and repository from which this project is forked.
- [sdli1995](https://github.com/sdli1995) developed the separate
  [`dlssg_for_sm86`](https://github.com/sdli1995/dlssg_for_sm86) implementation
  that brings DLSS-G multi-frame generation to supported RTX 30-series/SM86
  configurations.
- [Matias Lombo](https://github.com/matiasLombo/mfg-unlock) identified and
  validated the benefit of rebuilding DLSS-G's Blackwell framework kernels for
  Ada, including the motion-vector estimate, inpaint, and inpaint-decision
  stages. This fork's experimental full-kernel path follows his proven
  precompiled-cubin, exact-fingerprint, in-place replacement method; its
  release payload table is generated with his `rebuild_cubins.py` workflow.
- Tony Joaca, author of DLSSG-Transfusion, publicly identified
  `Kernel_BlendCandidatesFused` as the useful intervention point behind his
  `qualityValidWarp` quality option. That research informed this fork's
  separately implemented and more conservative **Validated warp blend**
  experiment. No code or binary payload from DLSSG-Transfusion is included.
- The **Intermediate scatter retention** analysis and experimental `+120`
  motion-consistency variant were developed independently in this fork. The
  underlying DLSS-G kernels remain NVIDIA technology and are not claimed as
  original project code.
- Special thanks to [mugensc](https://next.nexusmods.com/profile/mugensc) for the
  RenoDX DLSS5 compatibility testing and known-good runtime combination.
- Special thanks to Artur from DLSS Enabler for the valuable debugging insights
  during the investigation of the Hogwarts Legacy HDR + Frame Generation issue,
  which helped lead to the fix included in this fork.
- [u/amart565](https://www.reddit.com/user/amart565/) tested and documented the
  ReShade + MFG Unlock installation workflow for Xbox Game Pass / UWP-style game packages,
  including Vulkan titles such as Indiana Jones and DOOM: The Dark Ages.
  See the [community installation guide](https://www.reddit.com/r/ReShade/comments/1wd6dyr/guide_to_installing_reshade_on_uwpxbox_game_pass/).
- Built on [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse, and
  [ReShade](https://github.com/crosire/reshade) by crosire.

## Disclaimer

Not affiliated with or endorsed by NVIDIA. This modifies process memory of a
running game; use it on your own hardware at your own risk, and expect anti-cheat
in multiplayer titles to object. Results on hardware NVIDIA did not ship this
feature for are to be judged by eye.

## Licence

MIT — see [LICENSE](LICENSE).
