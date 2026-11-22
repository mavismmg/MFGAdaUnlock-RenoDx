# Troubleshooting

Part of the [MFG Unlock](../README.md) documentation. Sections: [Troubleshooting](#troubleshooting).

## Troubleshooting

### The addon does not appear in ReShade

- Confirm that ReShade was installed with full addon support.
- Confirm that the `.addon64` file is in the addon location used by that game.
- Check the ReShade log for an addon loading or API-version error.
- If the log shows error **1114** when loading the addon, open `ReShade.ini` and remove
  the addon's name from `DisabledAddons` in the `[ADDON]` section. ReShade refuses to
  start an addon listed there, and that is the usual cause of this error.

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
- 1.11.0: a game that never calls `slDLSSGGetState` (Microsoft Flight Simulator
  2020) used to leave the panel at "the runtime has not answered the Dynamic
  support query yet". The addon now asks itself; the log says "MFG Unlock asks
  for Dynamic MFG support itself". Toggle Frame Generation off and on once after.

### "The last session may have closed unexpectedly" after a normal exit

- 1.11.0 counts a session that started closing (its last swap chain gone, or the
  game ending itself with `TerminateProcess`) as a clean exit. If the notice still
  appears after a normal exit, send `ReShade.log` and `MFGAdaUnlock.ini`.

### The game stops responding

- When the game is stuck for 15 s inside something MFG Unlock watches (starting
  Streamline, creating the swap chain, Present, NGX), `MFGAdaUnlock-hang.txt` is
  written beside the game with where it is stuck. Wait about 20 s, close the game
  and send that file with `ReShade.log`. With `[Diagnostics] CrashDump=1` a dump
  file is written too; **Save support files** collects both.

### Performance collapses when RenoDX DLSS5 is also installed

- Follow the [Using with RenoDX DLSS5](xemfg.md#using-with-renodx-dlss5) section.
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
