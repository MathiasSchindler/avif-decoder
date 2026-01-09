# avif-decoder

A dependency-free (standard C library only) AVIF decoder project, built methodically in milestones.

This repo currently contains:
- ISO-BMFF/HEIF container parsing and AVIF metadata extraction
- Primary `av01` item extraction to a raw AV1 size-delimited OBU stream
- AV1 bitstream parsing and early “frame header / tile boundary” decode scaffolding

It is a work in progress (not a production-ready decoder yet).

## Quick start

### Build

```sh
make all
```

Binaries are written to `build/`.

### Run

Container / metadata tools:

```sh
./build/avif_boxdump path/to/file.avif
./build/avif_metadump path/to/file.avif
./build/avif_extract_av1 out.av1 path/to/file.avif
```

AV1 parsing / decode scaffolding:

```sh
./build/av1_parse out.av1
./build/av1_framehdr out.av1
```

## Tests

```sh
make test
```

Notes:
- Many test targets expect locally-generated vectors under `testFiles/generated/`.
- The `testFiles/` folder is intentionally ignored and must not be committed (copyrighted corpora).

## Generating local test vectors (not committed)

Helper scripts in `tools/` may rely on external tools like `avifenc` / `avifdec`.

```sh
python3 tools/gen_test_vectors.py
```

This will create (locally):
- `testFiles/generated/src_png/`
- `testFiles/generated/avif/`
- `testFiles/generated/oracle_png/`
- `testFiles/generated/manifest.txt`

More details: see `tools/README.md`.

## Project layout

- `src/m0-container-parser/`: ISO-BMFF box walker (`avif_boxdump`)
- `src/m1-meta-parser/`: HEIF/AVIF `meta` essentials (`avif_metadump`)
- `src/m2-av1-extract/`: extract primary AV1 item payload (`avif_extract_av1`)
- `src/m3a-av1-parse/`: AV1 OBU parsing (`av1_parse`)
- `src/m3b-av1-decode/`: AV1 decode scaffolding (`av1_framehdr`)
- `tests/`: small C test harnesses and generated-vector verifier
- `tools/`: Python helpers (allowed to depend on external tooling)

The detailed milestone plan and rationale is tracked in `plan.md`.

## About AI assistance

The source code in this repository was almost entirely created using GPT-5.2 with human guidance, review, and iteration.

## License

CC0-1.0 (public domain dedication). See `LICENSE`.
