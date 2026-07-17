# Profiling and optimization

This document records profiling of the x86-64 Linux decoder and the changes made from those results. Measurements were taken on the normal freestanding static PIE build with GCC 15.2.0. The primary workload was `images/tribu-large.avif`.

## Reproducing the main measurement

Build and inspect the static PIE relocations:

```sh
make clean
make -j"$(nproc)"
readelf -rW build/x86_64/avifdec
```

The startup code applies symbol-free `R_X86_64_RELATIVE` entries before calling
`main`. Any other runtime relocation type is rejected with exit status 127.
GCC 15 emits one relative relocation for the optimized decoder and several for
the optimized strict unit binary.

Measure the diagnostic decode path:

```sh
/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss_kb=%M faults=%R' \
    build/x86_64/avifdec images/tribu-large.avif >/dev/null
```

Collect flat hardware samples and counters:

```sh
perf record -e cycles:u -o build/perf.data -- \
    build/x86_64/avifdec images/tribu-large.avif
perf report --stdio -i build/perf.data
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
    build/x86_64/avifdec images/tribu-large.avif >/dev/null
```

Flat symbols are more reliable than call graphs for this binary because the release flags intentionally omit unwind tables and frame pointers.

## Baseline

Before these changes, the default `-Os` binary was 378,864 bytes. Stable runs of the large workload took 7.64 to 7.73 seconds, used approximately 2,064 MB maximum RSS, and incurred about 516,000 minor faults.

The flat CPU profile was:

| Symbol or area | Samples |
| --- | ---: |
| `avifdec_memory_fill` | 22.1% |
| `av1_tile_checkpoint_hash` | 16.3% |
| `av1_symbol_read` | 10.1% |
| `avifdec_memory_copy` | 6.4% |
| coefficient parsing | 6.2% |
| predictor checksum | 6.0% |
| transform helpers | about 17% |

Hardware counters reported approximately 142.2 billion instructions and 41.7 billion cycles, or about 3.4 instructions per cycle. Branch misses were 0.4% and L1 data misses were 0.9%. This is primarily instruction and memory-bandwidth work rather than branch misprediction or cache-miss latency.

The large `avifdec_memory_fill` share came from byte-at-a-time loops under `-Os -fno-builtin` and from initializing the caller-provided workspace. Stronger optimization lets GCC widen these loops without adding a libc dependency.

## Workspace memory

The large file requires 2,386,828,722 bytes of AVIF workspace. This is structural allocation, not a leak. The AV1 workspace includes:

- fourteen padded planar `uint16_t` frame sets for reconstruction, filters, and references;
- reference/current/temporal motion storage for non-reduced streams;
- a per-4x4-cell grid containing `Av1BlockCell`, block metadata, restoration units, CDFs, and contexts.

`Av1BlockCell` is 124 bytes in this build. Reduced-still frames that do not
permit intra block copy use a 24-byte stride containing dimensions,
segmentation, intra modes, transform state, and deltas.
Inter-capable frames and frames permitting intra block copy retain the full
cell because motion vectors and reference state remain observable.
Palette colors use a 33,800-byte tile-local rolling above/left context and the
current block trace rather than 48 bytes in every persistent cell.

### Large-image workspace reductions

The same 7563x5042 4:2:0 file uses the reduced-still AV1 header and selects
identity CDEF, super-resolution, and restoration stages. Three conservative
layout changes reduce its queried workspace:

| Layout | Workspace | Incremental saving |
| --- | ---: | ---: |
| Previous layout | 2,386,828,722 bytes | - |
| Restoration units bounded at the minimum legal 32-pixel geometry | 2,308,649,742 bytes | 78,178,980 bytes |
| Reduced-still reference planes and motion fields elided | 1,364,931,342 bytes | 943,718,400 bytes |
| Identity filter and super-resolution planes aliased | 893,072,142 bytes | 471,859,200 bytes |
| Reduced-still block cells use the 72-byte base stride | 654,301,742 bytes | 238,770,400 bytes |
| Active quantizer matrices expand into caller workspace | 654,311,774 bytes | -10,032 bytes |
| Palette colors move out of every block cell | 539,701,982 bytes | 114,609,792 bytes |
| Obsolete flat pixel margin replaced by exact allocation terms | 310,906,106 bytes | 228,795,876 bytes |
| Unused reduced-still inter-prediction scratch elided | 310,824,185 bytes | 81,921 bytes |

The cumulative reduction is 2,076,004,537 bytes (87.0%, or 1,979.8 MiB).
Restoration capacity still covers every legal unit-size choice. Inter-frame
streams retain all reference and motion allocations, and non-reduced streams
retain the worst-case filter-plane layout because later frames may select
different tools.

The diagnostic command used for the original baseline now takes 4.06 seconds
and 310,272 KiB maximum RSS, compared with 5.30 seconds and 2,063,424 KiB
before these memory changes. A raw-output decode has a 4.13-second median and
about 411.9 MiB maximum RSS; the difference is the caller-owned planar output,
which is not part of decoder workspace.

### Streaming compressed PNG

The previous PNG writer emitted filter-0 scanlines in stored DEFLATE blocks,
and the CLI first allocated a complete packed presentation image. The new row
path converts one RGB/RGBA row at a time, evaluates None/Sub/Up/Average/Paeth,
uses fixed-Huffman LZ77 derived from the CC0 newos compressor, and flushes
bounded 32 KiB `IDAT` chunks. Its caller-owned workspace is 458,752 fixed
bytes plus three scanline buffers and alignment.

For `tribu-large.avif`, a packed RGB image would require 114,397,938 bytes.
The row path avoids that allocation. The filtered stream is 114,402,980 bytes;
the resulting PNG is 60,883,969 bytes, 46.8% smaller than the uncompressed
scanlines. End-to-end PNG decode, conversion, and encoding takes 7.48 seconds
and 768,060 KiB maximum RSS. The corresponding raw decode peaks at 766,272
KiB, so compressed PNG output adds about 1.7 MiB rather than a full packed
frame.

## Implemented changes

### Default `-O2` build

The default optimization level is now `-O2`. Two correctness issues exposed by this change were fixed first:

- inter mode decoding rejects an empty motion-vector stack before clamping an index, avoiding `stack.count - 1` underflow;
- AVIF extent offset and length locals are initialized before fallible sized reads.

At `-O2`, GCC also pooled tile callback addresses into a static aggregate and
emitted an `R_X86_64_RELATIVE` relocation. The previous startup code did not
process relocations, so the resulting indirect call jumped to an unrelocated
link-time address. The x86-64 startup now derives the image base from `AT_PHDR`,
walks the dynamic relocation table, and applies relative relocations before
calling `main`. It fails closed on unsupported relocation types.

The optimized freestanding unit binary also requires compiler-generated
`memcpy` for a large structure copy. The freestanding runtime provides that
standard entry point through the existing byte-copy implementation; hosted
sanitizer builds continue to use the host C library.

The clean default traced workload took 5.30 seconds (4.73 seconds user and
0.57 seconds system), a reduction of about 31% from the 7.64 to 7.73 second
baseline. It used 2,063,424 KiB peak RSS and incurred 516,089 minor faults, both
effectively unchanged. The executable is 450,112 bytes, an increase of 71,248
bytes or 18.8%.

### Optional diagnostics

`avifdec_decode` now preserves a null trace through direct AV1 items, grids, sample transforms, and alpha items. AV1 decoding retains internal scratch trace storage so existing structural code remains simple, but an explicit flag skips reporting-only work when the caller did not request diagnostics.

The disabled work includes block-field hashes, coefficient/dequantized/residual checkpoint hashes, CDF aggregation, reference-state hashes, reconstruction checksums, and post-filter plane checksums. Decode state mutation, entropy parsing, reconstruction, filtering, and output copying remain unconditional.

On an otherwise identical hosted `-O2` harness decoding `tribu-large.avif` and hashing every output sample:

| Mode | Time | Output checksum |
| --- | ---: | --- |
| trace requested | 5.41 s | `2c91e62d878bbb16` |
| null trace | 4.17 s | `2c91e62d878bbb16` |

Not requesting diagnostics reduced this workload by about 23% with identical decoded output.

Predictor checkpoint hashing is intentionally still performed. An experiment that removed it changed pixels on the large inter-frame workload even though the hash should be observational only. This indicates an unresolved aliasing, initialization, or other undefined-behavior dependency in the predictor path. The optimization was not shipped until that dependency can be isolated and reference-tested.

## Tool observations

System `perf` produced reliable flat samples for the normal static PIE. NewOS `perf` sampled it but could not resolve the PIE ASLR slide, so symbol attribution was incorrect. NewOS instrumentation produced useful call counts, but changed a roughly 0.02-second small workload into a 6.58-second run and emitted 452 MB of trace data, so its timing was not representative.

The NewOS profiler runtime in the tested checkout also reused a shared static environment buffer: reading `NEWOS_PROFILE_MAX_EVENTS` overwrote the path returned for `NEWOS_PROFILE`. NewOS host `strace` did not have a functioning Linux ptrace backend in this checkout, so system `strace` was used instead.

## Top-level grid threading

The 1.1 executor API keeps serial decoding as the default and optionally
dispatches independent top-level AVIF grid tiles through the newos task-pool
model. The Linux/x86-64 CLI exposes this with `--workers`; macOS currently uses
the serial backend.

A 2x2 benchmark was generated from four 2048x2048 crops of
`images/tribu-large.jpg`, encoded with:

```sh
avifenc --qcolor 55 --jobs 1 --codec aom --yuv 420 --grid 2x2 \
    t00.png t01.png t10.png t11.png grid.avif
```

Three warm runs per width used:

```sh
/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss_kb=%M' \
    build/x86_64/avifdec --workers WIDTH --raw grid.avif /dev/null
```

| Workers | Median elapsed | User time | Maximum RSS | Decoder workspace |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.81 s | 1.72 s | 287 MiB | 269,790,871 bytes |
| 2 | 1.07 s | 1.74 s | 513 MiB | 539,796,237 bytes |
| 4 | 0.64 s | 1.81 s | 976 MiB | 1,079,591,453 bytes |

Width 4 reduced wall time by about 65% (2.8x speedup) while increasing
workspace almost exactly fourfold. All widths produced identical raw output,
entropy checksums, and restoration checksums. Because the memory tradeoff is
substantial, CLI threading remains explicit rather than automatic.

The resulting static PIE is 459,552 bytes on the measurement build, 9,440
bytes (2.1%) above the previously recorded 450,112-byte optimized binary.

## Sample-transform row threading

Sample-transform input items are still decoded serially. After all input
planes are immutable, output evaluation is dispatched as one flattened set of
plane rows. Each callback reads the parsed expression and input planes and
writes complete, disjoint output rows. This adds no decoder workspace; the
only additional memory is the caller task pool's worker stacks.

The integration test constructs a two-input sample-transform item and runs the
same decode through the serial API and an executor that deliberately evaluates
row ranges out of order. All output planes are exact matches.

## CDEF and restoration row-unit profile

The normal large workload does not exercise CDEF and selects no restoration
filter, so it is not representative for deciding whether to thread these
stages. A 4096x4096 benchmark with both stages active was generated from the
center of `images/tribu-large.jpg`:

```sh
magick images/tribu-large.jpg -gravity center -crop 4096x4096+0+0 \
    +repage -quality 92 build/filter-active.jpg
avifenc --codec aom --jobs 1 --speed 4 --qcolor 35 \
    --advanced enable-cdef=1 --advanced enable-restoration=1 \
    build/filter-active.jpg build/filter-active-restoration.avif
```

The deblocked, CDEF, and restoration checksums are all distinct, confirming
that both filters change samples. System hardware sampling was unavailable
because `perf_event_paranoid` was 4, so a temporary hosted `-O2 -pg` harness
decoded the image three times with a null trace. The run took 19.38 seconds
elapsed, 18.89 seconds user time, and 1,688,808 KiB maximum RSS. `gprof`
attributed 13.59 sampled seconds:

| Stage | Inclusive sampled time | Share |
| --- | ---: | ---: |
| CDEF frame | 6.07 s | 44.7% |
| restoration frame | 2.13 s | 15.7% |
| loop filter frame | 0.54 s | 4.0% |

CDEF's total consists of 5.94 seconds in block filtering, 0.11 seconds in
direction search, and negligible plane-copy overhead. Restoration spent 1.05
seconds in filtering/control and 1.08 seconds copying unchanged 4x4 blocks.
The combined 60.4% share makes these stages materially more valuable than AV1
tile refactoring on this filter-heavy workload.

The dependency audit found these safe future execution boundaries:

- CDEF must first copy all visible input planes to output. Its remaining work
  can be divided on two-mi-row boundaries. Each unit reads only the immutable
  deblocked planes, block cells, strength indices, and CDEF parameters, then
  writes a distinct 8-luma-row region and its corresponding chroma rows.
- Restoration can be divided by plane and by luma-row bands aligned to four
  rows. Wiener and self-guided restoration scratch is stack-local. Every unit
  reads only immutable upscaled CDEF/deblocked planes and restoration-unit
  metadata, while writing distinct `4 >> subsampling_y` output rows. Stripe
  selection depends only on the aligned luma-row coordinate.

The CDEF implementation now performs the plane copy and an ordered validation
pass before dispatching two-mi-row units. This keeps invalid strength/index
errors deterministic and adds no decoder workspace. The executor is passed
only into a direct primary AV1 item; grid children, sample-transform inputs,
auxiliary alpha, and sequences remain serial to avoid nested dispatch.

Three warm runs of the same 4096x4096 filter-active image used:

```sh
/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss_kb=%M' \
    build/x86_64/avifdec --workers WIDTH --raw \
    build/filter-active-restoration.avif /dev/null
```

| Workers | Median elapsed | User time | Maximum RSS | Decoder workspace |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 5.90 s | 5.43 s | 1,647 MiB | 1,732,566,678 bytes |
| 2 | 5.01 s | 5.49 s | 1,647 MiB | 1,732,566,678 bytes |
| 4 | 4.53 s | 5.58 s | 1,647 MiB | 1,732,566,678 bytes |

Four workers reduce wall time by about 23% (1.30x) without changing decoder
workspace or peak RSS. Widths 1 and 4 produced byte-identical planar output,
the same CDEF checksum `0x26fd6feea1d4f415`, and the same restoration checksum
`0x6d8bb6a201107db9`. The smaller speedup than CDEF's instrumented profile
share indicates bandwidth pressure and the remaining serial decode/filter
stages.

Restoration now validates unit metadata in plane/unit order, then dispatches
flattened plane/four-luma-row units. Each unit copies complete output rows from
the immutable CDEF source before applying Wiener or self-guided restoration.
This replaces millions of tiny 4x4 copies, keeps scratch state worker-local,
and adds no decoder workspace.

The same three-run benchmark after restoration threading measured:

| Workers | Median elapsed | User time | Maximum RSS | Decoder workspace |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 6.02 s | 5.53 s | 1,648 MiB | 1,732,566,678 bytes |
| 2 | 4.82 s | 5.52 s | 1,647 MiB | 1,732,566,678 bytes |
| 4 | 4.27 s | 5.62 s | 1,647 MiB | 1,732,566,678 bytes |

Four workers reduce wall time by about 29% (1.41x) relative to width 1.
Compared with CDEF-only threading's 4.53-second median, restoration contributes
an additional roughly 6% wall-time reduction on this workload. Widths 1 and 4
again produced byte-identical planar output and unchanged CDEF/restoration
checksums. The width-1 result is effectively unchanged within run-to-run
variance, so the row-copy refactor does not provide a material serial speedup.

## Balanced speed, binary-size, and memory pass

The entropy reader now recognizes tile payloads contained in one input span,
loads raw bits from a bounded byte window, uses a specialized equiprobable
literal step, and retains the exact multi-span fallback. On the three-decode
hosted profile, `av1_symbol_read` fell from 20.1% self time to 11.7%;
`av1_symbol_raw_bits` and `av1_symbol_read_literal` account for another 6.7%.
Coefficient parsing and residual reconstruction are now the largest serial
costs. The large-fixture serial wall time fell from about 4.52 seconds before
the entropy work to a 4.20-4.22 second median.

The literal 100,320-byte quantizer-matrix table is generated as 44,824 bytes
of exact Huffman-coded deltas plus a generated decode tree. At frame start the
decoder expands only the selected Y, U, and V matrices into 10,032 bytes of
caller workspace, so coefficient dequantization still performs direct indexed
loads. Cold metadata, sequence, container, CLI, and platform translation units
use `-Os`; hot entropy, coefficient, transform, prediction, and filter code
remain at `-O2`.

| Static PIE measurement | Before pass | After pass | Change |
| --- | ---: | ---: | ---: |
| File size | 468,656 bytes | 408,080 bytes | -60,576 bytes (-12.9%) |
| Text and read-only data reported in the `size` text column | 436,885 bytes | 373,197 bytes | -63,688 bytes (-14.6%) |

Complete AV1 bitstream tiles now run through the caller executor. Tile payload
boundaries are parsed serially; each worker owns entropy CDFs, coefficient
contexts, transform buffers, prediction buffers, palette maps, and restoration
parser state. Frame planes, block grids, filter metadata, and restoration
units receive tile-disjoint writes. Results and trace fragments are committed
in tile order, the selected context-update CDF is retained deterministically,
and frame filtering remains serial.

Three raw-output runs of the eight-tile large fixture measured:

| Workers | Median elapsed | User time | Maximum RSS | Decoder workspace |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4.22 s | 4.06 s | 534.7 MiB | 654,311,774 bytes |
| 2 | 2.52 s | 4.13 s | 534.6 MiB | 654,468,233 bytes |
| 4 | 1.70 s | 4.25 s | 534.3 MiB | 654,762,705 bytes |

Four workers reduce wall time by about 60% (2.48x) while adding 450,931 bytes
of decoder workspace. Widths 1 and 4 produced byte-identical planar output and
identical ordered diagnostic traces. The reference suite also generates a
four-tile image and compares serial and threaded output and trace data.

## Second balanced optimization pass

Profiling found that CDEF spent most of its time rechecking bounds for taps
inside the frame. A four-corner block test now proves the complete +/-2-pixel
neighborhood is available and bypasses those per-tap checks. Wiener and
self-guided restoration use the same exact approach for blocks wholly inside
the frame and restoration stripe; boundary blocks retain the original
clamping path.

Palette colors were the largest remaining block-grid field. Only the immediate
above and left palettes are used while parsing, and reconstruction consumes the
current block before it can be overwritten. Colors therefore moved from every
cell to a fixed tile-local rolling context. The compact cell stride fell from
72 to 24 bytes and the full cell from 172 to 124 bytes. Workspace sizing also
replaced an unmatched six-byte-per-pixel margin with exact non-reduced motion
storage and omits inter-prediction scratch for reduced-still frames without
intra block copy.

The release link now strips non-runtime symbols. `src/av1.c` and `src/avif.c`
join the existing cold `-Os` set; entropy, coefficient, prediction, transform,
and filter translation units remain at `-O2`.

| Static PIE measurement | Round-one baseline | Round-two result | Change |
| --- | ---: | ---: | ---: |
| File size | 408,080 bytes | 365,456 bytes | -42,624 bytes (-10.4%) |
| Text and read-only data reported in the `size` text column | 373,389 bytes | 354,605 bytes | -18,784 bytes (-5.0%) |

Three warm raw-output runs of `images/tribu-large.avif` measured:

| Workers | Median elapsed | User time | Maximum RSS | Decoder workspace |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4.13 s | 3.99 s | 411.9 MiB | 310,824,185 bytes |
| 4 | 1.61 s | 4.07 s | 412.7 MiB | 311,029,356 bytes |

The immediately preceding speed audit measured 4.36-4.46 seconds with one
worker and 1.72-1.76 seconds with four workers. The second pass therefore
improves this workload by about 6-7% while cutting single-worker decoder
workspace by 343,487,589 bytes (52.5%).

The 4096x4096 CDEF/restoration fixture from the row-unit profile measured:

| Workers | Median elapsed | User time | Maximum RSS | Decoder workspace |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4.86 s | 4.69 s | 511.9 MiB | 435,946,949 bytes |
| 4 | 1.87 s | 4.84 s | 511.9 MiB | 435,946,949 bytes |

The audit baseline was 5.46-5.58 seconds with one worker and 2.06-2.09 seconds
with four. Serial and threaded output remained byte-identical, and the CDEF
and restoration checksums remained `0x26fd6feea1d4f415` and
`0x6d8bb6a201107db9`.

## Worker option and output-row follow-up

The CLI now removes one validated `--workers N` pair before interpreting the
remaining mode and path arguments, so the option can appear at the beginning,
between operands, or after the output path. Duplicate, missing, non-numeric,
and over-limit counts remain errors.

After still-image decoding, packed RGB/RGBA conversion reuses the synchronous
task pool across presentation rows. Streaming PNG keeps its adaptive filters
and fixed-Huffman LZ77 stream ordered, but converts bounded batches of up to 64
source rows in parallel and then supplies the cached rows to the compressor in
order. The cache adds at most `64 * packed_row_bytes` of CLI-only memory and
does not change decoder or PNG API workspace.

One scaling run of `images/tribu-large.avif` measured:

| Workers | Packed RGB elapsed | PNG elapsed |
| ---: | ---: | ---: |
| 1 | 4.52 s | 7.04 s |
| 2 | 2.66 s | 5.26 s |
| 4 | 1.75 s | 4.34 s |
| 8 | 1.29 s | 3.89 s |
| 0 (24 available CPUs) | 1.28 s | 3.87 s |

Before row-parallel output conversion, automatic workers took about 1.60
seconds for packed RGB and 4.15 seconds for PNG on the same machine. Packed
RGB now adds almost no wall time beyond the 1.23-second parallel raw decode.
PNG improves by about 7%; its remaining roughly 2.6-second tail is primarily
the single ordered filter/LZ77/IDAT stream. Serial and parallel RGB and PNG
files are byte-identical. A clean link remains 365,456 bytes because the added
code fits the existing file alignment; the allocated text/read-only column
increases from 354,605 to 355,629 bytes (1,024 bytes, 0.3%).

## Remaining decoder parallelism

The executor now also covers loop filtering, super-resolution, film-grain
stripe application, frame/reference copies, diagnostic plane checksums, and
the AV1 work inside ordered sequence-frame replay. Loop filtering uses a
barrier between vertical mi-row units and horizontal mi-column units.
Super-resolution dispatches independent output rows. Film-grain template and
scaling-table generation remains serial; chroma stripes complete before luma
stripes so chroma never reads concurrently modified luma samples.

The sequence `_ex` query scans each sync group once and returns the maximum
workspace required by any group. Tile workspace planning tracks the maximum
tile count within a replay rather than the cumulative count across frames.
This preserves the size-once/decode-any-frame contract. Auxiliary alpha is
still decoded serially in the same reusable workspace.

Three warm runs used the 7563x5042 eight-tile image plus generated 1920x1080
active-filter, super-resolution, film-grain, and three-frame sequence
fixtures:

| Workload | 1 worker | 4 workers | 8 workers | Automatic (24 CPUs) |
| --- | ---: | ---: | ---: | ---: |
| Large raw output | 3.96 s | 1.30 s | 0.86 s | 0.85 s |
| Large diagnostic trace | 3.89 s | 1.25 s | 0.82 s | 0.82 s |
| Active loop filter/CDEF | 0.56 s | 0.17 s | 0.12 s | 0.10 s |
| Active super-resolution | 0.29 s | 0.17 s | 0.16 s | 0.15 s |
| Active film grain | 0.35 s | 0.23 s | 0.21 s | 0.21 s |
| Sequence frame 2, three-frame replay | 2.23 s | 0.68 s | 0.48 s | 0.46 s |

Automatic workers therefore provide 4.7x diagnostic-trace speedup, 5.6x on
the active-filter fixture, and 4.8x on the sequence replay. Super-resolution
and film grain scale less because their serial setup and the rest of the AV1
pipeline dominate these short fixtures. All worker widths produced
byte-identical raw frames and identical normalized trace/checksum output.

Loop-filter, super-resolution, copy, and checksum dispatch add no decoder
workspace. Film grain adds
`2 * 34 * (width + 64) * sizeof(int16_t)` bytes per additional worker; at
1920 pixels this is 269,824 bytes. The 1920x1080 grain fixture requires
31,132,278 bytes with one worker, 32,109,673 with four, and 33,411,321 with
eight; the remainder is existing tile-worker scratch. Peak RSS stayed within
measurement noise for every non-grain workload. The clean x86-64 static PIE
is 373,648 bytes, 8,192 bytes above the prior 365,456-byte baseline; its
text/read-only column is 362,797 bytes.

## Validation

Run the complete suite without overriding `CFLAGS` on the `make test` command line:

```sh
make clean
make -j"$(nproc)"
make test
```

Custom freestanding `CFLAGS`, especially `-nostdinc`, can otherwise leak into the hosted libaom CMake build used by reference tests.

The optimization was additionally checked with:

- `-O2 -Werror` compilation of the affected translation units and the complete decoder;
- inspection that every `readelf -rW` runtime entry is
    `R_X86_64_RELATIVE`;
- successful startup and execution of both the decoder and strict unit binary
    with their generated relative relocations;
- the freestanding smoke test;
- exact diagnostic output comparison against the previous build on `tribu-large.avif`;
- an ASan/UBSan unit regression that decodes the embedded AVIF fixture with and without a trace and compares all output planes;
- equal full-image checksums from traced and untraced large hosted decodes.
