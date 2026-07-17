# Browser (WebAssembly) experiment

An experimental WebAssembly build exposes the same decoder core through a
drag-and-drop browser viewer. It requires [Emscripten](https://emscripten.org/):

```sh
brew install emscripten
make wasm
python3 -m http.server 8000 -d build/wasm
```

Open `http://localhost:8000`. Decoding runs locally in a Web Worker; files are
not uploaded.

The wrapper limits images to 8192 pixels per dimension and 33,554,432 total
pixels, with a 768 MiB decoder-workspace budget. Some large images can hit the
workspace budget below the pixel limit. The Emscripten build uses an 8 MiB stack
because the decoder's parsing state exceeds Emscripten's small default stack.

The viewer assets and the WebAssembly wrapper live under `wasm/`; the same
allocation-free decoder core described in [`architecture.md`](architecture.md)
is compiled to WebAssembly with no changes.
