#!/usr/bin/env python3
"""Extract Default_Coeff_Br_Cdf initializer from local av1bitstream.html.

Outputs a C initializer for a selected plane type:
    static const uint16_t kDefaultCoeffBrCdfLuma[4][TX][21][5] = { ... };
    static const uint16_t kDefaultCoeffBrCdfChroma[4][TX][21][5] = { ... };

Where TX is typically 4 (4x4..32x32). The extractor auto-detects TX dimension
(4 or 5) based on integer count.

Robust against HTML span wrappers by slicing initializer via brace depth.
"""

from __future__ import annotations

import os
import re
import sys
import argparse
from typing import List, Optional, Tuple


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


def try_dims(nums: List[int], dims: List[int]) -> Optional[object]:
    expected = 1
    for d in dims:
        expected *= d
    if len(nums) != expected:
        return None
    return reshape(nums, dims)


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

    needle = 'Default_Coeff_Br_Cdf</span><span class="p">['
    i = text.find(needle)
    if i < 0:
        die(f"did not find C initializer anchor for Default_Coeff_Br_Cdf in {html_path}")

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

    init = text[i:end]
    nums = [int(m.group(0)) for m in re.finditer(r"\b\d+\b", init)]

    # Spec: Default_Coeff_Br_Cdf[COEFF_CDF_Q_CTXS][TX_SIZES][PLANE_TYPES][LEVEL_CONTEXTS][BR_CDF_SIZE+1]
    # COEFF_CDF_Q_CTXS=4, PLANE_TYPES=2, LEVEL_CONTEXTS=21, BR_CDF_SIZE+1=5
    candidates: List[Tuple[int, List[int]]] = [
        (4, [4, 4, 2, 21, 5]),
        (5, [4, 5, 2, 21, 5]),
    ]

    arr = None
    tx_dim = None
    for tx_dim_try, dims in candidates:
        arr_try = try_dims(nums, dims)
        if arr_try is not None:
            arr = arr_try
            tx_dim = tx_dim_try
            break

    if arr is None or tx_dim is None:
        die(
            "unexpected integer count for Default_Coeff_Br_Cdf; "
            f"got {len(nums)} ints, expected one of {[4*4*2*21*5, 4*5*2*21*5]}"
        )

    ptype = 0 if args.plane == "luma" else 1
    suffix = "Luma" if ptype == 0 else "Chroma"

    sel = []
    for qctx in range(4):
        q_list = []
        for tx in range(tx_dim):
            q_list.append(arr[qctx][tx][ptype])
        sel.append(q_list)

    print(f"static const uint16_t kDefaultCoeffBrCdf{suffix}[4][{tx_dim}][21][5] = {{")
    for qctx in range(4):
        print("   {")
        for tx in range(tx_dim):
            print("      {")
            for ctx in range(21):
                cdf = sel[qctx][tx][ctx]
                print(f"         {{{cdf[0]}, {cdf[1]}, {cdf[2]}, {cdf[3]}, {cdf[4]}}},")
            print("      },")
        print("   },")
    print("};")


if __name__ == "__main__":
    main()
