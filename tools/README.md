# Tools

These helper scripts are allowed to use external tools (oracle/reference) and non-C dependencies.
They are **not** part of the standalone decoder.

## Generate tiny test vectors

- `python3 tools/gen_test_vectors.py`

Requires:
- `avifenc`, `avifdec` on PATH

Outputs:
- `testFiles/generated/src_png/`
- `testFiles/generated/avif/` (multiple encode presets per base PNG)
- `testFiles/generated/oracle_png/` (oracle PNG per encoded AVIF)
- `testFiles/generated/manifest.txt`

## Sweep third-party corpus (m2 + m3a)

- `python3 tools/sweep_m3a.py`

Runs:
- `./build/avif_extract_av1` to extract the primary `av01` item
- `./build/av1_parse` to parse the extracted AV1 OBUs / Sequence Header

Goal:
- Robustness + quick visibility into unsupported cases across `testFiles/**` (excluding generated).

## Sweep third-party corpus (m2 + m3b-step1)

- `python3 tools/sweep_m3b.py`

Runs:
- `./build/avif_extract_av1` to extract the primary `av01` item
- `./build/av1_framehdr` to parse a minimal uncompressed frame header subset

Goal:
- Robustness + visibility into how many files match the initial m3b subset.

## Verify m3a vs oracle (generated vectors)

- `python3 tools/verify_generated_m3a.py`

Checks:
- `avifdec --info` matches our parsed AV1 bit depth + chroma format (YUV420/422/444 or monochrome).

## Verify generated vectors (C-only, no external tools)

- `make test-generated`

Runs a dependency-free verifier (`build/verify_generated`) that reads `testFiles/generated/manifest.txt` and checks:
- m0: `avif_boxdump` runs without error
- m1: `avif_metadump` runs without error and `pixi` matches AVIF `av1C`-derived bit depth/channels
- m2: `avif_extract_av1` succeeds
- m3a: `av1_parse` succeeds
- m3b-step1: `av1_framehdr` succeeds and dimensions match the manifest

## Verify m3b-step1 vs oracle (generated vectors)

- `python3 tools/verify_generated_m3b.py`

Checks:
- `avifdec --info` Resolution matches `av1_framehdr` reported presentation size.

## Generate synthetic AV1 tile-group vectors (m3b step2)

- `python3 tools/gen_av1_tilegroup_vectors.py`

Outputs:
- `testFiles/generated/av1/` (raw AV1 OBU streams containing `OBU_FRAME_HEADER` + `OBU_TILE_GROUP`)

## Verify m3b tile-group parsing (synthetic vectors)

- `python3 tools/verify_generated_m3b_tilegroups.py`

Checks:
- `av1_framehdr` parses `tile_info()` from `OBU_FRAME_HEADER`
- `av1_framehdr` parses `OBU_TILE_GROUP` headers and reports per-tile payload sizes (including
	the `tile_start_and_end_present_flag` subset case)

## Verify m3b tile-group parsing (embedded in OBU_FRAME)

- `python3 tools/verify_generated_m3b_embedded_tilegroups.py`

Checks:
- For generated AVIFs, `av1_framehdr` can locate and print `tile_group` payload ranges when tile data is embedded inside `OBU_FRAME`.

## Verify m1 spec constraints (generated vectors)

- `python3 tools/verify_generated_m1_pixi_av1c.py`

Checks:
- If `pixi` is present for the primary item: `pixi` channels/bit depth match values derived from `av1C`.

## Reference decoder notes

- `aomdec` can decode the raw extracted `.av1` OBU stream directly.
- The `dav1d` CLI typically expects a container. You can wrap the extracted `.av1` as IVF:
	- `python3 tools/av1_wrap_ivf.py primary.av1 primary.ivf`
	- `dav1d --demuxer ivf --muxer null -o /dev/null -i primary.ivf`

