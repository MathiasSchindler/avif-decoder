#!/usr/bin/env python3
"""Generate tiny deterministic AVIF test vectors.

Creates:
- testFiles/generated/src_png/*.png (source PNGs)
- testFiles/generated/avif/*.avif (encoded with avifenc, multiple presets)
- testFiles/generated/oracle_png/*.png (decoded with avifdec, one per preset)

PNG generation uses only Python stdlib (zlib/binascii/struct).
"""

from __future__ import annotations

import binascii
import hashlib
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC_PNG_DIR = os.path.join(ROOT, "testFiles", "generated", "src_png")
AVIF_DIR = os.path.join(ROOT, "testFiles", "generated", "avif")
ORACLE_PNG_DIR = os.path.join(ROOT, "testFiles", "generated", "oracle_png")
MANIFEST_PATH = os.path.join(ROOT, "testFiles", "generated", "manifest.txt")


def _chunk(chunk_type: bytes, data: bytes) -> bytes:
    assert len(chunk_type) == 4
    length = struct.pack(">I", len(data))
    crc = binascii.crc32(chunk_type)
    crc = binascii.crc32(data, crc) & 0xFFFFFFFF
    return length + chunk_type + data + struct.pack(">I", crc)


def write_png_rgba(path: str, width: int, height: int, rgba: bytes) -> None:
    if width <= 0 or height <= 0:
        raise ValueError("invalid dimensions")
    if len(rgba) != width * height * 4:
        raise ValueError("RGBA size mismatch")

    # PNG scanlines: filter byte 0 + RGBA pixels.
    raw = bytearray()
    row_bytes = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0
        row = rgba[y * row_bytes : (y + 1) * row_bytes]
        raw.extend(row)

    compressed = zlib.compress(bytes(raw), level=9)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = bytearray()
    png += sig
    png += _chunk(b"IHDR", ihdr)
    png += _chunk(b"IDAT", compressed)
    png += _chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(png)


def img_1x1_red() -> tuple[int, int, bytes]:
    return 1, 1, bytes([255, 0, 0, 255])


def img_4x4_quadrants() -> tuple[int, int, bytes]:
    # TL red, TR green, BL blue, BR white
    w, h = 4, 4
    out = bytearray(w * h * 4)

    def set_px(x: int, y: int, r: int, g: int, b: int, a: int = 255) -> None:
        i = (y * w + x) * 4
        out[i : i + 4] = bytes([r, g, b, a])

    for y in range(h):
        for x in range(w):
            left = x < w // 2
            top = y < h // 2
            if top and left:
                set_px(x, y, 255, 0, 0)
            elif top and not left:
                set_px(x, y, 0, 255, 0)
            elif (not top) and left:
                set_px(x, y, 0, 0, 255)
            else:
                set_px(x, y, 255, 255, 255)

    return w, h, bytes(out)


def img_16x16_gradient() -> tuple[int, int, bytes]:
    w, h = 16, 16
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            r = int(round(255 * x / (w - 1)))
            g = int(round(255 * y / (h - 1)))
            b = int(round(255 * (x + y) / (2 * (w - 1))))
            i = (y * w + x) * 4
            out[i : i + 4] = bytes([r, g, b, 255])
    return w, h, bytes(out)


def img_checkerboard(w: int, h: int, cell: int = 1) -> tuple[int, int, bytes]:
    if w <= 0 or h <= 0:
        raise ValueError("invalid dimensions")
    if cell <= 0:
        raise ValueError("invalid cell size")
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            on = ((x // cell) + (y // cell)) % 2 == 0
            r, g, b = (0, 0, 0) if on else (255, 255, 255)
            i = (y * w + x) * 4
            out[i : i + 4] = bytes([r, g, b, 255])
    return w, h, bytes(out)


def img_diagonal_lines(w: int, h: int, period: int = 4) -> tuple[int, int, bytes]:
    if w <= 0 or h <= 0:
        raise ValueError("invalid dimensions")
    if period <= 0:
        raise ValueError("invalid period")
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            v = ((x + y) % period) == 0
            # Magenta lines on dark background.
            r, g, b = (255, 0, 255) if v else (10, 10, 10)
            i = (y * w + x) * 4
            out[i : i + 4] = bytes([r, g, b, 255])
    return w, h, bytes(out)


def img_stripes(w: int, h: int, stripe: int = 2, vertical: bool = True) -> tuple[int, int, bytes]:
    if w <= 0 or h <= 0:
        raise ValueError("invalid dimensions")
    if stripe <= 0:
        raise ValueError("invalid stripe width")
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            k = (x // stripe) if vertical else (y // stripe)
            # Alternating cyan / yellow stripes.
            r, g, b = (0, 255, 255) if (k % 2 == 0) else (255, 255, 0)
            i = (y * w + x) * 4
            out[i : i + 4] = bytes([r, g, b, 255])
    return w, h, bytes(out)


def img_radial_gradient(w: int, h: int) -> tuple[int, int, bytes]:
    if w <= 0 or h <= 0:
        raise ValueError("invalid dimensions")
    out = bytearray(w * h * 4)
    cx = (w - 1) / 2.0
    cy = (h - 1) / 2.0
    max_r = (cx * cx + cy * cy) ** 0.5
    for y in range(h):
        for x in range(w):
            dx = x - cx
            dy = y - cy
            r = (dx * dx + dy * dy) ** 0.5
            t = 0.0 if max_r == 0 else min(1.0, r / max_r)
            # Center bright, edges dark, with a bit of color.
            rr = int(round(255 * (1.0 - t)))
            gg = int(round(180 * (1.0 - t) + 20 * t))
            bb = int(round(40 * (1.0 - t) + 200 * t))
            i = (y * w + x) * 4
            out[i : i + 4] = bytes([rr, gg, bb, 255])
    return w, h, bytes(out)


def img_circle(w: int, h: int) -> tuple[int, int, bytes]:
    if w <= 0 or h <= 0:
        raise ValueError("invalid dimensions")
    out = bytearray(w * h * 4)
    cx = (w - 1) / 2.0
    cy = (h - 1) / 2.0
    r = min(w, h) * 0.35
    r2 = r * r
    for y in range(h):
        for x in range(w):
            dx = x - cx
            dy = y - cy
            inside = (dx * dx + dy * dy) <= r2
            # Red circle on green background.
            rr, gg, bb = (220, 30, 30) if inside else (30, 140, 30)
            i = (y * w + x) * 4
            out[i : i + 4] = bytes([rr, gg, bb, 255])
    return w, h, bytes(out)


def run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stdout}")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def clean_dir(dirpath: str, exts: tuple[str, ...]) -> None:
    if not os.path.isdir(dirpath):
        return
    for name in os.listdir(dirpath):
        p = os.path.join(dirpath, name)
        if not os.path.isfile(p):
            continue
        if name.endswith(exts):
            os.unlink(p)


def main() -> int:
    os.makedirs(SRC_PNG_DIR, exist_ok=True)
    os.makedirs(AVIF_DIR, exist_ok=True)
    os.makedirs(ORACLE_PNG_DIR, exist_ok=True)

    # Ensure the generated set on disk matches the current vectors/presets.
    clean_dir(SRC_PNG_DIR, (".png",))
    clean_dir(AVIF_DIR, (".avif",))
    clean_dir(ORACLE_PNG_DIR, (".png",))

    # Base source images (PNG) used for all encode presets.
    vectors = [
        ("tiny-1x1-red", img_1x1_red()),
        ("tiny-4x4-quadrants", img_4x4_quadrants()),
        ("tiny-16x16-gradient", img_16x16_gradient()),
        ("tiny-9x7-checkerboard", img_checkerboard(9, 7, cell=1)),
        ("tiny-7x5-diagonal", img_diagonal_lines(7, 5, period=3)),
        ("tiny-31x17-stripes", img_stripes(31, 17, stripe=3, vertical=True)),
        ("tiny-17x13-radial", img_radial_gradient(17, 13)),
        ("tiny-32x32-circle", img_circle(32, 32)),
    ]

    # Encode presets. Keep them explicit for determinism.
    # - For lossless, use avifenc's lossless defaults.
    # - For lossy, use fixed speed + a few representative quality points.
    presets: list[tuple[str, list[str]]] = [
        ("lossless", ["--jobs", "1", "--speed", "6", "--lossless"]),
        ("q90", ["--jobs", "1", "--speed", "6", "-q", "90"]),
        ("q50", ["--jobs", "1", "--speed", "6", "-q", "50"]),
        ("q10", ["--jobs", "1", "--speed", "6", "-q", "10"]),
    ]

    lines: list[str] = []
    lines.append("# generated test vector manifest")
    lines.append("# format: base preset width height sha256(src_png) sha256(avif) sha256(oracle_png)")

    for name, (w, h, rgba) in vectors:
        src_png = os.path.join(SRC_PNG_DIR, f"{name}.png")

        write_png_rgba(src_png, w, h, rgba)

        for preset_name, preset_args in presets:
            if preset_name == "lossless":
                # Keep backwards-compatible filenames for the baseline lossless vector.
                avif = os.path.join(AVIF_DIR, f"{name}.avif")
                oracle_png = os.path.join(ORACLE_PNG_DIR, f"{name}.png")
            else:
                avif = os.path.join(AVIF_DIR, f"{name}__{preset_name}.avif")
                oracle_png = os.path.join(ORACLE_PNG_DIR, f"{name}__{preset_name}.png")

            run([
                "avifenc",
                *preset_args,
                src_png,
                avif,
            ])

            run([
                "avifdec",
                avif,
                oracle_png,
            ])

            lines.append(
                f"{name} {preset_name} {w} {h} {sha256_file(src_png)} {sha256_file(avif)} {sha256_file(oracle_png)}"
            )

    with open(MANIFEST_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(
        f"Wrote {len(vectors)} base PNGs, {len(vectors) * len(presets)} AVIFs. "
        f"Manifest: {os.path.relpath(MANIFEST_PATH, ROOT)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(str(e), file=sys.stderr)
        raise SystemExit(1)
