# avif-decoder

`avif-decoder` is a dependency-free AVIF still-image and image-sequence
decoder written in freestanding C. The command-line executable uses native
system calls directly and does not link a C library, codec library, or image
library. Linux builds are static PIE executables without an interpreter. macOS
builds use the system dyld as their process launcher but have no dynamic-library
load commands.

The decoder supports native planar YUV, packed RGB/RGBA, and compressed
streaming PNG output. Its core API operates on immutable input memory and
caller-owned workspace and output buffers, with explicit limits and structured
errors for untrusted input.

## Build

The supplied executable targets Linux/x86-64 and macOS/arm64. Build for the
current host with:

```sh
make
```

The macOS build requires the Xcode command-line tools and targets macOS 11 or
newer.

This produces one of:

```text
Linux/x86-64: build/x86_64/avifdec
macOS/arm64:  build/arm64/avifdec
```

Run the complete test suite with:

```sh
make test
```

The decoder has no runtime dependencies. The integration tests additionally
expect `ffmpeg`, `ffprobe`, `avifenc`, `avifdec`, `aomenc`, `magick`, and
`perl` on `PATH`. On macOS, install the non-system test tools with:

```sh
brew install ffmpeg libavif aom imagemagick
```

The generated-table reproduction checks run when the ignored
`docs/av1.html` specification file is available. Without that maintainer
input, `make test` reports the skipped check and runs the rest of the suite.

`make`, `make test`, `make wasm`, and `make clean` are the complete public
Makefile interface.

The Linux/x86-64 CLI can optionally decode independent top-level AVIF grid
tiles, sample-transform output rows, and AV1 CDEF row units through the
imported newos clone/futex task-pool substrate. AV1 restoration copying and
filtering use the same row-unit executor:

```sh
build/x86_64/avifdec --workers 4 --png grid.avif grid.png
```

The default is one worker. `--workers 0` selects the available CPU count,
capped by the available tile, plane-row, or CDEF-row work and 32 workers.
macOS currently uses the serial task-pool backend.

### Browser experiment

An experimental WebAssembly build exposes the same decoder core through a
drag-and-drop browser viewer. It requires Emscripten:

```sh
brew install emscripten
make wasm
python3 -m http.server 8000 -d build/wasm
```

Open `http://localhost:8000`. Decoding runs locally in a Web Worker; files are
not uploaded. The wrapper limits images to 8192 pixels per dimension and
33,554,432 total pixels, with a 768 MiB decoder-workspace budget. Some large
images can hit the workspace budget below the pixel limit. The Emscripten build
uses an 8 MiB stack because the decoder's parsing state exceeds Emscripten's
small default stack.

The examples below use the Linux output path. On macOS, substitute
`build/arm64/avifdec`.

## Command-line usage

Inspect and validate a still image:

```sh
build/x86_64/avifdec image.avif
```

Inspect only the ISOBMFF box structure:

```sh
build/x86_64/avifdec --boxes image.avif
```

Decode a still image:

```sh
build/x86_64/avifdec --png image.avif image.png
build/x86_64/avifdec --raw image.avif image.yuv
build/x86_64/avifdec --rgb image.avif image.rgb
build/x86_64/avifdec --rgba image.avif image.rgba
build/x86_64/avifdec --rgb16 image.avif image.rgb16
build/x86_64/avifdec --rgba16 image.avif image.rgba16
```

The `--rgba-premul` and `--rgba16-premul` variants request premultiplied
packed output. Normal RGBA and PNG output use straight alpha.

Query an `avis` image sequence:

```sh
build/x86_64/avifdec animation.avif
```

Decode an indexed sequence frame:

```sh
build/x86_64/avifdec --png-frame 7 animation.avif frame-7.png
build/x86_64/avifdec --raw-frame 7 animation.avif frame-7.yuv
```

Sequence query output includes frame count, timescale, duration, alpha mode,
and repetition information. Frame output includes DTS, duration, sync status,
and the sync frame used for random access.

## Output formats

### PNG

PNG output is encoded internally without zlib or another image library.

- 8-bit AVIF input produces PNG8.
- 10- and 12-bit AVIF input produces PNG16.
- Alpha is written as straight RGBA.
- `pasp` is preserved through a PNG `pHYs` chunk.
- NCLX primaries and transfer characteristics are preserved through `cICP`.

The CLI converts one presentation row at a time, selects among the five PNG
row filters, and feeds a fixed-Huffman LZ77 encoder adapted from newos's CC0
compression code. Compressed bytes are emitted as bounded 32 KiB `IDAT`
chunks. PNG output therefore does not allocate a complete packed RGB/RGBA
image.

### Raw planar YUV

`--raw` and `--raw-frame` write the decoded Y plane followed by U and V.
Monochrome images contain only Y. Samples are one byte at 8-bit depth and
little-endian 16-bit values at 10- or 12-bit depth.

Raw output does not include an auxiliary alpha plane; use PNG or the public API
when alpha is required.

### Packed RGB

Packed output is tightly ordered RGB or RGBA. The 16-bit CLI formats use
host-native byte order.

The converter supports:

- identity RGB mapping for 4:4:4 content;
- BT.709;
- BT.601-compatible matrix coefficients;
- BT.2020 non-constant-luminance;
- full and limited ranges;
- monochrome input;
- straight and premultiplied alpha;
- clean aperture, rotation, and mirroring.

Chroma upsampling is deterministic nearest-neighbor sampling. ICC transforms,
transfer-function conversion, HDR display mapping, and tone-map application
are not performed. The original ICC, NCLX, HDR, and tone-map metadata remains
available through the API.

## AVIF container support

Still-image item support includes:

- `pitm`, `iloc` versions 0-2, `iinf`/`infe`, `iprp`/`ipco`/`ipma`;
- `mdat` and `idat` payloads with multiple extents;
- item-wide `iref` relationships and cycle-checked derived graphs;
- `av1C`, `ispe`, `pixi`, ICC and NCLX `colr`;
- `clap`, `irot`, `imir`, and `pasp`;
- auxiliary alpha through `auxC`, `auxl`, and `prem`;
- full and partial-edge image grids;
- `a1op`, `lsel`, and `a1lx` layered images;
- checked sample-transform (`sato`) expressions;
- CLL/MDCV and opaque tone-map/gain-map metadata.

Unknown non-essential item properties are skipped. Unknown essential
properties return `AVIFDEC_UNSUPPORTED`.

Image-sequence track support includes:

- `moov`, `trak`, `mdia`, `minf`, and `stbl`;
- `av01` visual sample entries and `av1C`;
- `stts` and version 0/1 `ctts` timing;
- `stsc`, `stsz`, compact 4/8/16-bit `stz2`, `stco`, and `co64`;
- explicit `stss` sync samples or implicit all-sync tracks;
- finite and infinite repetition reporting;
- synchronized `auxv` alpha tracks;
- `auxl` and `prem` track relationships;
- nearest-sync random access followed by dependent-sample decoding.

One normal edit-list entry with media time zero and playback rate 1.0 is
supported. Multi-entry edits and non-identity track matrices return
`AVIFDEC_UNSUPPORTED`.

## AV1 decoding support

The AV1 decoder supports Main, High, and Professional profiles at 8, 10, and
12 bits, including monochrome, 4:2:0, 4:2:2, and 4:4:4.

Implemented decoding includes:

- low-overhead OBU framing and explicitly selected Annex-B framing;
- operating-point and spatial-layer selection;
- key, intra-only, inter, switch, and show-existing frames;
- retained reference frames and frame-context updates;
- all intra predictors, directional modes, filter intra, CfL, and palettes;
- translational, scaled, compound, inter-intra, warped, OBMC, intrabc, and
  skip-mode prediction;
- sub-8x8 chroma prediction and variable inter transforms;
- coefficient entropy decoding, quantization matrices, dequantization, and
  inverse transforms;
- deblocking, CDEF, super-resolution, Wiener restoration, and self-guided
  restoration;
- display-only film-grain synthesis without contaminating reference frames;
- HDR CLL/MDCV, scalability, ITU-T T.35, and timecode metadata.

Tile-list OBUs and large-scale-tile mode are intentionally not supported and
are reported through capability flags and `AVIFDEC_UNSUPPORTED`.

## Public API

The API is declared in [`src/avifdec.h`](src/avifdec.h). Version 1.2.0 is
reported by:

```c
AVIFDEC_VERSION_MAJOR
AVIFDEC_VERSION_MINOR
AVIFDEC_VERSION_PATCH
avifdec_version_string()
```

### Still images

The usual workflow is:

1. Call `avifdec_query()` to validate the image, inspect its format, and obtain
   `workspace_required`.
2. Allocate workspace and output planes.
3. Call `avifdec_decode()`.
4. Optionally call `avifdec_image_to_rgb()` for packed presentation output.

`avifdec_image_to_rgb_row()` converts one presentation row into a caller-owned
packed row buffer. `avifdec_png_workspace_requirement()` and
`avifdec_png_write_rows()` provide adaptive-filtered streaming PNG output from
a row callback. The PNG workspace contains the fixed 32 KiB LZ77 window,
393,216-byte hash chains, a bounded `IDAT` buffer, and three image rows.
`avifdec_png_write()` remains an allocation-free convenience API and emits a
fixed-Huffman filter-0 stream from an existing packed image.

`avifdec_trace()` performs full decoding without caller output planes and
returns deterministic syntax and reconstruction checksums.

`avifdec_query_ex()` and `avifdec_decode_ex()` accept an optional
`AvifdecExecutor`. The executor is a structured `parallel_for` callback owned
by the caller; the decoder still performs no allocation and retains no thread
state. The current parallel regions cover independent tiles of a top-level
primary grid, independent output rows of a primary sample transform, and
two-mi-row CDEF units plus four-luma-row restoration units in a direct primary
AV1 item. Sample-transform input images remain serial. Nested derived images,
auxiliary alpha, sequences, RGB conversion, loop filtering, super-resolution,
and AV1 bitstream tiles remain serial.

Parallel grid query results include one copied parser context, tile buffer,
and child decoder workspace per executor worker. Sample-transform row
parallelism, CDEF row parallelism, and restoration row parallelism add no
decoder workspace. Callers must use the workspace requirement returned for the
same executor width used during decoding.

### Image sequences

Use:

- `avifdec_sequence_query()` for track-level metadata;
- `avifdec_sequence_frame_query()` for one frame's format, timing, sync point,
  and workspace requirement;
- `avifdec_sequence_decode_frame()` for indexed decoding.

Decoding frame *N* starts from its nearest preceding sync sample and processes
all required samples through *N*. This reconstructs reference-dependent frames
without keeping decoder state between API calls.

### Ownership and memory

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

Reduced-still AV1 streams do not allocate inter-frame pixel references or
motion fields. When CDEF, super-resolution, or restoration is an exact
pass-through, their plane views alias the preceding stage. The queried
`workspace_plane_buffer_count` records the conservative plane-buffer plan used
for the returned workspace requirement.

### Limits and errors

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

## Validation

`make test` includes:

- strict freestanding and hosted ASan/UBSan unit binaries;
- checked arithmetic, readers, arena alignment, PNG, transforms, prediction,
  filters, film grain, and malformed-input vectors;
- recursive BMFF and 34-file AVIF corpus tests;
- generated-table reproduction checks when `docs/av1.html` is available;
- byte-exact native YUV comparisons against libaom/libavif;
- block-level syntax, predictor, coefficient, motion-vector, reference-state,
  and filter-stage comparisons against instrumented libaom;
- RGB(A) presentation checks for transforms, alpha, grids, and layers;
- PNG8 and PNG16 round-trip comparisons;
- timed image-sequence tests covering dependent-frame seeking, varied
  durations, finite/infinite repetition, compact sample tables, straight and
  premultiplied alpha, repeated decoding, and malformed tables.
- native clone/futex task-pool tests and byte-exact width-1/width-4 grid
  comparisons on Linux/x86-64.

The trusted test programs are development-time or test-time tools only. They
are never linked into `avifdec`.

A hosted coverage-guided harness is available at
[`tests/fuzz.c`](tests/fuzz.c) for Clang
`-fsanitize=fuzzer,address,undefined` campaigns.

## Known limitations

- The supplied freestanding executable is wired only for Linux/x86-64.
- Tile-list OBUs and large-scale-tile mode are unsupported.
- Tone-map/gain-map metadata is retained but not applied.
- ICC color transforms and transfer-function conversion are not applied.
- Sequence edit lists are restricted to the normal single-entry form.
- Sequence track matrices must be identity.
- Internal parallel decoding is currently limited to top-level AVIF grids,
  primary sample-transform output rows, and direct-primary AV1 CDEF and
  restoration rows.

## License and authorship

The project source code is released under the
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
public-domain dedication.

The freestanding platform substrate and the PNG fixed-Huffman/LZ77 design are
adapted from the vendored CC0 newos project.

Most of the source code was written by large language models, predominantly
GPT-5.6 Sol, under human direction and review.