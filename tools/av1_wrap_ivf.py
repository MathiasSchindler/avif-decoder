#!/usr/bin/env python3
"""Wrap a raw AV1 OBU stream into an IVF container.

This repo's extracted AV1 samples (from AVIF) are "size-delimited OBU" streams.
Some reference decoders (notably the `dav1d` CLI) prefer/require a container.

This tool creates a minimal IVF file with a single frame payload containing the
input bytes.

Usage:
  python3 tools/av1_wrap_ivf.py in.av1 out.ivf

Optional:
  --w N --h N        Override coded width/height.
  --timebase D N     IVF timebase denominator/numerator (default 1 1).

Width/height autodetection:
  If not overridden, runs `./build/av1_framehdr in.av1` and parses coded_width/
  coded_height from its output.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from typing import Tuple


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def parse_dims_from_av1_framehdr(repo_root: str, av1_path: str) -> Tuple[int, int]:
    exe = os.path.join(repo_root, "build", "av1_framehdr")
    if not os.path.exists(exe):
        die(f"missing {exe}; build first (e.g. `make all`) or pass --w/--h")

    try:
        cp = subprocess.run(
            [exe, av1_path],
            cwd=repo_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    except OSError as e:
        die(f"failed to run {exe}: {e}")

    out = cp.stdout
    m_w = re.search(r"^\s*coded_width=(\d+)\s*$", out, re.MULTILINE)
    m_h = re.search(r"^\s*coded_height=(\d+)\s*$", out, re.MULTILINE)
    if not (m_w and m_h):
        # Fallback: frame_width/frame_height.
        m_w = re.search(r"^\s*frame_width=(\d+)\s*$", out, re.MULTILINE)
        m_h = re.search(r"^\s*frame_height=(\d+)\s*$", out, re.MULTILINE)

    if not (m_w and m_h):
        die("could not parse coded_width/coded_height from av1_framehdr output; pass --w/--h")

    w = int(m_w.group(1))
    h = int(m_h.group(1))
    if w <= 0 or h <= 0 or w > 65535 or h > 65535:
        die(f"invalid parsed dimensions: {w}x{h}")
    return w, h


def write_ivf(out_path: str, payload: bytes, w: int, h: int, tb_d: int, tb_n: int) -> None:
    # IVF header: https://wiki.multimedia.cx/index.php/IVF
    # Signature 'DKIF', version 0, header size 32.
    # FourCC 'AV01' for AV1.
    # little-endian fields.
    num_frames = 1

    header = struct.pack(
        "<4sHH4sHHIIII",
        b"DKIF",
        0,
        32,
        b"AV01",
        w,
        h,
        tb_d,
        tb_n,
        num_frames,
        0,
    )
    if len(header) != 32:
        die("internal error: IVF header size mismatch")

    frame_hdr = struct.pack("<IQ", len(payload), 0)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(frame_hdr)
        f.write(payload)


def main() -> None:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("in_av1")
    ap.add_argument("out_ivf")
    ap.add_argument("--w", type=int, default=0)
    ap.add_argument("--h", type=int, default=0)
    ap.add_argument("--timebase", nargs=2, type=int, metavar=("DEN", "NUM"), default=(1, 1))
    args = ap.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    try:
        payload = open(args.in_av1, "rb").read()
    except OSError as e:
        die(f"failed to read {args.in_av1}: {e}")

    w, h = args.w, args.h
    if w <= 0 or h <= 0:
        w, h = parse_dims_from_av1_framehdr(repo_root, args.in_av1)

    tb_d, tb_n = args.timebase
    if tb_d <= 0 or tb_n <= 0:
        die("timebase must be positive")

    write_ivf(args.out_ivf, payload, w, h, tb_d, tb_n)


if __name__ == "__main__":
    main()
