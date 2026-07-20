# Browser WebAssembly codec

The WebAssembly build exposes the decoder and reduced-still encoder through a
drag-and-drop browser interface. It requires
[Emscripten](https://emscripten.org/):

```sh
brew install emscripten
make wasm
python3 -m http.server 8000 -d build/wasm
```

Open `http://localhost:8000`. Both operations run locally in a Web Worker;
files are not uploaded:

- Decode an AVIF still image, inspect it, and save the result as PNG.
- Encode an 8-bit PNG or baseline JPEG as a reduced-still 8-bit YUV420 AVIF,
  with quantizer and speed controls, then download the `.avif` output.

PNG alpha and transparency are discarded because the encoder currently emits
color planes only. Odd input dimensions are extended by one pixel at the right
or bottom edge to meet the encoder's even-dimension requirement.

The wrapper limits images to 8192 pixels per dimension and 33,554,432 total
pixels, with a 768 MiB decoder-workspace or aggregate encoder-buffer budget.
Some large images can hit the memory budget below the pixel limit. The
Emscripten build uses an 8 MiB stack because the decoder's parsing state exceeds
Emscripten's small default stack.

The viewer assets and the WebAssembly wrapper live under `wasm/`. The decoder
core and caller-owned encoder API described in
[`architecture.md`](architecture.md) are compiled into one WebAssembly module.
