# avif-decoder

`avif-decoder` is a dependency-free AVIF codec project written in freestanding
C. It provides a broad still-image and image-sequence decoder plus a narrower
reduced-still encoder. The command-line executables use native system calls
directly and do not link a C library, codec library, or image library. Linux
builds are static PIE executables without an interpreter; macOS builds use the
system dyld as their process launcher but have no dynamic-library load commands.

The decoder and encoder core APIs operate on caller-owned input, workspace, and
output buffers, perform no allocation and no file I/O, and report explicit
limits and structured errors. The decoder accepts immutable encoded input; the
encoder accepts immutable planar source images.

## Features

- AVIF still images and `avis` image sequences.
- AV1 Main, High, and Professional profiles at 8/10/12-bit, monochrome and
  4:2:0/4:2:2/4:4:4, with the full in-loop and post-filter pipeline and
  display-only film grain.
- Native planar YUV, packed RGB/RGBA, and allocation-free streaming PNG output.
- Optional caller-driven parallelism through a structured `parallel_for`
  executor, with built-in native worker pools on Linux/x86-64 and macOS/arm64.
- A deterministic 8-bit YUV420 reduced-still encoder with lossless and lossy
  quantization, bounded rate control, and caller-driven tile parallelism.
- Allocation- and I/O-free reentrant decoder and encoder cores.

See [`docs/support.md`](docs/support.md) for the full feature matrix.

## Build

The supplied executable targets Linux/x86-64 and macOS/arm64. Build for the
current host with:

```sh
make
```

This produces one of:

```text
Linux/x86-64: build/x86_64/avifdec
macOS/arm64:  build/arm64/avifdec
```

The freestanding encoder writes one 8-bit 4:2:0 reduced-still AVIF from planar
YUV, PNG, or JPEG input. Build its public API and CLI with:

```sh
make encoder
make test-encoder
```

Encode a frame by supplying its even luma dimensions. The input contains the Y
plane followed by half-width, half-height U and V planes:

```sh
build/arm64/avifenc --quantizer 128 --speed 0 \
  640 480 frame.yuv frame.avif
```

PNG and JPEG dimensions are read from the input file:

```sh
build/arm64/avifenc --quantizer 128 --speed 0 \
  image.jpg image.avif
```

Image loading remains dependency-free and allocation-bounded: the CLI uses
the same freestanding platform and caller-sized workspace substrate as the
rest of the project. PNG input supports non-interlaced 8-bit grayscale,
grayscale-alpha, indexed, RGB, and RGBA images. JPEG input supports 8-bit
baseline Huffman grayscale and YCbCr/RGB images with common 4:4:4, 4:2:2, and
4:2:0 sampling. Progressive JPEG and interlaced PNG are rejected. RGB is
converted to limited-range BT.709 YUV 4:2:0. Because the encoder contract
requires even dimensions, an odd final row or column is repeated once.
Supported source dimensions are preserved and split into deterministic AV1
tiles when one tile would exceed width or area limits.

Quantizers 0 through 255 and speed levels 0 through 2 are supported. Quantizer
0 uses the exact lossless 4x4 WHT path. Lossy encoding supports separate legal
Y/U/V DC and AC deltas, fixed or activity-selected quantization matrices,
three-segment activity AQ, and finite target-quality or target-size searches.
Speed 0
performs the broadest bounded mode and legal angle-delta search, speed 1 uses
narrower angle and partition budgets, and speed 2 keeps the classification-only
4x4 baseline. Eligible blocks can use all AV1 intra directions, smooth variants,
Paeth, CfL, filter intra, and exact luma or paired chroma palettes. Prediction
and transform rate-distortion costs are selected together, with doubled chroma
distortion weight. The encoder supports bounded square/horizontal/vertical
partition decisions through 32x32, square transforms from 4x4 through 32x32,
and the six corresponding 2:1 rectangular transform shapes. `--workers`
selects bounded tile parallelism independently of speed; both supported native
targets use the platform task pool. It is intended as a bounded interoperable
baseline rather than a compression-efficiency replacement for libaom.

The macOS build requires the Xcode command-line tools and targets macOS 11 or
newer. The release executable is stripped at link time; the unit and trace
binaries keep their symbols for diagnostics.

## Quick start

```sh
build/x86_64/avifdec image.avif                 # inspect and validate
build/x86_64/avifdec --png image.avif image.png # decode to PNG
build/x86_64/avifdec --workers 4 --png grid.avif grid.png
build/x86_64/avifenc --quantizer 96 image.png image.avif
```

See [`docs/usage.md`](docs/usage.md) for the full command line, output formats,
and image-sequence handling.

## Testing

```sh
make test       # self-contained: compiler + coreutils only, no third-party tools
make test-all   # adds reference/differential comparisons against ffmpeg/libavif/aom
```

`make test` runs decoder and encoder unit, fixture, and corpus suites with no
codec, image, or network dependencies. `make test-all` additionally requires
`ffmpeg`, `ffprobe`, `avifenc`/`avifdec`, `aomenc`/`aomdec`, `magick`, and `perl` (and `git` +
`cmake` for the libaom block reference). Details in
[`docs/testing.md`](docs/testing.md).

## Documentation

- [Architecture](docs/architecture.md) — design goals, source tree, memory and
  parallelism model.
- [Command-line usage](docs/usage.md) — CLI options and output formats.
- [Public API](docs/api.md) — the decoder and encoder library interfaces.
- [Format support](docs/support.md) — decoded and encoded AVIF/AV1 feature
  matrices and known limitations.
- [Testing](docs/testing.md) — the `test` / `test-all` split and requirements.
- [Fuzzing](docs/fuzzing.md) — coverage-guided robustness workflow.
- [Profiling](docs/profiling.md) — performance measurement.
- [Encoder scorecard](docs/encoder/benchmark.md) — deterministic quality/work
  baselines and reproducible encoder timing.
- [Browser experiment](docs/wasm.md) — the WebAssembly viewer.

## Public API

The decoder API is declared in [`src/avifdec.h`](src/avifdec.h), and the
encoder API in [`src/encoder/avifenc.h`](src/encoder/avifenc.h). Both are
documented in [`docs/api.md`](docs/api.md). Decoder version 1.3.0 and encoder
version 0.2.0 are reported through their respective version constants and
version-string functions.

Decoder internals and its CLI live under `src/decoder`; encoder core and CLI
code live under `src/encoder`. Shared AV1 primitives are under `src/codec`.

## Makefile interface

`make`, `make encoder`, `make test`, `make test-encoder`, `make test-all`,
`make wasm`, `make fuzz`, `make fuzz-seeds`, `make fuzz-smoke`,
`make fuzz-campaign`, `make fuzz-differential`, `make encoder-fuzz`,
`make encoder-fuzz-seeds`, `make encoder-fuzz-smoke`,
`make encoder-fuzz-campaign`, `make encoder-scorecard`,
`make encoder-benchmark`, `make encoder-benchmark-json`, and `make clean` are
the complete public Makefile interface.

## License and authorship

The project source code is released under the
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
public-domain dedication.

The freestanding platform substrate and the PNG fixed-Huffman/LZ77 design are
adapted from the vendored CC0 newos project.

Most of the source code was written by large language models, predominantly
GPT-5.6 Sol, under human direction and review.
