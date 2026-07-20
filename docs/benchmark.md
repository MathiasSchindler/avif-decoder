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

## Peak resident memory

Peak RSS was measured with `/usr/bin/time -l` after one warm run.

| Image | Repository CLI | libavif + dav1d | libavif + libaom | FFmpeg + dav1d |
| --- | ---: | ---: | ---: | ---: |
| 11.2 MP, 8-bit 4:2:0 lossy | 151.7 MiB | 24.3 MiB | 152.9 MiB | 53.8 MiB |
| 30.1 MP, 10-bit 4:2:0 lossy | 403.5 MiB | 105.7 MiB | 442.0 MiB | 204.9 MiB |

The repository decoder is close to libaom in peak memory on these workloads,
but uses substantially more memory than dav1d.

## Interpretation

For ordinary lossy 4:2:0 still images, the current scalar decoder core is
approximately 8x to 12x slower than the measured libavif+dav1d command. The
lossless 4:4:4 case narrows to about 2.2x. CLI tracing and startup account for
approximately 19% to 35% of repository CLI elapsed time, but do not explain the
main difference.

No NEON, SIMD, or architecture-specific decode routines were found in `src/`.
dav1d and libaom both contain extensively optimized architecture-specific
kernels. SIMD coverage, hot-loop specialization, and workspace traffic are
therefore the leading areas for further profiling. These measurements should
be rerun after optimization work and should not be compared across machines or
codec versions without recording a new environment section.
