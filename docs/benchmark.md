# Decoder benchmark

This document records a decoder comparison performed on 2026-07-20. It is a
measurement snapshot, not a claim that the implementations perform identical
internal work.

## Environment

| Component | Version |
| --- | --- |
| Host | Apple M4 Max (`Mac16,6`), 16 logical CPUs |
| Operating system | macOS 26.5.2 |
| Repository revision | `be62596f6118ee7a5bb1539995a7e24fd4905e6b` |
| C compiler | Homebrew Clang 22.1.8 |
| libavif | 1.4.2 |
| dav1d | 1.5.3 |
| libaom | 3.14.1 |
| FFmpeg | 8.1.2 |

The repository decoder was the normal optimized freestanding arm64 build:

```sh
make
```

All command-line comparisons used one decoder worker and wrote raw planar
output to `/dev/null`. Each command was warmed once. Timed commands were then
run in deterministic randomized order using `time.perf_counter_ns()`. The table
reports medians from 9 runs for the 2.5 MP case, 7 runs for both intermediate
cases, and 5 runs for the 30.1 MP case.

## Corpus

| File | Dimensions | Format | Encoded bytes | SHA-256 |
| --- | ---: | --- | ---: | --- |
| `images/image-check/flycatcher-lossy-420-8.avif` | 1920x1280 | 8-bit 4:2:0 lossy | 113,944 | existing fixture |
| `images/benchmarkset/pastell-4096-420-8.avif` | 4096x2731 | 8-bit 4:2:0 lossy | 248,085 | `dbc4bb9e2dca0740ec657ddecbffc87caa7c8057f592a91759ed57ac61138fc2` |
| `images/benchmarkset/pastell-2048-lossless-444.avif` | 2048x2048 | 8-bit 4:4:4 lossless | 5,921,692 | `f613addaf927541f7f4cbdf8edcad51ddc098e22e76214048ba5e6fa00f60963` |
| `images/image-check/pastell-lossy-420-10.avif` | 6720x4480 | 10-bit 4:2:0 lossy | 1,401,885 | existing fixture |

The two files under `images/benchmarkset/` were generated from the local
`images/pastell.jpg` source. The intermediate PNGs are not required for decode
benchmarking and are not retained. They can be regenerated with:

```sh
mkdir -p build/benchmarkset
magick images/pastell.jpg -resize '4096x4096>' -strip \
    build/benchmarkset/pastell-4096.png
avifenc --codec aom --jobs 1 --speed 6 --qcolor 45 --yuv 420 \
    build/benchmarkset/pastell-4096.png \
    images/benchmarkset/pastell-4096-420-8.avif

magick build/benchmarkset/pastell-4096.png -gravity center \
    -crop 2048x2048+0+0 +repage \
    build/benchmarkset/pastell-2048.png
avifenc --codec aom --jobs 1 --speed 6 --lossless --yuv 444 \
    build/benchmarkset/pastell-2048.png \
    images/benchmarkset/pastell-2048-lossless-444.avif
```

The generating `avifenc` was libavif 1.4.2 with libaom 3.14.1. Exact bytes can
change with encoder versions even when the command line remains unchanged.

## Command-line comparison

The measured decode commands were equivalent to:

```sh
build/arm64/avifdec --raw INPUT.avif /dev/null

# OUTPUT.y4m was a symbolic link to /dev/null.
avifdec --jobs 1 --codec dav1d INPUT.avif OUTPUT.y4m
avifdec --jobs 1 --codec aom INPUT.avif OUTPUT.y4m

ffmpeg -v error -threads 1 -c:v libdav1d -i INPUT.avif \
    -frames:v 1 -f rawvideo -y /dev/null
```

Full output sizes were checked before timing. For the 30.1 MP 10-bit image,
the repository decoder and FFmpeg each emitted 90,316,800 raw bytes;
libavif emitted the same samples plus an 81-byte Y4M header.

### Median elapsed time

| Image | Repository CLI | libavif + dav1d | libavif + libaom | FFmpeg + dav1d | CLI / libavif+dav1d |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2.5 MP, 8-bit 4:2:0 lossy | 193.890 ms | 15.631 ms | 24.659 ms | 40.939 ms | 12.40x |
| 11.2 MP, 8-bit 4:2:0 lossy | 862.060 ms | 47.303 ms | 72.594 ms | 106.733 ms | 18.22x |
| 4.2 MP, 8-bit 4:4:4 lossless | 825.153 ms | 299.051 ms | 426.723 ms | 599.247 ms | 2.76x |
| 30.1 MP, 10-bit 4:2:0 lossy | 2,662.876 ms | 202.545 ms | 324.918 ms | 410.153 ms | 13.15x |

The geometric-mean CLI slowdown relative to libavif+dav1d is 14.38x across the
three lossy cases and 9.52x across all four cases.

FFmpeg's native `av1` decoder was excluded. On this host it selected the
VideoToolbox path, reported that hardware AV1 decoding was unavailable, exited
with status 69, and emitted zero bytes.

## Optimization rerun

The comparison was repeated later on 2026-07-20 using the optimized working
tree based on repository revision
`b25f2a12a2a35197dacd0750a5df28dba00f9929`. The measured
`build/arm64/avifdec` executable had SHA-256
`cfe1a68e06f4de4a2efe9503026e01107ee7a7ee9a36a219168b8424d7c25de7`.
The host, operating system, compiler, third-party decoder versions, fixture
bytes, warm-up policy, worker count, randomized ordering, and run counts were
unchanged. Before timing, the raw planar payload from every implementation was
compared byte-for-byte for all four files; sizes and SHA-256 hashes matched
exactly.

### Current median elapsed time

| Image | Repository CLI | libavif + dav1d | libavif + libaom | FFmpeg + dav1d | CLI / libavif+dav1d |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2.5 MP, 8-bit 4:2:0 lossy | 123.414 ms | 15.833 ms | 25.462 ms | 41.884 ms | 7.79x |
| 11.2 MP, 8-bit 4:2:0 lossy | 540.202 ms | 48.245 ms | 74.007 ms | 110.126 ms | 11.20x |
| 4.2 MP, 8-bit 4:4:4 lossless | 692.272 ms | 304.148 ms | 432.521 ms | 611.774 ms | 2.28x |
| 30.1 MP, 10-bit 4:2:0 lossy | 1,899.416 ms | 204.364 ms | 327.984 ms | 416.143 ms | 9.29x |

Third-party medians moved by approximately 0.9% to 2.7%, providing a useful
measure of run-to-run and system variation. The repository decoder moved much
further:

| Image | Original repository CLI | Current repository CLI | Time reduction | Original gap | Current gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2.5 MP, 8-bit 4:2:0 lossy | 193.890 ms | 123.414 ms | 36.3% | 12.40x | 7.79x |
| 11.2 MP, 8-bit 4:2:0 lossy | 862.060 ms | 540.202 ms | 37.3% | 18.22x | 11.20x |
| 4.2 MP, 8-bit 4:4:4 lossless | 825.153 ms | 692.272 ms | 16.1% | 2.76x | 2.28x |
| 30.1 MP, 10-bit 4:2:0 lossy | 2,662.876 ms | 1,899.416 ms | 28.7% | 13.15x | 9.29x |

The lossy geometric-mean gap to libavif+dav1d fell from 14.38x to 9.33x. The
all-case geometric-mean gap fell from 9.52x to 6.56x. The largest contributors
are normal output decoding no longer performing diagnostic trace hashing and
the new ARM64 residual-add and common inverse-DCT kernels. The original CLI
numbers include diagnostics, while the current default does not; this is an
intentional production-path change rather than an equal-work microbenchmark.
Both snapshots use the same documented command line.

## Null-trace core API

At the revision measured above, the command-line decoder passed a non-null
`AvifdecEntropyTrace` to `avifdec_decode_ex()` and reported detailed checksums.
The competing decoders did not perform this diagnostic hashing. A temporary
hosted C harness therefore also measured the public API with:

```c
avifdec_decode(data, size, 0, workspace, workspace_size,
               &image, 0, &error);
```

The harness loaded and queried each file once, allocated caller-owned buffers
once, performed one warm-up decode, then timed repeated in-process decodes. It
was compiled with `-O2`, strict warnings, and the same decoder core sources.
Output samples were hashed after the timed section to ensure that decoding was
observable.

| Image | Null-trace core | Throughput | CLI / core | Core / libavif+dav1d CLI |
| --- | ---: | ---: | ---: | ---: |
| 2.5 MP, 8-bit 4:2:0 lossy | 125.7 ms | 19.5 MP/s | 1.54x | 8.04x |
| 11.2 MP, 8-bit 4:2:0 lossy | 568.1 ms | 19.7 MP/s | 1.52x | 12.01x |
| 4.2 MP, 8-bit 4:4:4 lossless | 665.8 ms | 6.3 MP/s | 1.24x | 2.23x |
| 30.1 MP, 10-bit 4:2:0 lossy | 1,857.4 ms | 16.2 MP/s | 1.43x | 9.17x |

This API comparison favors the repository decoder: its number excludes process
startup, file loading, AVIF query, allocation, and output writing, while the
libavif+dav1d number includes those operations. It is useful for separating
CLI diagnostics from core work, not as a strictly symmetric race.

The CLI now uses the null-trace path for output modes by default. Pass
`--diagnostics` to restore the checksum work and reporting. The tables above
remain a historical snapshot of the revision and commands originally measured.

On the same host, five interleaved warm runs measured the new default against
`--diagnostics` on two large cases:

| Image | Default output | With diagnostics | Speedup | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| 11.2 MP, 8-bit 4:2:0 lossy | 589.108 ms | 898.393 ms | 1.53x | 34.4% |
| 30.1 MP, 10-bit 4:2:0 lossy | 1,890.896 ms | 2,697.202 ms | 1.43x | 29.9% |

## Portable CDEF interior-kernel A/B

A later same-machine, same-toolchain A/B retained the existing
full-neighborhood proof and added a dedicated interior CDEF kernel. It
precomputes signed tap offsets, leaves the boundary slow path unchanged, and
hoists the primary and secondary damping adjustments once per block.

Both executables used one worker and raw output to `/dev/null`. Each was warmed
before seeded, interleaved timing: 9 rounds per corpus image and 7 rounds for
the CDEF-active fixture. The saved baseline executable SHA-256 was
`ce066f8ffeb0f3bb14fa714817cdfd8924e3efe7696ff152cae88aa9cafa8443`;
the optimized executable SHA-256 was
`266f3c31a6d9319113d591c340d9cf28e1add0a2767f0b8b8c80cb1f2cfad0a8`.

| Workload | Saved-baseline median | Interior-kernel median | Delta |
| --- | ---: | ---: | ---: |
| 2.5 MP, 8-bit 4:2:0 lossy | 120.604 ms | 107.705 ms | -10.70% |
| 11.2 MP, 8-bit 4:2:0 lossy | 540.200 ms | 482.485 ms | -10.68% |
| 4.2 MP, 8-bit 4:4:4 lossless | 670.057 ms | 663.904 ms | -0.92% (near noise) |
| 30.1 MP, 10-bit 4:2:0 lossy | 1,887.100 ms | 1,587.646 ms | -15.87% |
| 4096x4096 CDEF-active | 1,587.873 ms | 1,360.671 ms | -14.31% |

Raw output was byte-identical between executables for every input. Because the
original active-fixture source was absent, that fixture was regenerated from
the local `images/pastell.jpg` with the settings recorded in
[`profiling.md`](profiling.md). These figures are only a same-build A/B; do not
compare them across machines, compiler/decoder versions, or earlier tables.

## Historical peak resident memory (pre-plane-alias)

These measurements belong to the original and optimization-rerun snapshots
above. They predate the reconstruction/deblocking plane alias in the later
same-build full-decoder pass and do not describe that integrated executable.
Peak RSS was measured with `/usr/bin/time -l` after one warm run.

| Image | Repository CLI | libavif + dav1d | libavif + libaom | FFmpeg + dav1d |
| --- | ---: | ---: | ---: | ---: |
| 11.2 MP, 8-bit 4:2:0 lossy | 151.7 MiB | 24.3 MiB | 152.9 MiB | 53.8 MiB |
| 30.1 MP, 10-bit 4:2:0 lossy | 403.5 MiB | 105.7 MiB | 442.0 MiB | 204.9 MiB |

In the original snapshot, the repository decoder was close to libaom in peak
memory on these workloads, but used substantially more memory than dav1d.

The pre-plane-alias optimization rerun produced effectively unchanged
peak-RSS results:

| Image | Repository CLI | libavif + dav1d | libavif + libaom | FFmpeg + dav1d |
| --- | ---: | ---: | ---: | ---: |
| 11.2 MP, 8-bit 4:2:0 lossy | 151.6 MiB | 24.3 MiB | 152.9 MiB | 53.7 MiB |
| 30.1 MP, 10-bit 4:2:0 lossy | 403.5 MiB | 105.8 MiB | 442.0 MiB | 204.9 MiB |

Within those two historical snapshots, the measured speed gains came without a
material still-image RSS change. For current integrated memory results, see the
same-build full-decoder pass below, which records the workspace and RSS savings
from plane aliasing. Native worker widths above one are excluded here to
preserve the original one-worker comparison.

## Full-decoder optimization pass

A subsequent same-build A/B used the post-CDEF executable above as its baseline.
The baseline SHA-256 was
`266f3c31a6d9319113d591c340d9cf28e1add0a2767f0b8b8c80cb1f2cfad0a8`;
the integrated executable SHA-256 was
`72b2b3a8f5b58e42fa6aa6fd24c359e532e16cb1e38d69cb75df0fa6bf972168`.
Both used one worker, wrote raw output to `/dev/null`, and received one warm-up.
A seeded schedule interleaved the executables for 9 rounds on every corpus
image.

| Workload | Post-CDEF median | Integrated median | Delta |
| --- | ---: | ---: | ---: |
| 2.5 MP, 8-bit 4:2:0 lossy | 110.283 ms | 109.107 ms | -1.07% |
| 11.2 MP, 8-bit 4:2:0 lossy | 515.016 ms | 500.474 ms | -2.82% |
| 4.2 MP, 8-bit 4:4:4 lossless | 738.249 ms | 720.921 ms | -2.35% |
| 30.1 MP, 10-bit 4:2:0 lossy | 1,739.383 ms | 1,664.922 ms | -4.28% |

The geometric-mean time reduction was 2.64%. Raw output from the two
executables was byte-identical for every image. The retained pass uses direct
loads for interior super-resolution taps, hoists quantizer-matrix dequantization
invariants, extends the ARM64 residual kernel to unflipped 4-wide 8/10/12-bit
blocks, and aliases the reconstruction and deblocking planes after
reconstruction is complete. Implementation-time direct kernel comparisons
qualitatively supported the first three changes, but their temporary focused
harnesses were removed and are not durable benchmark evidence.

Eliminating the separate deblocking plane reduced queried workspace by one
allocated plane set: 7,372,800, 33,816,576, 25,165,824, and 90,316,800 bytes in
corpus order. On the 11.2 MP and 30.1 MP cases, current same-build peak RSS fell
by 32.13 MiB and 86.16 MiB, respectively.

## Interpretation

The original snapshot found ordinary lossy decoding approximately 8x to 12x
slower at the null-trace core API and the lossless case approximately 2.2x
slower. The current production CLI is 7.79x to 11.20x slower than
libavif+dav1d on the three lossy still images and 2.28x slower on the lossless
case. This is a substantial shift, but most pixel and entropy paths remain
scalar.

The decoder now has ARM64 kernels for residual addition and common 8-bit
inverse DCT sizes, plus optional native macOS workers. dav1d and libaom still
have much broader architecture-specific coverage. Extending exact-tested DSP
coverage, profiling larger entropy/coefficient operation boundaries, and
reducing filter and workspace traffic remain the leading serial-performance
opportunities. Measurements should not be compared across machines or codec
versions without recording a new environment section.
