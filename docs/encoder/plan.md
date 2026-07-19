# First AVIF encoder plan

This plan describes a freestanding, dependency-free sister encoder built in
`src/encoder/`. Its first release targets one 8-bit 4:2:0 still image using a
reduced-still-picture AV1 sequence, one key frame, one tile, a fixed quantizer,
and a deliberately small set of intra modes and transform sizes. Caller-owned
input, workspace, and output buffers remain the governing memory model.

The first release does not include RGB conversion, alpha, grids, image
sequences, inter prediction, rate control, target-size encoding, film grain,
super-resolution, or advanced metadata. It should produce interoperable AVIF
files, not compete with mature AV1 encoders on compression efficiency.

## Reuse policy

Reuse existing code directly when the encoder needs exactly the same operation
and invariants. Checked arithmetic, memory helpers, arena allocation, intra
prediction, inverse reconstruction, selected AV1 tables, platform I/O, and the
task executor are likely candidates. Move a concrete primitive into
`src/shared/` only when both callers can use the same implementation without
mode flags, callbacks, or semantic compromises.

Keep naturally different operations separate. Byte readers and byte writers,
range decoders and range encoders, inverse and forward transforms, and AVIF
parsers and serializers should have focused implementations on their owning
side. Do not introduce a generalized codec layer merely to route two functions
through one interface. Every reuse refactor must leave decoder tests and traces
unchanged before encoder work proceeds.

## Work package 1: Contract, build target, and test skeleton

Define the first public contract in `src/encoder/avifenc.h`: planar Y, U, and V
input, even dimensions, 8-bit samples, 4:2:0 subsampling, color properties,
fixed quantizer options, caller-owned workspace, caller-owned output, status,
and error reporting. Specify output sizing behavior explicitly; the initial API
may require a conservative output capacity while reporting exact bytes written.

Add an encoder library target, a small `avifenc` CLI target, and a hosted
sanitizer unit target without changing the decoder's public targets. Establish
`tests/encoder_unit.c` and an encoder shell suite with a tiny deterministic YUV
fixture. Reuse the existing freestanding headers and build flags directly, but
keep encoder public names independent from `Avifdec*` names.

The first tests should exercise invalid dimensions, null planes, short strides,
invalid quantizers, insufficient workspace, insufficient output, and arithmetic
overflow before any valid bitstream exists. The package is complete when the
empty encoder path builds under strict freestanding and hosted sanitizer modes,
returns deterministic structured errors, and cannot write outside any
caller-provided region.

## Work package 2: Bounded byte and bit output primitives

Implement encoder-local bounded byte and bit writers with sizing-only and
materializing modes. Required operations include big-endian integers, raw byte
spans, MSB-first bit fields, byte alignment, unsigned LEB128, AV1 UVLC and NS
values, patching previously reserved fixed-width fields, and exact overflow
propagation. Writers must never partially claim success after capacity is
exhausted.

Reuse checked size arithmetic and memory helpers from `src/base.*`. Follow the
simple status propagation and caller-owned-buffer style already used by the
arena and PNG writer, but do not combine readers and writers behind a common
interface. A byte writer and a bit writer may remain separate concrete types if
that keeps their invariants obvious.

Add table-driven unit vectors for every primitive, including boundary values,
unaligned writes, zero-bit writes, maximum legal widths, output one byte too
short, and a sizing pass matching a materializing pass. Where practical, read
the produced fields with the existing byte and AV1 bit readers. The package is
complete when all later serializers can calculate and emit bounded output
without direct pointer arithmetic of their own.

## Work package 3: Minimal AVIF container serializer

Implement a focused ISOBMFF writer for one primary `av01` item stored in one
contiguous `mdat` extent. Emit `ftyp`, `meta`, `hdlr`, `pitm`, `iloc`, `iinf`,
`infe`, `iprp`, `ipco`, `ipma`, `ispe`, `pixi`, `av1C`, `colr`, and `mdat` with
the smallest versions and field widths that satisfy the supported dimensions
and offsets. Reject values that do not fit those selected forms.

Use the package 2 writers and existing FOURCC constants where they express the
same values. Keep serialization in `src/encoder/avif_write.*`; do not generalize
`src/bmff.c`, whose visitor, nesting, and error-offset behavior are specific to
untrusted input parsing. Reserve and patch box sizes locally rather than adding
writer behavior to the parser.

Unit tests should verify box boundaries, sizes, associations, item extent
offsets, brands, and color properties. Feed containers carrying a placeholder
AV1 payload through `avifdec_bmff_inspect()` to validate their structure while
expecting full image query to reject the payload. The package is complete when
container bytes are deterministic and the existing BMFF inspector reports the
intended box tree with no out-of-bounds extent.

## Work package 4: Reduced-still AV1 headers and OBU framing

Serialize the low-overhead AV1 sequence header, metadata-free frame header, and
OBU framing required for one reduced-still-picture key frame. Fix the supported
profile to Main, bit depth to 8, subsampling to 4:2:0, frame size to the source
size, operating point to zero, one tile, and all excluded coding tools to
consistent disabled values. Document every non-obvious fixed syntax choice next
to its specification name.

Build these serializers in encoder-owned files using the package 2 bit writer.
Reuse profile and level validation or constants only where their semantics are
already independent of parsing. Do not make the existing AV1 bit reader
bidirectional. Level selection can begin as a conservative checked table for
the supported dimensions rather than a general sequence-level abstraction.

Create golden vectors for several small even dimensions and inspect every
field with existing AV1 parsing code. At this stage the tile payload may be a
known test stub, so validation should distinguish correct headers from the
expected entropy failure. The package is complete when the decoder accepts the
sequence and frame configuration through the point where tile symbols are
needed and reports the encoded dimensions, profile, bit depth, and subsampling.

## Work package 5: AV1 symbol writer and CDF evolution

Implement the inverse of `Av1SymbolDecoder`: a bounded AV1 range encoder that
writes symbols against AV1 CDFs, writes literal bits where the tile syntax
requires them, normalizes its range, handles carry propagation, and finalizes a
byte-aligned tile payload. Its state and failure behavior should remain local to
the encoder even though its probability evolution must match the decoder.

Extract the normative CDF update operation only if encoder and decoder can call
one concrete function with identical inputs and results. Default CDF tables may
be reused directly. Do not create a pluggable entropy-coder interface or add an
encode/decode mode to `Av1SymbolDecoder`; the arithmetic state machines are
different enough to remain separate.

Build symbol round-trip tests that encode deterministic symbol sequences and
decode them with `av1_symbol_read()`, checking both symbols and final CDF state.
Cover binary and multi-symbol alphabets, disabled updates, long renormalization
runs, carry boundaries, a one-byte-short output, and final padding. Compare
selected vectors with a trusted AV1 implementation during `test-all`. The
package is complete when entropy output round-trips exactly under sanitizers
and strict freestanding compilation.

## Work package 6: Input validation, block layout, and intra prediction

Create the encoder frame state for source planes, reconstructed planes,
neighbor availability, block metadata, quantizer state, and tile-local CDFs.
Start with a deterministic superblock and block layout chosen from a small
supported set, with edge handling for dimensions that are even but not block
multiples. Emit the corresponding partition and intra-mode syntax through the
symbol writer.

Call existing standalone intra predictor functions where their inputs already
match encoder reconstruction state. Keep source sampling, block traversal,
mode candidates, and syntax emission encoder-local. If adapting a predictor
would require decoder-state flags or callback wrappers, write the small
encoder-specific preparation code instead of broadening the predictor API.

Begin with DC prediction as the mandatory path, then add vertical, horizontal,
Paeth, and selected smooth modes behind deterministic options. Tests should
compare predicted blocks with decoder unit vectors and verify legal traversal
at frame edges, unavailable neighbors, and chroma subsampling boundaries. The
package is complete when a zero-residual synthetic frame can be emitted as a
complete tile and decoded into the encoder's predicted reconstruction without
syntax or bounds errors.

## Work package 7: Forward transform, quantization, and reconstruction loop

Compute residuals from source and predicted samples, implement the forward
`DCT_DCT` transforms for the selected square sizes, quantize coefficients using
the fixed frame quantizer, scan them in AV1 order, and emit coefficient syntax.
Use explicit bounded intermediate widths and deterministic rounding for every
supported transform size. Unsupported transform or coefficient configurations
must fail internally rather than silently changing syntax.

Forward transforms and quantization belong in encoder files. Reuse checked
quantization tables where applicable, then call existing dequantization and
inverse-transform reconstruction code to produce the exact local reconstructed
samples used by subsequent blocks. Share a transform constant only when its
numeric representation is truly common; avoid wrapping forward and inverse
transforms in a generic direction-selected API.

Add impulse, constant, maximum-amplitude, and randomized block tests. Verify
forward/quantized/inverse behavior against libaom vectors in `test-all`, and
verify that encoder reconstruction is byte-exact with this decoder's output for
every self-encoded frame. The package is complete when nonzero residual images
decode successfully and reconstruction remains identical under strict,
sanitized, and differently aligned workspace runs.

## Work package 8: Complete tile, frame, and AVIF assembly

Join block traversal, entropy finalization, frame headers, OBU sizing, and AVIF
container serialization into one deterministic encode operation. Establish a
clear workspace layout for reconstruction planes, block state, CDFs, transform
scratch, and temporary AV1 payload. Resolve final payload and `mdat` offsets
without allocation, unbounded stack objects, or seeking through platform I/O.

Use the existing arena sizing pattern directly for workspace planning. The
encoder may perform a sizing pass where deterministic and inexpensive, or hold
the AV1 payload in caller-owned workspace before writing the final container.
Do not introduce a generic output graph to unify those choices. Keep one
straight-line assembly path for the single-item format.

Add end-to-end fixtures covering flat fields, ramps, sharp edges, chroma
variation, minimum supported dimensions, odd chroma-plane dimensions implied
by even luma sizes, and non-block-multiple edges. Encode, query, and decode each
file with the in-tree decoder, then compare decoded planes with the encoder's
reconstruction. The package is complete when the CLI produces AVIF files that
the in-tree decoder and at least one external decoder accept.

## Work package 9: Bounded mode selection and quality controls

Replace fixed choices with a small deterministic search across the supported
block sizes, intra predictors, and transform sizes. Score candidates using
distortion plus a bit-cost estimate derived from the current CDF state, while
keeping the public control to one fixed quantizer and an optional speed level.
Do not add target bitrate, multipass analysis, psychovisual tuning, or inter
prediction in this release.

Reuse predictor and reconstruction primitives directly, but keep candidate
generation, trial coefficient storage, cost calculation, and winner selection
inside the encoder. Prefer a few explicit search paths over a generalized mode
framework. Candidate trials must not accidentally mutate the committed CDF or
neighbor state; snapshot only the concrete state that actually changes.

Create deterministic quality tests over a small image set. Assert stable output
checksums, exact encoder/decoder reconstruction agreement, monotonic broad
quality behavior across selected quantizers, and bounded workspace independent
of image content. Record encoded size and error metrics as non-flaky regression
data with justified tolerances. The package is complete when search improves
representative output over the fixed baseline without changing the supported
format surface or compromising deterministic output.

## Work package 10: Hardening, interoperability, and release documentation

Integrate encoder coverage into the self-contained test suite: strict
freestanding units, hosted ASan/UBSan units, deterministic end-to-end fixtures,
short-buffer checks at varied alignments, and encode-then-decode comparisons.
Add a fuzz harness over dimensions, options, bounded source planes, workspace
sizes, and output capacities while keeping generated allocations within test
limits. Retain minimized regressions beside decoder regressions.

Extend `test-all` with decoding through libavif, libaom, and ffmpeg where
available. Validate container metadata with `ffprobe` and compare decoded YUV
against the in-tree reconstruction, allowing no difference for the encoded
bitstream. Test Linux/x86-64 and macOS/arm64 binaries for nolibc assumptions,
determinism, and identical output. External tools remain optional and never
become build dependencies.

Document the encoder API, CLI, exact feature limits, workspace/output sizing,
color assumptions, and expected quality tradeoffs. Update architecture,
testing, and top-level build documentation only after behavior is stable. The
package is complete when all self-contained tests pass, external decoders accept
the corpus, fuzz smoke tests are clean, and unsupported requests fail with
documented structured errors.

## Definition of the first release

The first encoder release is reached only when all ten work packages are
complete. Its files must decode successfully with this project and external
implementations, its internal reconstruction must match decoded YUV exactly,
and all memory requirements must be explicit and caller-owned. Compression
efficiency may be modest; correctness, bounded operation, deterministic output,
and a narrow maintainable implementation are the release criteria.