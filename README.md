# MFG Unlock

A [ReShade](https://reshade.me/) addon that enables **DLSS multi-frame generation
(3x / 4x and above) on GeForce RTX 40-series** cards, which NVIDIA ships gated to
RTX 50-series only — and corrects the frame interpolation so the extra frames
carry new motion instead of repeats.

Nothing on disk is modified. Every patch is applied to the mapped image at
runtime and reverted when the addon unloads. **No NVIDIA files are redistributed
here.**

> [!NOTE]
> The `develop` branch contains experimental work, including the first Vulkan
> compatibility path and additional runtime diagnostics. For the stable build,
> use the latest published release from `main`.

---

## Attribution

This repository is a fork of the [original project](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx)
created by [Dreamt](https://github.com/ImDreamt). Full credit for the original
implementation goes to Dreamt and the contributors already credited in this
repository.

This fork expands the original project with broader game and runtime
compatibility work. Its additions include:

- Support and fixes for **S.T.A.L.K.E.R. 2: Heart of Chornobyl**, including its
  native 3x/4x selector and bundled/OTA provider handling.
- Temporal-patch compatibility with newer 310.9 DLSS-G providers.
- Safer ReShade addon lifecycle handling across temporary device probing and
  addon reloads.
- Bounded background provider discovery, removing continuous module enumeration
  from the active `Present` path.
- Additional compatibility controls for Streamline flip metering and software
  pacing fallback behavior.
- Runtime diagnostics using `slDLSSGSetOptions` and `slDLSSGGetState`, including
  requested multipliers, DLSS-G status, and actual presentation telemetry.
- Experimental Vulkan renderer and NGX provider discovery on the `develop`
  branch.
- Compatibility testing and documentation across the games and runtime
  combinations listed below.

These additions extend Dreamt's work; they do not replace or claim authorship
of the original implementation.

## Contents

- [Tested Games](#tested-games)
- [Requirements](#requirements)
- [Usage](#usage)
- [Using with RenoDX DLSS5](#using-with-renodx-dlss5)
- [Verifying Operation](#verifying-operation)
- [Experimental Vulkan Support](#experimental-vulkan-support)
- [Settings](#settings)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
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

These are the games personally tested with this fork; this is not a claim of
universal compatibility. Results may vary with the game version, DLSS and
Streamline versions, GPU, drivers, and configuration.

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

- **GeForce RTX 40-series.** See [Why not 30-series?](#why-not-30-series) below.
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

`ForceOTAPlugins=0` means this addon does not force OTA loading. It does not
prevent the game itself from requesting an OTA provider. Check the ReShade and
Streamline logs to confirm the actual loaded module paths and versions rather
than assuming the DLL beside the executable was selected.

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

## Verifying Operation

Open ReShade, select the **Add-ons** tab, and open **MFG Unlock**. With DLSS
Frame Generation enabled in the game, check the following:

- The expected renderer is detected.
- The DLSS-G provider was found and its architecture gates were rewritten.
- The temporal fix was applied.
- `slDLSSGSetOptions` and `slDLSSGGetState` were intercepted.
- The DLSS-G runtime status is OK.
- Actual presentation telemetry rises above one when generated frames are
  active.

The presentation count comes from NVIDIA Streamline's `slDLSSGGetState`. For
frame-pacing analysis, use NVIDIA FrameView and inspect
`MsBetweenDisplayChange`; ordinary Present-based counters may not represent the
final display timing used by DLSS-G.

## Experimental Vulkan Support

The `develop` branch includes an experimental Vulkan compatibility path. It
recognizes Vulkan NGX providers while keeping the existing renderer-independent
Streamline `slDLSSGSetOptions` and `slDLSSGGetState` integration.

This does **not** add Frame Generation to games that do not already integrate
Streamline DLSS-G. Vulkan support has not yet completed the same game matrix as
Direct3D 12 and should not be considered stable or universal. When testing,
confirm that the overlay reports **Vulkan (experimental)** and use the
verification checklist above.

## Settings

Written to your `ReShade.ini` under `[RenoDX.MFGUnlock]`:

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Master switch for the whole addon |
| `MaxCount` | `4` | The `DLSSG.MultiFrameCountMax` value reported to the runtime |
| `ForceFlipMeteringOff` | `0` | Normally leave off. Enable only if 3x/4x freezes; this forces Streamline's legacy software pacing fallback and requires a game restart |
| `TemporalFix` | `1` | The interpolation correction. Leave on |
| `ForceMultiplier` | `0` | `0` respects the game's own choice; `2`–`6` forces that multiplier |
| `RaiseFrameCeiling` | `0` | Raises an old Streamline plugin's compiled hard limit to 6x. Off by default because that breaks some games; the stale device-limit bypass needed by STALKER 2 is always applied |
| `ForceOTAPlugins` | `0` | Asks Streamline to load the driver's OTA plugin set. Off by default; see notes |

If a game has its own multiplier selector, leave `ForceMultiplier` at `0` and use
the game's setting.

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

### Performance collapses when RenoDX DLSS5 is also installed

- Follow the [Using with RenoDX DLSS5](#using-with-renodx-dlss5) section.
- Test each addon individually and restart between tests.
- Use a complete matched runtime set; do not replace only one Streamline or
  NVIDIA DLL.
- Record the actual loaded module paths because an OTA provider may override a
  local DLL.

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

Unlocking the count alone is not enough. The interpolation kernel blends with a
compiled-in `0.5`, so every generated frame lands at the temporal midpoint: 4x
produces three identical half-way frames, the counter doubles and the motion does
not get smoother. The addon decompresses the kernel's PTX, rewrites the blend
weight to come from the kernel's own temporal parameter, and re-emits the fatbin
so the driver JITs the corrected version.

DLSS-G owns frame generation and presentation pacing; this addon does not
implement a separate frame scheduler or issue generated-frame presents. With
current Streamline builds, pacing is normally left to the runtime. The optional
legacy compatibility setting disables the plugin's flip-metering path and
forces its existing software fallback only when higher multipliers otherwise
freeze presentation.

Each source file documents its own area in detail — start with the header comment
in [`addon.cpp`](src/addons/mfgunlock/addon.cpp).

## Why not 30-series?

Not because of the gates — those are just constants. Because the DLSS 4 snippet
ships **no Ampere machine code**. Its 70 fatbins carry `PTX sm_89` ×70,
`PTX sm_120` ×31 and `cubin sm_89` ×31, and nothing for sm_80/sm_86. PTX is
forward-compatible only, so sm_89 PTX cannot be JIT-compiled down to sm_86; the
module load fails outright.

Retargeting is *theoretically* open — the kernels use only
`mma.sync m16n8k16/m16n8k8` FP16 and `ldmatrix`, with zero instructions newer
than sm_86 (no FP8, no wgmma, no TMA), and the old hardware optical-flow
dependency is gone in DLSS 4. But frame generation costs roughly a fixed amount
per generated frame, and Ampere has far less FP16 tensor throughput per SM, so
the generation pass would likely cost more than the frame it saves. It was
investigated and deliberately dropped.

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

## Credits

- The midpoint diagnosis, the injected PTX, and the fatbin truncation trick come
  from [dashdogy/RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock).
  That project ships no licence, so nothing here is copied from it — `midpoint.hpp`
  is an independent implementation of the same idea, verified by reproducing its
  published output digest byte-for-byte.
- Built on [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse, and
  [ReShade](https://github.com/crosire/reshade) by crosire.

## Disclaimer

Not affiliated with or endorsed by NVIDIA. This modifies process memory of a
running game; use it on your own hardware at your own risk, and expect anti-cheat
in multiplayer titles to object. Results on hardware NVIDIA did not ship this
feature for are to be judged by eye.

## Licence

MIT — see [LICENSE](LICENSE).
