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
- approximately six bytes per visible pixel of additional state for 8-bit color;
- a per-4x4-cell grid containing `Av1BlockCell`, block metadata, restoration units, CDFs, and contexts.

`Av1BlockCell` is 172 bytes in this build. Its motion-vector candidates, warp parameters, palettes, and inter/compound state make the cell grid a major contributor. Reducing this memory requires a separate representation change with broad reference testing; merely removing zero fills would expose stale caller workspace and invalid reference pixels.

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
