# Stage 3: independent warp-validity investigation

Date: 2026-09-11. Baseline: `develop`, commit `6fac842` (Stage 2).

Status: static investigation complete; no experimental implementation or game validation. This report is not a feature announcement or a supported-version list.

## Outcome

There is a concrete, independently identified candidate for a future thin-geometry experiment: motion-consistency rejection thresholds passed by the DLSS-G provider into its scatter kernels. Their producer and consumer are traceable in the inspected binaries. They are not merely suggestive strings.

However, **we have not identified the other mod's `qualityValidWarp` implementation**. No matching public API or exact parameter name was found in the searched material. We cannot equate our candidates with that option or claim its reported image-quality benefits.

The strongest candidate is a **host-computed kernel input that controls a rejection branch**, not a new neural model or a documented NVAPI quality switch. One candidate affects the same intermediate-motion kernel already used by this addon's Blackwell port and midpoint correction. An independent user toggle is conceivable, but algorithmic independence must not be claimed.

Evidence is sufficient to scope a narrowly guarded experiment, but **not sufficient to call a production implementation safe or beneficial**. First confirm the identified parameters in the active runtime dispatch. Do not start by changing a shared constant globally.

## Scope and method

Read-only inspection covered the current addon, local public Streamline/NGX/NVAPI headers, public NVIDIA documentation, locally installed DLSS-G providers, and cached Streamline wrappers. Input DLLs were read as files, not loaded or modified. No game process was patched or captured in this stage.

Methods:

- PE metadata, SHA-256, exports, ASCII strings and UTF-16 target strings.
- Bounds-checked extraction of embedded CUDA fatbins; `cuobjdump -ptx` on all 70 fatbins in each of five production provider samples.
- PTX comparison per architecture, with comments/blank lines removed; a second comparison also normalizes entry/parameter names.
- `cuobjdump -sass` on the stock Ada cubins for the two scatter candidates and warp-blending kernel in 310.9.1.
- x64 disassembly with RIP-relative data references and MSVC RTTI/vtable tracing to connect host parameter preparation to named kernel structures.

Tools: Python, pefile 2024.8.26, Capstone 5.0.9, locally installed CUDA tools. Raw DLL-derived PTX/disassembly and local helper scripts remain outside the repository in the local `Tooling/ValidWarpResearch` research directory. This repository receives only this report and a sanitized [evidence manifest](validwarp-stage3-evidence.json), not vendor payloads, local installation paths, or credentials.

Disassembly uses PE unwind ranges; leaf routines without unwind entries are not exhaustively covered. Negative string/API results are limited to the inspected material, not proof that no private control exists anywhere.

## Versions inspected

Full hashes, PE sizes and machine metadata are in the manifest. Prefixes below identify its records.

| Component | File version | SHA-256 prefix | Inspection depth |
| --- | --- | --- | --- |
| DLSS-G SDK release DLL | 310.7.0.0 | `135eaf0733c1e373` | Strings, all PTX, host code |
| DLSS-G SDK development DLL | 310.7.0.0 | `0d33b5de65d60a94` | Metadata/strings only |
| DLSS-G installed game DLL | 310.9.1.0 | `ff6e90eb78b82792` | Strings, all PTX, candidate Ada SASS, host code |
| DLSS-G OTA provider | 310.9.0.0 | `c64928fdb7c48a57` | Strings, all PTX, host code |
| DLSS-G OTA provider | 310.7.129.0 | `20c18281e88f376b` | Strings, all PTX |
| DLSS-G OTA provider | 310.6.0.0 | `83230804a087272c` | Strings, all PTX |
| Streamline game wrapper | 2.14.1.0 | `f4a6b2b14dcc0b14` | Metadata/strings; public API review |
| Streamline OTA wrapper | 2.14.0.0 | `73b8a78a275b5a3d` | Metadata/strings |
| Streamline OTA wrapper | 2.12.129.0 | `ee12fb6825564a02` | Metadata/strings |
| Streamline OTA wrapper | 2.11.0.0 | `86e55063193a5c82` | Metadata/strings |

The installed Cyberpunk 2077 and STALKER 2 DLSS-G copies matched the same 310.9.1 hash; their inspected 2.14.1 wrappers also matched. This establishes disk identity only, not which provider a running game actually uses.

## API/configuration findings

1. Searches found no `qualityValidWarp`, `ValidWarp`, or equivalent named control in the addon or searched public headers. Matching exact/related target strings were also absent from the scanned binaries and decompressed PTX. An external mod may use a private configuration name; that is an inference, not an identification.
2. The public Streamline DLSS-G options expose scheduling/configuration facilities, but no named warp-validity tolerance. Adding an invented NGX string through its generic parameter interface would not demonstrate that the provider consumes it. [Streamline 2.14.1 DLSS-G interface](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.1/include/sl_dlss_g.h)
3. NGX exposes depth-linearization/object-separation controls, motion-vector conventions, reset and resource inputs. These are meaningful integration controls, but are not evidence of a public equivalent to the scatter motion-consistency divisors below. `OutputDisableInterpolation` requests suppression of interpolated output; changing it is not an appropriate substitute for preserving thin geometry and can affect cadence. [NVIDIA DLSS-G parameter definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs_dlssg.h)
4. Streamline's `kBufferTypeNoWarpMask` describes pixels that should skip warping. The raw providers/wrapper also contain `DLSSG.NoWarp`. This is a resource/mask concept, not a discovered scalar that makes more geometry interpolate. [Streamline resource types](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.1/include/sl_core_types.h)
5. No matching public NVAPI control was found in the local header search. Display-warp APIs, `DLSSG.ReflexWarp.Available`, and CUDA `bar.warp.sync` must not be confused with pixel-reprojection validity. In particular, CUDA warp instructions describe execution groups, not image warping.
6. Provider-side NGX parsing and an RTTI-identified endpoint configuration/registry path were inspected in 310.9.1. Recognizable depth controls, indicator settings, preset/UI-related configuration and private driver IDs exist. No dataflow from a public/config key to the candidate motion divisors was established. Unexplained private IDs are not a basis for a patch.

The local NGX definitions inspected additionally have SHA-256 `c45756c6e97c524365ebaa74362b6b0aadcf57b4320f6e504d09047e0cf16a8c`; the local Streamline DLSS-G header has `1fc18cbe004e280df1f787276d08a1b28b8a8c4c65856fbaa659f56dff6a915d`. Public moving branches may differ from these local snapshots.

## Concrete binary evidence

The names below come from the 310.9.x provider. Older samples use the generic PTX entry name `main_kernel`. Indices are zero-based extraction inventory positions, **not stable dispatch IDs or patch addresses**.

| Candidate | Kernel and parameter byte offset | Observed function | Confidence |
| --- | --- | --- | --- |
| A | `Kernel_EstimatePrev2CurrScatter`, `+60` | Local motion-consistency test can skip packed scatter writes | High for static arithmetic/control flow; untested visual benefit |
| B | `Kernel_EstimateIntermMvecsScatter`, `+120` | Motion-consistency test combines with a depth mismatch test before scatter | High for static arithmetic/control flow; untested visual benefit |
| C | `Kernel_EstimatePrev2CurrScatter`, `+56` | Camera-derived versus supplied-motion classification, also affecting packed metadata/depth handling | High for classification; lower suitability as a first intervention |
| D | `Kernel_WarpBlendingWeights` | Samples using decoded motion and computes blending weights | Relevant pipeline evidence, but no isolated public quality scalar established |

### A: previous-to-current scatter

Inventory index 47 has a 144-byte parameter block. In the readable sm_120 PTX, a vector load from parameter offset 48 obtains dimensions plus the floats at offsets 56 and 60. The sm_89 PTX consumes the same fields.

For finite, ordinary inputs, the observed gate can be summarized as:

```text
limit = max(squared_motion_length / K, 1)
if squared_local_motion_difference >= limit:
    skip this scatter write
```

Here `K` is the positive float at parameter offset 60. There are several local sample/write cases; bounds checks precede the writes. The comparison instruction also handles unordered values, so this simplified expression is not a replacement for exact floating-point/NaN semantics.

Reducing a positive `K` increases the permitted mismatch once the one-unit floor stops dominating. That could preserve some inconsistent-but-useful motion around thin geometry; it could also admit genuinely wrong motion across an occlusion. Neither outcome has been measured.

The separate offset-56 field compares supplied motion with camera-projected motion and changes classification/metadata. It is not interchangeable with offset 60 and should initially remain untouched.

### B: intermediate-frame motion scatter

Inventory index 46 also has a 144-byte parameter block. Offset 32 supplies the interpolation position; **it is not the candidate quality field**. Offset 120 supplies the motion-consistency divisor. Offset 124 belongs to a separate depth-related control.

In the inspected repeated scatter branches, rejection combines two conditions:

```text
depth_mismatch = difference_in_processed_depth >= 3
motion_mismatch = squared_motion_difference >= max(squared_motion_length / K, 1)
if depth_mismatch AND motion_mismatch:
    skip this scatter write
```

The depth value is processed/quantized by this kernel; `3` must not be interpreted as meters or normalized input depth. Other bounds, depth and search-region logic also exists. Relaxing `K` can bypass this combined rejection in some cases; it does not manufacture missing disoccluded information or repair absent motion vectors.

### Host producers: not guessed kernel offsets

For 310.9.1, RTTI/vtables identify `NGXCubinParameterStruct<...EstimateIntermMvecsScatter...>` and `...EstimatePrev2CurrScatter...`. Their construction/copy sites connect the stack-built parameters to the dispatch structures. The host computes:

```text
K_previous_camera = float(H * H) / 900
K_previous_local  = float(H * H) / 10000
K_intermediate    = float(H * H) / 10000
```

`H` here deliberately means the integer read at offset 4 of the provider's dimension-state object. Nearby offsets 8/12 supply the kernel grid dimensions. Its exact relationship to output, input and dynamically scaled height still needs runtime confirmation. Do not hardcode a screen resolution or assume all these dimensions coincide. The host uses integer multiplication before conversion, which also matters to sanity checks.

The following RVAs locate the **division instructions for these exact file hashes**, not portable patch targets:

| Provider sample | Intermediate `+120` producer | Previous `+56` producer | Previous `+60` producer | Shared `10000.0f` data |
| --- | --- | --- | --- | --- |
| 310.7.0 release | `0x41d0a` | `0x4222b` | `0x42245` | `0xa7b24` |
| 310.9.0 OTA | `0x3deca` | `0x3e3eb` | `0x3e405` | `0x9cd1c` |
| 310.9.1 installed | `0x3de9a` | `0x3e3bb` | `0x3e3d5` | `0x9cd0c` |

Important safety findings:

- The 310.9.1 `10000.0f` data has another observed reference at instruction RVA `0x3f1b6`, whose parameter object is RTTI-identified as `Scatter3d`. A global data edit would affect more than the two selected motion tests.
- Constructor defaults are subsequently overwritten by the prepared parameter block on the traced path. Changing a superficially promising constructor immediate may have no intended effect.
- Stock Ada SASS corroborates divisor consumption: index 47 uses reciprocals of constant-bank offsets `0x198`/`0x19c`, and index 46 repeatedly uses `0x1d8`. These correspond to parameter offsets 56/60/120 with the observed `0x160` parameter base. This is static evidence, not proof of which cubin a live patched process executes.

### Other pipeline evidence and limits

`Kernel_InitMvecQualityMask` (index 49) averages three sampled channels, saturates the result and writes a two-channel output. Its name alone does not establish a boolean motion-validity mask or a temporal lifetime counter; the source resource semantics were not fully traced.

`Kernel_WarpBlendingWeights` (index 54) handles a packed invalid-motion sentinel, samples textures with/without motion offsets, and computes exponential/logistic-style weights from sampled differences. It is a plausible downstream quality stage, but no isolated configurable validity threshold was established there.

Inpainting/output kernels were inspected for context, not proposed as new modifications. There is **no evidence here for a configurable number of frames to keep a reprojected pixel alive**. The traced candidates are immediate motion-consistency decisions, not a confirmed temporal retention timer.

## Cross-version result

Five production samples yielded 350 fatbins in total. No target-name matches were found in their decompressed PTX.

- For indices 46/47, sm_89 and sm_120 PTX each match their corresponding architecture across all five versions after removing comments/blank lines and normalizing the entry/parameter name. The manifest records full comparison hashes. This does not assert equivalence between Ada and Blackwell code.
- The seven inspected framework fatbins (39, 46, 47, 49, 54, 56, 60) are byte-identical between the sampled 310.9.0 and 310.9.1 providers.
- Some other framework kernels differ between 310.6 and 310.7. There is no basis to declare the entire pipeline version-invariant.
- Host instruction/data addresses differ even between 310.9.0 and 310.9.1. Equal candidate kernels do not imply equal initialization, pacing, API behavior or configuration.
- Host formulas were traced in 310.7.0, 310.9.0 and 310.9.1. They were not separately traced in 310.6.0 or 310.7.129. Ada-compatible PTX/cubins are present; compatibility of a proposed intervention remains untested.

OTA replacement can change identity, host layout and the code actually executed. Detection must follow the active provider, never infer it solely from the game-folder filename or cache version number.

## Relationship to the existing addon

The source's full Blackwell path identifies these three roles through fingerprints and shared-memory sizes:

| Existing role | Shared bytes | Identified 310.9.x kernel | Relationship |
| --- | --- | --- | --- |
| Motion-vector estimate | 7776 | `Kernel_EstimateIntermMvecsScatter` | Same kernel as candidate B; its input divisor remains relevant |
| Inpaint | 3920 | `Kernel_Prev2CurrUnpackPull` | Downstream interaction possible; not candidate A/B's host field |
| Inpaint decision | 784 | `Kernel_OutputPull` | Downstream interaction possible; not candidate A/B's host field |

See [blackwell.hpp](../../src/addons/mfgunlock/blackwell.hpp), [midpoint.hpp](../../src/addons/mfgunlock/midpoint.hpp), and the selection/fallback path in [addon.cpp](../../src/addons/mfgunlock/addon.cpp).

Candidate A's previous-to-current kernel is not one of those three replacements. Candidate B is the same kernel involved in the temporal midpoint fix and full Blackwell replacement, but the proposed divisor is a different input from temporal position. The consumer exists in both inspected architectures. Any later implementation must validate the exact final payload selected by the addon, including fallback, rather than assume an on-disk Ada disassembly describes the final runtime.

No new control of these divisors was found in current addon code. This does not mean the addon is neutral to image reconstruction: its existing kernel and HDR/UI paths already affect it. They were not changed in Stage 3.

## Possible future path and safety requirements

Prefer an established API/configuration control if later dataflow evidence finds one. None is established for A/B yet. Do not add arbitrary NGX keys, private driver IDs or broad NVAPI hooks and call that support.

If Stage 4 is authorized, start with read-only confirmation of the exact active provider, kernel identity, parameter layout, `H`, grid size, divisors and selected Blackwell/fallback path. Confirm multiple evaluations and resolution changes. Do not rewrite anything when identity or parameter ownership is uncertain.

A potential experiment could then adjust **one identified local divisor at its validated producer/upload boundary**, or narrowly change the equivalent consumer operation in an exactly matched kernel. These are alternatives, not a recommendation to patch both. Do not blindly edit the shared `10000.0f`, globally hook every GPU launch, or bypass all rejection branches.

Required protections before any write:

1. Verify the module is the active DLSS-G provider through feature identity/exports, x64 PE bounds, full supported sample identity and expected code. Version text alone is insufficient.
2. Validate a unique instruction sequence, decoded instruction boundaries, surrounding dataflow, RIP target type/value and the destination parameter structure. RTTI/name strings alone are insufficient.
3. Match the actual consumer's parameter size, offsets and fingerprint. Validate both the selected Blackwell payload and any supported fallback separately.
4. Require finite positive divisors, sane dimensions, expected baseline formulas and safe object lifetime. Fail closed on ambiguity, unload/reload, unknown layout, or another mod's conflicting modification.
5. Avoid unsynchronized writes to shared or in-flight parameter blocks. Establish whether GPU graph capture caches parameters and when updates become effective. If a code redirection is required, validate relocation and calling convention before implementation.
6. Make the option experimental and off by default. No hidden changes to multiplier, interpolation positions, Dynamic MFG, Reflex, reset flags, HUD tags, color space or scheduling. Unsupported cases retain the current addon behavior.
7. Keep logs bounded and outside hot loops; no continuous Present enumeration, GPU readback or synchronization merely to apply the feature. Restart-only activation is preferable until live toggling/graph lifetime is proven safe.

Even without scheduling changes, accepting more scatter writes can change atomic contention, downstream workload and GPU time. **Pacing and latency cannot be guaranteed by code isolation alone.** They must be measured against the existing STALKER 2 baseline.

## Unresolved questions and next checkpoint

Still unverified:

- Which candidate, if any, corresponds to the other modder's option.
- Actual runtime parameter values, dimension meaning, graph/dispatch lifetime and active provider path.
- Whether the strongest benefit comes from A, B, both, or a different confidence/blending stage.
- How much usable motion exists for fences, vegetation, wires, weapon edges and screen borders in each game.
- Whether less rejection produces persistence/ghosting, stretched foreground objects, disocclusion errors, HUD artifacts or a GPU-time regression.
- Equivalent safe behavior across D3D12 and Vulkan dispatch paths. Shared embedded kernels do not prove backend integration compatibility.

Recommended next step: after explicit Stage 4 authorization, confirm the identified parameters in a read-only runtime probe first. If this matches the static evidence and a safe scoped intervention can be established, test one conservative, bounded candidate behind a separate disabled-by-default option. Preserve depth/bounds protection and the current working Blackwell/temporal path. If runtime validation fails, stop instead of modifying guessed memory.

Visual improvement and performance remain **not tested**. No new addon was compiled, no production code/configuration changed, and no merge, push, tag or release was performed in this stage. Existing credits and license remain unchanged. The investigation did not inspect or adopt unpublished `qualityValidWarp` code; existing project credit for prior techniques is preserved.

Stage 3 ends here. Stage 4 requires separate authorization.
