# MFG Unlock 0.8

## What's New

- **Automatic Guard + UI Composition (Recommended)** is now the default.
- Adds a guarded Streamline UI/HUD composition path with conservative HDR
  fallback and transition-safe temporal resets.
- Adds the validated Blackwell framework-kernel path for improved moving-detail
  stability at 3x/4x, with exact provider fingerprints and the 0.7 midpoint
  correction as a safe fallback.
- Adds NVIDIA Dynamic MFG integration for the validated DLSS-G 310.9.1 +
  Streamline 2.14.1 D3D12 stack.
- Improves Streamline/NGX discovery, native multiplier menus, late module loads,
  Vulkan compatibility, configuration transitions, status reporting, and
  failure handling.
- Keeps module enumeration and compatibility discovery out of the steady-state
  Present path. Production Present work is limited to primary-swapchain HDR
  state observation and an already-completed worker flag check.

## Recommended Configuration

Leave **Frame-generation input quality** on **Automatic Guard + UI Composition
(Recommended)**. It validates optional HUD-less/UI resources before allowing
Streamline UI Composition, uses final color when the inputs are incomplete or
incompatible, and fails conservatively to final color in HDR where tag metadata
cannot prove the required color-space match.

Leave **Force legacy software flip pacing** off with current Streamline builds.
Enable it only for a reproducible higher-multiplier presentation freeze, then
restart. Leave **Force frame multiplier** off in games with a native selector.

## UI Composition

UI Composition lets Streamline interpolate the scene and interface separately
when the game supplies a correct HUD-less image and UI color/alpha resource. It
can reduce HUD ghosting, flicker, halos and masking/composition artifacts caused
by those optional resources. It cannot repair unrelated bad motion vectors,
depth, exposure, camera data, distortion data, or game-rendered pixels, and not
every UI artifact has the same cause.

**UI Composition example:** This community video shows the type of UI/HUD-related
Frame Generation artifact that UI Composition is intended to mitigate in
affected integrations: https://www.youtube.com/watch?v=xV_E-cvyu8Q

The video is a visual reference, not NVIDIA documentation and not proof that
every game has the same integration defect.

## Major Improvements

- Requests Streamline's UI-capable path as soon as the primary swapchain is
  positively identified as SDR, avoiding a silent no-op in games that call
  `SetOptions` before their first resource tags. Optional HUD-less/UI inputs
  are still withheld until their lifecycle enum, resource-structure metadata,
  dimensions, formats and UI alpha precision pass the guard. The addon cannot
  prove that a game keeps the underlying pixels alive for the lifecycle it
  declares.
- Handles games that tag HUD-less and UI resources in separate calls without
  forwarding a one-sided transition batch.
- Clears only optional HUD/UI tags when the guard fails; final color, depth,
  motion vectors, multiplier and pacing remain under the game/provider path.
- Issues a single temporal-history reset after genuine HDR, primary swapchain,
  resolution, option, multiplier or quality-mode transitions; no camera-turn or
  continuous-reset behavior is added.
- Tracks and re-elects the main/largest swapchain so auxiliary devices cannot
  flap HDR or Dynamic renderer eligibility, including after swapchain teardown.
- Wraps feature functions only for the matching Streamline feature and avoids
  wrapping the addon's own wrapper on repeated retrieval.
- Uses atomic publication for SetOptions/GetState/Reflex function pointers and
  serializes per-viewport quality transitions.
- Bounds state/options retries, distinguishes transient startup failures from
  structural rejection, and always restores the game's fixed request when an
  advanced path is rejected.
- Observes the loaded Streamline wrapper and verified DLSS-G provider candidate
  versions instead of assuming a DLL beside the executable is active.
- Detects when a non-default Streamline runtime policy missed `slInit` because
  the game initialized Streamline before the normal ReShade addon scan, and can
  register the addon for early loading on the next restart.
- Keeps NVIDIA App/NGX override inspection read-only; no undocumented driver
  profile writes are made.

## Dynamic MFG

Dynamic MFG currently requires:

- **DLSS-G 310.9.1**
- **Streamline 2.14.1**
- NVIDIA display driver **595.41 or newer**
- Direct3D 12
- A compatible driver/runtime reporting
  `DLSSGState::bIsDynamicMFGSupported = eTrue`

The addon requests NVIDIA's native `DLSSGMode::eDynamic`; NVIDIA selects the
multiplier and owns pacing/hysteresis. `numFramesToGenerate` is ignored in this
mode. Target `0` follows display refresh; with VSync off, a nonzero value is the
requested Dynamic output target.

VSync is not required merely to enable Dynamic MFG. When VSync is active,
Streamline ignores the numeric target and aims near display refresh. If VSync
is used, the validated 310.9.1 + 2.14.1 stack and a working Independent Flip
path are important. Users who are not using G-SYNC through the NVIDIA driver do
not need to enable VSync solely for Dynamic MFG.

The optional Reflex setting is explicitly labeled as an advanced
application/source-frame cap. It is off by default and is not presented as a
final-output target.

Advanced users may need a complete manual runtime update:

- [NVIDIA Streamline 2.14.1](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1)
- [Official NVIDIA Windows DLSS libraries](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel)
- [NVIDIA DLSS 310.9.1 release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1)

The NVIDIA App/driver does not automatically place this exact matched stack in
every game, so installation is currently manual. Do not mix individual
Streamline DLLs from different releases. In NVIDIA App, reset both global and
per-game Frame Generation/model-preset overrides to **Use the 3D application
setting**. If NVPI was used, reset its DLSS FG/NGX overrides for the same profile
to NVIDIA defaults too. A loaded path under
`ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_g_override_0` proves a driver override
is winning over the DLL beside the executable. Use **Prefer local runtime**,
enable early addon loading when the panel reports that `slInit` was missed, and
restart. The panel/log—not the directory listing—is authoritative for the
runtime that was actually selected.

## Compatibility

| DLSS-G | Streamline | Fixed MFG / addon | Automatic Guard | UI Composition | Dynamic MFG | Synchronization |
|---|---|---|---|---|---|---|
| 310.9.0 | 2.12.x | Validated compatibility path | Validated | Not release-validated on the older wrapper | Not supported in this release | Preserve the game's established path; do not assume 2.14.1 Dynamic/VSync behavior |
| 310.9.1 | 2.14.1 | Validated current path | Validated | Validated for correct inputs; provider adds slight memory/performance cost | Supported on capable D3D12 systems; release-gate path validated | VSync optional; with VSync the numeric target is ignored. G-SYNC and Independent Flip remain system/presentation properties |

Game compatibility remains integration-specific. See the README tested-games
table and per-game caveats. The list is not a universal compatibility claim.

## Frame-Pacing Validation

The final gate was one 45-second STALKER 2 run on September 10, 2026 with the
diagnostic companion removed. The session used DLSS-G 310.9.1, Streamline
2.14.1, Dynamic MFG with VSync, and a 240 Hz display. PresentMon reported:

- Hardware: Independent Flip;
- 9,316 display intervals: 4.450 ms median, 5.637 ms p95, 7.953 ms p99, and
  224.48 FPS average;
- 2,337 source intervals: 15.831 ms median, with four intervals above the
  robust 31.662 ms outlier threshold; and
- a 3.986x generated/source cadence heuristic, consistent with active 4x.

This single release-gate trace validates the active 4x presentation path. It is
not a native-2x/addon A/B benchmark and does not claim zero game-side stutter,
universal compatibility, or an addon-overhead difference.

## Technical Details

- The Ada architecture/capability gates and Streamline's stale device ceiling
  are patched only after exact in-memory signatures are verified.
- The Blackwell motion-vector estimate, inpaint and inpaint-decision programs
  are accepted only with exact original ELF hashes and slot sizes, then replaced
  in the already mapped provider. Unknown providers fail to the 0.7 temporal
  path; NVIDIA DLLs are not modified on disk.
- `slGetFeatureFunction` returns wrapped `slDLSSGSetOptions`,
  `slDLSSGGetState`, and pass-through-by-default `slReflexSetOptions` functions.
- Older callers are probed with addon-owned v4 state storage; only fields within
  the caller's declared ABI are copied back.
- Advanced v4/v5 options are built in addon-owned storage. The game-owned
  structure is not overwritten.
- Optional tag batches larger than the bounded local copy are forwarded whole
  and reported, never partially modified.
- Runtime enable/disable is restart-bound so active patches and retained
  function pointers cannot enter a half-disabled state.
- Runtime-adjustable quality and Dynamic controls expose a pending state until
  a successful game-side `SetOptions` call applies the option-level change.
- Reflex limiter updates are submitted only from the game's Streamline options
  path, never directly from the ReShade/ImGui thread. Disabling the optional
  advanced source cap restores the game's saved Reflex request on the next
  successful game-side options call.
- Settings backed by load-time memory patches or early runtime selection now
  distinguish the saved next-launch value from the active-session value, so the
  panel cannot imply that a restart-bound change already took effect.
- Normal process termination relies on Windows reclaiming mapped memory;
  explicit unload restores verified live-module patches. Hot-unloading while a
  game retains wrapper pointers is not supported.

## Known Limitations

- Final image quality still depends on the game's native depth, motion vectors,
  exposure, camera matrices, jitter conventions, HUD resources and render order.
- Streamline tags reveal native format, not the pixels' real transfer function
  or color space. Automatic HDR handling therefore intentionally fails to final
  color rather than claiming it validated PQ/scRGB equivalence.
- UI Composition has a small NVIDIA-documented performance/memory cost.
- Dynamic MFG is D3D12-only and exact-version-gated in this release. A target is
  ignored while VSync is active; the addon cannot override that contract.
- NVIDIA App driver overrides and Streamline OTA selection are separate. The
  addon does not make undocumented NVAPI profile changes.
- Optional depth-edge tuning is integration-specific and off by default.
- Vulkan fixed MFG support remains experimental. Some games require a launch
  option for the ReShade Vulkan layer.
- Anti-cheat may reject runtime code modification. Multiplayer use is not
  supported or recommended.

## Installation / Updating

1. Install ReShade with addon support or the game's RenoDX package.
2. Replace the previous addon with `renodx-mfgunlock.addon64`; do not keep two
   differently named copies loaded together.
3. Use a suitable `nvngx_dlssg.dll`; for Dynamic, install the complete exact
   310.9.1 + Streamline 2.14.1 stack.
4. Start with **Automatic Guard + UI Composition (Recommended)**, native
   Streamline pacing, automatic/game multiplier selection, and the advanced
   Reflex source cap off.
5. Fully restart after addon, runtime, provider-selection, kernel or legacy
   pacing changes.

## Credits

- [dashdogy](https://github.com/dashdogy/RTX40MFG-Unlock) created the
  foundational working ASI approach, Streamline/NGX verification strategy and
  temporal midpoint correction research.
- [Dreamt](https://github.com/ImDreamt) created the original ReShade/RenoDX
  addon adaptation and repository from which this project is forked.
- [mavismmg](https://github.com/mavismmg) implemented and maintains this fork's
  current compatibility, quality/HDR handling, Dynamic MFG integration,
  initialization/pacing robustness, Vulkan work, diagnostics and documentation.
- [Matias Lombo](https://github.com/matiasLombo/mfg-unlock) identified and
  validated the image-quality benefit of the Blackwell framework kernels on Ada
  and provided the rebuild workflow used to generate the exact compatibility
  data for this release path.
- [mugensc](https://next.nexusmods.com/profile/mugensc) isolated and reported the
  RenoDX DLSS5 matched-runtime workaround.
- Artur from DLSS Enabler contributed the HUD-less/color-space debugging insight
  used during the HDR investigation.
- Built on RenoDX by clshortfuse and ReShade by crosire. The repository README
  retains the complete project credits and lineage.

## Short GitHub Release Header

Version 0.8 adds the recommended Automatic Guard + UI Composition path,
Blackwell framework-kernel quality improvements with safe fallback, exact-stack
NVIDIA Dynamic MFG support, stronger Streamline/NGX initialization and version
validation, and expanded diagnostics. Dynamic requires DLSS-G 310.9.1 +
Streamline 2.14.1 on D3D12; VSync is optional and causes Dynamic to target the
display refresh. The final STALKER 2 Dynamic/VSync 4x presentation gate passed.

## Discord Changelog

**MFG Unlock 0.8**

- New default: Automatic Guard + UI Composition (Recommended)
- Safer HDR/HUD-less handling and one-shot temporal synchronization
- Blackwell motion-vector/inpaint kernel path with exact validation and 0.7 fallback
- Dynamic MFG for DLSS-G 310.9.1 + Streamline 2.14.1 on D3D12
- Correct VSync target behavior; optional Reflex source cap is now clearly separate
- Stronger late-load hooks, version checks, retries, multi-swapchain handling and diagnostics
- STALKER 2 Dynamic/VSync release gate passed at an observed 3.986x cadence
