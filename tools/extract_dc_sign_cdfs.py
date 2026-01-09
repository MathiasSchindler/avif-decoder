#!/usr/bin/env python3
"""Extract Default_Dc_Sign_Cdf initializer from local av1bitstream.html.

Outputs a C initializer for a selected plane type:
    static const uint16_t kDefaultDcSignCdfLuma[4][3][3] = { ... };
    static const uint16_t kDefaultDcSignCdfChroma[4][3][3] = { ... };

Robust against HTML span wrappers by slicing initializer via brace depth.
"""

from __future__ import annotations

import os
import re
import sys
import argparse
from typing import List


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def reshape(nums: List[int], dims: List[int]) -> object:
    if not dims:
        return nums[0]
    d0 = dims[0]
    stride = 1
    for d in dims[1:]:
        stride *= d
    out = []
    for i in range(d0):
        start = i * stride
        end = start + stride
        out.append(reshape(nums[start:end], dims[1:]))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plane", choices=["luma", "chroma"], default="luma")
    args = ap.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    html_path = os.path.join(repo_root, "av1bitstream.html")

    try:
        text = open(html_path, "r", encoding="utf-8").read()
    except OSError as e:
        die(f"failed to read {html_path}: {e}")

    needle = 'Default_Dc_Sign_Cdf</span><span class="p">['
    i = text.find(needle)
    if i < 0:
        die(f"did not find C initializer anchor for Default_Dc_Sign_Cdf in {html_path}")

    i = text.find("=", i)
    if i < 0:
        die("did not find '=' after table name")
    i = text.find("{", i)
    if i < 0:
        die("did not find '{' starting initializer")

    depth = 0
    end = None
    for j in range(i, len(text)):
        ch = text[j]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = j + 1
                break

    if end is None:
        die("failed to find end of initializer via brace matching")

    init_html = text[i:end]
    # Strip HTML tags so simple expressions like "128 * 125" are visible.
    init = re.sub(r"<[^>]+>", " ", init_html)
    # Evaluate simple integer multiplications emitted by the spec (eg. "128 * 125").
    # This keeps the integer count consistent with the actual initializer.
    mul_re = re.compile(r"\b(\d+)\s*\*\s*(\d+)\b")
    while True:
        m = mul_re.search(init)
        if not m:
            break
        a = int(m.group(1))
        b = int(m.group(2))
        init = init[: m.start()] + str(a * b) + init[m.end() :]

    nums = [int(m.group(0)) for m in re.finditer(r"\b\d+\b", init)]

    # Spec: Default_Dc_Sign_Cdf[COEFF_CDF_Q_CTXS][PLANE_TYPES][DC_SIGN_CONTEXTS][3]
    dims = [4, 2, 3, 3]
    expected = 1
    for d in dims:
        expected *= d

    if len(nums) != expected:
        die(f"expected {expected} integers, found {len(nums)}")

    arr = reshape(nums, dims)  # type: ignore[assignment]

    ptype = 0 if args.plane == "luma" else 1
    suffix = "Luma" if ptype == 0 else "Chroma"
    sel = []
    for qctx in range(dims[0]):
        sel.append(arr[qctx][ptype])

    print(f"static const uint16_t kDefaultDcSignCdf{suffix}[4][3][3] = {{")
    for qctx in range(4):
        print("   {")
        for ctx in range(3):
            cdf = sel[qctx][ctx]
            print(f"      {{{cdf[0]}, {cdf[1]}, {cdf[2]}}},")
        print("   },")
    print("};")


if __name__ == "__main__":
    main()
