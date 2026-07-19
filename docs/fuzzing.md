# Defensive decoder robustness testing

This project uses coverage-guided randomized input testing solely to strengthen its own AVIF, ISOBMFF, and AV1 decoder before release. The campaign targets memory safety, arithmetic correctness, parser-state consistency, resource limits, workspace sizing, and deterministic serial/parallel behavior. It is not intended for testing third-party systems or developing offensive capability.

## Local campaign

The local harness requires Clang with libFuzzer support. Build it with:

```sh
make fuzz
```

Prepare a corpus with checked-in fixtures, existing generated test outputs, newly encoded still/grid/sequence images when `ffmpeg` and `avifenc` are installed, retained regression inputs, and deterministic malformed container/table variants:

```sh
make fuzz-seeds
```

Run a short validation campaign:

```sh
make fuzz-smoke
```

Run a sustained campaign for one hour, or override the duration in seconds:

```sh
make fuzz-campaign
FUZZ_SECONDS=21600 make fuzz-campaign
```

The harness performs BMFF inspection, still-image query/decode, and sequence query/decode. For accepted inputs it verifies:

- exact queried workspace never fails with `AVIFDEC_OUT_OF_MEMORY` at a varied base alignment; full decode may still reject entropy data that query does not parse;
- a one-byte-short workspace either succeeds equivalently or returns `AVIFDEC_OUT_OF_MEMORY`;
- serial, two-worker, and four-worker still decoding produce identical pixels and traces;
- serial and four-worker sequence replay produce identical pixels and traces;
- output planes are cleared between runs so stale data cannot hide an incomplete write.

Local builds use ASan, UBSan, and Clang's integer sanitizer. Diagnostics for unsigned wraparound and representation-changing integer conversions are disabled because AV1 arithmetic and FNV-style checksums intentionally use those operations. Signed overflow, invalid shifts, division errors, bounds violations, and the other undefined-behavior checks remain enabled. `UBSAN_OPTIONS=halt_on_error=1` makes actionable findings terminate the run.

Artifacts are written under `build/fuzz`. Minimize a reproducer with libFuzzer and place the resulting `.avif` file in `tests/fuzz-regressions`; `make fuzz-seeds` includes it in every later campaign.

## Reference comparison

After preparing the corpus, compare every still image accepted by this decoder with each usable libavif backend:

```sh
make fuzz-differential
```

The comparison is byte-exact over native planar color output. Alpha is removed from the reference format because the CLI's `--raw` contract intentionally emits only Y, U, and V. Intentionally malformed inputs rejected by this decoder are not sent through the pixel comparison.

The workflow intentionally stays local to this codec repository: it does not require containers, a service account, or an external fuzzing platform. The harness, seed preparation, dictionary, reference comparison, artifacts, and regression corpus all remain versioned beside the code they test.

## Encoder campaign

The encoder has a separate bounded harness and corpus:

```sh
make encoder-fuzz
make encoder-fuzz-seeds
make encoder-fuzz-smoke
FUZZ_SECONDS=21600 make encoder-fuzz-campaign
```

Inputs select even dimensions up to 32x32, quantizer, speed, source planes,
and independent workspace/output/decode alignments. Each run repeats query to
check stable capacities, verifies one-byte-short workspace and output failures,
encodes twice at different alignments, checks output guards and byte-identical
determinism, and decodes the result through the in-tree decoder. Static bounded
buffers cap workspace, output, and decoded images regardless of fuzzer input.

Minimized encoder regressions use the `.seed` suffix under
`tests/encoder-fuzz-regressions`. The seed target copies those files and the
checked-in starter vectors into `build/fuzz/encoder-corpus`. Encoder campaigns
use the same ASan, UBSan, integer-sanitizer, and halt-on-error policy as the
decoder campaign.
