# Frame generation quality

Part of the [MFG Unlock](../README.md) documentation. Sections: [Frame-generation Input Quality](#frame-generation-input-quality), [Experimental Thin-geometry Interpolation](#experimental-thin-geometry-interpolation), [Dynamic Multi Frame Generation](#dynamic-multi-frame-generation).

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
[post-1.4.2 research](research/post-1.4.2.md).

The earlier temporal backend is documented as
[historical research](research/temporal-confidence-history.md), not as a
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
Since 1.8.5, Dynamic MFG accepts any runtime from these versions on; NVIDIA's
runtime still has the final say through `bIsDynamicMFGSupported`. The
310.9.16 + 2.14.16 stack was tested in Cyberpunk 2077 with XeMFG DLSS-G on an RTX 4070 SUPER:

- **DLSS-G 310.9.1 or newer**
- **Streamline 2.14 or newer**, including the copy the NVIDIA app's frame generation
  override supplies (`ProgramData\NVIDIA\NGX\models\sl_dlss_g_override_0`), which
  1.8.4 did not recognize, so the panel stayed at "Waiting for runtime detection"
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

**Dynamic multiplier ceiling** (1.8.5) is the NVIDIA app's "Multiplier: Up to Nx".
The NVIDIA app stores it in the driver profile (setting `0x10562D0F`), and Streamline
reads it once when it starts. The addon answers that read in the game's process with
the ceiling chosen in the panel, so NVIDIA's own Dynamic MFG never picks more; the
driver profile, the NVIDIA app and other games are unchanged. **Auto** keeps the
driver's value. Streamline reads the value only at startup, so a change applies after
a game restart; the panel says which ceiling the current session uses. Changing it
during play would mean writing Streamline's internal memory, whose layout changes
between builds, so the addon does not do it.

**Stuck at 2x?** If the NVIDIA app's frame generation override is off for a game (a
per-game profile can turn it off even when the global setting is on), NVIDIA uses the
frame generation file built into the driver (310.2.1 on the development PC), which has no
multi-frame support: the game offers 2x at most and Dynamic MFG is unavailable. Since
1.8.5 the panel warns about it. In the NVIDIA app, open the game and set
**DLSS Override – Frame Generation** to **Latest**, then restart the game.

VSync is **not** a universal requirement for Dynamic MFG. When VSync is active,
Streamline ignores the numeric Dynamic target and instead aims near the active
display refresh for tear-free presentation. Users who are not using G-SYNC
through the NVIDIA driver do not need to enable VSync solely because Dynamic
MFG is enabled. If VSync is used with Dynamic MFG, use Streamline 2.14.1 or newer
(VSync support for Dynamic MFG arrived in 2.14.1), check that the addon reports VSync capability, and avoid a path
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
