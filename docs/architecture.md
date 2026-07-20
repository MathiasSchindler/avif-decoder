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

- The public decoder contract is `src/avifdec.h`. Decoder internals are under
  `src/decoder`; `main.c` and `png_write.c` form the decoder CLI layer.
- Shared AV1 entropy, coefficient, prediction, reconstruction, and DSP
  primitives used by both codec directions are under `src/codec`.
- The encoder core and public `src/encoder/avifenc.h` are under `src/encoder`.
  `src/encoder/main.c` is the CLI adapter, and
  `src/encoder/cli/image_input.c` dispatches its image input adapters.
- Root-level `src/base.c` and `src/base.h` own common checked arithmetic,
  readers, memory helpers, and the caller-backed arena. Root-level
  `src/task_pool.c` and `src/task_pool.h` adapt the public executors to native
  workers.
- `src/platform/platform.h` is the I/O, page-memory, and worker abstraction;
  `src/platform/<os>` implements it. `src/arch/<arch>/<os>` owns process
  startup, raw syscall declarations/stubs, and architecture assembly.
- `src/shared` contains only the freestanding standard-header shims selected
  by `-nostdinc`.

The decoder and encoder core source sets in the `Makefile` are allocation-free
and I/O-free. CLI adapters add native file I/O and the optional task pool
without broadening either public core contract.

## Source tree

```text
src/
  avifdec.h              Public decoder API
  base.c, base.h         Checked arithmetic, readers, memory, caller arena
  task_pool.c, .h        Common native-worker scheduler and executor adapter
  decoder/
    bmff.c, avif*.c       AVIF parse, recursive item decode, and presentation
    av1*.c                Decoder-specific AV1 parsing and reconstruction flow
    png.c                 Allocation-free streaming PNG encoder
    png_write.c           Decoder CLI RGB conversion and PNG file output
    main.c                Freestanding avifdec command-line front end
  codec/                 AV1 primitives shared by decoder and encoder
  encoder/
    avifenc.h             Public allocation-free encoder API
    avifenc.c             Validation, sizing, workspace, and assembly
    avif_write.c          Single-item AVIF serializer
    av1_*.c               Reduced-still AV1 coding and transforms
    main.c                Freestanding avifenc command-line front end
    cli/image_input.c     CLI image-format dispatch adapter
    cli/image_input_png.c Allocation-free PNG query and decode
    cli/image_input_jpeg.c Allocation-free baseline-JPEG query and decode
  shared/                Freestanding standard-header shims
  platform/
    platform.h           Native I/O, page-memory, and worker abstraction
    <os>/                 OS-specific I/O and thread implementations
  arch/<arch>/<os>/      crt0, syscall declarations/stubs, architecture code
tests/                   Unit binaries, shell test suites, fuzz harness/seeds
tools/                   Table generators and the macOS dylib remover
wasm/                    WebAssembly wrapper and browser viewer assets
docs/                    This documentation
```

Within `src/decoder`, AV1 work is split by stage: bitstream/OBU framing
(`av1_bitstream.c`), high-level frame flow (`av1.c`), frame-header syntax
(`av1_frame_header.c`), frame/image copies and output scaling (`av1_copy.c`),
and references and frame context (`av1_reference.c`). Tile partition and mode
syntax live in `av1_tile_mode.c`; `av1_tile.c` handles residual decoding and
block reconstruction, with specialized inter-mode, motion-vector, palette, and
restoration modules alongside it. Inter prediction and warping
(`av1_inter*.c`, `av1_warp.c`), partition/block flow (`av1_partition.c`,
`av1_block.c`), post-filters (`av1_filter.c`, `av1_cdef.c`, `av1_superres.c`,
`av1_restoration_filter.c`), film grain (`av1_film_grain.c`), profiles/levels
(`av1_profile.c`), and metadata (`av1_metadata.c`) remain separate. Entropy/CDF,
coefficients, intra prediction, DSP, and reconstruction primitives shared with
the encoder live in `src/codec`.

AVIF container processing has a similar internal boundary: `avif_parse.c`
validates and indexes container metadata and resolves item extents, while
`avif.c` recursively queries and decodes items and plans their workspace.

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

AV1 coefficient, palette, and quantization tables under `src/codec`, plus the
warp and film-grain tables under `src/decoder`, are generated from the AV1
specification by the Perl scripts under `tools/`. The build reproduces them
from the maintainer `docs/av1.html` file and byte-compares the result against
the checked-in copies when that file is present. See
[`testing.md`](testing.md).

## Platform support

The supplied freestanding executable targets Linux/x86-64 and macOS/arm64.
Adding a platform means providing an `arch/<arch>/<os>` startup path and a
`platform/<os>` I/O and thread implementation; the decoder core is unchanged.
