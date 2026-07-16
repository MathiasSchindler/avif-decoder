import createAvifDecoder from "./avif-decoder.js";

const statusNames = [
    "ok",
    "invalid argument",
    "truncated input",
    "invalid data",
    "arithmetic overflow",
    "configured limit exceeded",
    "out of memory",
    "I/O error",
    "unsupported feature",
];

const decoderPromise = createAvifDecoder();

function contextName(value) {
    const bytes = [value >>> 24, value >>> 16, value >>> 8, value];
    if (bytes.every((byte) => byte >= 32 && byte <= 126)) {
        return String.fromCharCode(...bytes);
    }
    if ((value >>> 8) === 0x4f4255) {
        return `OBU ${value & 0xff}`;
    }
    return `0x${value.toString(16).padStart(8, "0")}`;
}

self.addEventListener("message", async (event) => {
    const { id, name, bytes } = event.data;
    const decoder = await decoderPromise;
    const input = new Uint8Array(bytes);
    const inputPointer = decoder._malloc(input.byteLength);

    if (inputPointer === 0) {
        self.postMessage({ id, name, error: "Unable to allocate input memory." });
        return;
    }

    try {
        decoder.HEAPU8.set(input, inputPointer);
        const started = performance.now();
        const status = decoder._avif_wasm_decode(
            inputPointer, input.byteLength);
        const elapsed = performance.now() - started;

        if (status !== 0) {
            const offset = decoder._avif_wasm_error_offset();
            const context = contextName(decoder._avif_wasm_error_context());
            const statusName = statusNames[status] ?? `status ${status}`;
            if (status === 5 && decoder._avif_wasm_stage() === 1) {
                throw new Error(
                    "This image exceeds the browser decoder's memory budget.");
            }
            throw new Error(`${statusName} at byte ${offset} (${context})`);
        }

        const pixelPointer = decoder._avif_wasm_pixel_pointer();
        const pixelBytes = decoder._avif_wasm_pixel_bytes();
        const pixels = decoder.HEAPU8.slice(
            pixelPointer, pixelPointer + pixelBytes);
        const result = {
            id,
            name,
            pixels: pixels.buffer,
            width: decoder._avif_wasm_width(),
            height: decoder._avif_wasm_height(),
            sourceWidth: decoder._avif_wasm_source_width(),
            sourceHeight: decoder._avif_wasm_source_height(),
            bitDepth: decoder._avif_wasm_bit_depth(),
            hasAlpha: decoder._avif_wasm_has_alpha() !== 0,
            elapsed,
        };

        decoder._avif_wasm_reset();
        self.postMessage(result, [result.pixels]);
    } catch (error) {
        decoder._avif_wasm_reset();
        self.postMessage({
            id,
            name,
            error: error instanceof Error ? error.message : String(error),
        });
    } finally {
        decoder._free(inputPointer);
    }
});