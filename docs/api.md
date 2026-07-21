# Public API

The API is declared in [`src/avifdec.h`](../src/avifdec.h). Version 1.4.0 is
reported by:

```c
AVIFDEC_VERSION_MAJOR
AVIFDEC_VERSION_MINOR
AVIFDEC_VERSION_PATCH
avifdec_version_string()
```

The decoder core performs no allocation and no file I/O; see
[`architecture.md`](architecture.md) for the memory model.

## Encoder

The sister encoder API is declared in
[`src/encoder/avifenc.h`](../src/encoder/avifenc.h). Populate an
`AvifencImage` with 8-bit 4:2:0 Y, U, and V planes, then:

1. Initialize `AvifencOptions` with `avifenc_options_default()`.
2. Call `avifenc_query()` to validate the image and obtain caller-owned
  workspace and output capacities.
3. Call `avifenc_encode()` with buffers meeting those capacities.

The encoder produces one reduced-still AV1 key frame in a single-item AVIF.
Input dimensions must be nonzero and even, each no greater than
`AVIFENC_MAX_DIMENSION` (65,536). A deterministic uniform AV1 tile layout is
selected from the dimensions. One tile is retained whenever legal; larger
images use bounded tile columns and rows without resizing.

Plane strides are measured in bytes. Y is full resolution; U and V are each
half width and half height. `AvifencColor` is written as NCLX/AV1 color
configuration: each color index must fit in 8 bits, `full_range` is 0 or 1,
and `chroma_sample_position` is 0 through 3.

Quantizers 0 through 255 and speed levels 0 through `AVIFENC_MAX_SPEED` (2)
are supported. Quantizer 0 uses 4x4 WHT transforms and reconstructs every YUV
sample exactly. Lossless mode rejects nonzero quantizer deltas, matrices, and
adaptive quantization. Eligible blocks search all luma and chroma intra modes,
including legal directional deltas, smooth variants, Paeth, CfL, filter intra,
and exact palettes. Speed 0 uses the broadest bounded mode, delta, and partition
budgets; speed 1 narrows delta and partition trials; speed 2 follows a
classification-only 4x4 path. Speeds 0 and 1 may emit square blocks and
transforms through 32x32 plus the six 2:1 shapes from 4x8 through 32x16.
Prediction and transform rate costs share one decision, with U and V distortion
weighted twice. Lower quantizers generally preserve more detail and increase
output size. Speed changes bounded search work, not the format surface or
memory requirement.

`AvifencQuantization` exposes signed Y DC, U DC/AC, and V DC/AC deltas in the
legal -64 through 63 range. Matrix mode 0 disables qmatrices, mode 1 uses the
three explicit levels 0 through 14, and mode 2 resolves per-plane levels from
source activity. Adaptive-quantization mode 1 classifies luma and chroma
gradients into three ALT_Q segments and clips effective lossy qindices to
1 through 255. AQ strength is 0 through 63.

`AvifencRateControl` mode 0 uses the fixed quantizer, mode 1 targets the
0 through `AVIFENC_TARGET_QUALITY_MAX` quality score, and mode 2 targets a
nonzero maximum encoded byte count. The search is deterministic and capped at
9, 7, and 5 total passes for speeds 0, 1, and 2. The selected qindex is always
encoded again for final output. Target size never exceeds its limit and returns
`AVIFENC_LIMIT_EXCEEDED` when qindex 255 cannot fit; discrete small streams may
undershoot the target.

`avifenc_query()` returns conservative capacities that are independent of
source pixel values and speed. The workspace includes fixed qmatrix tables, an
AQ segment map, and a 2 KiB partition-trial reconstruction checkpoint in
addition to coding and transform contexts. Rate-control queries also include a
private trial-output span; no pass allocates or retains hidden state.
`avifenc_encode()` accepts an unaligned
workspace, requires capacities at least as large as the query result, sets
`output_written` to the exact encoded length on success, and sets it to zero on
failure. The core performs no allocation or file I/O. Repeated calls with the
same image bytes, metadata, and options are byte-identical.

`avifenc_encode_ex()` has the same encoding contract and optionally fills
`AvifencStatistics` with deterministic counts for tiles, partition nodes,
blocks, prediction and transform trials, committed transforms, entropy
symbols, literal bits, filter units, and per-plane reconstruction checksums.
Goal 5 additionally reports per-plane reconstruction SSE, selected qindex,
achieved quality, and total encode pass count.
The checksums cover visible sample values and are independent of host byte
order. Statistics are cleared on entry and are valid after success. They
contain no timers or platform state, so callers can compare coding work across
runs and architectures. Passing null disables reporting;
`avifenc_encode()` is the source-compatible wrapper.

`avifenc_query_with_executor()` and `avifenc_encode_with_executor()` accept an
optional caller-owned `AvifencExecutor`. Its synchronous `parallel_for` follows
the decoder executor contract: every index is invoked exactly once, calls
sharing a worker index do not overlap, and the callback returns only after all
work completes. Worker counts are 1 through
`AVIFENC_EXECUTOR_MAX_WORKERS` (32). Query includes one private tile scratch
span per advertised worker. Tile layout and output bytes are independent of
worker count; serial and parallel calls produce identical files and statistics.

Encoder failures distinguish invalid arguments, checked arithmetic overflow,
implementation limits, insufficient workspace, insufficient output,
and valid but unsupported requests. `AvifencError` identifies the failing
context and, for capacity errors, records the required and provided sizes.

The CLI in `src/encoder/main.c` is an adapter around this planar API. Raw input
is tightly packed Y, then U, then V. For image input,
`src/encoder/cli/image_input.c` dispatches to the PNG and baseline-JPEG adapters
in `image_input_png.c` and `image_input_jpeg.c`. Those project-owned
freestanding modules decode and convert input to limited-range BT.709 YUV420
with integer arithmetic. Odd image dimensions repeat the last row or column.
Supported image dimensions are preserved. Progressive JPEG and interlaced PNG
are unsupported.

## Still images

The usual workflow is:

1. Call `avifdec_query()` to validate the image, inspect its format, and obtain
   `workspace_required`.
2. Allocate workspace and output planes.
3. Call `avifdec_decode()`.
4. Optionally call `avifdec_image_to_rgb()` for packed presentation output.

`avifdec_image_to_rgb_row()` converts one presentation row into a caller-owned
packed row buffer. `avifdec_png_workspace_requirement()` and
`avifdec_png_write_rows()` provide adaptive-filtered streaming PNG output from a
row callback. The PNG workspace contains the fixed 32 KiB LZ77 window,
393,216-byte hash chains, a bounded `IDAT` buffer, and three image rows.
`avifdec_png_write()` remains an allocation-free convenience API and emits a
fixed-Huffman filter-0 stream from an existing packed image.

`avifdec_trace()` performs full decoding without caller output planes and
returns deterministic syntax and reconstruction checksums.

### Metadata views

`avifdec_metadata_query()` enumerates Exif, canonical XMP, arbitrary MIME, and
thumbnail items without allocation. A null/zero-capacity call returns required
metadata, thumbnail, and span counts after validating the complete structure.
A fill call is all-or-nothing; insufficient capacity returns
`AVIFDEC_OUT_OF_MEMORY` with the required counts and leaves arrays untouched.
Payload spans and length-delimited names/content types are immutable views into
the caller's input. Exif validates its displacement and TIFF header but does not
parse IFDs.

### Explicit color transforms

Legacy `avifdec_image_to_rgb*()` behavior is unchanged. For color-managed
output:

1. Obtain `AvifdecColorDescription` with
   `avifdec_image_color_description()`.
2. Initialize `AvifdecColorOptions` and query/init an immutable
   `AvifdecColorTransform`.
3. Call `avifdec_image_to_rgb_with_transform()` or its row variant.

AUTO selects a present ICC profile and otherwise CICP; a malformed or
unsupported selected ICC never silently falls back. The bounded ICC surface is
v2/v4 RGB or gray, XYZ PCS, matrix/TRC, and relative/absolute colorimetric
intent. CICP conversion implements every defined H.273 identifier and
deterministic nearest or bilinear chroma sampling. PQ/HLG require an explicit
HDR policy, reference white, and display peak.

`AVIFDEC_RGBF32` and `AVIFDEC_RGBAF32` are linear in destination primaries,
allow extended values, and define `1.0` as 203 nits. Integer formats use the
destination transfer and clip to their representable range.

### ISO 21496-1 gain maps

`avifdec_gain_map_query*()` discovers and validates a version-0 gain map.
Absence is success with `present == 0`. `avifdec_gain_map_decode*()` requires
separate caller-owned base and gain-map planes and decodes them sequentially;
its exact workspace requirement is the maximum child requirement for the same
executor width.

Application is explicitly opt-in through `avifdec_gain_map_apply()` or its row
variant. The output transform must describe the metadata-selected working color
to an explicit destination. `display_headroom` is a positive linear ratio.
Alpha comes only from the base image and is never gain-mapped. Existing
query/decode/RGB functions remain base-only.

## Parallel execution

`avifdec_query_ex()`, `avifdec_trace_ex()`, and `avifdec_decode_ex()` accept an
optional `AvifdecExecutor`. The executor is a structured `parallel_for`
callback owned by the caller; the decoder still performs no allocation and
retains no thread state. The current parallel regions cover independent tiles
of a top-level primary grid, independent AV1 bitstream tiles, independent
output rows of a primary sample transform, vertical loop-filter mi rows,
horizontal loop-filter mi columns, two-mi-row CDEF units, super-resolution
rows, four-luma-row restoration units, frame-plane copies, per-plane trace
checksums, and film-grain stripes. Sequence frames remain ordered, but each
replayed frame can use these AV1 regions through the sequence `_ex` APIs.
Sample-transform input images, nested derived images, and auxiliary alpha
remain serial. The core RGB conversion API is row-addressable but does not own
an executor; the CLI parallelizes those rows after decode.

Parallel grid query results include one copied parser context, tile buffer, and
child decoder workspace per executor worker. Sample-transform, loop-filter,
CDEF, super-resolution, restoration, copy, and checksum parallelism add no
decoder workspace. Direct AV1 tile threading adds bounded entropy,
coefficient-context, prediction, and transform scratch per advertised worker,
but shares frame planes and block grids. Film grain adds one private
current/previous stripe pair per additional worker:
`2 * 34 * (width + 64) * sizeof(int16_t)` bytes. Callers must use the workspace
requirement returned for the same executor width used during decoding.

## Image sequences

Use:

- `avifdec_sequence_query()` for track-level metadata;
- `avifdec_sequence_frame_query()` for one frame's format, timing, sync point,
  and workspace requirement;
- `avifdec_sequence_decode_frame()` for indexed decoding.

The corresponding `_ex` functions accept an `AvifdecExecutor`.
`avifdec_sequence_query_ex()` sizes one allocation across every sync group, so
it remains valid for any frame decoded with the same executor width.

Decoding frame *N* starts from its nearest preceding sync sample and processes
all required samples through *N*. This reconstructs reference-dependent frames
without keeping decoder state between API calls.

For edits, fragments, or multiple visual tracks, first query and initialize an
`AvifdecSequenceIndex` in caller-owned persistent workspace. The immutable
index retains input/workspace views and supports:

- track and track-reference enumeration;
- explicit main/alpha selection;
- normalized presentation query/decode with independent main and alpha sync
  replay;
- classic and fragmented samples in one decode-order index;
- sequence-wide and direct track-scoped metadata through
  `avifdec_sequence_metadata_query()`.

Index query reports the exact persistent requirement; one byte less fails with
`AVIFDEC_OUT_OF_MEMORY`. Null selection auto-selects only when one eligible
visual track exists. The caller must name a main track when selection is
ambiguous. Output plane contents may be partially modified on decode failure,
matching the existing decode APIs; result descriptors commit only on success.

## Ownership and memory

- Input bytes remain owned by the caller.
- ICC and other byte views point into the input buffer.
- Workspace and all output planes are caller-owned.
- The core performs no allocation and no file I/O.
- Separate input, workspace, output, trace, and error objects make calls
  reentrant.
- Workspace may be unaligned; internal arena allocations align absolute
  pointers safely.

`AvifdecImage` uses 16-bit sample storage for all source bit depths. Strides are
measured in samples, not bytes.

Reduced-still AV1 streams do not allocate inter-frame pixel references or motion
fields. When CDEF, super-resolution, or restoration is an exact pass-through,
their plane views alias the preceding stage. The queried
`workspace_plane_buffer_count` records the conservative plane-buffer plan used
for the returned workspace requirement. Reduced-still frames without intra block
copy use a 24-byte intra/base cell stride; inter-capable cells are 124 bytes.
Palette colors are kept in a fixed tile-local above/left context and in the
current block trace rather than repeated in every 4x4 cell. Reduced-still frames
without intra block copy also omit unused inter-prediction scratch.

## Limits and errors

`AvifdecLimits` bounds:

- width, height, and total pixels;
- items, extents, and properties;
- metadata items/spans, tracks, edits, and fragments;
- ICC bytes and sampled-curve entries;
- OBUs and frames;
- operating point and selected spatial layer;
- low-overhead versus Annex-B framing.

Default sequence and AV1 frame capacity is 256 frames. The CLI additionally
rejects input files larger than 1 GiB.

All APIs return `AvifdecStatus`. `AvifdecError` reports the first failure's
absolute byte offset and containing box or OBU type when available. Invalid
syntax, truncation, arithmetic overflow, configured limits, insufficient
workspace, and valid-but-unsupported features are distinct outcomes.

`avifdec_capabilities()` additionally reports metadata, CICP and ICC
matrix/TRC color, ISO gain-map metadata/application, and normalized sequence
index/edit/fragment/track-selection support. Tile-list and large-scale-tile
bits remain clear because AVIF forbids those modes.
