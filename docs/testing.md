# Testing

The test suite is split so that everyday validation needs nothing beyond the
compiler and coreutils, while the full reference and differential comparisons
against third-party codecs are opt-in.

## `make test` — self-contained

`make test` builds the decoder plus the strict freestanding, hosted ASan/UBSan,
and threading unit binaries and runs the checked-in fixture and corpus tests. It
uses **only the compiler, coreutils, and repository fixtures** — no ffmpeg,
libavif, aom, ImageMagick, perl, or network access — so it runs anywhere the
decoder itself builds. It includes:

- strict freestanding and hosted ASan/UBSan unit binaries;
- checked arithmetic, readers, arena alignment, PNG, transforms, prediction,
  filters, film grain, and malformed-input vectors;
- native clone/futex task-pool tests on Linux/x86-64;
- a smoke test that confirms the freestanding binary decodes;
- AV1 feature-fixture checks (including a corrupt-input rejection built with
  `od`/`dd`);
- recursive BMFF and 34-file AVIF corpus traces.

## `make test-all` — full reference suite

`make test-all` runs everything in `make test` and then the reference and
differential comparisons. These additionally require the following on `PATH`:
`ffmpeg`, `ffprobe`, `avifenc`/`avifdec` (libavif), `aomenc` (aom), `magick`
(ImageMagick), and `perl`; `tests/reference-block.sh` also builds libaom through
`git` and `cmake`. On macOS, install the non-system tools with:

```sh
brew install ffmpeg libavif aom imagemagick
```

The additional coverage includes:

- byte-exact native YUV comparisons against libaom/libavif;
- block-level syntax, predictor, coefficient, motion-vector, reference-state,
  and filter-stage comparisons against instrumented libaom;
- RGB(A) presentation checks for transforms, alpha, grids, and layers;
- PNG8 and PNG16 round-trip comparisons;
- timed image-sequence tests covering dependent-frame seeking, varied
  durations, finite/infinite repetition, compact sample tables, straight and
  premultiplied alpha, repeated decoding, and malformed tables;
- byte-exact width-1/width-4 grid comparisons on Linux/x86-64.

### Generated-table reproduction

When the ignored `docs/av1.html` specification file is present, `make test-all`
regenerates the AV1 constant tables (see [`architecture.md`](architecture.md))
with the `tools/` Perl scripts and byte-compares them against the checked-in
copies. Without that maintainer input the check reports as skipped and the rest
of the suite runs.

## Notes

The trusted test programs are development-time or test-time tools only. They are
never linked into `avifdec`.

For coverage-guided robustness testing, see [`fuzzing.md`](fuzzing.md). For
performance work, see [`profiling.md`](profiling.md).
