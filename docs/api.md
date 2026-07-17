# Public API

The API is declared in [`src/avifdec.h`](../src/avifdec.h). Version 1.3.0 is
reported by:

```c
AVIFDEC_VERSION_MAJOR
AVIFDEC_VERSION_MINOR
AVIFDEC_VERSION_PATCH
avifdec_version_string()
```

The decoder core performs no allocation and no file I/O; see
[`architecture.md`](architecture.md) for the memory model.

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
- OBUs and frames;
- operating point and selected spatial layer;
- low-overhead versus Annex-B framing.

Default sequence and AV1 frame capacity is 256 frames. The CLI additionally
rejects input files larger than 1 GiB.

All APIs return `AvifdecStatus`. `AvifdecError` reports the first failure's
absolute byte offset and containing box or OBU type when available. Invalid
syntax, truncation, arithmetic overflow, configured limits, insufficient
workspace, and valid-but-unsupported features are distinct outcomes.

`avifdec_capabilities()` returns feature bits for the supported AV1, AVIF,
presentation, RGB, film-grain, and sequence surfaces.
