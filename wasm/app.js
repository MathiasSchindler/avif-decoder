const MAX_FILE_BYTES = 128 * 1024 * 1024;
const dropTarget = document.querySelector("#drop-target");
const fileInput = document.querySelector("#file-input");
const fileLabel = document.querySelector("#file-label");
const canvas = document.querySelector("#preview");
const placeholder = document.querySelector("#placeholder");
const fileMark = document.querySelector(".file-mark");
const dropMessage = document.querySelector("#drop-message");
const status = document.querySelector("#status");
const details = document.querySelector("#details");
const filename = document.querySelector("#filename");
const saveButton = document.querySelector("#save-button");
const decodeMode = document.querySelector("#decode-mode");
const encodeMode = document.querySelector("#encode-mode");
const encoderOptions = document.querySelector("#encoder-options");
const quantizer = document.querySelector("#quantizer");
const quantizerValue = document.querySelector("#quantizer-value");
const speed = document.querySelector("#speed");
const context = canvas.getContext("2d", { alpha: true });
const worker = new Worker("./decoder-worker.js", { type: "module" });
let operation = "decode";
let requestId = 0;
let encodedBlob = null;
let encodedName = "encoded.avif";

function formatBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function baseName(name) {
    return name.replace(/\.(avif|png|jpe?g)$/i, "") || "converted";
}

function setBusy(name) {
    document.body.classList.add("is-busy");
    status.textContent = operation === "encode" ? "Encoding" : "Decoding";
    status.dataset.state = "busy";
    filename.textContent = name;
    details.replaceChildren();
    saveButton.disabled = true;
    encodedBlob = null;
}

function showError(message) {
    document.body.classList.remove("is-busy");
    status.textContent = operation === "encode"
        ? "Could not encode" : "Could not decode";
    status.dataset.state = "error";
    details.textContent = message;
    saveButton.disabled = true;
}

function resetWorkspace() {
    ++requestId;
    document.body.classList.remove("is-busy");
    filename.textContent = "No file selected";
    details.textContent = operation === "encode"
        ? "PNG or JPEG images up to 128 MiB"
        : "AVIF still images up to 128 MiB";
    status.textContent = "Ready";
    status.dataset.state = "idle";
    placeholder.hidden = false;
    canvas.hidden = true;
    canvas.width = 0;
    canvas.height = 0;
    saveButton.disabled = true;
    encodedBlob = null;
}

function setOperation(nextOperation) {
    operation = nextOperation;
    const encoding = operation === "encode";

    decodeMode.setAttribute("aria-selected", String(!encoding));
    encodeMode.setAttribute("aria-selected", String(encoding));
    encoderOptions.hidden = !encoding;
    fileInput.accept = encoding
        ? ".png,.jpg,.jpeg,image/png,image/jpeg"
        : ".avif,image/avif";
    fileLabel.textContent = encoding ? "Choose PNG/JPEG" : "Choose AVIF";
    saveButton.textContent = encoding ? "Save AVIF" : "Save PNG";
    fileMark.textContent = encoding ? "PNG/JPG" : "AVIF";
    dropMessage.textContent = encoding
        ? "Drop a PNG or JPEG here" : "Drop an AVIF here";
    resetWorkspace();
}

function fileMatchesOperation(file) {
    const name = file.name.toLowerCase();

    if (operation === "encode") {
        return name.endsWith(".png") || name.endsWith(".jpg") ||
            name.endsWith(".jpeg") || file.type === "image/png" ||
            file.type === "image/jpeg";
    }
    return name.endsWith(".avif") || file.type === "image/avif";
}

async function showSourcePreview(file) {
    const bitmap = await createImageBitmap(file);
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
    context.clearRect(0, 0, canvas.width, canvas.height);
    context.drawImage(bitmap, 0, 0);
    bitmap.close();
    placeholder.hidden = true;
    canvas.hidden = false;
}

async function processFile(file) {
    if (!fileMatchesOperation(file)) {
        showError(operation === "encode"
            ? "Choose a PNG or JPEG image."
            : "Choose a file with the .avif extension.");
        return;
    }
    if (file.size > MAX_FILE_BYTES) {
        showError(`The 128 MiB browser limit was exceeded (${formatBytes(file.size)}).`);
        return;
    }

    const id = ++requestId;
    setBusy(file.name);
    try {
        if (operation === "encode") await showSourcePreview(file);
        const bytes = await file.arrayBuffer();
        worker.postMessage({
            id,
            operation,
            name: file.name,
            bytes,
            quantizer: Number(quantizer.value),
            speed: Number(speed.value),
        }, [bytes]);
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

    if (result.operation === "encode") {
        encodedBlob = new Blob([result.output], { type: "image/avif" });
        encodedName = `${baseName(result.name)}.avif`;
        saveButton.disabled = false;
        document.body.classList.remove("is-busy");
        status.textContent = "Encoded";
        status.dataset.state = "ready";
        details.textContent = `${result.width} x ${result.height} | ` +
            `q ${result.quantizer} | ${formatBytes(encodedBlob.size)} | ` +
            `${result.elapsed.toFixed(1)} ms`;
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
    showError(event.message || "The codec worker stopped unexpectedly.");
});

decodeMode.addEventListener("click", () => setOperation("decode"));
encodeMode.addEventListener("click", () => setOperation("encode"));

quantizer.addEventListener("input", () => {
    quantizerValue.value = quantizer.value;
});

fileInput.addEventListener("change", () => {
    const [file] = fileInput.files;
    if (file) processFile(file);
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
    if (file) processFile(file);
});

saveButton.addEventListener("click", () => {
    if (operation === "encode") {
        if (!encodedBlob) return;
        const anchor = document.createElement("a");
        const url = URL.createObjectURL(encodedBlob);
        anchor.href = url;
        anchor.download = encodedName;
        document.body.append(anchor);
        anchor.click();
        anchor.remove();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
        return;
    }

    canvas.toBlob((blob) => {
        if (!blob) return;
        const anchor = document.createElement("a");
        const url = URL.createObjectURL(blob);
        anchor.href = url;
        anchor.download = `${baseName(filename.textContent)}.png`;
        document.body.append(anchor);
        anchor.click();
        anchor.remove();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
    }, "image/png");
});