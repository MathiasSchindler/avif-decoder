# m2 — Extract AV1 Image Item Data

Build:

- `make build-m2`

Run:

- `./build/avif_extract_av1 <in.avif> <out.av1>`

What it does:

- Parses `meta` enough to locate the primary item (still-image path).
- Extracts the primary `av01` item payload byte-for-byte using `iloc` extents.
- Supports `iloc` construction_method:
  - `0` (file offsets)
  - `1` (`idat`-relative)
- Performs a minimal AV1 OBU scan of the extracted bytes:
  - validates basic framing (size-delimited OBUs)
  - counts Sequence Header OBUs (expects exactly 1 for the simple still-image path)

Notes:

- This is not a full AV1 decoder; it only extracts + sanity checks the bitstream.
- For unsupported cases (derived primary items, external data refs, construction_method=2, implicit extents), it fails with a clear message.
