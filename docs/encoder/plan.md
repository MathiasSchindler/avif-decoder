# AVIF encoder roadmap

The dependency-free 0.1 encoder is the baseline for this roadmap. It already
writes deterministic, interoperable 8-bit 4:2:0 reduced-still AVIF files from
caller-owned planar input, with a freestanding PNG/JPEG command-line adapter.
The next phase develops that baseline toward practical AVIF encoder parity in
ten ordered goals spanning **speed**, **features**, and **encoding quality**.

| Goal | Primary outcome | Speed | Features | Quality | Status |
| --- | --- | :---: | :---: | :---: | --- |
| 1. Measurement and regression budgets | Reproducible decisions | Yes |  | Yes | Complete |
| 2. Multi-tile encoding and bounded parallelism | Large images without resizing | Yes | Yes |  | Complete |
| 3. Variable partitions and transforms | Better local adaptation | Yes | Yes | Yes | Complete |
| 4. Complete intra and chroma prediction | Stronger still-image coding | Yes | Yes | Yes | Complete |
| 5. Quantization, lossless, and rate control | Useful quality/size controls | Yes | Yes | Yes | Complete |
| 6. In-loop filters and restoration | Better reconstruction per byte | Yes | Yes | Yes | Planned |
| 7. Bit-depth and chroma-format parity | Monochrome, 4:2:2, 4:4:4, HDR |  | Yes | Yes | Planned |
| 8. Alpha and metadata-rich still images | Practical AVIF item parity |  | Yes | Yes | Planned |
| 9. Grids and layered still images | Scalable derived images | Yes | Yes |  | Planned |
| 10. Image sequences and inter prediction | Timed AVIF and temporal coding | Yes | Yes | Yes | Planned |

## Non-negotiable constraints

Every goal keeps the properties that define this project:

- the encoder implementation remains freestanding C11; existing minimal
  startup and raw-syscall assembly stays confined to the platform substrate;
- the build remains `nolibc`: no external runtime, codec, image, threading, or
  standard C library dependency is introduced;
- source incorporated into the project must remain compatible with the CC0
  dedication;
- core APIs perform no allocation and no file I/O;
- input, workspace, output, executor state, and metadata remain caller-owned;
- all size arithmetic, writes, and worker-local regions remain explicitly
  bounded and queryable before encoding;
- existing planar API calls, quantizer values, speed values, and CLI forms
  remain valid; extensions are additive;
- output is deterministic across runs, workspace alignments, worker counts,
  Linux/x86-64, and macOS/arm64;
- external implementations may be used by optional reference tests, but never
  by production builds or self-contained tests.

The target is useful encoder parity, not one encoder implementation for every
syntax the decoder can consume. Decoder features that do not improve common
encoding workflows should not displace work on speed or reconstruction quality.

## Priority rationale

Feature parity is the direction, but container checkboxes should not be the
first implementation step. Multi-tile output removes the current large-image
resizing limitation and creates the safest unit of parallel work. Better block,
transform, prediction, quantization, and filtering decisions then improve every
later pixel format and container feature. High bit depth, alpha, grids, and
sequences build on those coding tools rather than cloning the current minimal
path several times.

Each goal is independently releasable. A goal is complete only when its public
contract, strict and sanitizer coverage, fuzz coverage, external
interoperability, documentation, and measured speed/quality effects land
together.

## Goal 1: Measurement and regression budgets

Establish a stable scorecard before changing coding decisions. Use a compact
checked-in corpus covering natural photographs, animation-like art, text and
sharp edges, noise, gradients, chroma detail, odd block edges, and the current
minimum and maximum practical image sizes. Keep source provenance compatible
with CC0 and avoid downloaded test data in the self-contained suite.

Measure the dimensions that later goals must trade deliberately:

- encode time, megapixels per second, peak workspace, and output capacity;
- encoded bytes and bits per pixel;
- exact encoder/decoder reconstruction agreement;
- luma and chroma SSE/PSNR, plus a small dependency-free structural metric;
- per-stage work counts for prediction trials, transforms, symbols, filters,
  and tiles;
- deterministic output checksums on Linux/x86-64 and macOS/arm64.

Add a reproducible benchmark command that emits machine-readable records and a
human summary. Performance numbers are informative by default; stable operation
counts, output checksums, workspace limits, and quality floors are test gates.
Wall-clock regression budgets are maintained for named reference machines so
normal hardware variance does not make `make test` flaky.

This goal is complete when every later proposal can be compared against a
versioned 0.1 baseline by speed, memory, size, and reconstruction quality, and
when accidental regressions fail a focused test rather than relying on visual
inspection.

Implemented by `tests/encoder_benchmark.c`, the checked-in JSON Lines baseline,
the optional `AvifencStatistics` API, and the `encoder-scorecard`,
`encoder-benchmark`, and `encoder-benchmark-json` Make targets. The scorecard is
part of `make test-encoder`; timing remains non-gating. Corpus definitions,
metric semantics, the first named reference-host budget, and baseline review
rules are recorded in [`benchmark.md`](benchmark.md).

## Goal 2: Multi-tile encoding and bounded parallelism

Replace the one-tile assembly assumption with explicit AV1 tile columns, tile
rows, and tile-group serialization. Preserve one tile as the default for images
where it is legal, while selecting a deterministic multi-tile layout for larger
images. The CLI must stop resizing an otherwise supported image merely because
it exceeds the current one-tile superblock limit.

Introduce an optional caller-owned encoder executor following the decoder's
`parallel_for` model. Tiles own their entropy state, reconstruction region,
scratch, and payload span so they can encode independently without locks.
Query accounts for the requested worker width and exposes the complete
worker-local workspace before encoding. Serial and parallel calls must produce
identical bytes, not merely equivalent pixels.

Speed levels continue to describe coding effort, not thread count. Worker count
is orthogonal and defaults to one. The freestanding CLI may expose `--workers`
using the existing newos task-pool substrate on supported platforms; the core
never creates threads itself.

This goal is complete when large images encode without implicit resizing,
multi-tile output decodes exactly through the in-tree decoder, libavif, libaom,
and FFmpeg, worker counts produce byte-identical files, and representative
large images show useful multicore scaling without increasing serial output
size or reducing quality.

Implemented by the dimension-derived uniform tile layout in
`src/encoder/av1_write.c`, executor-aware query and encode APIs, independent
tile jobs and ordered serialization in `src/encoder/avifenc.c`, and the CLI's
`--workers` adapter. Strict layout vectors, hosted concurrent sanitizer tests,
CLI byte-identity tests, the nine-case scorecard, and external interoperability
cover the contract. On the named M4 Max host, a hosted two-worker executor
encodes the balanced 8192x64 case 1.95x faster than serial. The freestanding
Linux and macOS CLIs both use the native task pool when multiple workers are
requested.

## Goal 3: Variable partitions and transforms

Replace the fixed descent to 4x4 coding blocks with bounded recursive partition
search over the useful square and rectangular AV1 partition shapes. Add forward
transforms, coefficient scans, contexts, and syntax for 8x8, 16x16, and 32x32
sizes before considering larger or exotic transform shapes. Reuse decoder
inverse transforms and reconstruction only where their contracts already fit.

Use deterministic rate-distortion search with exact syntax-cost estimates from
trial CDF state. Candidate state is isolated from committed state. Avoid an
exponential search: reject clearly unsuitable partitions from source variance,
edge activity, and parent results, and place explicit trial budgets behind each
speed level.

- speed 2 keeps a narrow, low-overhead partition and transform path;
- speed 1 evaluates a bounded practical subset;
- speed 0 performs the broadest search supported by the workspace contract.

This goal is complete when all supported edge geometries round-trip exactly,
larger smooth regions use larger blocks/transforms, detailed regions retain
smaller units, speed levels have monotonic work budgets, and the corpus improves
rate-distortion results without an unbounded time or workspace increase.

Implemented by bounded `NONE`, `SPLIT`, `HORZ`, and `VERT` selection for fully
visible blocks through 32x32, with activity pruning before deterministic
rate-distortion trials. Each node evaluates only one partition level; only the
committed split recurses. A fixed 2 KiB caller-workspace checkpoint isolates
luma reconstruction while local checkpoints isolate coding-block state, and
transform trials leave coefficient state unchanged. Speed 0, 1, and 2 use
monotonically narrower mode and partition trial budgets.

The transform path supports DCT_DCT at 4x4, 8x8, 16x16, 32x32, and the six
2:1 shapes from 4x8 through 32x16. Focused sanitizer vectors cover smooth,
detailed, vertical-edge, horizontal-edge, padded-edge, workspace-alignment,
and deterministic reconstruction behavior. The nine-case scorecard records
the reviewed rate-distortion and work changes; external FFmpeg, libaom, dav1d,
and libavif comparisons decode both rectangular orientations exactly.

## Goal 4: Complete intra and chroma prediction (complete)

Prediction now covers the high-value AV1 intra tools already understood by the
decoder:

- directional luma and chroma modes with legal angle deltas;
- all smooth variants and Paeth where applicable;
- chroma-from-luma with bounded alpha search;
- filter intra for suitable small luma blocks;
- palette coding for low-color images and screen content;
- intra block copy only after overlap, dependency, and search bounds are proven.

Prediction and transform trials share one rate-distortion decision rather than
choosing modes from distortion alone. Chroma distortion participates in the
score with documented weighting, so luma gains cannot hide severe color loss.
Fast levels use source classification and winning-neighbor hints to prune mode
and angle trials; they must not silently change the supported bitstream surface.

This goal is complete when each new mode has strict vectors against decoder
predictors, syntax matches external decoders, screen-content fixtures improve
materially with palettes, photographic chroma improves with CfL or directional
prediction, and the default corpus has no quality regression at comparable
size.

The bounded implementation now covers all listed tools except intra block copy.
Intrabc remains deferred until overlap-safe DV search and motion-vector stack
infrastructure exist. Strict reconstruction vectors, external decoder checks,
and the deterministic scorecard cover the committed Goal 4 surface.

## Goal 5: Quantization, lossless, and rate control

Turn the fixed base quantizer into a complete but bounded quantization layer.
The existing `--quantizer 1..255` behavior remains available and stable. Add:

- quantizer zero and a genuinely lossless coding path where the selected AV1
  profile permits it;
- separate legal DC/AC and plane deltas;
- quantization matrices and activity-aware selection;
- bounded delta-Q or segmentation for spatial adaptation;
- optional target-quality and target-size modes implemented through a finite,
  deterministic number of encode/analyse passes.

Rate control may use caller workspace for summaries but must not allocate frame
graphs or retain hidden state. Target-size mode reports failure when the target
cannot be reached within declared limits rather than running an open-ended
search. Speed levels cap analysis passes and spatial decisions.

This goal is complete when lossless output is pixel-exact, fixed-quantizer
results remain deterministic, target-quality behavior is monotonic, target-size
fixtures meet documented tolerances, and adaptive quantization improves the
corpus score without unacceptable chroma or edge regressions.

Implemented by the exact 4x4 WHT lossless path, shared encoder/decoder
quantization-step math, legal per-plane deltas and qmatrices, and three ALT_Q
activity segments derived from luma and chroma gradients. AQ retains variable
partitions and clips effective lossy qindices to 1 through 255. Fixed,
target-quality, and target-size modes use caller-owned trial storage and
deterministic 9/7/5 total-pass caps for speeds 0/1/2; unreachable byte targets
fail explicitly.

Strict, sanitizer, parallel, CLI, 1,000-case fuzz, and external FFmpeg,
libaom, dav1d, and libavif checks cover the surface. Quantizer zero is
pixel-exact across external decoders. The default fixed-q scorecard remains
byte-identical to Goal 4. At fixed base qindices, activity AQ strength 12 spends
9.18% more bytes while improving aggregate Y/U/V SSE and structural edge error
on every measured plane; [`benchmark.md`](benchmark.md) records the reviewed
tradeoff and reproducible commands.

## Goal 6: In-loop filters and restoration

Enable encoder-side decisions for the reconstruction stages currently signaled
as identity: deblocking first, then CDEF, followed by Wiener and self-guided
restoration where the measured gain justifies their cost. Super-resolution is
considered only after ordinary filtering is stable; it must win a measured
rate-distortion comparison rather than being enabled for syntax parity.

Every candidate filter is evaluated against the same reconstructed pixels that
subsequent coding decisions and the decoder will observe. Reuse decoder filter
kernels when encode and decode need the identical operation, while keeping
parameter search encoder-local. Store only bounded row, stripe, or restoration
unit summaries; do not multiply full-frame workspace for every candidate.

Speed 2 may retain identity filters. Speed 1 uses cheap deblocking/CDEF
selection. Speed 0 may search restoration units under explicit candidate and
workspace limits.

This goal is complete when filtered reconstruction remains byte-exact with all
reference decoders, ringing/blocking metrics improve on targeted fixtures,
whole-corpus rate-distortion does not regress, and each enabled stage has a
measured benefit larger than its encode-time and signaling cost.

## Goal 7: Bit-depth and chroma-format parity

Generalize the core image description and coding pipeline from 8-bit 4:2:0 to
the decoder's common still-image format surface:

- 8-, 10-, and 12-bit samples;
- monochrome, 4:2:0, 4:2:2, and 4:4:4;
- Main, High, and Professional profile selection derived from the image;
- full- and limited-range signaling with explicit chroma sample position;
- 16-bit PNG input and output-preserving CLI conversion paths where applicable.

Keep the existing `AvifencImage` source-compatible. Add an extended image/API
entry point rather than changing the meaning or layout of established fields.
Internal sample widths, transform intermediates, quantization, clipping, and
workspace calculations must be explicit for every bit depth. The CLI must not
silently reduce precision or subsampling unless the user requests conversion.

This goal is complete when the format matrix passes strict, sanitizer, fuzz,
and external decoder tests; reconstruction is exact at every depth and
subsampling; profile and `pixi`/`av1C` metadata are correct; and high-bit-depth
quality tests show no precision lost inside the encoder pipeline.

## Goal 8: Alpha and metadata-rich still images

Extend AVIF assembly from one color item to practical still-image item graphs:

- auxiliary alpha items with straight and premultiplied relationships;
- independent alpha quantizer and lossless-alpha options;
- ICC and NCLX color information without hidden color transformation;
- EXIF and XMP payload items supplied as immutable caller byte views;
- pixel aspect ratio, clean aperture, rotation, and mirroring properties;
- CLL/MDCV and opaque tone-map or gain-map metadata preservation.

Metadata remains caller-owned and is copied only into the bounded output. Item
IDs, property associations, extents, and ordering are deterministic. Query
includes every auxiliary payload and rejects cyclic, contradictory, oversized,
or unsupported graphs before writing. The CLI preserves supported source
metadata only when it can do so without an external parser or color library.

This goal is complete when alpha and metadata round-trip through the in-tree
decoder and libavif, premultiplication semantics are exact, malformed metadata
cannot alter bounds, and adding metadata does not change encoded color pixels.

## Goal 9: Grids and layered still images

Add AVIF image grids for dimensions or workflows better represented by
multiple independently coded cells. Reuse the multi-tile executor and item
assembly from goals 2 and 8, but keep AV1 tiles and AVIF grid cells as distinct
abstractions. Grid edge cells, chroma alignment, alpha grids, property
inheritance, and cell extents must match the decoder's checked model.

After grids are stable, add a small layered/progressive still-image surface
only if benchmarked use cases justify it. Layer selection and operating-point
metadata must be explicit; do not emit layers merely because the decoder can
parse `a1op`, `lsel`, or `a1lx`. Independent cells/layers may encode in
parallel, but deterministic item and payload ordering is fixed before workers
run.

This goal is complete when grid dimensions no longer require a monolithic
workspace, serial and parallel files are identical, transformed/alpha grids
decode exactly through internal and external implementations, and any layered
mode demonstrates a useful progressive-size or latency tradeoff.

## Goal 10: Image sequences and inter prediction

Build timed AVIF in stages rather than introducing motion estimation and track
serialization simultaneously:

1. write `avis` tracks containing deterministic all-intra frames, durations,
   timescale, sync samples, repetition, and optional synchronized alpha;
2. add retained reference frames and legal inter-frame headers;
3. add bounded integer-pel motion estimation, then selected subpel,
   compound, warped, inter-intra, and skip tools only when measurements justify
   each search surface;
4. add deterministic GOP/keyframe decisions and random-access validation.

The sequence API exposes all frame input, timing, reference, workspace, and
output requirements up front or through an explicit caller-owned streaming
state. It must not hide allocation, file seeking, or an unbounded lookahead.
Parallel work remains inside one frame unless dependencies prove a safe wider
region. Speed levels cap motion ranges, reference counts, partition candidates,
and lookahead.

This goal is complete when all-intra and inter sequences seek and replay exactly
through the in-tree decoder and external implementations, timing and repetition
metadata round-trip, random access starts from the documented sync frame,
temporal coding materially beats all-intra size at comparable quality, and
memory remains bounded by the declared reference and lookahead limits.

## Parity boundary after goal 10

Completing these goals provides broad parity for common still images, grids,
alpha, metadata, pixel formats, and timed sequences. It does not automatically
commit the encoder to tile-list OBUs, large-scale-tile mode, arbitrary sample
transforms, film-grain estimation, tone-map generation, every inter tool, or
every layered-image combination. Those become separate proposals supported by
a concrete use case, a bounded design, and measured value.

The roadmap should be reordered only when evidence from Goal 1 changes the
cost/benefit picture or a real interoperability need makes a later feature
urgent. The freestanding, dependency-free, caller-owned memory model is never a
tradeable optimization.