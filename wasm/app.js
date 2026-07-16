const MAX_FILE_BYTES = 128 * 1024 * 1024;
const dropTarget = document.querySelector("#drop-target");
const fileInput = document.querySelector("#file-input");
const canvas = document.querySelector("#preview");
const placeholder = document.querySelector("#placeholder");
const status = document.querySelector("#status");
const details = document.querySelector("#details");
const filename = document.querySelector("#filename");
const saveButton = document.querySelector("#save-button");
const context = canvas.getContext("2d", { alpha: true });
const worker = new Worker("./decoder-worker.js", { type: "module" });
let requestId = 0;

function formatBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function setBusy(name) {
    document.body.classList.add("is-busy");
    status.textContent = "Decoding";
    status.dataset.state = "busy";
    filename.textContent = name;
    details.replaceChildren();
    saveButton.disabled = true;
}

function showError(message) {
    document.body.classList.remove("is-busy");
    status.textContent = "Could not decode";
    status.dataset.state = "error";
    details.textContent = message;
    placeholder.hidden = false;
}

async function decodeFile(file) {
    if (!file.name.toLowerCase().endsWith(".avif")) {
        showError("Choose a file with the .avif extension.");
        return;
    }
    if (file.size > MAX_FILE_BYTES) {
        showError(`The 128 MiB browser limit was exceeded (${formatBytes(file.size)}).`);
        return;
    }

    const id = ++requestId;
    setBusy(file.name);
    try {
        const bytes = await file.arrayBuffer();
        worker.postMessage({ id, name: file.name, bytes }, [bytes]);
    } catch (error) {
        showError(error instanceof Error ? error.message : String(error));
    }
}

worker.addEventListener("message", (event) => {
    const result = event.data;
    if (result.id !== requestId) return;
    if (result.error) {
        showError(result.error);
        return;
    }

    const pixels = new Uint8ClampedArray(result.pixels);
    canvas.width = result.width;
    canvas.height = result.height;
    context.putImageData(
        new ImageData(pixels, result.width, result.height), 0, 0);
    placeholder.hidden = true;
    canvas.hidden = false;
    saveButton.disabled = false;
    document.body.classList.remove("is-busy");
    status.textContent = "Decoded";
    status.dataset.state = "ready";
    const sourceSize = result.sourceWidth === result.width &&
        result.sourceHeight === result.height
        ? `${result.width} x ${result.height}`
        : `${result.sourceWidth} x ${result.sourceHeight} -> ` +
          `${result.width} x ${result.height}`;
    details.textContent = `${sourceSize} | ${result.bitDepth}-bit | ` +
        `${result.hasAlpha ? "alpha" : "opaque"} | ` +
        `${result.elapsed.toFixed(1)} ms`;
});

worker.addEventListener("error", (event) => {
    showError(event.message || "The decoder worker stopped unexpectedly.");
});

fileInput.addEventListener("change", () => {
    const [file] = fileInput.files;
    if (file) decodeFile(file);
    fileInput.value = "";
});

for (const eventName of ["dragenter", "dragover"]) {
    dropTarget.addEventListener(eventName, (event) => {
        event.preventDefault();
        dropTarget.classList.add("is-dragging");
    });
}

for (const eventName of ["dragleave", "drop"]) {
    dropTarget.addEventListener(eventName, (event) => {
        event.preventDefault();
        dropTarget.classList.remove("is-dragging");
    });
}

dropTarget.addEventListener("drop", (event) => {
    const [file] = event.dataTransfer.files;
    if (file) decodeFile(file);
});

saveButton.addEventListener("click", () => {
    canvas.toBlob((blob) => {
        if (!blob) return;
        const anchor = document.createElement("a");
        const base = filename.textContent.replace(/\.avif$/i, "") || "decoded";
        const url = URL.createObjectURL(blob);
        anchor.href = url;
        anchor.download = `${base}.png`;
        document.body.append(anchor);
        anchor.click();
        anchor.remove();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
    }, "image/png");
});