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

Experimental end-of-tile check:
- `./build/av1_framehdr --decode-tile-syntax-try-eot <in.av1>`

This opt-in mode disables the probe's default early-stop (after 1-2 blocks), traverses the full partition tree (including non-SPLIT partition types), and attempts to run `exit_symbol()` after the traversal. It is expected to fail on most real tiles until full tile syntax decode exists.

When `exit_symbol()` fails, the probe annotates the error with the symbol-decoder bit position and `SymbolMaxBits` (e.g. `bitpos=... smb=...`) to help track how close we are to the true end-of-tile.

Current probe behavior (m3b.D):
- Traverses the full partition tree.
- Decodes a growing prefix of `decode_block()` in strict spec order.
- Iterates luma transform blocks (first two tx blocks in block0) and exercises residual contexts by decoding two leaf blocks, reaching coeff milestones for block1 through `coeff_base` (c==0 milestone).
- Also enters chroma residual decoding (U/V): computes plane transform size via spec `get_tx_size(plane, txSz)` and derives the chroma transform type via `Mode_To_Txfm[UVMode]` (with transform-set fallback to `DCT_DCT`), then decodes a stable coeff prefix up to `coeff_base_eob` for the first chroma tx block in each plane.

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
