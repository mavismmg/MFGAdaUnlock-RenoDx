# Thin-geometry release-candidate notes

Date: 2026-09-11. Branch: `develop`.

This candidate keeps the thin-geometry mechanisms independent and enables
**Intermediate scatter retention** plus **Validated warp blend** together as
the experimental recommended default. Persisted user selections remain
authoritative.

## Controls

- **Intermediate scatter retention**: independently developed in this fork. It
  relaxes the motion-consistency input at `+120` while the Blackwell
  `Kernel_EstimateIntermMvecsScatter` program constructs intermediate-frame
  motion vectors. The separate depth-mismatch test is preserved.
- **Validated warp blend**: enabled by default while remaining independently
  selectable. Tony Joaca's public
  DLSSG-Transfusion `qualityValidWarp` work identified
  `Kernel_BlendCandidatesFused` as a useful quality point. This fork implements
  a separate conservative variant with coordinate, sentinel, finite-color, and
  candidate-agreement validation plus a gradual `0.85..1.0` weight floor.
- **Previous-to-current scatter retention**: retained as an advanced research
  control, disabled by default, and marked unstable after an initial startup
  crash report. It is not recommended for normal use.

The controls do not intentionally alter frame multiplier selection, Dynamic
MFG, Reflex, presentation pacing, HUD/UI tagging, or HDR/color-space handling.

## Safety revision

The redirected-fatbin builder now preserves every entry following the replaced
PTX payload rather than truncating the container at that entry. This prevents
architecture-specific cubins or metadata later in the original fatbin from
being lost. Restore logic also keeps the replacement allocation alive if any
live provider descriptor cannot be restored, avoiding a dangling pointer during
an abnormal unload.

Unknown providers and changed payloads continue to fail closed. No NVIDIA DLL
is modified on disk, and generated NVIDIA-derived cubin payloads remain ignored
from source control.

## Release-note warning

> After installing or updating the addon, the first launch may perform DLSS-G
> kernel compilation and exhibit temporary stutter or uneven pacing. Restart
> the game once before evaluating performance or image quality.
