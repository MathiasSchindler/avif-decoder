# m1 — `meta` essentials parser

Build:

- `make build-m1`

Run:

- `./build/avif_metadump <file.avif>`
- `./build/avif_metadump --extract-primary primary.av1 <file.avif>`

What it does:

- Locates the top-level `meta` box.
- Parses core item metadata needed to find the primary item and its payload:
  - `hdlr`, `pitm`, `iinf`/`infe`, `iloc`, `iprp`/`ipco`/`ipma`
- Recognizes the most important early item properties:
  - `ispe`, `pixi`, `av1C`

Notes:

- For files that use features outside the current scope (derived images, sequences, etc.), the tool should fail with a clear “unsupported (yet)” message rather than crashing.
