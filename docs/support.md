# Format support

This document lists the AVIF container, AV1 bitstream, and image-sequence
features the decoder handles, the reduced-still surface the encoder emits, and
known limitations of both codec directions.

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

Unknown non-essential item properties are skipped. Unknown essential properties
return `AVIFDEC_UNSUPPORTED`.

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

The AV1 decoder supports Main, High, and Professional profiles at 8, 10, and 12
bits, including monochrome, 4:2:0, 4:2:2, and 4:4:4.

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

Tile-list OBUs and large-scale-tile mode are intentionally not supported and are
reported through capability flags and `AVIFDEC_UNSUPPORTED`.

## Encoder support

The encoder core accepts one nonzero, even-sized 8-bit planar YUV420 frame and
writes one Main-profile reduced-still key frame in a single-item AVIF. It uses
deterministic uniform tiles and bounded recursive square, horizontal, and
vertical partition decisions through 32x32 coding blocks. DCT transforms cover
4x4, 8x8, 16x16, 32x32, and the 4x8, 8x4, 8x16, 16x8, 16x32, and 32x16
shapes needed by YUV420 rectangular blocks. Its bounded rate-distortion search
covers all luma and chroma intra directions, legal angle deltas, smooth modes,
Paeth, CfL, filter intra, and exact luma and paired chroma palettes. Intrabc is
not emitted. Quantizers 0 through 255 and speeds 0 through 2 are supported.
Quantizer 0 uses the exact lossless 4x4 WHT path. Lossy coding supports
separate Y/U/V DC and AC deltas, fixed or activity-selected quantization
matrices, three-segment activity AQ, and deterministic finite target-quality
and target-size searches.

Each dimension is at most 65,536. Images exceeding AV1's per-tile width or area
limits are split into bounded tile columns and rows. The encoder API does not
resize inputs. It performs no allocation or I/O and reports conservative
caller-owned workspace and output capacities before encoding.

The `avifenc` CLI additionally accepts non-interlaced 8-bit PNG and baseline
8-bit JPEG. It converts decoded RGB to limited-range BT.709 YUV420, extends odd
edges to even dimensions, and preserves supported source dimensions. The core
API remains planar and preserves the caller-supplied NCLX color fields. The
optional executor and CLI `--workers` setting parallelize independent tiles;
Linux and macOS both use native worker threads.

## Known limitations

- Supplied freestanding executables target Linux/x86-64 and macOS/arm64.
- Encoding is limited to one 8-bit 4:2:0 reduced-still key frame. Alpha,
  sequences, grids, inter prediction, film grain, super-resolution, in-loop
  filtering, restoration, and advanced metadata are not supported.
- Encoder image input rejects progressive JPEG and interlaced PNG.
- Tile-list OBUs and large-scale-tile mode are unsupported.
- Tone-map/gain-map metadata is retained but not applied.
- ICC color transforms and transfer-function conversion are not applied.
- Sequence edit lists are restricted to the normal single-entry form.
- Sequence track matrices must be identity.
- Sample-transform input images, nested derived images, and auxiliary alpha
  decoding remain serial. Sequence frames are replayed in decode order even
  though each frame's independent AV1 regions can run in parallel.
