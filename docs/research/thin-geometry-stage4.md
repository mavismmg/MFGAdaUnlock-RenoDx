# Stage 4: experimental thin-geometry implementation

Date: 2026-09-11. Branch: `develop`. Baseline: `6fac842` plus the preserved Stage 3 research files.

This is an experimental implementation for manual A/B validation. It is disabled by default, is not a release, and has not been merged to `main`.

> Historical checkpoint: the later release candidate enables Intermediate
> scatter retention and Validated warp blend together as experimental defaults. See
> [`thin-geometry-release-candidate.md`](thin-geometry-release-candidate.md).

## Independent mechanisms

The work deliberately does not collapse the two discoveries into one heuristic. Three independently persisted restart-time controls are exposed under **Enhanced thin-geometry interpolation (experimental)**:

| Control | Intervention point | Change |
| --- | --- | --- |
| Validated warp blend | `Kernel_BlendCandidatesFused` | Validates warped coordinates, invalid-vector sentinels, finite RGB, and two-candidate color agreement. Accepted candidates receive a gradual `0.85..1.0` warp-weight floor. |
| Previous-to-current scatter retention | `Kernel_EstimatePrev2CurrScatter`, parameter `+60` | Multiplies only the local motion-consistency divisor by `0.5`, relaxing that rejection test while preserving its bounds logic. |
| Intermediate scatter retention | `Kernel_EstimateIntermMvecsScatter`, parameter `+120` | Multiplies only the motion-consistency divisor by `0.5`; the separate depth mismatch test remains present. |

The blend experiment is informed by Tony Joaca/DLSSG-Transfusion's published `qualityValidWarp` behavior, with clear credit in source. It is intentionally more conservative than the inspected v1.4.0 binary: it uses the embedded gradual 0.85 floor rather than the binary's effective 1.0 replacement, omits asymmetric candidate copying, and does not use the UI-recomposition flag as an acceptance bypass. This makes blend behavior independently measurable from the addon's HUD/UI modes.

The two scatter controls implement the separate Stage 3 findings. They do not depend on the blend control and do not alter each other.

## Application paths

- The validated blend and previous-scatter cubins do not fit their original Ada cubin slots. They therefore use validated in-process fatbin descriptor redirection to an uncompressed, retargeted PTX rebuild. No provider file is modified on disk.
- The intermediate-scatter variant fits the original motion-vector slot and is selected in the existing Blackwell in-place replacement path. It is not applied when the full Blackwell path is unavailable; the established temporal fallback remains unchanged.
- Generated cubin payloads are derived locally from installed NVIDIA providers and remain excluded from source control.

## Compatibility guards

The experiment currently admits only the inspected providers:

| Provider | PE timestamp | Image size | PTX/cubin validation |
| --- | ---: | ---: | --- |
| DLSS-G 310.9.0 | `0x6A8745AD` | `7565312` | Exact normalized PTX length/FNV for redirected kernels; exact original cubin ELF fingerprint, slot size, and full-slot FNV for the in-place variant |
| DLSS-G 310.9.1 | `0x6A986031` | `7565312` | Same independent payload validation |

Additional checks require a currently mapped x64 PE image, unique target fatbin, exact entry and parameter-block signatures, unique rewrite anchors, bounded fatbin sizes, and a descriptor-reference count in the safe range. Unknown versions, missing or ambiguous signatures, changed payloads, allocation failures, or unsupported temporal paths fail closed and retain the previous behavior.

Logs report provider path/version, mechanism, detected state, application path, applied state, and the validation/failure reason for each requested control.

## Isolation

This stage does not change `framecount.hpp`, `force_policy.hpp`, `pacing_policy.hpp`, Quality Guard, HDR/UI tagging, Dynamic MFG, Reflex, Present hooks, color-space handling, or multiplier logic. The options are all false on fresh configurations and do not overwrite existing configuration.

## Static/build validation

- Full Release addon build: passed.
- Unit/integration suite: 9/9 passed.
- Offline `DONT_RESOLVE_DLL_REFERENCES` provider probe: all three mechanisms applied and restored on both 310.9.0 and 310.9.1.
- Exact descriptor references observed: 8 for blend and 8 for previous scatter on both providers.
- Modified PTX assembled successfully with local `ptxas` for sm_89.
- Experimental artifact: `out/renodx-mfgunlock-stage4-thin-geometry-experimental.addon64`.
- Artifact SHA-256: `7C14B2A84D7D983CAD95040F6D649B72F01B84998F1D82291B0EA49763F5465E`.

Runtime image quality, GPU cost, latency, and frame pacing remain unvalidated until Stage 5/manual A/B testing.
