# m3a — AV1 bitstream parser (OBUs + Sequence Header)

Build:

- `make build-m3a`

Run:

- `./build/av1_parse <in.av1>`

What it does:

- Parses a size-delimited OBU stream (as extracted by m2).
- Finds and parses the Sequence Header OBU and prints a concise summary:
  - profile
  - still_picture / reduced_still_picture_header
  - operating_point_idc (if present)
  - bit_depth
  - monochrome
  - subsampling_x / subsampling_y
  - color range + CICP (color primaries / transfer / matrix)

Notes:

- This is intentionally a validator/parser only (no frame reconstruction yet).
- For now, it expects a **single** Sequence Header OBU; multiple sequence headers are treated as unsupported.
- Supports both reduced and non-reduced sequence headers (m3a focuses on robust header parsing/validation, not decode).
