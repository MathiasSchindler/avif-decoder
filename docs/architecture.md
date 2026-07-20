# Architecture

`avif-decoder` is a dependency-free AVIF still-image and image-sequence codec
written in freestanding C. The decoder remains the larger API; its sister
encoder writes a deliberately narrow reduced-still AVIF profile. This document
describes how the project is put together; see [`usage.md`](usage.md) for the
command line, [`api.md`](api.md) for the library interfaces, and
[`support.md`](support.md) for the feature matrix.

## Design goals

- **No external dependencies.** The command-line executable issues native
  system calls directly and links no C library, codec library, or image
  library. Linux builds are static PIE executables without an interpreter.
  macOS builds use the system dyld as their process launcher but carry no
  dynamic-library load commands.
- **Allocation- and I/O-free cores.** The decoder and encoder cores never
  allocate memory or perform file I/O. They operate on caller-owned input,
  workspace, and output buffers, with explicit limits and structured errors.
- **Reentrancy.** Separate input, workspace, output, trace, and error objects
  make every call reentrant.

## Layering

The code separates portable, freestanding codec cores from thin
**CLI/runtime/platform** layers:

- The **core** (`CORE_C_SOURCES` in the `Makefile`) is the only code compiled
  into the library surface, the tests, the fuzzer, and the WebAssembly build.
  It is allocation-free and I/O-free.
- The **runtime and platform** layer (`src/main.c`, `src/task_pool.c`,
  `src/platform/<os>/io.c`, `src/platform/<os>/thread.c`, and the
  `src/arch/<arch>/<os>` startup/syscall stubs) turns the core into a
  standalone executable and provides the optional parallel task pool.
- The **encoder core** (`ENCODER_MODULE_C_SOURCES`) accepts planar YUV420 and
  owns byte/range writing, AV1 transform and tile coding, and AVIF assembly.
  `src/encoder/main.c` adds raw file I/O plus project-owned PNG/JPEG decoding
  and RGB-to-YUV conversion. None of those CLI adapters broaden the planar
  public encoder contract.

## Source tree

```text
src/
  avifdec.h              Public library API (the only public header)
  base.c                 Freestanding helpers: checked arithmetic, memory, arena
  bmff.c                 ISOBMFF box parsing
  avif.c                 AVIF still-image item model and properties
  avif_sequence.c        AVIF 'avis' image-sequence track model
  avif_sato.c            Sample-transform ('sato') expression evaluation
  avif_rgb.c             Planar YUV to packed RGB/RGBA conversion
  png.c                  Allocation-free streaming PNG encoder
  av1*.c                 AV1 decoder (see below)
  main.c                 Command-line front end
  task_pool.c            clone/futex worker pool (Linux)
  arch/<arch>/<os>/      crt0 startup and raw syscall stubs
  platform/<os>/         io.c (native I/O) and thread.c (thread primitives)
  shared/                Freestanding stdint/stddef/stdbool and platform shims
  encoder/
    avifenc.h             Public allocation-free encoder API
    avifenc.c             Validation, sizing, workspace, and assembly
    avif_write.c          Single-item AVIF serializer
    av1_*_write.c         Reduced-still AV1 headers, symbols, tile, transform
    image_input.c         Allocation-free PNG/baseline-JPEG CLI decoder
    main.c                Freestanding avifenc command-line front end
  av1_*_tables.inc       Generated AV1 constant tables (see below)
tests/                   Unit binaries, shell test suites, fuzz harness/seeds
tools/                   Table generators and the macOS dylib remover
wasm/                    WebAssembly wrapper and browser viewer assets
docs/                    This documentation
```

The AV1 decoder is split by stage: bitstream/OBU framing
(`av1_bitstream.c`), high-level frame flow (`av1.c`), references and frame
context (`av1_reference.c`), tiles and entropy (`av1_tile*.c`, `av1_symbol.c`,
`av1_coeff.c`), prediction (`av1_intra.c`, `av1_inter*.c`, `av1_predict.c`,
`av1_warp.c`, `av1_partition.c`, `av1_block.c`, `av1_recon.c`), post-filters
(`av1_filter.c`, `av1_cdef.c`, `av1_superres.c`, `av1_restoration_filter.c`),
film grain (`av1_film_grain.c`), profiles/levels (`av1_profile.c`), and
metadata (`av1_metadata.c`).

## Workspace and memory model

`AvifdecImage` uses 16-bit sample storage for all source bit depths, and
strides are measured in samples, not bytes. The caller sizes a single
workspace buffer from the `workspace_required` reported by a query, then
passes it to the matching decode call. Workspace may be unaligned; internal
arena allocations align absolute pointers safely.

The queried `workspace_plane_buffer_count` records the conservative
plane-buffer plan behind the returned requirement. Reduced-still AV1 streams
avoid allocating inter-frame pixel references and motion fields, and when
CDEF, super-resolution, or restoration is an exact pass-through, their plane
views alias the preceding stage instead of consuming new buffers.

## Parallelism

The `_ex` API variants accept an optional `AvifdecExecutor`: a structured
`parallel_for` callback owned entirely by the caller. The decoder performs no
allocation for it and retains no thread state. Parallel regions cover
independent grid tiles, AV1 bitstream tiles, sample-transform output rows,
loop-filter row/column units, CDEF units, super-resolution rows, restoration
units, frame-plane copies, per-plane trace checksums, and film-grain stripes.
Sequence frames stay ordered, but each replayed frame can use those AV1
regions. See [`api.md`](api.md) for the exact workspace implications.

The CLI supplies this executor through the task-pool scheduler in
`src/task_pool.c`. Linux/x86-64 workers use clone/futex primitives; macOS/arm64
workers use pthread and ulock primitives from the already-linked `libSystem`.

## Generated tables

Several AV1 constant tables (`src/av1_*_tables.inc`,
`src/av1_film_grain_gaussian.inc`) are generated from the AV1 specification by
the Perl scripts under `tools/`. The build reproduces them from the maintainer
`docs/av1.html` file and byte-compares the result against the checked-in copies
when that file is present. See [`testing.md`](testing.md).

## Platform support

The supplied freestanding executable targets Linux/x86-64 and macOS/arm64.
Adding a platform means providing an `arch/<arch>/<os>` startup path and a
`platform/<os>` I/O and thread implementation; the decoder core is unchanged.
