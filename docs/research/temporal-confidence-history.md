# Historical temporal-confidence research

These paths are **disabled in the 1.4.2 release and the border-axis candidate**.
They describe earlier research builds, whose activation failures and inconsistent
visual benefit led to the return to Local Stable geometry and V2 inpaint.
See the 1.4.1 release notes. Defaults below apply only to those research builds.
No proposal here re-enables them or repairs missing deformation motion vectors.

### Adaptive Quality V3.2 Stability and launch latency

V3.2 keeps V3.1's luminance-relative photometric confidence and oriented
geometry, but makes silhouette and screen-edge decisions less binary. Diagonal
support fades continuously across ambiguity and motion-direction thresholds,
an isolated neighbor can contribute only one eighth of the add-on relaxation,
and full two-neighbor consensus still reaches one half. Orientation fades in
between 0.5 and 1.5 pixels of motion. Warp candidates supported from only one
side require a native weight ramp from 0.50 to 0.75, while the native weight
always remains the lower bound. Entry at all four screen edges is limited to
one extra pixel and also constrained by the perpendicular edge distance.

`Local Stable` uses the provider's existing 3x3 tile and introduces no texture
read. `Temporal Stable` is the default for a missing setting and stores only
8-bit geometry confidence: no RGB, depth, motion vector or frame image is kept.
Recovery is capped at 0.20 per source frame and confidence loss is immediate.
Symmetric 2x/4x/6x phases share buckets with their directions exchanged; 4K at
6x uses 49,766,400 active confidence bytes. The allocation is bounded at 64 MiB.

V3.2 ships separate Local and Temporal ptxas cubins. The Local artifact has no
history symbol, global load/store or history branch. A Temporal request falls
back to that dedicated Local artifact before V2, V1 and native; a Local request
never installs the CUDA hooks.

The CUDA launch hooks are not installed until an exact 310.9.0 or 310.9.1
provider has accepted the V3.2 Temporal geometry cubin. Temporal history remains disabled
while a read-only probe verifies the kernel name, module magic, 144-byte ABI,
launch API, stream, dimensions, multiplier and complete cyclic phase sequence.
If a validated integration has not loaded the delay-loaded CUDA Driver yet, the
addon acquires `nvcuda.dll` from System32 before installing the hooks; it never
searches the game directory for a replacement driver DLL.
Any mismatch, unsupported API, allocation failure or size above 64 MiB falls
back to `Local Stable`; unrelated kernels pass through unchanged. The overlay
and diagnostics report the requested and effective mode and the precise
fallback reason. After both requested target kernels are associated, every
unrelated launch performs one atomic immutable-dispatch load and two pointer
comparisons before the original trampoline:
there is no lock, hash lookup, name/module/ABI query or per-launch logging.

### Adaptive Quality V3.4 Temporal Inpaint Stability

V3.4 leaves the V3.2 warp, photometric thresholds and geometry cubins
unchanged. It adds separate decision-only inpaint variants. `Local V3` tags
coordinate and non-finite hard rejects inside the existing 3x3 tile while
remaining decision-equivalent to V2 and adding no texture or global-memory
access. `Temporal V3` keeps one encoded confidence byte per pixel, direction
and symmetric phase bucket; reconstructed color, depth and motion vectors are
never retained.

The byte uses six confidence bits and four states: Cold, Armed, GraceUsed and
RearmSeen. Hard rejects clear confidence immediately. An Armed soft ambiguity
may retain at most 12/62 confidence for one observation; repeated drops are
immediate. Recovery is limited to 12/62 per source frame and rearming requires
two stable high-confidence observations. NVIDIA's native inpaint decision is
always a lower bound, so the temporal path can never reject native work.

`Kernel_OutputPull` has no temporal-phase argument. The addon therefore accepts
only the phase validated from the immediately preceding
`Kernel_EstimateIntermMvecsScatter` launch in the same module, context and
stream. Missing, duplicated or ambiguous sequencing falls back only the
inpaint component to Local V3. Geometry and inpaint share a bounded 96 MiB
arena; at 4K/6x they use 49,766,400 bytes each (99,532,800 bytes total). If the
combined requirement does not fit, inpaint falls back first and temporal
geometry remains available.

The Local cubin uses 48 registers, 784 bytes of shared memory and zero
stack/spill/local memory, with 9,344 bytes of `.text`. Temporal uses the same
register/shared limits, 10,368 bytes of `.text`, and exactly one 8-bit history
read plus one 8-bit history write per output pixel. During the test phase,
`Temporal V3` is the default when the V3.4 setting is absent. Existing saved
choices remain unchanged, and invalid values still normalize to
`V2 Compatibility`.
