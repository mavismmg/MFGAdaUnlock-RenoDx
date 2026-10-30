# MFG Unlock

> **New in 1.5.0: the XeMFG backend.** One `.addon64` now contains two
> frame-generation backends. **DLSS-G** (unchanged) unlocks NVIDIA multi-frame
> generation on RTX 40-series. **XeMFG** runs Intel XeSS frame generation with
> a multi-frame model on **RTX 20/30/40/50**. It is fed by the inputs the game
> already produces for DLSS-G, by its own XeSS-FG, or by FSR 3.1 frame
> generation. No OptiScaler or ASI loader is needed; Intel's runtime files are
> installed from the addon panel. See [XeMFG backend](#xemfg-backend) for the
> tutorial, the [XeMFG tested games](#xemfg-backend-tested-games) and the
> [1.5.0 release notes](docs/releases/1.5.0.md).

> Release **1.4.5** strengthens kernel and binary-patch identity, rollback,
> allocation lifetime, hook error handling and latency-trial measurements.
> **Local Stable geometry + V2 Compatibility inpaint**, native pacing and the
> existing HUD/UI behavior are retained. See the [1.4.5 release notes](docs/releases/1.4.5.md).
> In-game performance and image-quality gains have not been established.

<p align="center">
  <a href="https://ko-fi.com/mavismmg"><img src="https://img.shields.io/badge/Support%20me%20on-Ko--fi-FF5E5B?logo=ko-fi&amp;logoColor=white" alt="Support me on Ko-fi"></a>
</p>

A [ReShade](https://reshade.me/) addon that enables **DLSS multi-frame generation
(3x / 4x and above) on GeForce RTX 40-series** cards, which NVIDIA ships gated to
RTX 50-series only — and corrects the frame interpolation so the extra frames
carry new motion instead of repeats. With the **XeMFG** backend, it also
provides 2x–6x frame generation through Intel XeSS-FG on RTX 20/30/40/50.

**Release 1.4.4 hotfix:** adds DLSS-G **310.9.16** and Streamline **2.14.16**
compatibility while retaining the previous .1 versions. The new blend-kernel
layout has its own exact fingerprint and register mapping. Offline checks
passed; in-game validation on the new runtime remains pending. See the
[validation record](docs/runtime-310.9.16-validation.md).

The default for a new installation is **Native**, which preserves the game's
existing HUD/UI tags and is recommended for most games. **Automatic Guard + UI
Composition (HDR compatibility)** remains available for known HDR-related
issues and keeps the game's required color, depth and motion-vector inputs
intact.

Nothing in the game installation is modified. Every patch is applied to the
mapped image at runtime. Release 1.4.5 retains module references and keeps the
addon resident after the first observed Present; completely restart the game
to change the addon or provider. Hot unload/reload is unsupported.
**No complete NVIDIA DLL or provider package is redistributed here.**

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
- **Boundary Artifact Mitigation**, an independently developed motion/depth
  confidence guard for silhouette disocclusion, foreground/background bleeding,
  and edge stretching. Balanced is the recommended default for fresh
  configurations; Off and Aggressive remain available for per-game testing.
- Exact DLSS-G 310.9.0/310.9.1/310.9.16 provider and payload validation for the
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

The **XeMFG backend** (release 1.5.0) is a further contribution by
[mavismmg](https://github.com/mavismmg). It builds on the multi-frame XeFG
model port, runtime layout and pacing work from
[dashdogy's MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker) (MIT,
provenance in [`vendor/PROVENANCE.md`](src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md)).
The following parts are new in this fork:

- A native ReShade **Streamline → Intel XeFG bridge** (MIT, independent code)
  that replaces the game's DLSS-G with XeFG in the same `.addon64`, without
  OptiScaler or an ASI loader. XeLL provides latency reduction.
- **Universal operation without per-game code:**
  - declarative profiles (`profiles.json`), each rule with its evidence;
  - depth/jitter conventions learned automatically per executable;
  - Streamline structures from newer SDKs read through their known prefix;
  - late-loaded Streamline interposers;
  - a refusal to hook Streamline 1.x;
  - frame-generation viewport selection by complete inputs.
- **Data corrections:**
  - a GPU pass that removes jitter from motion vectors (this is what made
    Death Stranding 2 work);
  - support for backbuffer subrectangles;
  - FP16/scRGB HDR swapchains converted to HDR10 for XeFG.
- **Input adapters:**
  - games that run their **own XeSS-FG** use the user's 2x–6x multiplier;
  - **FidelityFX FSR 3.1** frame generation can feed XeMFG;
  - an experimental, opt-in **DLSS-SR** input.
- **XeLL frame sequencing** that keeps Intel's latency library in step with
  games that send constant Reflex tokens (the Resonance: A Plague Tale Legacy
  fix).
- A guided **Intel runtime installer** in the panel. It verifies exact file
  hashes, downloads the supported pair from a pinned official Intel commit,
  updates every copy in the game folder and provides rollback.
- **Diagnostics for bug reports:**
  - session and module inventory;
  - XeLL and SDK messages;
  - a compatibility report written beside the game;
  - a field-session replay corpus with a triage tool;
  - a conformance test matrix that covers official Streamline sample versions.

These additions extend Dreamt's addon and dashdogy's research; they do not
replace or claim authorship of either original contribution.

If you'd like to support ongoing compatibility work on this fork, you can do so
through [my Ko-fi](https://ko-fi.com/mavismmg).

## Contents

- [Tested Games](#tested-games)
  - [DLSS-G backend](#dlss-g-backend-tested-games)
  - [XeMFG backend](#xemfg-backend-tested-games)
- [Choosing a backend](#choosing-a-backend)
- [Requirements](#requirements)
- [Usage](#usage)
- [Community installer: RenoDX MFG Unlocker](#community-installer-renodx-mfg-unlocker)
- [XeMFG backend](#xemfg-backend)
  - [Installing XeMFG](#installing-xemfg)
  - [XeMFG settings](#xemfg-settings)
  - [Supported inputs](#supported-xemfg-inputs)
  - [Game profiles](#game-profiles)
  - [XeMFG troubleshooting](#xemfg-troubleshooting)
  - [Reporting an XeMFG problem](#reporting-an-xemfg-problem)
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

The two backends are tested separately. A game listed for one backend has not
necessarily been tested with the other.

### DLSS-G backend tested games

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
| [Monster Hunter Wilds](#monster-hunter-wilds-first-launch-note) | Working |
| Star Wars Outlaws | Working — Dynamic MFG not supported |
| The Sinking City 2 | Working |
| Control Resonant | Working |
| Phantom Blade Zero | Working |

These are the games personally tested with this fork; this is not a claim of
universal compatibility. Results may vary with the game version, DLSS and
Streamline versions, GPU, drivers, and configuration.

**Working with HDR fix** means selecting **Automatic Guard + UI Composition
(HDR compatibility)** if the HDR issue occurs. If that mode introduces an
artifact on a HUD/UI element, switch back to **Native**.

### XeMFG backend tested games

| Game | API | Status |
|---|---|---|
| Resident Evil Requiem | DX12 | Working |
| Monster Hunter Wilds | DX12 | Working |
| Dragon's Dogma 2 | DX12 | Working |
| Onimusha: Way of the Sword | DX12 | Working |
| PRAGMATA | DX12 | Working |
| Control Resonant | DX12 | Working |
| Assassin's Creed IV: Black Flag Resynced | DX12 | Working — a built-in profile turns off native-output retention for this game |
| Cyberpunk 2077 | DX12 | Working — after a game update, run **Install / update files** again (see [below](#the-panel-says-intel-files-are-missing-or-a-different-version)) |
| The Witcher 3: Wild Hunt (next-gen) | DX12 | Working — turn off the game's **VSync** for correct frame pacing. Fixed in 1.5.3: changing path tracing or other graphics settings with frame generation on no longer loses the graphics device |
| Alan Wake 2 | DX12 | Working |
| The Last of Us Part II Remastered | DX12 | Working |
| Mafia: The Old Country | DX12 | Working |
| Resonance: A Plague Tale Legacy | DX12 | Working |
| Death Stranding 2: On the Beach | DX12 | Working — its jittered motion vectors are corrected automatically |
| Ghostrunner 2 | DX12 | Working |
| S.T.A.L.K.E.R. 2: Heart of Chornobyl | DX12 | Working — set the display mode to **Borderless / Windowed Fullscreen** |
| Warhammer 40,000: Darktide | DX12 | Working — fixed in 1.5.1; on 1.5.0, add `[Adapters] FidelityFX=0` to `MFGAdaUnlock.ini` |
| Black Myth: Wukong | DX12 | Working |
| Kingdom Come: Deliverance II | DX12 | Working with the DLSS-SR input: turn it on in the panel (see [Enabling the DLSS-SR input](#enabling-the-dlss-sr-input)) |
| Red Dead Redemption 2 | DX12, Vulkan | DX12: working with the DLSS-SR input (user report; the game has no native frame generation): turn it on in the panel (see [Enabling the DLSS-SR input](#enabling-the-dlss-sr-input)). Vulkan (experimental, 1.7): working with DLSS on, in fullscreen, borderless or windowed; no setting needed |
| Elden Ring | DX12 | Working with the DLSS-SR input and the EldenRingDLSS addon, offline with Easy Anti-Cheat off; turn on **Force game VSync off** in the panel for correct frame pacing (see [Elden Ring](#elden-ring)) |
| Metro Exodus Enhanced Edition | DX12 | Working with the DLSS-SR input: turn it on in the panel (see [Enabling the DLSS-SR input](#enabling-the-dlss-sr-input)) |
| Indiana Jones and the Great Circle | Vulkan | Working (experimental, 1.7): turn DLSS on (Super Resolution or Ray Reconstruction); the game's Frame Generation switch turns XeMFG on and off. Launch with `+r_allowBlackListedLayers 1` so ReShade can load |

These games were tested with the XeMFG backend of this fork. This is not a
claim of universal compatibility. The XeMFG backend needs a game that
provides frame-generation inputs (see [Supported inputs](#supported-xemfg-inputs)).

> [!IMPORTANT]
> **XeMFG does not work in exclusive fullscreen.** Use **Borderless** or
> **Windowed Fullscreen** in the game's display settings. In exclusive
> fullscreen the game runs normally, but XeMFG keeps frame generation off and
> the panel reports *"XeMFG needs a windowed output"*. Many Unreal Engine games,
> such as S.T.A.L.K.E.R. 2, default to exclusive fullscreen. For games without
> a borderless mode, turn on **Exclusive fullscreen as borderless** in the
> panel (**General → Smoothness and display**): XeMFG then serves the game's
> exclusive fullscreen as a borderless window covering the monitor.

> [!IMPORTANT]
> **Capcom / RE Engine games (Resident Evil Requiem, Monster Hunter Wilds,
> Dragon's Dogma 2, Onimusha: Way of the Sword, PRAGMATA):** if you use
> REFramework, use a build that includes the **XeFG fixes**, such as
> [onehoon's REFramework](https://github.com/onehoon/REFramework). Builds without
> them keep reinstalling their XeFG hooks, and GPU utilization drops sharply
> while XeMFG is active.

### Monster Hunter Wilds first-launch note

After installing or updating the addon, or whenever the game rebuilds
`shader.cache2`, allow the shader compilation to finish completely. Then fully
close **Monster Hunter Wilds** and launch it again before evaluating Frame
Generation, image quality, latency, or frame pacing.

Testing during that initial compilation session, without a full game restart,
can result in severely uneven frame pacing. Toggling Frame Generation off and
back on in the in-game menu does not necessarily clear the affected state.

Special thanks to **Darkalibur** for helping identify this first-launch issue,
reproducing the frame-pacing behavior, and testing the restart workaround.

## Known Multiplier Behavior

| Game | Reaches | Notes |
|---|---|---|
| Cyberpunk 2077 | 6x | Has its own 2x/3x/4x selector; the addon can force beyond it |
| Deep Rock Galactic | 6x | FG is on/off only, so the addon drives the count entirely. Needs a modern `nvngx_dlssg.dll` (see below) |
| Grand Theft Auto V Enhanced | 4x | Genuine ceiling — its bundled `sl.dlss_g` 2.9.1.0 clamps to 3 generated frames |
| S.T.A.L.K.E.R. 2: Heart of Chornobyl | 4x | Uses both a bundled snippet and an opaque NVIDIA OTA provider. The addon patches both, bypasses Streamline's stale Ada limit, and exposes 3x/4x through the native menu |

Other titles may work, but compatibility should be evaluated per game and
runtime version.

## Choosing a backend

| | DLSS-G backend | XeMFG backend |
|---|---|---|
| GPU | GeForce RTX 40-series | GeForce RTX 20/30/40/50 |
| Frame generation | NVIDIA DLSS-G, unlocked to 3x–6x on Ada | Intel XeSS-FG with a multi-frame model, 2x–6x |
| Game requirement | DLSS frame generation through Streamline | DLSS-G inputs through Streamline, the game's own XeSS-FG, or FSR 3.1 frame generation (D3D12); DLSS Super Resolution or Ray Reconstruction (Vulkan) |
| Latency | NVIDIA Reflex | Intel XeLL |
| Extra files | A modern `nvngx_dlssg.dll` | Intel `libxess_fg.dll` + `libxell.dll`, installed from the panel |
| Graphics API | D3D12 (Vulkan experimental) | D3D12, Vulkan (experimental, 1.7) |

Both backends are in the same `renodx-mfgunlock.addon64`. A new installation
uses **DLSS-G**. Select the backend in the panel under **General → Frame
generation method**, then restart the game. Only one backend is active per
launch.

## Requirements

These requirements are for the **DLSS-G backend**. For XeMFG, see
[XeMFG requirements](#xemfg-requirements).

- **GeForce RTX 40-series.** For the separate RTX 30-series solution, see
  [RTX 30-series support](#rtx-30-series-support) below, or use the
  [XeMFG backend](#xemfg-backend).
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

## Community installer: RenoDX MFG Unlocker

[RenoDX MFG Unlocker](https://github.com/ExiZz1/RenoDX-MFG-Unlocker) by
[ExiZz](https://github.com/ExiZz1) is a small Windows app that lists this
project's published releases and installs the selected
`renodx-mfgunlock.addon64` next to a game's executable. It finds installed
Steam games automatically. It is an independent community tool, not part of
this project, and its source is published on GitHub.

What it does and does not do:

- It downloads the addon only from this repository's GitHub releases.
- It does **not** install ReShade; install ReShade with addon support first.
- It does **not** install Intel's XeMFG files; for the XeMFG backend, use
  **Install / update files** in the addon panel afterwards (see
  [Installing XeMFG](#installing-xemfg)).
- It places the addon next to the executable. Games whose RenoDX package uses
  its own addon folder still need the addon copied there by hand.
- It replaces an existing `renodx-mfgunlock.addon64` without making a backup.
  Keep only one MFG Unlock addon installed.
- Its executable is not digitally signed, so Windows SmartScreen may warn
  before the first launch.

Version 1.0.0 was reviewed before this section was added: the source and the
published executable contact only the GitHub API and this repository's
release downloads, write only the addon file in the chosen game folder, and
contain no other code than the app and the Microsoft .NET runtime.

## XeMFG backend

XeMFG replaces the game's frame generation with Intel XeSS frame generation
(XeFG), using a multi-frame model, at 2x–6x. It reads the game's existing
frame-generation inputs, so the game must offer DLSS frame generation, its own
XeSS-FG, or FSR 3.1 frame generation. The game's own frame generation is
turned off while XeMFG is active; you keep using the game's menu toggle to
turn frame generation on or off.

**Vulkan (experimental, new in 1.7, on by default):** in Vulkan games XeMFG
runs Intel's XeFG and XeLL on the game's own Vulkan device through a
D3D12-on-Vulkan layer from dashdogy's MFG Unlocker 1.7.0. Its inputs come from
the game's DLSS Super Resolution or Ray Reconstruction, so DLSS must be on. In
games with DLSS frame generation (Indiana Jones) the game's switch turns XeMFG
on and off; in games without it (Red Dead Redemption 2) XeMFG generates frames
whenever DLSS runs. The Vulkan path replaces the game's swap chain, so a game
it has not been tested with can still fail: if a Vulkan game crashes, freezes
or shows a black screen with XeMFG selected, set `[XeFG] Vulkan=0` in
`MFGAdaUnlock.ini` and report it (see [Reporting an XeMFG problem](#reporting-an-xemfg-problem)).
Tested Vulkan games are marked **Vulkan** in the
[XeMFG table](#xemfg-backend-tested-games).

### XeMFG requirements

- GeForce **RTX 20, 30, 40 or 50-series**, D3D12 or Vulkan (experimental).
- ReShade with addon support (or a RenoDX mod for the game).
- Windowed or borderless display. Exclusive fullscreen is not supported with
  XeMFG; by default the game's display mode is kept unchanged and frame
  generation stays off in that mode. The optional **Exclusive fullscreen as
  borderless** setting serves it as borderless instead.
- Intel's `libxess_fg.dll` and `libxell.dll` beside the game executable. The
  panel installs the supported versions for you (step 4 below). That is the
  folder of the running `.exe`, not the installation root: in Unreal Engine
  games it is `<Game>\Binaries\Win64` (S.T.A.L.K.E.R. 2:
  `Stalker2\Binaries\Win64`; Black Myth: Wukong: `b1\Binaries\Win64`), in
  Kingdom Come: Deliverance II it is `Bin\Win64MasterMasterSteamPGO`. The panel
  shows it as **Diagnostics → Intel runtime files → Intel files folder**.
- A game that provides frame-generation inputs; see
  [Supported inputs](#supported-xemfg-inputs).
- **Capcom / RE Engine games with REFramework:** a REFramework build with the
  XeFG fixes, such as [onehoon's REFramework](https://github.com/onehoon/REFramework).
  Without them, GPU utilization drops while XeMFG is active. The panel warns
  when it detects a REFramework build known to have this problem.

### Installing XeMFG

1. Install [ReShade](https://reshade.me/) with addon support, or the game's
   [RenoDX](https://github.com/clshortfuse/renodx) mod.
2. Put `renodx-mfgunlock.addon64` from the
   [latest release](../../releases/latest) in the ReShade addon location for the
   game (usually the folder with the game executable).
3. Launch the game, open the ReShade overlay (**Home**), open the **MFG Unlock**
   panel and set **General → Frame generation method** to **Intel XeMFG**.
   Close the game completely and launch it again.
4. If the panel says the Intel files are missing or a different version, open
   **Diagnostics → Intel runtime files** and select **Install / update files**.
   A helper lists every file it will change. Nothing changes until you confirm.
   The helper then waits for the game to close, backs up the existing files and
   installs the supported pair, downloaded from a pinned commit of Intel's
   official [XeSS repository](https://github.com/intel/xess). It also updates
   other Intel copies in the game folder, so the game cannot load a mismatched
   pair, and provides rollback. Launch the game again afterwards.
5. In the game's graphics settings, set the display mode to **Borderless**
   or **Windowed Fullscreen** (not exclusive fullscreen), and turn **Frame
   Generation** on (DLSS Frame Generation, XeSS Frame Generation, or FSR Frame
   Generation, depending on what the game offers).
6. Choose the multiplier in the panel (**General → Frame multiplier**, 2x–6x). For
   games that run their own XeSS-FG, the panel multiplier replaces the game's.

**Verifying:** **General** shows whether frame generation is active, the
effective multiplier and whether the model and Tensor acceleration are in use.
A first launch after installing or updating may compile shaders and stutter;
restart once before judging smoothness or image quality.

**Removing or switching back:** select **NVIDIA DLSS-G** in the panel and
restart. To remove the Intel files, use the rollback offered by the installer
helper.

### XeMFG settings

The panel saves XeMFG settings to `MFGAdaUnlock.ini` beside the game. Most
users never need to edit it.

| Section / key | Default | Meaning |
|---|---|---|
| `[Backend] Selected` | `DLSSG` | `XeMFG` selects the XeMFG backend; restart required |
| `[XeFG] Multiplier` | `4` | Output multiplier, `2`–`6` |
| `[XeFG] UIComposition` | `0` | `1` composes the HUD from the game's HUD-less image, which avoids HUD interpolation artifacts (the HUD then updates at the real frame rate). Needs the game to provide a HUD-less image |
| `[XeFG] UIStabilization` | `0` | Experimental, for games without a HUD-less image. `1` composes back-buffer pixels that stayed unchanged for a few real frames (static HUD text and icons) over every generated frame instead of interpolating them, which removes their flicker. Moving or semi-transparent HUD elements are still interpolated. In games that provide a HUD-less image it replaces `UIComposition`: the game's HUD inputs are not used, the static HUD is composed and moving HUD stays interpolated at the output rate |
| `[XeFG] XeFGFlipMetering` | `auto` | NVIDIA driver flip metering for generated frames; can also be toggled live in the panel |
| `[XeFG] ForceVSyncOff` | `0` | `1` presents the game's frames with sync interval 0, as with VSync off in the game, for games that offer no VSync option (Elden Ring). Game VSync keeps flip metering and generated-frame pacing from applying; the panel warns when that happens. Also in the panel as **Force game VSync off** |
| `[Latency] Input` / `Output` | `Auto` | Latency route. XeMFG always uses XeLL for output; Reflex input from the game is translated to XeLL |
| `[XeFG] Vulkan` | `1` | Experimental. XeMFG on Vulkan games: Intel's own XeFG/XeLL run on the game's Vulkan device through a D3D12-on-Vulkan layer (from dashdogy's MFG Unlocker 1.7.0). Inputs come from the game's DLSS Super Resolution or Ray Reconstruction, so turn DLSS on; the game's frame generation switch turns XeMFG on and off. `0` leaves Vulkan games untouched. Restart required |
| `[XeFG] VulkanPresentMetering` | `1` | NVIDIA present metering (`VK_NV_present_metering`, the Vulkan form of flip metering) for XeMFG's Vulkan output, where the driver offers it; not used with VSync on. `0` paces with XeLL and the CPU pacer only |
| `[XeFG] VulkanGameLatency` | `0` | Experimental, not yet measured in a game. `1` lets a Vulkan game's own Reflex sleep and simulation markers drive XeLL, instead of sleeping when the game acquires its image |
| `[XeFG] VulkanDynamicMFG` | `0` | Experimental, not yet measured in a game. `1` turns on Dynamic MFG in Vulkan games: the multiplier follows the real frame rate, up to the one set in the panel. `VulkanDynamicTargetFps` (`0`: the FPS limit, else just under the refresh rate), `VulkanDynamicRounding` (`0` at most, `1` nearest, `2` at least), `VulkanDynamicAllow1x` (`1`) and `VulkanDynamicTargetAsCap` (`0`) adjust it |
| `[XeFG] XeFGNativeLatency` | `0` | Maximum frame latency of XeFG's own swap chain in D3D12 games, `1` to `16`, for latency tests. `0` keeps Intel's (at most 3). Not used with `XeFGFlipQueue=1`; applies from the next output |
| `[XeFG] InvertedDepth` / `JitteredMotion` | not set | Manual convention overrides for unusual games. Normally learned automatically (see below) |
| `[Adapters] FidelityFX` | `1` | `0` stops XeMFG from taking over FSR 3.1 frame generation |
| `[Adapters] DlssSr` | `0` | `1` enables the experimental DLSS-SR input (see below) |
| `[Compatibility] RetainNativeOutput` | profile | `0`/`1` overrides whether the native swapchain is retained across output recreation |

`[XeFG.Learned]` is written by the addon itself and records the depth and
motion-vector conventions observed for that exact game executable. Delete the
section to make the addon learn them again after a game update.

### Supported XeMFG inputs

XeMFG chooses its input from what the game actually calls.

| Game provides | How XeMFG uses it | Status |
|---|---|---|
| DLSS frame generation through Streamline | Streamline's depth, motion vectors, HUD-less image, camera constants and Reflex markers feed XeFG; NVIDIA DLSS-G is turned off | Main route; all games in the XeMFG table |
| The game's own XeSS frame generation | The game's XeFG context is kept and uses the panel multiplier | Supported |
| FidelityFX FSR 3.1 frame generation | The FSR swapchain becomes the XeMFG output and FSR's prepare data feeds XeFG. The addon synthesizes latency markers, because FSR provides none | Supported; disable with `[Adapters] FidelityFX=0` |
| DLSS Super Resolution only (no frame generation) | DLSS-SR evaluations provide depth and motion vectors | Experimental, off by default (`[Adapters] DlssSr=1`). NGX has no camera data, so a default projection is assumed and the HUD is interpolated |
| Vulkan: DLSS Super Resolution or Ray Reconstruction | The game's DLSS evaluations provide depth and motion vectors, copied at the frame's present | Experimental, on by default (`[XeFG] Vulkan`). A default projection is assumed and the HUD is interpolated |

**Motion vectors and HDR:** games whose motion vectors include camera jitter
(such as Death Stranding 2) are corrected on the GPU before XeFG. HDR10
output is supported directly. scRGB (FP16) HDR output is converted to HDR10
for XeFG; in scRGB games, HUD composition from a HUD-less image is not
available yet.

### Enabling the DLSS-SR input

Some games offer DLSS Super Resolution but no DLSS frame generation, such as
Kingdom Come: Deliverance II. For those, XeMFG can take its depth and motion
vectors from DLSS-SR. The input is experimental and off by default; the panel
offers it only in such games.

1. Install XeMFG as described in [Installing XeMFG](#installing-xemfg), select
   the XeMFG backend, and restart the game.
2. In the game's graphics settings, select **DLSS** as the upscaler (any
   quality mode, DLAA included) and turn off any frame generation the game
   offers itself.
3. Open the **MFG Unlock** panel. When the game has no frame generation XeMFG
   can use but runs DLSS, **General** says so and shows **Enable DLSS-SR input
   (experimental)**. Select it, or tick **DLSS-SR input (experimental)** under
   **Frame generation**.
4. Restart the game. The setting is read at startup; the panel shows
   *"Restart the game to apply"* until then.

The panel does not offer the input in games with Streamline (DLSS frame
generation), FSR 3.1 frame generation or their own XeSS-FG; those routes give
better results. The same row turns the input off again (restart required).

<details>
<summary>Setting it by hand in <code>MFGAdaUnlock.ini</code></summary>

The panel writes the same setting. `MFGAdaUnlock.ini` is in the folder of the
game's executable (for Kingdom Come: Deliverance II,
`Bin\Win64MasterMasterSteamPGO`). With the game closed, add:

```ini
[Adapters]
DlssSr=1
```

If the file already has an `[Adapters]` section (for example with
`FidelityFX=0`), add only `DlssSr=1` under it. `DlssSr=0` turns it off.

</details>

Limitations: DLSS-SR passes no camera data, so a default projection is
assumed, and the HUD is interpolated with the image (no HUD-less image is
available).

#### Elden Ring

Elden Ring has no DLSS of its own, so XeMFG takes its inputs from the
EldenRingDLSS ReShade addon by masonpetrosky (tested with
[0.4.0](https://github.com/masonpetrosky/EldenRingDLSS/releases/tag/v0.4.0)).
Only its `.addon64` is needed; the rest of its install guide (me3, the renamed
ReShade DLL, the Streamline and DLSS files) does not apply.

1. Download the EldenRingDLSS zip from Nexus Mods and copy the `.addon64` from
   its `EldenRingDLSS` folder into the game's `Game` folder, next to
   `eldenring.exe` and the MFG Unlock addon.
2. Play offline with Easy Anti-Cheat off; ReShade addons do not load under it.
3. Keep EldenRingDLSS running DLSS, with its own experimental frame
   generation off; XeMFG generates the frames.
4. Continue from step 3 of [Enabling the DLSS-SR input](#enabling-the-dlss-sr-input):
   enable the DLSS-SR input in the panel and restart the game.
5. **Force VSync off.** Elden Ring always presents with VSync and has no
   option to turn it off; with it, frame pacing does not apply and generated
   frames are unevenly spaced. In the panel, under **General → Smoothness and
   display**, turn on **Force game VSync off** (`[XeFG] ForceVSyncOff=1`).
   It applies immediately, and **Game VSync** then reads *On (forced off)*.

### Game profiles

XeMFG contains no game-specific code. The few behaviors observed to differ
per game are data in a built-in `profiles.json`, compiled into the addon. Each
rule records its evidence, and the rules apply automatically; nothing needs to
be created or downloaded. Current built-in rules:

- the Steam overlay retains the native swapchain;
- the native swapchain is not retained in AC Black Flag Resynced;
- the observed depth conventions of TLOU2, Alan Wake 2 and Death Stranding 2;
- conflicting frame-generation plugins;
- a REFramework build with a known XeFG hook issue.

An optional `MFGAdaUnlock.profiles.json` in the game folder can add or
override rules for local testing. An invalid file is rejected as a whole and
the addon keeps the built-in rules.

### XeMFG troubleshooting

#### The panel says Intel files are missing or a different version

The panel names each file's problem (for example *libxess_fg.dll: missing* or
*libxell.dll: 1.3.0.5, not the supported 1.3.2.10*) and offers **Install /
update files** right there; the same button is under **Diagnostics → Intel
runtime files**. Game updates and Steam's **Verify integrity of game files**
can restore the game's original Intel DLLs. When the installed pair is not a
supported one, XeMFG does not start and the game runs normally without it. Run
the installer again after every game update.

The files must be in the folder shown as **Intel files folder** (the running
executable's folder, see [XeMFG requirements](#xemfg-requirements)), not in the
installation root. If the installer does not finish:

- close the game completely; the helper waits for the game to exit before it
  changes anything;
- accept the administrator prompt for games under `Program Files`;
- if it still cannot write, copy `libxess_fg.dll` and `libxell.dll` (version
  1.3.1.78 and 1.3.2.10) into the **Intel files folder** yourself, then use
  **Check again**. Other copies the game ships elsewhere are listed there too
  and must be the same versions.

The helper writes a log of every step in the installation's
`.mfg-dependency-backups` folder (`helper-<date>-<id>.log`), or in the temporary
folder when that folder needs administrator rights; its messages name the
file. If it still fails, send that log together with the `runtime_files` and
`startup_rejected` lines of `MFGAdaUnlock-XeMFG.log` (see
[Reporting an XeMFG problem](#reporting-an-xemfg-problem)).

#### Frame generation stays off

- Turn frame generation on in the game's menu. XeMFG follows the game's
  toggle, and many games only enable it during gameplay, not in menus.
- Use **Borderless** or **Windowed Fullscreen**. If the panel says *"XeMFG
  needs a windowed output"*, the game is in exclusive fullscreen. In Unreal
  Engine games the setting is usually called *Fullscreen Mode*; with the game
  closed, `FullscreenMode=1` in the game's `GameUserSettings.ini` also selects
  windowed fullscreen. If the game has no borderless mode, turn on **Exclusive
  fullscreen as borderless** in the panel and switch the game's display mode
  once, or restart the game. The monitor's resolution and refresh rate are not
  changed in that mode; a lower game resolution is scaled to the screen.
- Check **General**: it states the current reason, such as waiting for the
  game to enable frame generation, missing inputs, or unsupported format.
- If the game offers both DLSS and FSR frame generation and the panel never
  leaves *"The game has paused frame generation"* with DLSS Frame Generation
  on, update to 1.5.1 or later. On 1.5.0, add `[Adapters] FidelityFX=0` to
  `MFGAdaUnlock.ini`.
- If you changed the backend, restart the game completely.

#### The game shows a DirectX error or crashes at start

Restore the previous addon from its backup and report the problem with the
files listed below. As a temporary test, add `[Compatibility]
RetainNativeOutput=0` to `MFGAdaUnlock.ini`. This setting fixed AC Black Flag
Resynced, which now has a built-in rule.

#### HUD elements smear or ghost

Enable **Quality → UI Composition** if the game provides a HUD-less image.
Otherwise the HUD is interpolated together with the scene.

#### GPU utilization drops in a Capcom / RE Engine game

This happens with REFramework builds without the XeFG fixes: they reinstall
their XeFG hooks repeatedly. Replace REFramework with a build that includes
the fixes, such as [onehoon's REFramework](https://github.com/onehoon/REFramework),
or remove REFramework to confirm.

### Reporting an XeMFG problem

Attach these files from the game folder:

- `MFGAdaUnlock-XeMFG.log` contains the session header, the input route and
  the reason frame generation was or was not admitted;
- `MFGAdaUnlock-XeMFG-compatibility.json` is written automatically when the
  output closes;
- the text from **Diagnostics → Copy compatibility report**;
- the game's `ReShade.log`.

Also mention the game version, GPU and driver, and whether the problem
appears with the **NVIDIA DLSS-G** backend too.

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
DLSS-G 310.9.0, 310.9.1 or 310.9.16 provider; unknown or changed providers fail closed and
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

### Boundary Artifact Mitigation

**Boundary Artifact Mitigation** conditions this fork's additional
Intermediate Scatter Retention using local motion and processed-depth evidence.
It targets foreground/background bleeding, silhouette stretching, motion
boundaries, and depth-discontinuity artifacts without changing Streamline tags,
the selected multiplier, presentation pacing, Reflex, or Dynamic MFG.

- **Balanced (Recommended)** preserves additional intermediate motion only
  when at least one nearby same-depth sample provides coherent motion support.
  It is the default for fresh configurations.
- **Aggressive** requires stronger multi-neighbor support and stays closer to
  NVIDIA's native rejection behavior near uncertain boundaries. It may reduce
  some boundary persistence, but can sacrifice thin detail or introduce
  flicker in integrations whose depth or motion inputs are noisy.
- **Off** restores the version 0.9 unconditional Intermediate Scatter
  Retention behavior.

The guard and Validated Warp Blend operate at different stages and may be used
together. Existing saved choices, including an explicitly saved Off, are never
silently overwritten. Changing the mode requires a full game restart. Every
variant remains protected by exact provider, payload, kernel-role,
architecture, and slot-size validation; unsupported providers fail closed to a
validated compatibility path or the baseline kernel.

### Local quality path in release 1.4.3

The official release uses **Local Stable geometry and V2 Compatibility inpaint**.
The local geometry path reuses the provider's tile. Confidence history, temporal
inpaint, CUDA/NVAPI launch interception and backend synchronization are disabled.
Saved temporal settings are preserved but ignored; no INI reset is required.

V3 retains luminance-relative validation, local oriented geometry, native warp
anchors and structural hard rejects. Release 1.4.3 interpolates horizontal and
vertical border treatments continuously while preserving the 1.4.2 movement-
magnitude transition. Paired diagonal supports remain limited by their weaker
neighbor. These paths still require exactly validated 310.9.0/310.9.1 providers.

Initial in-game feedback on the border-axis candidate was positive. Comparative
frame-time and latency measurements remain pending. The 1.4.2 control and a
separately identified candidate are retained as research targets. See
[post-1.4.2 research](docs/research/post-1.4.2.md).

The earlier temporal backend is documented as
[historical research](docs/research/temporal-confidence-history.md), not as a
release default. Its previous activation and benefit limitations still apply.

### Reflex / Pacing Lab V3.3

The Latency page includes an opt-in Reflex/Pacing Lab. Its defaults are fully
native: the game controls the Reflex mode, `slReflexSleep` is forwarded
directly, and VRR headroom is disabled. `Off (FG-safe sleep bypass)` keeps
Streamline's internal Reflex mode at Low Latency because submitting mode Off
also disables Frame Generation in some integrations; it removes only the
`slReflexSleep` pacing wait. Markers, frame tokens, `useMarkersToOptimize`, the
PCL hotkey/thread fields and every other game-owned option remain intact. On
and On + Boost still override only the mode. A rejected override is retried
immediately with the exact native options.

The independent VRR headroom control uses the Reflex final/output limiter only
while NVAPI confirms VRR/G-SYNC. It accepts 0.5%-3.0% and defaults to 1.0% when
enabled. The effective limiter is always the strictest of the game limit, the
explicit Output FPS Cap, Latency Guard and VRR headroom; the addon never
relaxes a native cap.

`DXGI Waitable` is an experimental alternative to Reflex sleep for integrations
with uneven native pacing. It never adds the waitable flag or recreates a
swapchain. Activation requires an existing D3D11/D3D12 waitable swapchain,
active 2x-6x MFG, a duplicated wait handle, and 120 unique monotonic frame
tokens with successful native sleep. It then temporarily sets
`MaximumFrameLatency` to 1 and restores the original value on disable, resize,
swapchain replacement or unload. A timeout, invalid handle, concurrent sleep,
bad token sequence or failed DXGI call immediately returns to native sleep.
Automatic Latency Guard actions are suspended while this alternate pacer is
probing or active so two controllers cannot compete.

The in-game cadence figures are application-level diagnostics. They do not
observe every generated/displayed frame; use PresentMon or FrameView for a real
V2/V3.2/V3.3 pacing comparison. No V3.2 image-quality kernel or cubin is changed
by the Pacing Lab.

## Dynamic Multi Frame Generation

**Use NVIDIA Dynamic MFG** requests Streamline's native
[`DLSSGMode::eDynamic`](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#63-enabling-dynamic-multi-frame-generation).
Dynamic MFG accepts the following explicit runtime versions. The new
310.9.16 + 2.14.16 stack has passed offline interface and patch checks;
in-game validation is pending. The previously supported .1 stack is retained:

- **DLSS-G 310.9.1 or 310.9.16**
- **Streamline 2.14.1 or 2.14.16**
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
MFG is enabled. If VSync is used with Dynamic MFG, use a supported 310.9.1/310.9.16 +
2.14.1/2.14.16 stack, check that the addon reports VSync capability, and avoid a path
that cannot reach Independent Flip; NVIDIA warns that such a path can add high
latency.

The optional **Output FPS Cap (Reflex)** setting is off by default. It changes
`ReflexOptions::frameLimitUs` and represents the final/output FPS ceiling: a
value of `120` targets up to approximately 120 displayed FPS. Reflex and the
DLSS-G pacer account for generated frames, so fixed 4x corresponds to roughly
30 game-rendered FPS at that ceiling. Actual output can be lower because of the
game, GPU, display or VSync. When the cap is disabled, the game-owned Reflex
options pass through unchanged.

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
| 310.9.1 | 2.14.1 | Previously validated path | Validated | Validated where the game supplies a correct pair; slight provider cost is expected | Supported on D3D12 when the runtime capability bit is true; final in-game release retest pending | VSync optional; with VSync, target follows refresh. G-SYNC remains a driver/display choice; capability and Independent Flip still matter |

| 310.9.16 | 2.14.16 | Offline patch/restore checks passed; in-game testing pending | In-game validation pending | In-game validation pending | Accepted by the version gate on D3D12; runtime capability is still required; in-game validation pending | Same public Dynamic/VSync interface as 2.14.1; verify capability and presentation behavior in-game |

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
| `DynamicMFG` | `0` | Requests native NVIDIA Dynamic MFG only on the supported 310.9.1/310.9.16 + 2.14.1/2.14.16 D3D12 runtimes after the provider reports support; takes priority over `ForceMultiplier` while active |
| `DynamicTargetFPS` | `0` | Dynamic output target; `0` follows display refresh. With VSync active, Streamline ignores a nonzero value and follows refresh instead |
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

- Confirm the panel observes supported versions: **DLSS-G 310.9.1/310.9.16**, **Streamline 2.14.1/2.14.16**, D3D12,
  a driver version of at least **595.41**, and
  `bIsDynamicMFGSupported = true`. A DLL merely present beside the game is not
  proof that Streamline selected it.
- Toggle Frame Generation off/on after changing the addon setting. The addon
  waits for the game's next `slDLSSGSetOptions` call instead of injecting one
  from an unrelated UI thread. The panel keeps the change visibly marked as
  pending until a successful call applies it.
- If VSync is active, a target such as 100 FPS is intentionally ignored by
  Streamline and Dynamic follows display refresh. Disable VSync for a custom
  Dynamic scheduler target. **Output FPS Cap (Reflex)** is a separate final
  ceiling and does not make an ignored Dynamic target authoritative.
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

RTX 30-series (and, since its 0.4 releases, RTX 20-series) support for NVIDIA's
own DLSS-G is available through
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

- **`dlssg_for_sm86`** in Direct3D 12 games that ship DLSS Frame Generation,
  for NVIDIA's frame-generation model and exclusive fullscreen.
- **The [XeMFG backend](#xemfg-backend)** in games without DLSS Frame
  Generation (it also reads FSR 3.1 and DLSS Super Resolution inputs), or when
  `dlssg_for_sm86` does not support the game.

`dlssg_for_sm86` is an independent implementation (GPLv3) and is not bundled
with or maintained by this fork. The DLSS-G backend of MFG Unlock remains
targeted at RTX 40-series GPUs.

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

Run CTest and see [the research workflow](docs/research/post-1.4.2.md) for CUDA
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
`XEMFG_SL_SAMPLES_ROOT` points at builds made with `tools/sl_sample.py`. The
execution plan and record are in [`docs/plan/`](docs/plan/).

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
- [Tony Joaca](https://github.com/TonyJoaca/DLSSG-Transfusion), author of DLSSG-Transfusion, publicly identified
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
- Special thanks to **Darkalibur** for helping identify and validate the
  `shader.cache2` first-launch frame-pacing issue in Monster Hunter Wilds.
- Special thanks to **harddaysmike** for the Star Wars Outlaws compatibility
  and regression testing.
- [u/amart565](https://www.reddit.com/user/amart565/) tested and documented the
  ReShade + MFG Unlock installation workflow for Xbox Game Pass / UWP-style game packages,
  including Vulkan titles such as Indiana Jones and DOOM: The Dark Ages.
  See the [community installation guide](https://www.reddit.com/r/ReShade/comments/1wd6dyr/guide_to_installing_reshade_on_uwpxbox_game_pass/).
- [dashdogy/MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker) (MIT)
  provided the multi-frame XeFG model port, runtime layout, extra-output pacing
  and optional NVIDIA Tensor route that the XeMFG backend builds on. Exact
  provenance is in
  [`vendor/PROVENANCE.md`](src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md).
- XeMFG uses Intel's [XeSS frame generation and XeLL](https://github.com/intel/xess)
  runtime, installed from Intel's official repository under Intel's terms, and
  AMD's public [FidelityFX API](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
  headers (MIT) for the FSR 3.1 input. No Intel or AMD runtime binary is
  redistributed here.
- NVIDIA's [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) (MIT)
  provides the headers for the Streamline bridge. Its official Streamline_Sample
  builds are part of the XeMFG test matrix. The NGX SDK headers that ship with
  Streamline define the experimental DLSS-SR input.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD
  2-Clause) performs the XeMFG function hooks. Its notice is embedded in the
  addon.
- [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann (MIT)
  writes the XeMFG compatibility report.
- [OptiScaler](https://github.com/optiscaler/OptiScaler) was studied as an
  API, ownership and architecture reference for the XeFG bridge. No GPL
  OptiScaler code is included (see
  [`vendor/PROVENANCE.md`](src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md)).
- [REFramework](https://github.com/praydog/REFramework) by praydog. The XeMFG
  bridge interoperates with REFramework's XeFG monitor in RE Engine games.
  [onehoon's REFramework](https://github.com/onehoon/REFramework) includes the
  XeFG fixes recommended for XeMFG in Capcom games.
- Built on [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse, and
  [ReShade](https://github.com/crosire/reshade) by crosire.

## Disclaimer

Not affiliated with or endorsed by NVIDIA, Intel or AMD. This modifies process memory of a
running game; use it on your own hardware at your own risk, and expect anti-cheat
in multiplayer titles to object. Results on hardware NVIDIA did not ship this
feature for are to be judged by eye.

## Licence

MIT — see [LICENSE](LICENSE).
