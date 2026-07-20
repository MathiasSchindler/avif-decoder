# Encoder scorecard and benchmark

The encoder scorecard makes speed, memory, output size, deterministic work, and reconstruction quality visible before later roadmap goals change coding decisions. It is self-contained: the runner links only project code and the host C runtime used by other development tools. Production encoder binaries remain freestanding and `nolibc`.

## Corpus

The runner uses eight deterministic generated sources and one checked-in CC0 photographic fixture:

| Case | Dimensions | Quantizer | Speed | Coverage |
| --- | ---: | ---: | ---: | --- |
| `minimum` | 2x2 | 96 | 0 | Minimum dimensions and padded block edges |
| `gradient` | 64x48 | 96 | 0 | Smooth ramps and low-frequency detail |
| `text-edge` | 66x50 | 96 | 1 | Text-like strokes, sharp edges, non-block-multiple dimensions |
| `animation` | 64x48 | 96 | 1 | Flat regions, simple shapes, and discrete chroma |
| `noise` | 64x48 | 128 | 2 | Deterministic high-frequency luma/chroma noise |
| `chroma-detail` | 64x48 | 96 | 0 | Constant luma with alternating chroma detail |
| `large-practical` | 1024x768 | 128 | 2 | Practical 0.1 upper routine size and sustained traversal |
| `multi-tile-wide` | 8192x64 | 128 | 2 | Two balanced tile columns and parallel scaling |
| `photograph` | 330x220 | 96 | 1 | `images/image-check/tribu.png` through the project PNG decoder |

The corpus deliberately exercises all three speed levels. The one-megapixel
`large-practical` case is the largest routine regression input considered
practical for the 0.1 encoder's bounded partition traversal, not the syntax maximum;
larger timing belongs in an explicit profiling campaign rather than every
`make test` run.

## Commands

Run the exact cross-platform regression gate:

```sh
make encoder-scorecard
```

Run the human timing table or JSON Lines output:

```sh
make encoder-benchmark
make encoder-benchmark-json
```

Increase repetitions when measuring time:

```sh
BENCHMARK_ITERATIONS=20 make encoder-benchmark
BENCHMARK_ITERATIONS=20 make encoder-benchmark-json
build/host/encoder-benchmark --human --iterations 20 --workers 2
```

The human table reports aggregate monotonic wall-clock milliseconds for the selected iteration count, throughput in megapixels per second, output bytes, luma PSNR, and prediction trials. The timed loop uses `avifenc_encode_with_executor()` with null statistics, so statistics collection and reconstruction hashing do not distort the encoder baseline. JSON includes those timing fields plus the complete stable scorecard.

`--workers N` uses a hosted pthread executor for reproducible core scaling
measurements without adding a production dependency. Stable JSON requires one
worker so its workspace values remain cross-platform. On the named M4 Max host,
the Goal 4 whole-corpus median of five 20-iteration runs is 8801.412 ms with
one worker and 8542.427 ms with two, a 1.03x speedup. The other eight cases are
legally single-tile, so whole-corpus scaling understates the benefit available
to the `multi-tile-wide` case.

## Stable metrics

`tests/encoder-scorecard-baseline.jsonl` records only deterministic integer data:

- workspace and conservative output capacity;
- exact output bytes, bits per pixel, and deterministic 64-bit output checksum;
- Y, U, and V sum of squared errors;
- Y, U, and V structural edge error;
- tile, partition-node, block, prediction-trial, transform-trial, committed-transform, entropy-symbol, literal-bit, and filter-unit counts.

Each case also compares per-plane encoder reconstruction checksums from
`AvifencStatistics` with checksums over the in-tree decoder output. Any mismatch
fails before metrics are emitted.

Structural edge error is the sum of absolute differences between source and reconstructed horizontal/vertical sample gradients. It is intentionally simple, bounded, dependency-free, and sensitive to blurred or distorted edges. Normal JSON and human output additionally report PSNR, but `libm` rounding and wall-clock values are excluded from the byte-exact baseline.

`avifenc_encode_ex()` supplies operation counts through `AvifencStatistics`. `avifenc_encode()` remains the source-compatible wrapper and produces the same bytes. Statistics are cleared on entry, valid after success, and optional.

The scorecard script regenerates stable JSON and byte-compares it with the checked-in baseline. A difference can represent a bug, an intentional coding change, or a real quality improvement; it always requires review. Update the baseline only after inspecting every changed size, checksum, quality metric, capacity, and work count, then run `make test` and the applicable external interoperability suite.

The Goal 4 baseline records the expanded bounded intra and chroma search.
Relative to Goal 3, `gradient` is 6.8% smaller with 1.4% lower luma SSE;
`text-edge` is 53.7% smaller with 91.4% lower luma SSE; and `animation` is
20.8% smaller with 68.2%, 79.4%, and 73.2% lower Y, U, and V SSE.
`chroma-detail` is 19.1% smaller with 57.8% lower U SSE and 28.7% lower V SSE.
`photograph` trades 1.5% more bytes for 2.6%, 4.2%, and 3.2% lower Y, U, and V
SSE. The speed-2 cases retain identical reconstruction quality while using
2.2% more bytes for `noise` and about 7.0% more for the two large cases.
Workspace remains explicitly bounded and grows by less than 2 KiB for the
small single-tile scorecard cases.

## Named timing reference

Wall-clock timing is informative and never gates `make test`. The first reference calibration is:

| Host | Toolchain | Iterations | Time per corpus | Throughput | Informational budget |
| --- | --- | ---: | ---: | ---: | --- |
| `Mac16,6`, Apple M4 Max, macOS 26.5.2 | Homebrew Clang 22.1.8, `-O2` | 20 | 440.071 ms | 3.179 MP/s | at most 550 ms and at least 2.5 MP/s |

Run timing on an otherwise idle named machine, use at least 20 iterations, and compare the median of five process runs. Add Linux/x86-64 reference rows when a stable named host is available. A budget miss prompts profiling; it does not justify weakening deterministic or quality gates.
