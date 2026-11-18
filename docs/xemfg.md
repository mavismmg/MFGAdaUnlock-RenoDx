# XeMFG

Part of the [MFG Unlock](../README.md) documentation. Sections: [XeMFG backend](#xemfg-backend), [XeMFG DLSS-G mode](#xemfg-dlss-g-mode), [Using with RenoDX DLSS5](#using-with-renodx-dlss5).

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
[XeMFG table](games.md#xemfg-backend-tested-games).

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
   [latest release](https://github.com/mavismmg/MFGAdaUnlock-RenoDx/releases/latest) in the ReShade addon location for the
   game (usually the folder with the game executable).
3. Launch the game, open the ReShade overlay (**Home**), open the **MFG Unlock**
   panel and set **General → Frame generation method** to **XeMFG**.
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

**Removing or switching back:** select **DLSS-G** in the panel and
restart. To remove the Intel files, use the rollback offered by the installer
helper.

### XeMFG settings

The panel saves XeMFG settings to `MFGAdaUnlock.ini` beside the game. Most
users never need to edit it: since 1.8, every key has a control, under
**Diagnostics → All settings (advanced)**. Those controls apply after a restart.

| Section / key | Default | Meaning |
|---|---|---|
| `[Backend] Selected` | `DLSSG` | `XeMFG` selects the XeMFG backend; restart required |
| `[DLSSG] XmxGenerator` | `0` | `1` with `Selected=DLSSG` selects [XeMFG DLSS-G](#xemfg-dlss-g-mode); restart required |
| `[XeFG] Multiplier` | `4` | Output multiplier, `2`–`6` |
| `[XeFG] XeFGStock2xModel` | `0` | `1` uses Intel's own 2x model instead of the ported one. Set the multiplier to `2`: above it, frame generation does not start |
| `[XeFG] UIComposition` | `0` | `1` composes the HUD from the game's HUD-less image, which avoids HUD interpolation artifacts (the HUD then updates at the real frame rate). Needs the game to provide a HUD-less image |
| `[XeFG] UIStabilization` | `0` | Experimental, for games without a HUD-less image. `1` composes back-buffer pixels that stayed unchanged for a few real frames (static HUD text and icons) over every generated frame instead of interpolating them, which removes their flicker. Moving or semi-transparent HUD elements are still interpolated. In games that provide a HUD-less image it replaces `UIComposition`: the game's HUD inputs are not used, the static HUD is composed and moving HUD stays interpolated at the output rate |
| `[XeFG] XeFGFlipMetering` | `auto` | NVIDIA driver flip metering for generated frames; can also be toggled live in the panel |
| `[XeFG] ForceVSyncOff` | `0` | `1` presents the game's frames with sync interval 0, as with VSync off in the game, for games that offer no VSync option (Elden Ring). Game VSync keeps flip metering and generated-frame pacing from applying; the panel warns when that happens. Also in the panel as **Force game VSync off** |
| `[Latency] Input` / `Output` | `Auto` | Latency route. XeMFG always uses XeLL for output; Reflex input from the game is translated to XeLL |
| `[XeFG] Vulkan` | `1` | Experimental. XeMFG on Vulkan games: Intel's own XeFG/XeLL run on the game's Vulkan device through a D3D12-on-Vulkan layer (from dashdogy's MFG Unlocker 1.7.0). Inputs come from the game's DLSS Super Resolution or Ray Reconstruction, so turn DLSS on; the game's frame generation switch turns XeMFG on and off. `0` leaves Vulkan games untouched. Restart required |
| `[XeFG] VulkanPresentMetering` | `1` | NVIDIA present metering (`VK_NV_present_metering`, the Vulkan form of flip metering) for XeMFG's Vulkan output, where the driver offers it; not used with VSync on. `0` paces with XeLL and the CPU pacer only |
| `[XeFG] VulkanGameLatency` | `0` | Experimental, not yet measured in a game. `1` lets a Vulkan game's own Reflex sleep and simulation markers drive XeLL, instead of sleeping when the game acquires its image |
| `[XeFG] VulkanDynamicMFG` | `0` | Experimental, not yet measured in a game. `1` turns on Dynamic MFG in Vulkan games: the multiplier follows the real frame rate, up to the one set in the panel. `VulkanDynamicTargetFps` (`0`: the FPS limit, else just under the refresh rate), `VulkanDynamicRounding` (`0` at most, `1` nearest, `2` at least), `VulkanDynamicAllow1x` (`1`) and `VulkanDynamicTargetAsCap` (`0`) adjust it |
| `[XeFG] XeFGFlipQueue` | `0` | `1` turns on the flip queue: since 1.10.0, dashdogy's v1.7.1 pacer, which presents each batch on an even cadence and sizes its own buffer and first frame (the 1.7 `XeFGFlipQueueBufferPct` and `XeFGFlipQueueFirst` are gone) |
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

#### Writing `MFGAdaUnlock.profiles.json`

The file uses the same format as the built-in rules. Its rules apply after the
built-in ones: a later rule replaces a yes/no setting and adds to a list. For
example, a mod that creates XeFG from its own DLL (as Elden Ring's ERSS does
from `zzz_ERSS.dll`) is treated as the game's own XeFG integration with:

```json
{
  "schema": 1,
  "profiles": [
    {
      "id": "my-game-mod-xefg",
      "match": {"executable": "mygame.exe"},
      "game_xefg_modules": ["my_mod.dll"],
      "evidence": "local test: my_mod.dll creates XeFG for the game"
    }
  ]
}
```

- `schema` must be `1`, and every rule needs a non-empty `id` and `evidence`
  (a note on why the rule exists).
- `match` limits the rule: `executable` (the game's .exe file name) and/or
  `module` (a DLL that must be loaded). `{}` matches every game. Names are not
  case-sensitive.
- Settings a rule can carry:

  | Key | Value | Effect |
  |---|---|---|
  | `game_xefg_modules` | list of DLL names | XeFG calls from these DLLs count as the game's own |
  | `depth_inverted` | `true`/`false` | the game's depth convention, when the automatic reading is wrong |
  | `motion_jittered` | `true`/`false` | whether the game's motion vectors carry the camera jitter |
  | `retain_native_output` | `true`/`false` | keep the native swap chain (as with the Steam overlay) |
  | `observer_modules` | list of DLL names | DLLs that only observe presentation and keep the original functions |
  | `conflicting_modules` | list of DLL names | other frame-generation plugins that conflict with XeMFG |
  | `accept_missing_dlssg_plugin` | `true`/`false` | accept a Streamline whose DLSS-G plugin is absent |
  | `known_bad_reframework_sha256` | list of SHA-256 hashes | REFramework builds with the XeFG hook issue |

Any other key rejects the whole file. The `native_profiles` line of
`MFGAdaUnlock-XeMFG.log` lists the rules that applied. The built-in rules are in
[`src/addons/mfgunlock/xemfg/profiles.json`](../src/addons/mfgunlock/xemfg/profiles.json).

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

#### The game crashes every time it starts

Since 1.8.3, after three sessions in a row without a clean exit, the notice at the
top of the panel offers **Safe mode next start**. The next start runs without the
DLSS presets, XeMFG DLSS-G and the forced multiplier, and keeps your settings for the start
after that. If the game closes before you can open the panel, set
`DlssSuperResolutionPreset=0` and `ForceMultiplier=0` under `[RenoDX.MFGUnlock]` in
`ReShade.ini`. In 1.8.2, a saved DLSS preset crashed The Last of Us Part II at every
start; 1.8.3 fixes that.

#### Red Dead Redemption 2: use DX12

With XeMFG, Red Dead Redemption 2 runs much better in DX12 than in Vulkan. At 4K
on an RTX 4070 SUPER with 2x, DX12 showed 111 fps with one frame every 8.7 ms,
give or take 1 ms. Vulkan showed 86 fps, with frames arriving in pairs, 3 to 5 ms
apart, then a 13 to 17 ms gap, and stalls up to 166 ms. On screen the image seems
to step back, and motion does not look smooth despite the higher count. A user
with an RTX 4090 reported the same. **Queue metered frames early** (removed in 1.10.0) did not change it.

In Vulkan, XeMFG's work and copies share the GPU with the game's frame and wait
behind it when the GPU is saturated. The game also gives no latency markers in
Vulkan, so XeLL cannot time its frames. Attempts to fix this in 1.8.3 did not make
Vulkan smooth, so switch the game to DX12 for frame generation.

#### HUD elements smear or ghost

Enable **Quality → UI Composition** if the game provides a HUD-less image.
Otherwise the HUD is interpolated together with the scene.

#### GPU utilization drops in a Capcom / RE Engine game

This happens with REFramework builds without the XeFG fixes: they reinstall
their XeFG hooks repeatedly. Replace REFramework with a build that includes
the fixes, such as [onehoon's REFramework](https://github.com/onehoon/REFramework),
or remove REFramework to confirm.

### Reporting an XeMFG problem

Since 1.8.1, the quickest way is **Save support files**. It appears on the card
when XeMFG DLSS-G or XeMFG stops, and after a session that may have closed unexpectedly. It
copies the logs, your settings and a report into a new `MFG-Support-<date>` folder
beside the game and opens it. Logs may include folder names from your PC, so check
them before posting publicly.

To attach the files by hand, take these from the game folder:

- `MFGAdaUnlock-XeMFG.log` contains the session header, the input route and
  the reason frame generation was or was not admitted;
- `MFGAdaUnlock-XeMFG-compatibility.json` is written automatically when the
  output closes;
- the text from **Diagnostics → Copy compatibility report**;
- the game's `ReShade.log`.

Also mention the game version, GPU and driver, and whether the problem
appears with the **NVIDIA DLSS-G** backend too.

### Panel languages

Since 1.10.0 the panel is available in English, Portuguese (Brazil), Spanish, Japanese, Korean and
Simplified Chinese. It follows ReShade's language, then Windows'; the selector at the bottom of the
panel changes it at once (`[Panel] Language` in `MFGAdaUnlock.ini`: `auto`, `en`, `pt-BR`, `es`,
`ja`, `ko`, `zh-CN`). The translations other than English are machine translations marked `*`;
corrections go in `src/addons/mfgunlock/i18n/strings_<language>.inl`. Logs and the support report
stay in English.

For Japanese, Korean and Chinese the panel needs a font with those characters. When ReShade's font
is empty or is the Cascadia Mono the addon set, the addon sets the Windows font ReShade itself uses for
that language; a font you chose is kept. If the font cannot draw the language, the panel stays in
English until the next start.

### The panel's font

ReShade allows one font for its whole interface. Since 1.8.1, on the first launch in each
game, if ReShade has no custom font and Windows includes Cascadia Mono, the addon sets it
as ReShade's font for that game. A font you already chose is kept. To go back, clear
**Font** in ReShade's **Settings** tab; the addon will not set it again.

## XeMFG DLSS-G mode

> [!IMPORTANT]
> **Always prefer XeMFG DLSS-G in games with native DLSS frame generation.** It is more stable and
> more compatible than the XeMFG backend. Keep the XeMFG backend for games with no native
> frame generation. XeMFG DLSS-G is still experimental and was tested in Crimson Desert and
> Cyberpunk 2077, so please [report the result](#try-it-and-report), whether it works or
> not.

### What it is

XeMFG DLSS-G runs Intel's multi-frame model inside DLSS-G's presentation. The game still
sees DLSS-G, and it keeps its own menu, Reflex and display handling, but Intel's model
generates the frames. There is no CUDA and no Intel swap chain.

Despite the name, this does not run on Intel's XeMFG DLSS-G hardware: on NVIDIA cards it uses
the same ported multi-frame model as XeMFG.

### Why prefer it

- **More stable and more compatible:** there is no proxy swap chain between the game and
  the display, the part of the XeMFG backend where overlays, window changes and Alt+Tab
  have caused problems.
- **Lower measured latency:** see [Measurements](#measurements).
- **The game keeps working as with DLSS-G:** its menu, Reflex and display handling stay
  in place.

### What works

Tested in Crimson Desert and Cyberpunk 2077, on an RTX 4070 SUPER:

| Feature | Result |
|---|---|
| Multiplier from the **game's own menu**, during play | Works: Cyberpunk 2077 went through 2x, 4x and 6x |
| **Frame Multiplier** in the panel, above what the menu offers | Works: up to 6x |
| **Output FPS Cap (Reflex)** | Works |
| Alt+Tab | Works |
| Changing resolution during play | Works |
| 1440p and 4K with DLDSR | Work |
| **Install / update files** from the XeMFG DLSS-G notice | Works: it replaced a `libxess_fg.dll` of the wrong version in Cyberpunk 2077 |
| Falling back when Intel's files are wrong | Works: NVIDIA's generator keeps running and the panel says why |

### HDR

| Output | XeMFG DLSS-G |
|---|---|
| SDR (`R8G8B8A8`) | Works |
| HDR10 (`R10G10B10A2`) | Works |
| scRGB (`R16G16B16A16_FLOAT`) | **Not tested.** XeMFG DLSS-G has no conversion for it like the XeMFG backend does |

The **System** card on the **General** page shows which output the game is using. If XeMFG DLSS-G
cannot start on scRGB, the panel suggests turning HDR off or using XeMFG. Please report
any scRGB game you try.

### What the status means

| Status | Meaning |
|---|---|
| **Generating frames** | XeMFG DLSS-G generated a frame in the last 2 seconds |
| **NVIDIA is generating** | XeMFG DLSS-G is hooked but never called, and frames are being generated, so NVIDIA's generator is doing the work |
| **Waiting for frames** | DLSS-G is calling XeMFG DLSS-G, but no frame came out; the panel gives the reason |
| **Paused or off in the game** | DLSS-G is not calling XeMFG DLSS-G. Games do this in menus and cutscenes, and it is normal if generation returns afterwards |
| **Frame generation stopped** | XeMFG DLSS-G failed; a card gives the reason and no frames are generated until the game restarts |

### Try it and report

1. Install the addon as usual. **Keep DLSS Frame Generation available in the game**.
2. In the panel, set **General → Frame generation method** to **XeMFG DLSS-G**, and restart the game.
3. If Intel's files are missing, the panel says so and offers **Install / update
   files**. It installs `libxess_fg.dll` 1.3.1.78, the only version XeMFG DLSS-G accepts, and
   `libxell.dll`. Restart the game afterwards.
4. Turn on Frame Generation in the game. The **Backend** card should read `XeMFG DLSS-G`, and
   **Generator** should read *Intel XeFG model, in DLSS-G's presentation*.
5. Choose the multiplier in the **game's own menu**, as you would with DLSS-G. It
   should change during play. The panel's **Frame Multiplier** can override it and go
   up to 6x.

> [!IMPORTANT]
> **You should be able to change the multiplier from the game's menu with XeMFG DLSS-G.** It
> worked in Cyberpunk 2077. If the game's selector does nothing, is missing or gets stuck
> in your game, [please open an issue](https://github.com/mavismmg/MFGAdaUnlock-RenoDx/issues).

Please report the game, the multiplier, your GPU and driver, and what the **Backend**
card showed. Attach `ReShade.log` from the game folder, or use **Copy report** or **Save
support files** when the panel offers them. Reports of games that work are as useful as
reports of problems.

### Measurements

Crimson Desert, RTX 4070 SUPER, driver 621.01, DLSS Quality. Each row is one 30-second
PresentMon capture of the same route, walked by hand. Base FPS and displayed FPS for
XeMFG DLSS-G and DLSS-G come from PresentMon's estimate.

> [!WARNING]
> **The latency column is not input latency.** It is PresentMon's `DisplayLatency`,
> averaged over every displayed frame, because these captures have no PC latency
> (`MsPCLatency`) column. With XeMFG DLSS-G and DLSS-G, PresentMon labels generated frames as
> the game's own, so they lower the average. With XeMFG, it labels them as XeSS-FG
> frames and counts them differently. Do not compare this column between routes, and
> do not read it as the delay between input and the screen. Frame generation adds
> latency: about one base frame, plus pacing. Use PC latency to compare.

**1440p**

| Mode | Displayed FPS | Base FPS | DisplayLatency (not input latency) | p99 frame interval | Large intervals |
|---|---|---|---|---|---|
| Generation off | 44 | 44 | 45 ms | not recorded | 0 |
| XeMFG 2x, Intel's model 18 | 78 | 39 | 47 ms | 17.8 ms | 3 |
| XeMFG 2x, ported model 16 | 81 | 41 | 54 ms | 14.5 ms | 1 |
| **XeMFG DLSS-G 2x** | 76 | 38 | **44 ms** | 17.4 ms | 3 |
| DLSS-G 4x | 136 | 34 | 43 ms | 10.3 ms | 8 |
| XeMFG 4x | 141 | 35 | 64 ms | 9.0 ms | 4 |
| **XeMFG DLSS-G 4x** | 139 | 35 | **42 ms** | 10.5 ms | 6 |
| XeMFG 6x | 175 | 29 | 75 ms | 8.1 ms | 4 |
| **XeMFG DLSS-G 6x** | 189 | 32 | **42 ms** | 7.8 ms | 18 |

**4K with DLDSR**

| Mode | Displayed FPS | Base FPS | DisplayLatency (not input latency) | p99 frame interval | Large intervals |
|---|---|---|---|---|---|
| Generation off | 23 | 23 | 91 ms | 48.3 ms | 0 |
| DLSS-G 4x | 73 | 18 | 80 ms | 17.8 ms | 8 |
| XeMFG 4x | 71 | 18 | 125 ms | 18.2 ms | 2 |
| **XeMFG DLSS-G 4x** | 69 | 17 | **80 ms** | 20.4 ms | 6 |

What the tester saw in that scene:

- **XeMFG DLSS-G and XeMFG at 6x:** a similar image, and much better input with XeMFG DLSS-G. HUD and
  artifacts got clearly worse from 4x to 6x in both.
- **DLSS-G at 4x:** a very good HUD, but a lot of artifacts and flicker in this game.
- **Model 18 against model 16 at 2x:** model 18 looked better, with a less blurred HUD
  and slightly fewer artifacts, even though model 16 did better in the numbers.
- **Motion blur:** a similar blur was noticed with XeMFG DLSS-G, DLSS-G and XeMFG, so it does
  not come from XeMFG DLSS-G. The cause is unknown.
- **Alt+Tab and changing resolution:** both worked with XeMFG DLSS-G.

How to read these numbers:

- It is one game and one capture per mode. Nothing here is a general result.
- **XeMFG is ahead of XeMFG DLSS-G in FPS and pacing at 2x and 4x.** XeMFG DLSS-G is ahead in FPS at 6x,
  where it had more large intervals (18 against 4).
- **The latency column does not show input latency** (see the warning above). The
  tester felt better input with XeMFG DLSS-G at 6x, but these captures cannot confirm it. PC
  latency captures are needed to compare the routes.
- The model 18 capture ran in a different presentation mode, so that comparison needs
  repeating.

### Limitations

- **Direct3D 12 games with DLSS frame generation only.** In Vulkan games, NVIDIA's
  generator keeps running. Games that use XeMFG through FSR or the DLSS-SR input have
  nothing for XeMFG DLSS-G to replace, so use XeMFG there.
- **Only `libxess_fg.dll` 1.3.1.78.** With another version or a missing file, XeMFG DLSS-G does
  not start, NVIDIA's generator keeps running, and the panel says why.
- **Tested on an RTX 4070 SUPER only.** Other cards are unknown.
- **RTX 20 and 30 GPUs are experimental** (1.8.3). See
  [RTX 20 and 30 GPUs](architecture.md#rtx-20-and-30-gpus): since 1.9.0 the same path also runs
  NVIDIA's own generator there. XeMFG DLSS-G was confirmed on an RTX 3060 Ti. If NVIDIA's runtime
  still cannot create frame generation on the card, the panel says why and offers
  **Use XeMFG**, which runs on RTX 20 and 30.
- **No more than 2x when the game's Streamline is older than 2.7** (Ghost of Tsushima,
  Horizon Zero Dawn Remastered). Forcing more crashed inside Streamline, so the panel's
  **Frame Multiplier** is not applied above 2x there, and the panel says why.
- **Games with jittered motion vectors** (Horizon Zero Dawn Remastered, Death Stranding 2):
  the ported multi-frame model refuses the jittered-motion flag, so 1.8.3–1.8.5 used
  Intel's own 2x model there and repeated its frame at higher multipliers. Since 1.8.6 the
  multi-frame model starts without that flag; the motion vectors still carry the camera
  jitter (about a render pixel at most), and no artifact was seen in Death Stranding 2.
  **XeMFG DLSS-G multi-frame with jittered motion: Off** in the panel's advanced settings, or
  `[DLSSG] XmxJitter=0` in `MFGAdaUnlock.ini`, restores the 2x model.
- **Only Crimson Desert was measured.** Cyberpunk 2077 was tested by playing it, without
  captures.
- **NVIDIA Dynamic MFG with XeMFG DLSS-G is experimental** (since 1.8.3). NVIDIA's runtime
  chooses the count and XeMFG DLSS-G generates it; tested on an RTX 4070 SUPER. It needs the
  same Streamline and DLSS-G versions as with NVIDIA's generator.
- **Settings for NVIDIA's generator are locked with XeMFG DLSS-G**, such as Adaptive Quality,
  the temporal fix and the Blackwell kernels. DLSS presets, Reflex and pacing remain.
- If XeMFG DLSS-G fails during play, no frames are generated until the game restarts: the real
  frame is shown again, and the panel says why. Select another mode if this repeats. A
  start Intel's model refuses is tried again when the game changes resolution or HDR.
- **Video memory (1.10.0).** NVIDIA's own frame generation feature is no longer
  created next to XeMFG DLSS-G, which never used it: about 400 MiB less at 1440p, so XeMFG DLSS-G now
  costs about as much as NVIDIA's frame generation (measured on the Streamline Sample;
  no game tested yet). If a game stops generating frames with XeMFG DLSS-G, turn off **XeMFG DLSS-G: free
  NVIDIA's unused memory** on the General page, or set `[DLSSG] XmxSaveVram=0` in
  `MFGAdaUnlock.ini`, and restart the game.
- On a compute command list (Streamline 2.14), XeMFG DLSS-G copies the game's render-target and
  depth inputs as non-pixel shader resources, the state Streamline hands NGX. The D3D12
  debug layer still reports a few barriers in the first frames.
- **A DLSS-G file with no exact profile** (a game's own build, or a newer NVIDIA
  download) is unlocked when it has exactly the gate every reviewed version has; the
  log then says `generic gate`. Turn it off with **Generic provider gate** in the
  panel's advanced settings, or `[DLSSG] GenericGate=0` in `MFGAdaUnlock.ini`, and
  restart the game. A file with another gate is still refused.
- **Elden Ring with ERSS:** if ERSS generates frames without NVIDIA's DLSS frame
  generation, XeMFG DLSS-G has nothing to replace. Use XeMFG.
- A newer `nvngx_dlssg.dll` might change how the module reacts to XeMFG DLSS-G's hook. It ran
  with 310.9.1 in Cyberpunk 2077 and 310.9.16 in the Streamline Sample.

### XeMFG DLSS-G in Vulkan games (experimental)

1.10.0. On by default with XeMFG DLSS-G (`[DLSSG] XmxVulkan=1`, under **XeMFG DLSS-G options** on the
panel's General page). It acts only when ReShade is loaded as the game's Vulkan
layer, for Vulkan games with DLSS frame generation such as DOOM: The Dark Ages
and Indiana Jones and the Great Circle.

- XeMFG DLSS-G runs inside DLSS frame generation on the game's own Vulkan device: Intel's
  ported multi-frame model records into each frame generation call, and
  Streamline's DLSS-G still presents and paces the frames (ReShade, HDR and the
  panel stay as they are).
- NVIDIA's own frame generation feature is not created, so its video memory is
  not spent.
- The engine is dashdogy's Vulkan DLSS-G provider from MFG Unlocker v1.7.1 (MIT),
  called from this addon's NGX Vulkan hooks. The addon only adds what it needs
  to the game's device; it never takes the game's swap chain.
- If the provider cannot run (no Intel files, an unknown XeSS build, a device it
  could not prepare), NVIDIA's frame generation runs as before. The panel and the
  support report's `chain_xmx_vulkan` line say which happened.
- Tested in DOOM: The Dark Ages. That game needs `+r_allowBlackListedLayers 1` on
  its command line to load ReShade's Vulkan layer.
- **Pacing in Vulkan games** (on by default, `[DLSSG] XmxVulkanPacing`): when the GPU
  cannot hold the display's refresh with XeMFG DLSS-G (DOOM: The Dark Ages with path
  tracing), the game's Reflex overshoots and the frame rate drops every ~2 s. The
  addon watches the real frames and limits the output just under what the GPU holds
  (~120 fps at 4x in DOOM), raising it again when the scene gets lighter. Your own
  frame rate cap takes its place. The panel shows the current limit.
- With the GPU at its limit, expect XeMFG DLSS-G to run below NVIDIA's generator
  (about 20 fps less in DOOM: The Dark Ages with path tracing at 4x): XeFG's network
  runs on the shader and tensor cores, NVIDIA's on the optical flow hardware.

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
