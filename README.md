# MFG Unlock

A [ReShade](https://reshade.me/) addon that enables **DLSS multi-frame generation
(3x / 4x and above) on GeForce RTX 40-series** cards, which NVIDIA ships gated to
RTX 50-series only — and corrects the frame interpolation so the extra frames
carry new motion instead of repeats.

Nothing on disk is modified. Every patch is applied to the mapped image at
runtime and reverted when the addon unloads. **No NVIDIA files are redistributed
here.**

---

## Attribution

This repository is a fork of the [original project](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx)
created by [Dreamt](https://github.com/ImDreamt). Full credit for the original
implementation goes to Dreamt and the contributors already credited in this
repository. This fork contains additional compatibility changes and fixes,
including support and fixes for **S.T.A.L.K.E.R. 2: Heart of Chornobyl**.

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

## Install

1. Install ReShade into the game, with addon support enabled.
2. Drop `renodx-mfgunlock.addon64` next to the game executable.
3. Launch, open the ReShade overlay, and find **MFG Unlock** in the Add-ons tab.

## Settings

Written to your `ReShade.ini` under `[RenoDX.MFGUnlock]`:

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Master switch for the whole addon |
| `MaxCount` | `4` | The `DLSSG.MultiFrameCountMax` value reported to the runtime |
| `ForceFlipMeteringOff` | `1` | Required for 3x+ on Ada. Leave on |
| `TemporalFix` | `1` | The interpolation correction. Leave on |
| `ForceMultiplier` | `0` | `0` respects the game's own choice; `2`–`6` forces that multiplier |
| `RaiseFrameCeiling` | `0` | Raises an old Streamline plugin's compiled hard limit to 6x. Off by default because that breaks some games; the stale device-limit bypass needed by STALKER 2 is always applied |
| `ForceOTAPlugins` | `0` | Asks Streamline to load the driver's OTA plugin set. Off by default; see notes |

If a game has its own multiplier selector, leave `ForceMultiplier` at `0` and use
the game's setting.

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

Blackwell paces multi-frame output with hardware flip metering Ada does not have;
left enabled it freezes the presented image. Streamline already ships a software
fallback, so the addon forces the plugin down it.

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
