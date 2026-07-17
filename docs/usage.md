# Command-line usage

The examples below use the Linux output path (`build/x86_64/avifdec`). On
macOS, substitute `build/arm64/avifdec`. See the [README](../README.md) for how
to build the executable.

## Inspecting

Inspect and validate a still image:

```sh
build/x86_64/avifdec image.avif
```

Inspect only the ISOBMFF box structure:

```sh
build/x86_64/avifdec --boxes image.avif
```

## Decoding still images

```sh
build/x86_64/avifdec --png image.avif image.png
build/x86_64/avifdec --raw image.avif image.yuv
build/x86_64/avifdec --rgb image.avif image.rgb
build/x86_64/avifdec --rgba image.avif image.rgba
build/x86_64/avifdec --rgb16 image.avif image.rgb16
build/x86_64/avifdec --rgba16 image.avif image.rgba16
```

The `--rgba-premul` and `--rgba16-premul` variants request premultiplied packed
output. Normal RGBA and PNG output use straight alpha.

## Image sequences

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

## Parallel workers

The Linux/x86-64 CLI can optionally decode independent top-level AVIF grid
tiles, AV1 bitstream tiles, sample-transform output rows, loop-filter
row/column units, CDEF/restoration row units, super-resolution rows,
film-grain stripes, frame copies, and diagnostic plane checksums through the
imported newos clone/futex task-pool substrate. Indexed sequence frames use
the same executor while retaining ordered inter-frame reconstruction:

```sh
build/x86_64/avifdec --workers 4 --png grid.avif grid.png
build/x86_64/avifdec --png grid.avif grid.png --workers 4
```

The default is one worker. `--workers 0` selects the available CPU count,
capped by the available work and 32 workers. One `--workers N` pair may appear
anywhere in the command line. For packed RGB/RGBA output, the CLI also reuses
the pool to convert presentation rows. Streaming PNG converts bounded batches
of up to 64 rows in parallel before feeding them to the ordered compressor.
macOS currently uses the serial task-pool backend.

The CLI rejects input files larger than 1 GiB.

## Output formats

### PNG

PNG output is encoded internally without zlib or another image library.

- 8-bit AVIF input produces PNG8.
- 10- and 12-bit AVIF input produces PNG16.
- Alpha is written as straight RGBA.
- `pasp` is preserved through a PNG `pHYs` chunk.
- NCLX primaries and transfer characteristics are preserved through `cICP`.

The CLI supplies presentation rows to the encoder in order, selects among the
five PNG row filters, and feeds a fixed-Huffman LZ77 encoder adapted from
newos's CC0 compression code. With multiple workers it converts up to 64 RGB
rows at a time into a bounded temporary cache; filtering and the DEFLATE stream
remain ordered. Compressed bytes are emitted as bounded 32 KiB `IDAT` chunks.
PNG output therefore does not allocate a complete packed RGB/RGBA image.

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
transfer-function conversion, HDR display mapping, and tone-map application are
not performed. The original ICC, NCLX, HDR, and tone-map metadata remains
available through the API.
