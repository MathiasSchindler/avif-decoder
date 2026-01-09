# m3b — Minimal AV1 still-image reconstruction (starting with frame header parsing)

This milestone is where we start bridging from “header parsing” into the AV1 decode pipeline.

The full AV1 decoder is large, so we build it incrementally. The first step is a robust parser
for the uncompressed frame header, because later decode work needs reliable access to:

- coded frame size
- tile layout (later)
- safe boundaries for tile payload data

## Build

- `make build-m3b`

## Run

- `./build/av1_framehdr <in.av1>`

To dump raw tile payload bytes (entropy-coded) for debugging / future reconstruction work:

- `./build/av1_framehdr --dump-tiles <dir> <in.av1>`

To validate tile payload boundaries without decoding tile syntax:

- `./build/av1_framehdr --check-tile-trailingbits-strict <in.av1>`

To start m3b.D work (tile syntax traversal scaffolding):

- `./build/av1_framehdr --decode-tile-syntax <in.av1>`
- `./build/av1_framehdr --decode-tile-syntax --tile-consume-bools 8 <in.av1>`

Note: This probe is expected to report `UNSUPPORTED` on real tiles until we decode real tile syntax up to the correct end-of-tile point.

Current probe behavior (m3b.D):
- Traverses the full partition tree.
- Decodes a growing prefix of `decode_block()` in strict spec order.
- Currently iterates luma transform blocks (first two tx blocks in block0) and exercises residual contexts by decoding two leaf blocks, reaching coeff milestones for block1 through `coeff_base` (c==0 milestone).

It now also reports derived per-tile geometry (tile size in MI units and the superblock grid) to help pick “easy” tiles to start implementing `decode_partition()`.

Input is a size-delimited OBU stream as extracted by m2 (`avif_extract_av1`).

## Current scope

- Single-frame intra subset (KEY_FRAME with `show_frame=1`)
- Accepts both `still_picture=1` and `still_picture=0` bitstreams (many real-world AVIF still images use `still_picture=0`)
- Reduced still-picture sequences and the supported non-reduced keyframe path
- Parses:
  - Sequence Header enough to derive `max_frame_width/height`
  - Minimal prefix of `uncompressed_header()` sufficient to validate the stream and report coded size

This does **not** decode pixels yet.
