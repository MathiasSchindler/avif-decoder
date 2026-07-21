# Format support

This document lists the AVIF container, AV1 bitstream, and image-sequence
features the decoder handles, the reduced-still surface the encoder emits, and
known limitations of both codec directions.

## AVIF container support

Still-image item support includes:

- `pitm`, `iloc` versions 0-2, `iinf`/`infe`, `iprp`/`ipco`/`ipma`;
- `mdat` and `idat` payloads with multiple extents;
- item-wide `iref` relationships and cycle-checked derived graphs;
- `grpl`/`altr` entity groups;
- `av1C`, `ispe`, `pixi`, ICC and NCLX `colr`;
- `clap`, `irot`, `imir`, and `pasp`;
- auxiliary alpha through `auxC`, `auxl`, and `prem`;
- full and partial-edge image grids;
- `a1op`, `lsel`, and `a1lx` layered images;
- checked sample-transform (`sato`) expressions;
- CLL/MDCV metadata;
- allocation-free Exif, XMP, arbitrary MIME, and thumbnail enumeration;
- ISO 21496-1 version-0 gain-map discovery, metadata, child decoding, and
  opt-in application.

Unknown non-essential item properties are skipped. Unknown essential properties
return `AVIFDEC_UNSUPPORTED`.

Image-sequence track support includes:

- `moov`, `trak`, `mdia`, `minf`, and `stbl`;
- `av01` visual sample entries and `av1C`;
- `stts` decode timing; AV1 `ctts` is rejected as invalid;
- `stsc`, `stsz`, compact 4/8/16-bit `stz2`, `stco`, and `co64`;
- explicit `stss` sync samples or implicit all-sync tracks;
- finite and infinite repetition reporting;
- independent `auxv` alpha sample and sync cadence;
- `auxl` and `prem` track relationships;
- nearest-sync random access followed by dependent-sample decoding;
- multiple visual tracks with explicit selection and `altr` reporting;
- zero, one, or multiple version-0/1 rate-1 edit-list entries, including empty
  edits and nonzero media times;
- identity and exact orthogonal track matrices with integer translation;
- `mvex`/`trex` and `moof`/`traf`/`tfhd`/`tfdt`/`trun` fragmented samples;
- checked 32-bit, extended-size, and size-to-parent-end child boxes.

The normalized sequence-index API exposes tracks, references, edits, fragments,
presentations, explicit track selection, and sequence-wide or track-scoped
metadata. The legacy frame-index API remains available for one unambiguous
classic visual track and its unique alpha track.

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

AV1-in-AVIF forbids tile-list OBUs and large-scale-tile mode. Both are rejected
as `AVIFDEC_INVALID_DATA`, including tile-list OBUs in unselected layers, and
their generic AV1 capability bits remain clear.

## Color and HDR output

The legacy packed-RGB functions preserve their source-encoded, nearest-chroma
behavior. The opt-in color-transform API additionally supports:

- every defined H.273 primary, transfer, and matrix identifier, including
  constant-luminance, YCgCo variants, ICtCp, and chromaticity-derived matrices;
- NCLX/AV1 consistency validation;
- deterministic bilinear chroma reconstruction with signaled siting;
- linear-light primary conversion, PQ and HLG with explicit display policy,
  and RGB/RGBA 8-bit, 16-bit, or binary32 output;
- bounded ICC v2/v4 RGB and gray matrix/TRC profiles with XYZ PCS and relative
  or absolute colorimetric intent;
- straight or final-domain premultiplied alpha.

ISO 21496-1 gain maps are opt-in. Query validates the ordered derived images and
metadata, decode reuses `max(base, gain-map)` child workspace, and application
performs pixel-center bilinear gain sampling in straight linear light. Float
output is extended linear SDR with `1.0 == 203 nits`; 16-bit output uses the
explicit destination transfer.

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
- Generic AV1 tile-list anchors and large-scale-tile decoding are not exposed;
  those modes are invalid in AVIF.
- Gain-map application supports ISO 21496-1 metadata version 0 with one or three
  channels. Existing decode/RGB APIs and the CLI remain base-image-only.
- ICC support is limited to bounded v2/v4 RGB/gray matrix plus curve or
  parametric-TRC profiles in XYZ PCS. CLUT, CMYK, multichannel, Lab PCS,
  `mAB`/`mBA`, `mft1`/`mft2`, device-link, perceptual, and saturation LUT
  processing are unsupported.
- Metadata APIs expose validated payload views but do not parse TIFF IFDs or
  XML. Protected items, external data references, and encoded MIME payloads are
  unsupported.
- Sequence composition offsets, multiple `stsd` entries on an AVIF track,
  non-1x/reverse/dwell edits, non-unit scaling, shear, perspective, incremental
  streaming, and generic non-AVIF tracks are unsupported.
- Sample-transform input images, nested derived images, and auxiliary alpha
  decoding remain serial. Sequence frames are replayed in decode order even
  though each frame's independent AV1 regions can run in parallel.
