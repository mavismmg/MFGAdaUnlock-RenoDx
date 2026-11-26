# MFG Unlock

<p align="center">
  <a href="https://ko-fi.com/mavismmg"><img src="https://img.shields.io/badge/Support%20MFG%20Unlock%20on-Ko--fi-FF5E5B?style=for-the-badge&amp;logo=ko-fi&amp;logoColor=white" alt="Support MFG Unlock on Ko-fi" height="36"></a>
</p>
<p align="center">
  MFG Unlock is free. If it gave your GPU more frames, a
  <a href="https://ko-fi.com/mavismmg"><b>Ko-fi</b></a> helps keep the fixes and new game support coming. Thank you!
</p>

> [!NOTE]
> ### New in 1.12.1
>
> - **DLSS 5 with less ghosting:** camera cuts reset NR's history, the HUD no longer
>   drags with the camera in games that give an image without it, and a new
>   **Protect silhouettes** option softens trails at a moving character's outline.
> - **The game's own motion before Present** in games with their own Streamline:
>   about half the cost and less flicker (CONTROL: 10.6 ms to 5.2 ms).
> - **DLSS 5 with XeMFG**, a **GPU budget** for NR, and the runtime checked before it loads.
> - Fixes: a GPU hang in CONTROL Resonant when frame generation was toggled in the menu,
>   and a notice when another add-on (GITC Uplift) takes the swap chain from Add frame generation.
>
> See the [1.12.1 release notes](docs/releases/1.12.1.md) (and the
> [1.12.0 release notes](docs/releases/1.12.0.md) for DLSS 5 itself).

> [!IMPORTANT]
> ### 1.10.0: XMX is now called XeMFG DLSS-G
>
> The method called **XMX** until 1.9.0 is now **XeMFG DLSS-G**. The panel's
> three methods are now:
>
> | Method | Generates the frames | Presents and paces them |
> |---|---|---|
> | **DLSS-G** | NVIDIA's generator | NVIDIA's DLSS-G (Streamline) |
> | **XeMFG** | Intel's XeFG multi-frame model | XeFG's own presenter |
> | **XeMFG DLSS-G** (was XMX) | Intel's XeFG multi-frame model | NVIDIA's DLSS-G (Streamline) |
>
> **Why the change:** XMX is the name of Intel's matrix hardware (the units of Arc
> GPUs), not of a frame generator. In the OptiScaler community the old name made
> people think of Intel hardware or of a separate generator, when the mode is the
> same XeFG network as XeMFG, presented and paced by NVIDIA's DLSS-G (Reflex, flip
> metering). The new name says both parts.
>
> The INI keys keep their names (`[DLSSG] XmxGenerator`, `XmxVulkan`, ...), so
> existing settings still apply. Older release notes keep the old name.

> [!NOTE]
> ### New in 1.10.0
>
> - **Frame generation in games with DLSS Super Resolution only** (Kingdom Come:
>   Deliverance II, Red Dead Redemption 2 in DX12 and Vulkan): turn on **Add frame
>   generation**; MFG Unlock starts NVIDIA's Streamline itself, for DLSS-G or XeMFG
>   DLSS-G, with a HUD fix in DX12. See [DLSS frame generation in games without
>   it](docs/add-frame-generation.md#dlss-frame-generation-in-games-without-it).
> - **XeMFG DLSS-G in Vulkan games** (DOOM: The Dark Ages), with **Pacing in Vulkan
>   games** so the frame rate holds when the GPU is at its limit.
> - **The panel in six languages** and a new General page.
> - RTX 20/30 diagnostics, a crash record, a compatibility matrix.
>
> See the [1.10.0 release notes](docs/releases/1.10.0.md).

> [!NOTE]
> ### New in 1.9.0: NVIDIA's frame generation on RTX 20/30
>
> DLSS Frame Generation now runs on RTX 20 and 30 GPUs with NVIDIA's own generator, not
> only with XeMFG DLSS-G: the addon prepares NVIDIA's GPU code for these cards (from
> [nefh's MFGAmpereUnlock-RenoDx](https://github.com/nefh/MFGAmpereUnlock-RenoDx)) and
> adds itself to ReShade's early loading, which they need. On RTX 50, Dynamic MFG can be
> turned on again (confirmed on an RTX 5090 D). The panel shows where each part of frame
> generation came from and warns about duplicate addon files. See the [1.9.0 release notes](docs/releases/1.9.0.md) and
> [RTX 20 and 30 GPUs](docs/architecture.md#rtx-20-and-30-gpus).
>
> ### 1.8.6: XeMFG DLSS-G multi-frame in games with jittered motion vectors
>
> XeMFG DLSS-G generates 3x–6x in games whose motion vectors carry the camera jitter, such as
> Death Stranding 2 and Horizon Zero Dawn Remastered. See the
> [1.8.6 release notes](docs/releases/1.8.6.md); for 1.8.5 (Dynamic MFG ceiling) see the
> [1.8.5 release notes](docs/releases/1.8.5.md).

> [!TIP]
> ### New in 1.8: use XeMFG DLSS-G whenever the game has frame generation
>
> **XeMFG DLSS-G** runs Intel's multi-frame model **inside DLSS-G's own presentation**, without
> CUDA and without Intel's swap chain. **It is our recommendation for every game with
> native DLSS frame generation**, because it is more stable and more compatible than the
> XeMFG backend. Keep the XeMFG backend for games with no native frame generation.
>
> In our Crimson Desert test, the image looked about the same as XeMFG with CUDA,
> and measured latency was much lower: **42 ms against 75 ms at 6x**. The difference
> could be felt on the input.
>
> The game's multiplier menu and the Reflex FPS cap work, and so does Alt+Tab. It is
> still experimental, so **please send your reports**. Select **XeMFG DLSS-G**
> in the panel and restart the game: [how to use it, and the measurements](docs/xemfg.md#xemfg-dlss-g-mode).

> **New in 1.5.0: the XeMFG backend.** One `.addon64` now contains two
> frame-generation backends. **DLSS-G** (unchanged) unlocks NVIDIA multi-frame
> generation on RTX 40-series. **XeMFG** runs Intel XeSS frame generation with
> a multi-frame model on **RTX 20/30/40/50**. It is fed by the inputs the game
> already produces for DLSS-G, by its own XeSS-FG, or by FSR 3.1 frame
> generation. No OptiScaler or ASI loader is needed; Intel's runtime files are
> installed from the addon panel. See [XeMFG backend](docs/xemfg.md#xemfg-backend) for the
> tutorial, the [XeMFG tested games](docs/games.md#xemfg-backend-tested-games) and the
> [1.5.0 release notes](docs/releases/1.5.0.md).

> Release **1.4.5** strengthens kernel and binary-patch identity, rollback,
> allocation lifetime, hook error handling and latency-trial measurements.
> **Local Stable geometry + V2 Compatibility inpaint**, native pacing and the
> existing HUD/UI behavior are retained. See the [1.4.5 release notes](docs/releases/1.4.5.md).
> In-game performance and image-quality gains have not been established.

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

Later releases added, also by [mavismmg](https://github.com/mavismmg):

- **XeMFG DLSS-G mode (1.8):** an independently written generator that runs XeFG's own
  context inside DLSS-G's presentation, with a check of Intel's files before it
  hooks anything, and a repeat of the real frame if it fails during play. The
  multi-frame model it runs is the port from dashdogy's MFG-Unlocker. The idea of
  calling XeFG directly comes from FakeMichau's
  [dlssg-to-xefg](https://github.com/FakeMichau/dlssg-to-xefg) and realdody's fork,
  [dlssg-to-xefg-xmx](https://github.com/realdody/dlssg-to-xefg-xmx); see
  [Credits](#credits).
- **Panel (1.8):** a selector for the three modes, a status that names what is
  generating, and a control for every INI setting, enforced by a test.
- **XeMFG stability:** the focus fix for games that stop their Reflex waits during
  a pause (1.7.2), the fix for losing the graphics device after graphics changes
  (1.5.3, 1.5.6), and XeLL frame numbering for fixed Reflex tokens.
- **XeMFG options:** Force game VSync off and the Elden Ring route (1.5.4), the
  frame rate limit to the display refresh and HUD texture handling (1.5.7),
  exclusive fullscreen as borderless (1.5.8), and HUD stabilization (1.7).
- **Vulkan integration (1.7):** the Vulkan layer, host and engine update are
  dashdogy's. The bridge to them, and the fixes found in Indiana Jones and Red Dead
  Redemption 2, are from this fork, and are marked in
  [`vendor/PROVENANCE.md`](src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md).
- **DLSS-G:** live DLSS Super Resolution and Ray Reconstruction presets (1.6), and
  leaving the provider alone on RTX 50 (1.7).
- **XeMFG DLSS-G on more games and GPUs (1.8.3–1.8.6):** XeMFG DLSS-G with Dynamic MFG, XeMFG DLSS-G's
  multi-frame model in games with jittered motion vectors (Death Stranding 2,
  Horizon Zero Dawn Remastered), and the RTX 20/30 integration of the admission
  adapted from nefh's MFGAmpereUnlock-RenoDx.
- **DLSS-G (1.8.4–1.8.5):** a generic multi-frame gate for frame generation files
  without an exact profile, a Dynamic MFG ceiling chosen in the panel without
  changing the driver profile, recognition of Streamline from the NVIDIA app
  override, and a warning when only the driver's 2x file is in use.
- **RTX 20/30 with NVIDIA's generator (1.9):** the integration of nefh's code
  preparation and temporal correction into DLSS-G and XeMFG DLSS-G, with automatic early
  loading; and the Dynamic MFG fix on RTX 50.
- **Diagnostics (1.8.6–1.9):** the frame generation chain card, the read-only
  driver-profile report, the duplicate-addon warning, the `game_xefg_modules`
  profiles and a release script that publishes nothing by itself.
- **DLSS-G in games without it (1.10):** **Add frame generation** starts MFG Unlock's
  own Streamline for games with DLSS Super Resolution only (Kingdom Come: Deliverance II,
  Red Dead Redemption 2, Uncharted, Shadow of the Tomb Raider, Lies of P, Metro Exodus),
  tags the game's DLSS inputs for it and finds the image before the HUD (the **HUD fix**);
  **forced frame generation** adds DLSS-G to a Streamline game that ships without it.
- **XeMFG DLSS-G (1.10):** Vulkan games through dashdogy's Vulkan provider, and
  "free NVIDIA's unused memory" (about 400 MiB at 1440p).
- **Living with other addons (1.10.1–1.11):** one Streamline swap chain under RenoDX's DLSS
  addon, NGX hooks that survive a "DLSS 5" addon removing its own, XeMFG DLSS-G standing
  aside for addons that hook NGX, and an `other_hooks` line in the support report.
- **Performance overlay (1.11):** frame rate, frame times, 1% and 0.1% lows, frame
  generation status, latency and video memory over the game; Dynamic MFG for games that
  never ask the runtime; a hang record and a session notice that knows a normal exit.
- **DLSS 5 neural rendering (1.12):** NVIDIA's NR on the game's frames in D3D12 and
  Vulkan games, after the game's upscaler, before its frame generation or before
  Present, with MFG Unlock's own motion estimation where the game gives none.

These additions extend Dreamt's addon and dashdogy's research; they do not
replace or claim authorship of either original contribution.

If you'd like to support ongoing compatibility work on this fork, you can do so
through [my Ko-fi](https://ko-fi.com/mavismmg).

## Contents

- [Tested Games](docs/games.md#tested-games)
  - [DLSS-G backend](docs/games.md#dlss-g-backend-tested-games)
  - [XeMFG backend](docs/games.md#xemfg-backend-tested-games)
- [Choosing a backend](#choosing-a-backend)
- [Requirements](#requirements)
- [Usage](#usage)
- [Community installer: RenoDX MFG Unlocker](#community-installer-renodx-mfg-unlocker)
- [XeMFG backend](docs/xemfg.md#xemfg-backend)
  - [Installing XeMFG](docs/xemfg.md#installing-xemfg)
  - [XeMFG settings](docs/xemfg.md#xemfg-settings)
  - [Supported inputs](docs/xemfg.md#supported-xemfg-inputs)
  - [Game profiles](docs/xemfg.md#game-profiles)
  - [XeMFG troubleshooting](docs/xemfg.md#xemfg-troubleshooting)
  - [Reporting an XeMFG problem](docs/xemfg.md#reporting-an-xemfg-problem)
- [XeMFG DLSS-G mode](docs/xemfg.md#xemfg-dlss-g-mode)
- [Using with RenoDX DLSS5](docs/xemfg.md#using-with-renodx-dlss5)
- [Selecting the Streamline Runtime](docs/add-frame-generation.md#selecting-the-streamline-runtime)
- [Verifying Operation](docs/add-frame-generation.md#verifying-operation)
- [Experimental Vulkan Support](docs/add-frame-generation.md#experimental-vulkan-support)
- [Frame-generation Input Quality](docs/quality.md#frame-generation-input-quality)
- [Experimental Thin-geometry Interpolation](docs/quality.md#experimental-thin-geometry-interpolation)
- [Dynamic Multi Frame Generation](docs/quality.md#dynamic-multi-frame-generation)
- [Version Matrix and Advanced Runtime Setup](docs/settings.md#version-matrix-and-advanced-runtime-setup)
- [Settings](docs/settings.md#settings)
- [Troubleshooting](docs/troubleshooting.md#troubleshooting)
- [How it works](docs/architecture.md#how-it-works)
- [Release Validation](docs/architecture.md#release-validation)
- [Building](docs/architecture.md#building)
- [Credits](#credits)

## Tested Games

Games reported working with each method. The full tables, with versions, notes and the games still marked
**Maybe**, are in [docs/games.md](docs/games.md#tested-games); every report also goes into the generated
[compatibility matrix](docs/compat/matrix.md). Not a claim of universal compatibility: results depend on the
game version, the DLSS and Streamline files, the GPU, the driver and the settings.

- **DLSS-G (NVIDIA's generator), 51 games:** S.T.A.L.K.E.R. 2: Heart of Chornobyl, God of War Ragnarök, Death Stranding 2: On the Beach, Clair Obscur: Expedition 33, The Last of Us Part II Remastered, Resident Evil Requiem, Assassin's Creed IV: Black Flag, PRAGMATA, Cyberpunk 2077, Alan Wake 2, Dragon's Dogma 2, Starfield, Marvel's Spider-Man 2, Mortal Shell II, Resonance: A Plague Tale Legacy, Black Myth: Wukong, Assetto Corsa Rally, Indiana Jones and the Great Circle, Hell Is Us, Silent Hill 2, Forza Horizon 6, Assassin's Creed Shadows, Stellar Blade, Doom the Dark Ages, Horizon Forbidden West, 007 The First Light, Avatar: Frontiers of Pandora, Borderlands 4, Payday 3, Ghost of Tsushima, Crimson Desert, Gothic 1 Remake, Jusant, Hogwarts Legacy, Mafia: The Old Country, Dying Light: The Beast, Onimusha: Way of the Sword, Monster Hunter Wilds, Star Wars Outlaws, The Sinking City 2, Control Resonant, Phantom Blade Zero, Call of Duty Modern Warfare 4, Kingdom Come: Deliverance II, Red Dead Redemption 2, Uncharted: Legacy of Thieves Collection, Shadow of the Tomb Raider, Lies of P, Metro Exodus Enhanced Edition, Microsoft Flight Simulator 2024, Microsoft Flight Simulator 2020.
- **Add frame generation** (DLSS-G in games with DLSS Super Resolution only), 6 games: Kingdom Come: Deliverance II, Red Dead Redemption 2, Uncharted: Legacy of Thieves Collection, Shadow of the Tomb Raider, Lies of P, Metro Exodus Enhanced Edition.
- **XeMFG (CUDA), 29 games:** Resident Evil Requiem, Monster Hunter Wilds, Dragon's Dogma 2, Onimusha: Way of the Sword, PRAGMATA, Control Resonant, Assassin's Creed IV: Black Flag Resynced, Cyberpunk 2077, The Witcher 3: Wild Hunt (next-gen), Alan Wake 2, The Last of Us Part II Remastered, Mafia: The Old Country, Resonance: A Plague Tale Legacy, Death Stranding 2: On the Beach, Ghostrunner 2, S.T.A.L.K.E.R. 2: Heart of Chornobyl, Warhammer 40,000: Darktide, Black Myth: Wukong, Kingdom Come: Deliverance II, Red Dead Redemption 2, Elden Ring, Metro Exodus Enhanced Edition, Uncharted: Legacy of Thieves Collection, Shadow of the Tomb Raider, Indiana Jones and the Great Circle, DOOM: The Dark Ages, Crimson Desert, Dying Light 2, Titans of The Tide.
- **XeMFG DLSS-G, 37 games:** Resident Evil Requiem, Monster Hunter Wilds, Dragon's Dogma 2, Onimusha: Way of the Sword, PRAGMATA, Control Resonant, Assassin's Creed IV: Black Flag Resynced, Cyberpunk 2077, The Witcher 3: Wild Hunt (next-gen), Alan Wake 2, Stellar Blade, Ghost of Tsushima, Horizon Zero Dawn Remastered, The Last of Us Part II Remastered, Resonance: A Plague Tale Legacy, Death Stranding 2: On the Beach, Ghostrunner 2, Warhammer 40,000: Darktide, Black Myth: Wukong, Kingdom Come: Deliverance II, Red Dead Redemption 2, Uncharted: Legacy of Thieves Collection, Shadow of the Tomb Raider, Silent Hill f, DOOM: The Dark Ages, Arknights: Endfield, Crimson Desert, Phantom Blade Zero, Dying Light 2, Grand Theft Auto 5 Enhanced, Hogwarts Legacy, STAR WARS: Galactic Racer, Call of Duty Modern Warfare 4, The Blood of Dawnwalker, Silent Hill 2 Remake, Gothic 1 Remake, Once Human.

To report a game, use the **Game report** issue form and paste the panel's support report.

## Known Multiplier Behavior

How some games react to multipliers above their own. See [docs/games.md](docs/games.md#known-multiplier-behavior).

## Choosing a backend

| | NVIDIA DLSS-G | Intel XeMFG (CUDA) | XeMFG DLSS-G (experimental) |
|---|---|---|---|
| Who generates the frames | NVIDIA DLSS-G, unlocked to 3x–6x on Ada | Intel's multi-frame model, 2x–6x, on NVIDIA Tensor Cores where available | Intel's multi-frame model, 2x–6x, without CUDA |
| Who presents them | DLSS-G | Intel's swap chain | DLSS-G |
| GPU | GeForce RTX 40-series (RTX 20/30 experimental since 1.9.0) | GeForce RTX 20/30/40/50 | Tested on RTX 40; confirmed on an RTX 3060 Ti |
| Game requirement | DLSS frame generation through Streamline | DLSS-G inputs through Streamline, the game's own XeSS-FG, or FSR 3.1 frame generation (D3D12); DLSS Super Resolution or Ray Reconstruction (Vulkan) | DLSS frame generation in D3D12 |
| Exclusive fullscreen | Works | Not supported directly; can be served as borderless | Not tested |
| Latency | NVIDIA Reflex | Intel XeLL | NVIDIA Reflex |
| Extra files | A modern `nvngx_dlssg.dll` | Intel `libxess_fg.dll` + `libxell.dll`, installed from the panel | All of them: the DLSS-G files and Intel's two |
| Graphics API | D3D12 (Vulkan experimental) | D3D12, Vulkan (experimental, 1.7) | D3D12 |

**Which one to use:** in a game with native DLSS frame generation, prefer **XeMFG DLSS-G**. It is
more stable and more compatible, because the game keeps DLSS-G's presentation. Use the
**XeMFG backend** for games with no native frame generation, through its FSR, DLSS-SR and
Vulkan inputs. This is the project's recommendation, based on the tests so far; see
[XeMFG DLSS-G mode](docs/xemfg.md#xemfg-dlss-g-mode).

All three are in the same `renodx-mfgunlock.addon64`. A new installation
uses **DLSS-G**. Select the mode in the panel under **General → Frame
generation method**, then restart the game. Only one is active per launch. The
**Backend** card at the top of the panel shows what is generating: `DLSS-G`, `XeMFG DLSS-G`,
`XeMFG (CUDA)`, `XeMFG (shaders)` or `XeFG 2x (stock)`.

## Requirements

These requirements are for the **DLSS-G backend**. For XeMFG, see
[XeMFG requirements](docs/xemfg.md#xemfg-requirements).

- **GeForce RTX 40-series or 50-series.** RTX 20 and 30 are experimental since 1.9.0:
  see [RTX 20 and 30 GPUs](docs/architecture.md#rtx-20-and-30-gpus), or use the [XeMFG backend](docs/xemfg.md#xemfg-backend).
- ReShade with addon support (this is an `.addon64`, not an effect).
- A game shipping DLSS frame generation via Streamline, with a reasonably modern
  `nvngx_dlssg.dll` (310.x). Games still on the DLSS 3 snippet (3.5.x) contain no
  multi-frame code at all and need a newer one dropped in beside the executable.
  When an update is needed, use the latest
  [`nvngx_dlssg.dll` available from TechPowerUp](https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/).

## Usage

1. Install [ReShade](https://reshade.me/) with addon support, or install the
   appropriate [RenoDX](https://github.com/clshortfuse/renodx) mod for the game.
2. Download the [latest release from this fork](https://github.com/mavismmg/MFGAdaUnlock-RenoDx/releases/latest).
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
  [Installing XeMFG](docs/xemfg.md#installing-xemfg)).
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

Intel's XeFG model as the frame generator: install, inputs, settings, troubleshooting. See [docs/xemfg.md](docs/xemfg.md#xemfg-backend).

## XeMFG DLSS-G mode

XeMFG DLSS-G: Intel's model inside DLSS-G, in games with DLSS-G. See [docs/xemfg.md](docs/xemfg.md#xemfg-dlss-g-mode).

## Using with RenoDX DLSS5

Running MFG Unlock beside the RenoDX DLSS5 addon. See [docs/xemfg.md](docs/xemfg.md#using-with-renodx-dlss5).

## DLSS frame generation in games without it

Add frame generation: DLSS-G in games that only have DLSS Super Resolution. See [docs/add-frame-generation.md](docs/add-frame-generation.md#dlss-frame-generation-in-games-without-it).

## Selecting the Streamline Runtime

Choosing the game's or the OTA Streamline runtime. See [docs/add-frame-generation.md](docs/add-frame-generation.md#selecting-the-streamline-runtime).

## Verifying Operation

How to confirm frame generation is running. See [docs/add-frame-generation.md](docs/add-frame-generation.md#verifying-operation).

## Experimental Vulkan Support

What works on Vulkan and what is Direct3D 12 only. See [docs/add-frame-generation.md](docs/add-frame-generation.md#experimental-vulkan-support).

## Frame-generation Input Quality

Quality options for the frame generator inputs. See [docs/quality.md](docs/quality.md#frame-generation-input-quality).

## Experimental Thin-geometry Interpolation

Thin-geometry interpolation fixes. See [docs/quality.md](docs/quality.md#experimental-thin-geometry-interpolation).

## Dynamic Multi Frame Generation

NVIDIA's Dynamic MFG with MFG Unlock. See [docs/quality.md](docs/quality.md#dynamic-multi-frame-generation).

## Version Matrix and Advanced Runtime Setup

Runtime versions and advanced setup. See [docs/settings.md](docs/settings.md#version-matrix-and-advanced-runtime-setup).

## Settings

Every setting in the panel and the INI. See [docs/settings.md](docs/settings.md#settings).

## Troubleshooting

Common problems and what to send when reporting one. See [docs/troubleshooting.md](docs/troubleshooting.md#troubleshooting).

## How it works

How MFG Unlock hooks Streamline and NGX. See [docs/architecture.md](docs/architecture.md#how-it-works).

## Release Validation

How releases are validated. See [docs/architecture.md](docs/architecture.md#release-validation).

## RTX 20 and 30 GPUs

DLSS-G and XeMFG DLSS-G on RTX 20 and 30 cards. See [docs/architecture.md](docs/architecture.md#rtx-20-and-30-gpus).

## Building

Building the addon and the XeMFG backend. See [docs/architecture.md](docs/architecture.md#building).

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
- [FakeMichau/dlssg-to-xefg](https://github.com/FakeMichau/dlssg-to-xefg) showed that
  DLSS-G's frame generation can be replaced by calling XeFG's internal functions directly,
  without Intel's swap chain, the idea behind the **XeMFG DLSS-G mode**.
  [realdody/dlssg-to-xefg-xmx](https://github.com/realdody/dlssg-to-xefg-xmx) is the fork
  of that project that this fork studied. Both are GPLv3 and this project is MIT, so they
  were used as a reference only: no code is included, and this fork's implementation and
  the addresses it uses were written and verified independently against `libxess_fg.dll`.
- [nefh/MFGAmpereUnlock-RenoDx](https://github.com/nefh/MFGAmpereUnlock-RenoDx) (MIT, a
  fork of this project) showed how NVIDIA's DLSS-G runtime admits RTX 20 and 30 GPUs. The
  1.8.3 RTX 20/30 path adapts its Streamline and NGX admission, its scoped NVAPI
  architecture answer and its provider minimum (`src/addons/mfgunlock/preada_admission.hpp`).
  Since 1.9.0 its provider preparation, which rewrites NVIDIA's GPU code for sm_86 and
  sm_75, is included in `src/addons/mfgunlock/ampere/` (namespace `mfgunlock::ampere`,
  with its MIT license in `LICENSE.nefh`), and two of its unit tests run in
  `tests/ampere/`. It is what lets NVIDIA's runtime create frame generation on these GPUs.
- [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) documents where the
  NGX core refuses frame generation on RTX 20 and the D3D12 states Streamline hands NGX.
  Its source is GPLv3, so it was used as a reference only: no code is included.
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
