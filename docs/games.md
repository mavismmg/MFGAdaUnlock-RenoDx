# Tested games

Part of the [MFG Unlock](../README.md) documentation. Sections: [Tested Games](#tested-games), [Known Multiplier Behavior](#known-multiplier-behavior).

## Tested Games

Since 1.10.0 every report also goes into the generated [compatibility matrix](compat/matrix.md)
(backend, GPU, runtime versions, source and date). To report a game, use the **Game report** issue
form and paste the panel's support report.

The two backends are tested separately. A game listed for one backend has not
necessarily been tested with the other.

### DLSS-G backend tested games

| Game | Status |
|---|---|
| May work with every game | |
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
| Crimson Desert | Working |
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
| Call of Duty Modern Warfare 4 | Working |
| Kingdom Come: Deliverance II | Working — DLSS-G added by MFG Unlock (1.10.0, **Add frame generation**; the game has DLSS Super Resolution only) |
| Red Dead Redemption 2 | Working — DX12 and Vulkan; DLSS-G added by MFG Unlock (1.10.0, **Add frame generation**; the game has DLSS Super Resolution only) |
| Uncharted: Legacy of Thieves Collection | Working — DLSS-G added by MFG Unlock (1.10.0, **Add frame generation**; the game has DLSS Super Resolution only) |
| Shadow of the Tomb Raider | Working — DLSS-G added by MFG Unlock (1.10.0, **Add frame generation**; the game has DLSS Super Resolution only) |
| Lies of P | Working — **Add frame generation**. With RenoDX's DLSS addon, the "Compatibility issue" is fixed in 1.11.0 (awaiting the tester's confirmation) |
| Hi-Fi RUSH | Maybe — **Add frame generation**; the "Compatibility issue" with RenoDX DLSS and DLSS-G's status 0x10 are fixed in 1.11.0, awaiting the tester's confirmation |
| The Medium | Maybe — **Add frame generation**; DLSS-G's status 0x10 is fixed in 1.11.0, awaiting the tester's confirmation |
| Metro Exodus Enhanced Edition | Working — **Add frame generation** (4x). In HDR the crosshair still repeats with the HUD fix |
| Microsoft Flight Simulator 2024 | Working — Dynamic MFG included |
| Microsoft Flight Simulator 2020 | Working — fixed MFG; Dynamic MFG starts since 1.11.0 (awaiting the reporter's confirmation) |
| Ninja Gaiden 4 | Maybe — forced DLSS frame generation needs an addon that moves the game's swap chain into Streamline (RenoDX DLSS) and one matching set of `sl.*.dll` |

These are the games personally tested with this fork; this is not a claim of
universal compatibility. Results may vary with the game version, DLSS and
Streamline versions, GPU, drivers, and configuration.

**Working with HDR fix** means selecting **Automatic Guard + UI Composition
(HDR compatibility)** if the HDR issue occurs. If that mode introduces an
artifact on a HUD/UI element, switch back to **Native**.

### XeMFG backend tested games

| Game | API | XeMFG CUDA | XeMFG DLSS-G | Notes |
|---|---|---|---|---|
| Resident Evil Requiem | DX12 | Working | Working |  |
| Monster Hunter Wilds | DX12 | Working | Working |  |
| Dragon's Dogma 2 | DX12 | Working | Working |  |
| Onimusha: Way of the Sword | DX12 | Working | Working |  |
| PRAGMATA | DX12 | Working | Working |  |
| Control Resonant | DX12 | Working | Working |  |
| Assassin's Creed IV: Black Flag Resynced | DX12 | Working | Working | A built-in profile turns off native-output retention for this game. MFG with XeMFG DLSS-G and DLSS-G since 1.8.4 |
| Cyberpunk 2077 | DX12 | Working | Working | After a game update, run **Install / update files** again (see [below](xemfg.md#the-panel-says-intel-files-are-missing-or-a-different-version)) |
| The Witcher 3: Wild Hunt (next-gen) | DX12 | Working | Working | Turn off the game's **VSync** for correct frame pacing on CUDA. Fixed in 1.5.3: changing path tracing or other graphics settings with frame generation on no longer loses the graphics device |
| Alan Wake 2 | DX12 | Working | Working | XeMFG DLSS-G since 1.8.2 |
| Stellar Blade | DX12 | Not tested | Working | XeMFG DLSS-G since 1.8.3 |
| Ghost of Tsushima | DX12 | Not tested | Working | XeMFG DLSS-G since 1.8.3. Its Streamline (2.4) has no multi-frame generation: 2x only, and a forced 3x or more is not applied |
| Horizon Zero Dawn Remastered | DX12 | Not tested | Working | XeMFG DLSS-G since 1.8.3. It sends jittered motion vectors: Intel's 2x model until 1.8.5, the multi-frame model since 1.8.6 (not retested). Its Streamline (2.2.1) offers 2x only |
| The Last of Us Part II Remastered | DX12 | Working | Working | XeMFG DLSS-G since 1.8.3; in 1.8.2 a saved DLSS preset crashed the game at start |
| Mafia: The Old Country | DX12 | Working | Maybe |  |
| Resonance: A Plague Tale Legacy | DX12 | Working | Working |  |
| Death Stranding 2: On the Beach | DX12 | Working | Working | Its jittered motion vectors are corrected automatically. XeMFG DLSS-G: 3x–6x since 1.8.6 (Intel's 2x model before) |
| Ghostrunner 2 | DX12 | Working | Working |  |
| S.T.A.L.K.E.R. 2: Heart of Chornobyl | DX12 | Working | Maybe | Set the display mode to **Borderless / Windowed Fullscreen** |
| Warhammer 40,000: Darktide | DX12 | Working | Working | Fixed in 1.5.1; on 1.5.0, add `[Adapters] FidelityFX=0` to `MFGAdaUnlock.ini` |
| Black Myth: Wukong | DX12 | Working | Working |  |
| Kingdom Come: Deliverance II | DX12 | Working | Working | XeMFG: with the DLSS-SR input, turn it on in the panel (see [Enabling the DLSS-SR input](xemfg.md#enabling-the-dlss-sr-input)). XeMFG DLSS-G since 1.10.0: turn on **Add frame generation** (MFG Unlock starts Streamline for this game); the **HUD fix** keeps the crosshair and HUD from repeating |
| Red Dead Redemption 2 | DX12, Vulkan | Working | Working | XeMFG: DX12 with the DLSS-SR input (see [Enabling the DLSS-SR input](xemfg.md#enabling-the-dlss-sr-input)); Vulkan (experimental, 1.7) generates with DLSS on, but pacing is poor (see [Red Dead Redemption 2: use DX12](xemfg.md#red-dead-redemption-2-use-dx12)). XeMFG DLSS-G since 1.10.0, DX12 and Vulkan: turn on **Add frame generation** (MFG Unlock starts Streamline for this game). The HUD fix is DX12 only |
| Elden Ring | DX12 | Working | Maybe | XeMFG: with the DLSS-SR input and the EldenRingDLSS addon, offline with Easy Anti-Cheat off; turn on **Force game VSync off** in the panel for correct frame pacing (see [Elden Ring](xemfg.md#elden-ring)). XeMFG DLSS-G: **Add frame generation** (1.10.0) may work with the DLSS addon; not tested yet |
| Metro Exodus Enhanced Edition | DX12 | Working | Maybe | XeMFG: with the DLSS-SR input (see [Enabling the DLSS-SR input](xemfg.md#enabling-the-dlss-sr-input)). XeMFG DLSS-G with **Add frame generation** (1.10.0): works per a tester; a dim, flickering image in HDR was fixed in 1.10.0 and awaits the tester's confirmation |
| Uncharted: Legacy of Thieves Collection | DX12 | Working | Working | XeMFG: with the DLSS-SR input (see [Enabling the DLSS-SR input](xemfg.md#enabling-the-dlss-sr-input)). XeMFG DLSS-G: turn on **Add frame generation** (MFG Unlock starts Streamline for this game) |
| Shadow of the Tomb Raider | DX12 | Working | Working | XeMFG: with the DLSS-SR input (see [Enabling the DLSS-SR input](xemfg.md#enabling-the-dlss-sr-input)). XeMFG DLSS-G: turn on **Add frame generation** (MFG Unlock starts Streamline for this game) |
| Silent Hill f | DX12 | Not tested | Working |  |
| Indiana Jones and the Great Circle | Vulkan | Working | Maybe | XeMFG experimental (1.7): turn DLSS on (Super Resolution or Ray Reconstruction); the game's Frame Generation switch turns XeMFG on and off. XeMFG DLSS-G in Vulkan games since 1.10.0; not tested in this game yet. Launch with `+r_allowBlackListedLayers 1` so ReShade can load |
| DOOM: The Dark Ages | Vulkan | Working | Working | XeMFG DLSS-G in Vulkan since 1.10.0, with **Pacing in Vulkan games** (with path tracing it holds ~120 fps at 4x on an RTX 4070 SUPER, about 20 fps under NVIDIA's generator). XeMFG reads DLSS Ray Reconstruction's inputs. Launch with `+r_allowBlackListedLayers 1` so ReShade can load |
| Arknights: Endfield | Vulkan | Not tested | Working | XeMFG DLSS-G in Vulkan (1.10.0 test build, user report on an RTX 4060 Laptop) |
| Crimson Desert | DX12 | Working | Working |  |
| Phantom Blade Zero | DX12 | Not Working | Working | XeMFG DLSS-G since 1.8.2 |
| Dying Light 2 | DX12 | Working | Working | XeMFG DLSS-G since 1.8.2 |
| Grand Theft Auto 5 Enhanced | DX12 | Maybe | Working |  |
| Titans of The Tide | DX12 | Working | Maybe |  |
| Hogwarts Legacy | DX12 | Maybe | Working |  |
| STAR WARS: Galactic Racer | DX12 | Maybe | Working |  |
| Call of Duty Modern Warfare 4 | DX12 | Maybe | Working |  |
| The Blood of Dawnwalker | DX12 | Maybe | Working |  |
| Silent Hill 2 Remake | DX12 | Not Working | Working |  |
| Gothic 1 Remake | DX12 | Not Working | Working |  |
| Once Human | DX12 | Maybe | Working |  |
| The Medium | DX12 | Not tested | Maybe | XeMFG DLSS-G with **Add frame generation**: in 1.10.x it saw a history reset almost every frame; 1.11.0 fixes DLSS-G's status 0x10, which should be the cause. Awaiting the tester's confirmation |

**XeMFG CUDA** shows the games tested with the XeMFG backend. In the **XeMFG DLSS-G** column,
**Maybe** means it has not been tested in that game yet, so
[your report](xemfg.md#try-it-and-report) helps. These results are not a
claim of universal compatibility. The XeMFG backend needs a game that
provides frame-generation inputs (see [Supported inputs](xemfg.md#supported-xemfg-inputs)).

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
