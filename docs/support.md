# Format support

This document lists the AVIF container, AV1 bitstream, and image-sequence
features the decoder handles, plus known limitations.

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

## Known limitations

- The supplied freestanding executable is wired only for Linux/x86-64.
- Tile-list OBUs and large-scale-tile mode are unsupported.
- Tone-map/gain-map metadata is retained but not applied.
- ICC color transforms and transfer-function conversion are not applied.
- Sequence edit lists are restricted to the normal single-entry form.
- Sequence track matrices must be identity.
- Sample-transform input images, nested derived images, and auxiliary alpha
  decoding remain serial. Sequence frames are replayed in decode order even
  though each frame's independent AV1 regions can run in parallel.
