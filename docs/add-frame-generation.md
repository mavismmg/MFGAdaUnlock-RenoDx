# Add frame generation, runtimes and Vulkan

Part of the [MFG Unlock](../README.md) documentation. Sections: [DLSS frame generation in games without it](#dlss-frame-generation-in-games-without-it), [Selecting the Streamline Runtime](#selecting-the-streamline-runtime), [Verifying Operation](#verifying-operation), [Experimental Vulkan Support](#experimental-vulkan-support).

## DLSS frame generation in games without it

Experimental, off by default (1.10.0). Turn on **Add frame generation** under
**Games without frame generation** on the panel's General page
(`[DLSSG] AddFrameGeneration=1` in `MFGAdaUnlock.ini`) and restart the game. It
covers two kinds of games; the panel shows which route runs and its status.

### Games with Streamline and DLSS Super Resolution only

Such a game already gives Streamline what DLSS frame generation reads: depth,
motion vectors and the camera constants of every frame.

1. When the game starts Streamline asking for DLSS Super Resolution and not for
   frame generation, the addon adds DLSS frame generation (and Reflex and PCL if
   missing) to what Streamline loads.
2. Once the game sends its camera constants, the addon turns frame generation on
   at the panel's multiplier. With **XeMFG DLSS-G** selected, XeMFG DLSS-G generates the frames.

Requirements and limits:

- The addon must load before the game starts Streamline (ReShade's
  `LoadFromDllMain`, which the addon adds itself; the panel says when it did not).
- DLSS frame generation presents and paces through Reflex. For a game that does
  not use Reflex (Ninja Gaiden 4), the addon turns Reflex on and marks the game's
  own frames (1.10.1); the log says "Route A Reflex".
- Games with Streamline 1.x (The Last of Us Part I) are not supported; the
  support report's `chain_streamline` line says "Streamline 1.x".
- Without a HUD-less image from the game, the HUD is interpolated with the scene.
- A game that asks for frame generation itself is never changed, nor a game that
  loads every Streamline feature (it may have its own frame generation turned off
  in its menu).
- The support report's `chain_force_fg` line says what happened.

Tested with NVIDIA's Streamline Sample made into an SR-only game. DLSS frame
generation ran at 4x with the same video memory cost as the sample's own (about
580 MiB at 1440p), and XeMFG DLSS-G generated every frame in it.

### Games with DLSS Super Resolution and no Streamline

Red Dead Redemption 2, Kingdom Come: Deliverance II and Metro Exodus Enhanced
Edition call DLSS Super Resolution straight through NGX. For these, the addon
starts NVIDIA's Streamline itself and gives the game DLSS frame generation, or
XeMFG DLSS-G when it is selected.

- The addon needs NVIDIA's Streamline 2.14.16 files in a folder named
  `MFGUnlock-Streamline` next to it. They are not in this release. Since 1.10.1
  the panel installs them: under **Add frame generation**, **Install / update
  Streamline files** opens a helper that downloads them from NVIDIA's Streamline
  SDK release on GitHub (about 5 MB of the 276 MB archive), checks each one
  against a pinned hash, shows what it will install and installs it after the game
  exits (with a backup and a rollback journal). To do it by hand: download the
  Streamline SDK 2.14.16 (<https://github.com/NVIDIA-RTX/Streamline/releases>) and
  copy from its `bin/x64` folder `sl.interposer.dll`, `sl.common.dll`,
  `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `nvngx_dlssg.dll` into
  `MFGUnlock-Streamline`.
- Direct3D 12 and Vulkan. Vulkan needs ReShade installed as the Vulkan layer;
  `[DLSSG] HostVulkan=0` turns off the Vulkan route alone.
- **HUD fix** (on by default, Direct3D 12 only): the addon gives DLSS frame
  generation the image right before the game draws its HUD. With XeMFG DLSS-G, it puts
  the real frame's HUD over each generated frame, so the crosshair and the HUD do
  not repeat. **Show what counts as HUD** paints that part magenta, to check it.
  Some games draw their HUD in a way it cannot see; the HUD is then interpolated
  as before.
- NGX gives no camera, so the camera constants are a fixed projection and DLSS
  frame generation's automatic scene change detection stays off.
- A game that has its own Streamline, or ships `sl.interposer.dll`, is never
  hosted. An `sl.interposer.dll` another addon loaded without starting Streamline
  (RenoDX's DLSS addon) no longer counts as the game's (1.10.1).
- **RenoDX HDR in a game running in SDR**: RenoDX changes the swap chain's format
  under Streamline, whose buffers keep the game's. Since 1.10.1 the addon sees it
  and runs the game without Streamline for that session (no frame generation; the
  panel and the support report say why) instead of crashing. Use the game's own
  HDR with RenoDX, or turn RenoDX's swap chain upgrade off.
- Loading screens without DLSS SR pause DLSS frame generation until the game uses
  DLSS SR again (1.10.1).
- NVIDIA's DLSS-G does not generate on an FP16 (scRGB) HDR swap chain (DLSS-G
  programming guide, section 11). Kingdom Come: Deliverance II's own HDR is one:
  with it on, Add frame generation produces no frames, and the panel says so
  (1.10.1). Turn the game's HDR off, or use the XeMFG method.
- For testers: `[DLSSG] HudTrace=3` logs the draws, dispatches and copies of 3
  frames after DLSS SR, to find where a game draws its HUD when the HUD fix finds
  nothing.
- Tested:
  - Kingdom Come: Deliverance II (Direct3D 12): XeMFG DLSS-G with the HUD fix.
  - Red Dead Redemption 2, Direct3D 12 and Vulkan: DLSS frame generation and XeMFG DLSS-G.

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

Vulkan is still experimental: fewer games have been tested on it than on
Direct3D 12. ReShade has to load as a Vulkan layer. When testing, confirm that
the panel reports **Vulkan** and use the verification checklist above.

What works on Vulkan, as of 1.11.0:

| Feature | Vulkan | Notes |
|---|---|---|
| DLSS-G multiplier unlock in games with DLSS-G | Yes | The same Streamline `slDLSSGSetOptions` / `slDLSSGGetState` path as Direct3D 12. |
| XeMFG DLSS-G (Intel's model inside DLSS-G) | Yes | NVIDIA's unused feature is never created. Includes real-frame pacing (`[DLSSG] XmxVulkanPacing`, on by default). |
| [Add frame generation](#dlss-frame-generation-in-games-without-it) (games with DLSS SR only) | Yes | MFG Unlock starts its own Streamline for the game's Vulkan device. Tested in Red Dead Redemption 2. |
| FP16 (scRGB) HDR notice | Yes (1.11.0) | DLSS-G generates nothing on an FP16 swap chain; the panel says so. |
| HUD fix (the image before the HUD) | No | Direct3D 12 only. On Vulkan the HUD is interpolated with the scene. |
| RenoDX HDR upgrade with Add frame generation | No | When another addon changes the swap chain's format, Add frame generation steps aside. Use the game's own HDR instead. |

Red Dead Redemption 2 asks for every queue of its graphics family. Add frame
generation then shares the game's last queue with Streamline.

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
