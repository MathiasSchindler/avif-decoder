# AVIF standalone decoder: methodical build plan

Goal: a dependency-free (standard C library only) AVIF decoder that outputs PNG.

Non-goals (for now): performance, streaming I/O, full HEIF feature coverage, encoder.

Quality gate: we do not proceed to the next milestone until the current milestone meets its exit criteria and matches the spec + oracle behavior for the agreed test set.

Oracle/reference behavior:
- `avifdec` (libavif CLI) is the output oracle.
- `avifenc` is used only to generate controlled test vectors.

Test sets (used by every milestone):
- Generated vectors: `testFiles/generated/**` (small, deterministic, used for tight regression testing).
- Third-party corpus: `testFiles/**` excluding `testFiles/generated/**` (real-world coverage).

Per-milestone expectation:
- For the full third-party corpus: **no crashes** and **clear “unsupported feature” errors** are acceptable where we haven’t implemented a feature yet.
- For the generated vectors: meet the milestone’s stronger assertions (metadata extraction, byte-for-byte extraction, and later pixel equivalence).

Repo structure rule
- Milestone implementations live under `src/mX-…/`.
- Each milestone folder contains a small CLI + minimal library code for that milestone.
- Later milestones may reuse code from earlier milestones by copying forward or extracting shared pieces into `src/common/` *after* we have stable APIs.

---

## Milestone status

| Milestone | Status | Notes |
| --- | --- | --- |
| pre-m1: Spec “gotchas” pass | In progress | Keep the running log below |
| m0: Container box walker | Done | `avif_boxdump` parses the current corpus without crashing |
| m1: Parse `meta` essentials | Done | Parser robust on generated + 172 third-party; m1 `--extract-primary` validated vs m2 (byte-identical) on generated vectors |
| m2: Extract AV1 item payload | Done | Generated vectors OK; third-party sweep: 169 OK, 3 unsupported (primary not `av01`) |
| m3a: AV1 bitstream parse/validate | Done | `av1_parse` parses OBUs + Sequence Header for all extracted samples |
| m3b: Minimal AV1 still decode | In progress | See sub-milestones below (m3b.A/B/C/D). Keep each gate testable. |
| m3b.A: Tile payload dump + accounting | Done | `--dump-tiles`; strict tile-group byte accounting; deterministic generated-vector regression |
| m3b.B: Tile payload safety invariants | Done | No-overread harness + trailing-bits check across corpus (diagnostic is OK; no crashes) |
| m3b.C: Entropy decoder + probes | In progress | Symbol decoder + trailing-bits validation implemented; exit_symbol probe exists (expected to fail until real tile decode) |
| m3b.D: Tile syntax traversal (intra path scaffolding) | In progress | Tile-syntax probe runs on real tiles; root partition milestone implemented; full partition tree + block decode next |
| m4: Color convert + PNG out | To do | End-to-end decode for generated vectors |
| m5: Expand core coverage | To do | Subsampling/bitdepth/odd sizes (no alpha/animation) |
| m6 (optional): Transparency | To do | Alpha auxiliary items + premultiplied handling |
| m7 (optional): Animation | To do | Image sequences / tracks |
| m8 (optional): Derived images | To do | `grid`/`tmap`/`sato` (detect earlier; implement later) |
| m9 (optional): Encoder work | To do | Including lossless encoding (not required for decoder) |

---

## Tracked checklist (meaningful order)

This is the working checklist we’ll actively keep up-to-date as we move from “probe” to “first pixel” without shortcuts.

### m3b.D — Finish entropy decode spine (no pixels yet)

- [x] Traverse full partition tree (boundary-forced SPLIT included)
- [x] Decode block0 mode-info prefix (`skip`, `y_mode`, `uv_mode`, CFL/angles/filter-intra where applicable)
- [x] Add persistent luma coeff contexts (Above/Left level + DC sign categories)
- [x] Decode coeff milestones for block0 through `dc_sign` (keeping strict symbol order)
- [x] Exercise coeff contexts across blocks by decoding block1 through `txb_skip`
- [x] Extend block1 to the next coeff milestones: `eob_pt` and derived `eob`
- [x] Extend block1 further to `coeff_base_eob` (stop after that)
- [x] Extend block1 further to `coeff_base` (stop after c==0 coeff_base milestone)
- [x] Generalize residual decoding beyond the “first luma tx block” (iterate transform blocks)
- [x] Add chroma-plane residual traversal (UV coeff contexts + decoding)
- [ ] Decode all leaf blocks (remove “stop after N blocks” once we have full per-block decode paths)
- [ ] Make `exit_symbol()` meaningful on at least one tiny generated tile by stopping at true end-of-tile

### m3b.E — Reconstruction (pixels)

- [ ] Implement inverse quant + inverse transform for the baseline tx sizes we decode
- [ ] Implement intra prediction (start with DC + a minimal set, then expand)
- [ ] Reconstruct luma plane end-to-end on a tiny generated vector (hash gate)
- [ ] Reconstruct chroma planes (subsampling-aware) and crop to displayed dimensions

### m3b.F — In-loop filters (spec order)

- [ ] Deblocking (parse + apply)
- [ ] CDEF (parse + apply)
- [ ] Loop restoration (parse + apply)

### m4 — RGB + PNG output

- [ ] YUV -> RGB conversion (range + matrix via `colr`/CICP)
- [ ] Minimal PNG writer (stored DEFLATE blocks)
- [ ] First end-to-end `.avif -> .png` for generated 1x1 with oracle pixel equivalence

### Testing + documentation (always on)

- [x] Deterministic generated-vector verifier gates every added milestone (`make test`)
- [ ] Add a “first pixel” oracle gate (hash of RGBA) once reconstruction starts
- [ ] Keep [src/m3b-av1-decode/README.md](src/m3b-av1-decode/README.md) updated as flags/output fields evolve

---

## Spec concerns / gotchas log

This section is intentionally cumulative: if we discover a spec nuance or a real-world file that contradicts an assumption, we write it down here and adjust scope/validation before moving on.

Current concerns worth designing for early:
- Payload may be in `idat` instead of `mdat` (and then `mdat` is not required). Extraction must not assume a single `mdat` payload.
- `meta` hierarchy is extensible: unknown boxes may appear and can often be ignored, but **essential** properties cannot.
- Box/property versions: versions listed in the AVIF “expected boxes” tables are normative unless a property is marked non-essential.
- Primary item may be **derived** (e.g., `grid`) rather than a coded `av01` item; detect and fail clearly (or follow references) rather than mis-decode.
- `av1C` constraints (AVIF v1.2.0 §2.2.1):
  - Sequence Header OBUs should not be present in `av1C`; if present they must match the item payload.
  - `av1C` fields must match the Sequence Header in the item payload.
  - If `pixi` is present, its bit depth and channel count must match values derived from `av1C`.
- `ispe` semantics (AVIF v1.2.0 §2.2.2): for AV1 images, `ispe.image_width` == AV1 `UpscaledWidth` and `ispe.image_height` == AV1 `FrameHeight` for the selected representation.
  - The selected representation depends on `a1op` (operating point) and `lsel` (layer selection) when present.
- Alpha items: `colr` should be omitted; if present, readers shall ignore it. Alpha bit depth must match the master.
- Transformative item properties: ordering restrictions apply (MIAF/HEIF). Additionally, `clap` origin is anchored at (0,0) unless the full uncropped image item is included.
- AV1 bitstreams in the wild may split the header out into `OBU_FRAME_HEADER` / `OBU_REDUNDANT_FRAME_HEADER` and can repeat identical header OBUs interspersed with `OBU_TILE_GROUP`.
- Reference decoder interop: `aomdec` accepts raw size-delimited OBU streams; the `dav1d` CLI often requires a container (IVF/AnnexB). We can wrap extracted `.av1` as IVF for dav1d.

Observed during m1 implementation/testing (so far):
- Many third-party files use `pixi` and `colr` marked essential; we must preserve/interpret these later (m4/m5).
- Some versioned boxes/properties may appear with versions we don't fully parse yet; m1 treats unknown versions as "details unavailable" but must still keep item graph intact.
- Extraction constraints to keep in mind:
  - `data_reference_index != 0` (external data) is rejected as unsupported.
  - `construction_method=2` (item-based construction) is unsupported.
  - extents with unknown/implicit length are unsupported.
 - Some real-world files have a primary item that is not a coded `av01` item; m2 treats these as unsupported (likely derived items / other representations).

## Milestones

### m0 — Container box walker (ISO-BMFF / HEIF skeleton)
**Goal**: Parse the ISOBMFF box structure robustly and deterministically.

Deliverables:
- `src/m0-container-parser/avif_boxdump` (CLI): prints a tree of boxes with offsets/sizes/types.
- Handles:
  - 32-bit and 64-bit box sizes
  - `uuid` extended type
  - recursive container boxes (generic)
  - graceful error handling (truncated file, invalid sizes)

Verification / metrics:
- Runs on every file in `testFiles/generated/**` and the third-party corpus without crashing.
- Reports `ftyp` as the first top-level box when present (see AVIF v1.2.0 §9.1.1: “FileTypeBox is required to appear first…”).
- Produces stable output (same input → same dump).

Oracle comparison:
- We don’t have `avifdump` installed; for m0 we validate via:
  - consistency across runs
  - sanity checks (monotonic offsets; box end within parent; no overlap)

Exit criteria:
- Box walker can locate top-level `meta` and `mdat` boxes and recurse into `meta`.

---

### m1 — Parse `meta` box essentials (item model scaffolding)
**Goal**: Parse enough HEIF item metadata to locate the primary image item and its payload.

Scope (minimum viable AVIF image item):
- From AVIF v1.2.0 §9.1.1 “Minimum set of boxes” (image items):
  - `meta` (v0)
    - `hdlr` (v0)
    - `pitm` (v0/v1)
    - `iloc` (v0/v1/v2)
    - `iinf` (v0/v1) → `infe` (v2/v3)
    - `iprp` → `ipco` and `ipma`
      - property types we must recognize early: `av1C`, `ispe`, `pixi`

Deliverables:
- `src/m1-meta-parser/avif_metadump` (CLI): prints decoded fields for the above boxes.
- Data structures:
  - item table: id → type, name (optional), extents (offset/length), construction method
  - property associations for the primary item

  Important early detection (AVIF v1.2.0 §2.* / §4.*):
  - Layering/operating points: `a1op` (if present, shall be essential), `lsel`, `a1lx` (shall not be essential)
  - Auxiliary items: `auxC` (alpha/depth), `prem` (premultiplied alpha)
  - Transformative properties: `irot`, `imir`, `clap` (affect how pixels are presented)
  - Derived items: `grid`, `tmap`, `sato` (must detect early; decode later)

Current status:
- Implemented and built `avif_metadump`.
- Verified it runs without crashing on:
  - generated vectors (`testFiles/generated/**`)
  - full third-party corpus (172 `.avif` files)
- Best-effort primary payload extraction exists as `avif_metadump --extract-primary OUT ...` for simple cases (construction_method 0/1), but the dedicated m2 tool will own oracle-equivalence verification.

Verification / metrics:
- For each AVIF in the generated vectors *and* the third-party corpus, we can:
  - identify `pitm` (primary item id)
  - find an `infe` entry for that id with item_type `av01` (for the primary image in simple files)
  - find at least one extent in `iloc` for that item

Robustness expectations (especially for third-party files):
- If the primary image is not a simple coded `av01` item (e.g., derived, alpha/aux, sequence/track-based), we must detect that and report a clear “unsupported (yet)” reason (not mis-parse or crash).

Oracle comparison:
- `avifdec --info` output (where available) matches our extracted width/height once we parse `ispe`.

Spec-driven checks:
- If `pixi` is present for the primary item, it must match bit depth + channel count derived from `av1C` (AVIF v1.2.0 §2.2.1).
  - Enforced on generated vectors by: `python3 tools/verify_generated_m1_pixi_av1c.py`.

Exit criteria:
- We can extract a byte-for-byte payload for the primary `av01` item for the supported simple still-image path.
- For files outside m1 scope (derived primary items, sequences, external data references), we fail clearly with an “unsupported (yet)” reason.

---

### m2 — Extract AV1 Image Item Data (OBU / sample equivalence)
**Goal**: Emit the AV1 sample (the actual coded bytes) for the primary item.

Deliverables:
- `src/m2-av1-extract/avif_extract_av1` (CLI): writes `primary.av1` from an input `.avif`.
- OBU scanner (minimal): validate “exactly one Sequence Header OBU” for still images where applicable (AVIF v1.2.0 §2.1).

Current status:
- Implemented `avif_extract_av1` and integrated it into `make all`.
- Generated vectors: extraction OK + minimal OBU validation passes.
- Third-party corpus sweep: many files extract successfully; a small number fail with a clear unsupported reason (primary not `av01`).

Verification / metrics:
- For generated vectors: `primary.av1` extracted from the `.avif` must be stable (byte-identical across runs) and pass minimal OBU validation.
- For third-party corpus: attempt extraction for files that fit the “simple still coded item” path; otherwise emit a clear unsupported reason.

Oracle comparison:
- `avifdec` must decode the original `.avif` (baseline sanity).
- Optional later: decode `primary.av1` with a separate AV1 toolchain to confirm sample equivalence.

Exit criteria:
- We reliably extract the correct AV1 bytes for simple still images.

Status:
- Implemented `avif_extract_av1`.
- Generated vectors (lossless + lossy presets) pass extraction + minimal OBU validation.
- Third-party corpus: 169 files extracted successfully; 3 fail clearly because primary item is not a coded `av01` item.

---

### m3a — AV1 bitstream parser + header validation (no reconstruction)
**Goal**: Parse AV1 OBUs and headers robustly and extract metadata we can compare against the oracle.

Rationale:
- This de-risks the AV1 work by delivering a verifiable, useful chunk before implementing the full reconstruction pipeline.

Deliverables:
- `src/m3a-av1-parse/` bitreader + OBU scanner/parser.
- CLI: prints a concise summary for an extracted `primary.av1` (or directly from `.avif`):
  - OBU types encountered and sizes
  - sequence header fields needed for AVIF (profile, still_picture, reduced_still_picture_header, bit_depth, subsampling, color config)
  - coded dimensions (where available)

Current status:
- Implemented `av1_parse` for size-delimited OBU streams.
- Parses the Sequence Header OBU and prints: profile, still_picture, reduced_still_picture_header, bit_depth, monochrome, subsampling.
- Note: for many AVIFs, presentation dimensions and CICP/range are signaled via container properties (`ispe`, `colr`) rather than being reliably derivable from the reduced still-picture Sequence Header alone.


Observed so far:
- Third-party corpus: m2 extracts 169/172 files; m3a parses 169/169 extracted samples.

Verification / metrics:
- Runs on extracted samples from generated vectors and on as many third-party samples as can be extracted (m2), without crashing.
- For generated vectors: AV1 bitstream **bit depth** and **chroma format** (YUV420/422/444 or monochrome) match what the oracle reports (`avifdec --info`).
  - Enforced by: `python3 tools/verify_generated_m3a.py`.
- For third-party corpus: `python3 tools/sweep_m3a.py` reports `m3a parse ok == m2 extract ok` (currently 169/169).

Exit criteria:
- `tools/verify_generated_m3a.py` passes on all generated vectors.
- `tools/sweep_m3a.py` shows all extracted third-party samples are parsed without failure.

---

### m3b.D — Tile syntax traversal (intra path scaffolding)

Current state (probe-only; stops after first leaf block to preserve bit alignment):
- Full partition recursion including boundary-forced SPLIT.
- `decode_block_stub()` now consumes in-spec intra order (for the first block):
  - `skip`
  - `y_mode`
  - optional `angle_delta_y`
  - `uv_mode` (CFL-allowed vs not-allowed CDF selection is spec-correct)
  - optional `cfl_alpha_signs/u/v` when `uv_mode == UV_CFL_PRED`
  - optional `angle_delta_uv`
  - optional `use_filter_intra` + `filter_intra_mode` when `enable_filter_intra` and block is eligible
  - tx/residual scaffolding (first luma transform block only):
    - `tx_size`
    - `txb_skip` (a.k.a. `all_zero`)
    - when `txb_skip==0`: `eob_pt_*` (records `eobPt = eob_pt + 1`)
    - when `txb_skip==0`: derived `eob` via `eob_extra` + `eob_extra_bit`
    - when `txb_skip==0`: `coeff_base_eob` (records last-coeff base level)

Quality gate:
- `make test-generated` stays green (currently 32/32).

Next steps:
- Decide how to represent/compute `allow_screen_content_tools` and either:
  - implement `palette_mode_info()` (or)
  - explicitly fail/skip safely when it’s enabled.
- Continue tx / residual scaffolding (still stopping early but consuming symbols in-order):
  - implement proper context derivation (currently simplified for the first block)
  - iterate tx blocks (not only the first luma block)
  - advance deeper into coefficient parsing after `coeff_base_eob`
- Output is deterministic across runs for the same input (same summary values).

---

### m3b — Minimal AV1 still-image reconstruction (narrow subset)
**Goal**: Produce decoded YUV planes for a narrow still-picture subset.

This milestone is big; we split it into smaller gated sub-milestones.

#### m3b.1 — Frame header subset (implemented)
- CLI: `build/av1_framehdr` parses Sequence Header + first frame header and prints dimensions.
- Handles: still_picture=1 with either reduced_still_picture_header=1 or reduced_still_picture_header=0 (keyframe shown).
- Also handles bitstreams that separate the header into `OBU_FRAME_HEADER`/`OBU_REDUNDANT_FRAME_HEADER` (no `OBU_FRAME`).

#### m3b.2 — tile_info() + OBU_TILE_GROUP boundary discovery (implemented for separate tile-group OBUs)
- `build/av1_framehdr` additionally parses `tile_info()` and prints tile grid info.
- If tiles are carried in separate `OBU_TILE_GROUP` OBUs (after a `OBU_FRAME_HEADER`), it prints per-tile payload byte ranges.
- Deterministic tests (synthetic raw AV1 OBUs):
  - Generate: `python3 tools/gen_av1_tilegroup_vectors.py` → `testFiles/generated/av1/*.av1`
  - Verify: `python3 tools/verify_generated_m3b_tilegroups.py`

Known limitation (tracked for next sub-milestone): many real-world AVIFs embed tile groups inside `OBU_FRAME`.

#### m3b.3 — Handle tiles embedded in OBU_FRAME (implemented)
- `build/av1_framehdr` now locates the end of `frame_header_obu()` inside `OBU_FRAME` (byte-aligned boundary) and prints per-tile payload byte ranges for the embedded tile group.
- Verified on generated AVIF vectors:
  - `python3 tools/verify_generated_m3b_embedded_tilegroups.py`

Note: film grain parameters are skipped so tile-boundary parsing remains reliable even when `apply_grain=1`.

Current scope for reconstruction work (next steps):
- still_picture=1 keyframe still images (start intra-only)
- start with 8-bit, but be prepared to handle YUV444 early (e.g. `tiny-1x1-red` is YUV444 per `avifdec --info`)

Deliverables:
- `src/m3b-av1-decode/` minimal AV1 frame decode to an internal YUV buffer.

Verification / metrics:
- Generated vectors: decode completes deterministically and produces the expected dimensions/plane layouts.
- Third-party corpus: decode supported files; for unsupported features, fail with a clear reason (never crash).

Current m3b step-1 metrics:
- Generated vectors oracle check: `python3 tools/verify_generated_m3b.py` passes (32/32).
- Third-party corpus sweep: `python3 tools/sweep_m3b.py` currently reports 144 parses OK out of 169 extracted; remaining failures are non-still streams (still_picture!=1).

Current m3b.2 metrics:
- Synthetic tile-group vectors: `python3 tools/verify_generated_m3b_tilegroups.py` passes.

Current m3b.3 metrics:
- Generated AVIFs embedded tile-group check: `python3 tools/verify_generated_m3b_embedded_tilegroups.py` passes.

Exit criteria:
- Successful deterministic decode of the generated baseline images into YUV.
- Pixel equivalence gating begins in m4 once we have color conversion + PNG output.

Additional incremental gates (testable, spec-driven):

#### m3b.A — Tile payload dump + safety invariants (implemented)
- Extend `build/av1_framehdr` with `--dump-tiles DIR` to emit:
  - `frame_info.txt` (key header fields for the decoded frame)
  - `tg*_tile*_r*_c*.bin` (raw tile payload bytes)
- This gate is about making the tile payload boundaries tangible and regression-testable.
- Verification:
  - Generated vectors: `make test-generated` (C-only)
  - Third-party corpus: `make sweep-corpus-m3b` (C-only)

#### m3b.B — Tile-level bitstream guardrails (to do)
- Add a strict “no overread” harness for tile payload parsing/decoding:
  - the tile decoder must never read beyond the tile payload boundaries computed from `tile_info()` / tile-group headers.
- Verification:
  - Deterministic: synthetic `testFiles/generated/av1/*.av1`
  - Corpus: `make sweep-corpus-m3b` must show no crashes; unsupported must be explicit.

Status update (implemented for boundary parsing):
- Tile-group parsing now enforces strict byte accounting:
  - every tile size is bounds-checked against the remaining payload
  - tile-group payload must be fully consumed (no trailing bytes tolerated)
- These guardrails ensure the tile payload boundaries we compute are self-consistent and safe to hand to later tile parsing/decoding.
- Verification:
  - `make test-generated`
  - `make sweep-corpus-m3b`

#### m3b.C — Entropy decoder + tile-level probes (in progress)
Goal: implement AV1’s per-tile symbol decoder and add syntax-independent validation probes.

Deliverables:
- `src/m3b-av1-decode/av1_symbol.*`: init_symbol/read_symbol/read_bool/read_literal/exit_symbol
- CLI plumbing in `build/av1_framehdr` to run probes on extracted tile payload buffers

Sub-milestones (keep them small and measurable):

##### m3b.C.1 — Symbol decoder core
Exit criteria:
- `make test-symbol` passes.
- Deterministic: can init and read symbols from a known test stream without UB/crashes.

##### m3b.C.2 — Trailing-bits check (syntax independent)
Rationale: valid as soon as tile payload boundaries are correct; does not require tile syntax decode.

Exit criteria:
- `make test-m3b-tile-trailing` passes on synthetic trailing-only vectors.
- Corpus diagnostics: `make sweep-corpus-m3b-trailingbits` produces no crashes; strict mode can be used for investigation.

##### m3b.C.3 — exit_symbol() probe (diagnostic-only until m3b.D is real)
Rationale: `exit_symbol()` only becomes a correctness check once we decode tile syntax up to the correct end-of-tile point.

Exit criteria (for now):
- Probe exists and never crashes; failures are reported with tile indices.
- Deterministic synthetic gates exist and pass:
  - `make test-m3b-tile-exit1bool`
  - `make test-m3b-tile-exit8bool`
  - `make test-m3b-tile-exit8bool-2x2`

Important note:
- For real-world tiles, `exit_symbol()` failures are expected until m3b.D can fully traverse tile syntax.

#### m3b.D — Tile syntax traversal (intra path scaffolding) (in progress)
Goal: incrementally traverse entropy-coded tile syntax (still pictures, intra-only first).

Deliverables:
- `src/m3b-av1-decode/av1_decode_tile.*`: tile syntax probe entrypoint callable from `av1_framehdr`.

Sub-milestones:

##### m3b.D.0 — Tile syntax probe prelude
What it does:
- Initializes the symbol decoder for each tile.
- Computes per-tile MI dimensions and superblock grid.
- Must never overread tile payload.

Exit criteria:
- `av1_framehdr --decode-tile-syntax` runs on all generated vectors without crashing.
- Third-party sweep is allowed to report UNSUPPORTED, but must not crash.

##### m3b.D.1 — Root partition milestone
What it does:
- Decode (or force, per spec boundary rules) the root superblock partition decision.
- Report the decoded/forced decision in `av1_framehdr` output.

Exit criteria:
- On generated 1-tile and 2x2 synthetic vectors, the probe reports `root_part=...` for every tile without ERROR.
- On tiny real frames (e.g. 1x1 red), boundary-forced behavior is handled (no ERROR).

##### m3b.D.2 — Full recursive partition traversal
What it does:
- Implement `decode_partition(r,c,bSize)` recursion down to leaf `decode_block()` calls.
- Add minimal MI-grid state to compute partition contexts (`ctx`) per spec.
- Implement split_or_horz/split_or_vert symbol decode paths (CDFs + mapping).

Exit criteria:
- Deterministic: on generated vectors, the traversal completes the partition tree for each tile without reading past payload.
- Diagnostic: `exit_symbol()` starts becoming meaningful on synthetic trailing-only tiles (should pass when we stop at the right place).

##### m3b.D.3 — decode_block() skeleton (no pixels yet)
What it does:
- Parse block-level mode info enough to walk the syntax correctly (even if we don’t reconstruct yet).
- Current implementation decodes a safe prefix for the first encountered blocks:
  - `skip`
  - `y_mode` (+ optional `angle_delta`)
  - `uv_mode` (+ optional `angle_delta`)
  - Uses correct UV CDF selection (CFL allowed vs not allowed) and skips chroma syntax for monochrome.
  - Begins tx/residual signaling (probe-only; first luma transform block only):
    - `tx_size` → `txb_skip` → optional `eob_pt_*`

Next steps (keep them incremental and bit-alignment-safe):
- If `uv_mode == CFL`, decode `cfl_alpha_signs` + `cfl_alpha_u/v` (requires CflSign/CflAlpha CDFs and quantizer-index selection).
- Add an explicit `is_inter`/FrameIsIntra guard so we fail clearly if a still stream surprises us with inter-coded syntax.
- Expand tx/residual traversal beyond the first luma block and continue deeper into coeff parsing after `eob_pt`.

Exit criteria:
- Traversal reaches a stable “end-of-tile” point on at least one tiny generated vector without parse errors.

##### m3b.D.4 — First pixels (Y plane only)
What it does:
- Implement minimal intra prediction + residual decode for a narrow subset to reconstruct luma.

Exit criteria:
- Deterministic Y-plane hash matches oracle-derived hash for the smallest generated vector we choose.

##### m3b.D.5 — Full YUV planes for baseline stills
Exit criteria:
- For generated baseline vectors, decode produces correct YUV plane hashes (and later feeds m4 PNG output).

#### m3b: Oracle parity tests (cross-cutting)
As decoding advances, keep adding tight parity checks against `avifdec`.

Current parity gates:
- Metadata parity on generated vectors via `make test-avifdec-info`.

Deterministic gate added:
- Synthetic tile-group vectors now include `*trailingonly*.av1` variants where each tile payload is only valid trailing bits.
  These should pass `--check-tile-trailing-strict` even without tile syntax decoding.
- Verification: `make test-m3b-tile-trailing` (also runs as part of `make test-generated`).

#### m3b.E — Expand prediction/transforms incrementally (to do)
- Add more intra modes, transform types/sizes, chroma planes, subsampling, odd sizes.
- Verification:
  - Expand generated set and keep corpus sweep green (no crashes).

Performance gates (optional but encouraged during m3b):
- Add a lightweight benchmark harness to time:
  - our pipeline stages (m2 extract, m3a parse, m3b frame/tile parsing)
  - optionally compare `avifdec --info` / decode time when `avifdec` is installed.
- Target: stable medians (repeatable) and no pathological slow cases on the third-party corpus.

Next spec-driven decode preconditions (AV1 bitstream §frame header / tile group):
- Parse `tile_info()` and use it to safely locate tile payload boundaries in `OBU_TILE_GROUP`.
- Add deterministic tests using synthetic raw AV1 OBU streams that already contain `OBU_FRAME_HEADER` + `OBU_TILE_GROUP`:
  - Generate: `python3 tools/gen_av1_tilegroup_vectors.py`
  - Verify: `python3 tools/verify_generated_m3b_tilegroups.py`
- Treat `OBU_FRAME_HEADER` copies as required-to-match duplicates when present.

---

### m4 — Color conversion + PNG writer
**Goal**: Convert decoded YUV (and optional alpha) to RGBA and write PNG with no external libs.

Deliverables:
- `src/m4-png-out/avif_to_png` (CLI): input `.avif` → output `.png`.
- PNG writer implemented with:
  - zlib header + DEFLATE “stored blocks” (no compression) + Adler32
  - CRC32 for PNG chunks

Verification / metrics:
- Byte-identical PNG output is NOT required (encoders differ), but pixel equivalence is.
- Compare against oracle pixels by hashing raw RGBA extracted from:
  - our PNG decode of our output
  - oracle output PNG from `avifdec`

Exit criteria:
- End-to-end `.avif` → `.png` for generated baseline test vectors.

---

### m5 — Expand core coverage (profiles, subsampling, bit depth, odd sizes)
**Goal**: Incrementally support common still-image AVIF features (excluding alpha and animation):
- YUV444/YUV422, monochrome
- 10/12-bit
- odd width/height edge cases (many exist in `testFiles/Link-U/*odd-*`)

Verification / metrics:
- Pixel equivalence against oracle for a curated corpus.
- Fuzz-ish robustness: malformed box sizes, unknown boxes, truncated extents.

---

## Optional late milestones (nice-to-have)

These are explicitly de-prioritized until the core still-image path is solid.

### m6 (optional) — Transparency (alpha)
**Goal**: Support alpha auxiliary items and compositing rules.

Scope:
- Parse and link alpha auxiliary image items (`auxl` relationships + aux type `urn:mpeg:mpegB:cicp:systems:auxiliary:alpha`).
- Enforce/validate that alpha bit depth matches the master image.
- Respect premultiplication signaling (`prem`) if present.

Verification / metrics:
- Pixel equivalence vs oracle on a transparency-focused corpus.

### m7 (optional) — Animation (image sequences)
**Goal**: Support AVIF image sequences / tracks.

Scope:
- Parse track-based structures as needed (`moov`/`trak`…) and emit a simple frame dump.

Verification / metrics:
- Frame count/timestamps/dimensions match oracle output.

### m8 (optional) — Derived images
**Goal**: Support derived image items used in real-world AVIF.

Scope:
- `grid` (tiled images)
- `tmap` (tone map derived items)
- `sato` (sample transform; higher precision workflows)

Verification / metrics:
- Pixel equivalence vs oracle on derived-image corpora.

### m9 (optional) — Encoder work (incl. lossless)
**Goal**: Not required for a decoder; only consider once decode is correct.

---

## Generated tests (controlled vectors)
We will create a small set of deterministic test images, encode them with `avifenc`, and decode with `avifdec` to produce oracle PNGs.

Planned assets (RGBA patterns):
- 1x1: solid red (and optionally transparent variants later)
- 4x4: 2x2 quadrant colors
- 16x16: gradient / checkerboard
 - odd sizes: small checkerboard / diagonal lines (exercise odd width/height early)
 - shapes: stripes, radial gradient, hard-edged circle

Encoding presets (per base PNG):
- lossless baseline
- lossy presets at multiple quality points (very low, medium, high)

Locations:
- Source PNGs: `testFiles/generated/src_png/`
- Encoded AVIFs: `testFiles/generated/avif/`
- Oracle decoded PNGs: `testFiles/generated/oracle_png/`

Metrics:
- For each generated test vector:
  - `avifdec` round-trip produces expected dimensions
  - our decoder output matches oracle pixels (hash of RGBA)

---

## General engineering rules
- Every parser is bounds-checked; every size is validated.
- No heap allocation until needed; when used, every allocation is checked.
- No undefined behavior: avoid unaligned reads; handle integer overflow.
- Deterministic output; error messages include offsets/types.

---

## Notes
- The container side is quite tractable; the AV1 decoder is the long pole.
- The step-by-step approach is the right way: we’ll keep m0–m2 purely about *finding and extracting* the AV1 sample before touching the codec.
- PNG output without zlib is still feasible (stored DEFLATE blocks); we’ll implement that in m4.
