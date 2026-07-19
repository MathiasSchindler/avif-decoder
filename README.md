# avif-decoder

`avif-decoder` is a dependency-free AVIF still-image and image-sequence decoder
written in freestanding C. The command-line executable uses native system calls
directly and does not link a C library, codec library, or image library. Linux
builds are static PIE executables without an interpreter; macOS builds use the
system dyld as their process launcher but have no dynamic-library load commands.

Its core API operates on immutable input memory and caller-owned workspace and
output buffers, performs no allocation and no file I/O, and reports explicit
limits and structured errors for untrusted input.

## Features

- AVIF still images and `avis` image sequences.
- AV1 Main, High, and Professional profiles at 8/10/12-bit, monochrome and
  4:2:0/4:2:2/4:4:4, with the full in-loop and post-filter pipeline and
  display-only film grain.
- Native planar YUV, packed RGB/RGBA, and allocation-free streaming PNG output.
- Optional caller-driven parallelism through a structured `parallel_for`
  executor, with a built-in clone/futex worker pool on Linux/x86-64.
- Allocation- and I/O-free reentrant core suitable for untrusted input.

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
YUV input. Build its public API and CLI with:

```sh
make encoder
make test-encoder
```

Encode a frame by supplying its even luma dimensions. The input contains the Y
plane followed by half-width, half-height U and V planes:

```sh
build/arm64/avifenc --quantizer 128 640 480 frame.yuv frame.avif
```

Quantizers 1 through 255 are supported. The current encoder uses one tile, DC
intra prediction, and 4x4 transforms; it is intended as a bounded interoperable
baseline rather than a compression-efficiency replacement for libaom.

The macOS build requires the Xcode command-line tools and targets macOS 11 or
newer. The release executable is stripped at link time; the unit and trace
binaries keep their symbols for diagnostics.

## Quick start

```sh
build/x86_64/avifdec image.avif                 # inspect and validate
build/x86_64/avifdec --png image.avif image.png # decode to PNG
build/x86_64/avifdec --workers 4 --png grid.avif grid.png
```

See [`docs/usage.md`](docs/usage.md) for the full command line, output formats,
and image-sequence handling.

## Testing

```sh
make test       # self-contained: compiler + coreutils only, no third-party tools
make test-all   # adds reference/differential comparisons against ffmpeg/libavif/aom
```

`make test` runs the unit, fixture, and corpus suites with no codec, image, or
network dependencies. `make test-all` additionally requires `ffmpeg`,
`ffprobe`, `avifenc`/`avifdec`, `aomenc`, `magick`, and `perl` (and `git` +
`cmake` for the libaom block reference). Details in
[`docs/testing.md`](docs/testing.md).

## Documentation

- [Architecture](docs/architecture.md) — design goals, source tree, memory and
  parallelism model.
- [Command-line usage](docs/usage.md) — CLI options and output formats.
- [Public API](docs/api.md) — the `src/avifdec.h` library interface.
- [Format support](docs/support.md) — AVIF, AV1, and sequence feature matrix and
  known limitations.
- [Testing](docs/testing.md) — the `test` / `test-all` split and requirements.
- [Fuzzing](docs/fuzzing.md) — coverage-guided robustness workflow.
- [Profiling](docs/profiling.md) — performance measurement.
- [Browser experiment](docs/wasm.md) — the WebAssembly viewer.

## Public API

The API is declared in [`src/avifdec.h`](src/avifdec.h) and documented in
[`docs/api.md`](docs/api.md). Version 1.3.0 is reported through
`AVIFDEC_VERSION_MAJOR`/`MINOR`/`PATCH` and `avifdec_version_string()`.

## Makefile interface

`make`, `make encoder`, `make test`, `make test-encoder`, `make test-all`, `make wasm`, `make fuzz`,
`make fuzz-seeds`, `make fuzz-smoke`, `make fuzz-campaign`,
`make fuzz-differential`, and `make clean` are the complete public Makefile
interface.

## License and authorship

The project source code is released under the
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
public-domain dedication.

The freestanding platform substrate and the PNG fixed-Huffman/LZ77 design are
adapted from the vendored CC0 newos project.

Most of the source code was written by large language models, predominantly
GPT-5.6 Sol, under human direction and review.
