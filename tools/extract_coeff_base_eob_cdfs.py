#!/usr/bin/env python3
"""Extract Default_Coeff_Base_Eob_Cdf initializer from local av1bitstream.html.

Outputs a C initializer for a selected plane type:
    static const uint16_t kDefaultCoeffBaseEobCdfLuma[4][5][4][4] = { ... };
    static const uint16_t kDefaultCoeffBaseEobCdfChroma[4][5][4][4] = { ... };

This intentionally tolerates the HTML "span" wrappers by slicing the
initializer via brace depth and then regexing integers.
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

    # The identifier appears multiple times in prose; anchor on the code block.
    needle = 'Default_Coeff_Base_Eob_Cdf</span><span class="p">['
    i = text.find(needle)
    if i < 0:
        die(f"did not find C initializer anchor for Default_Coeff_Base_Eob_Cdf in {html_path}")

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

    # Spec: Default_Coeff_Base_Eob_Cdf[COEFF_CDF_Q_CTXS][TX_SIZES][PLANE_TYPES][SIG_COEF_CONTEXTS_EOB][4]
    dims = [4, 5, 2, 4, 4]
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
        q_list = []
        for tx in range(dims[1]):
            q_list.append(arr[qctx][tx][ptype])
        sel.append(q_list)

    print(f"static const uint16_t kDefaultCoeffBaseEobCdf{suffix}[4][5][4][4] = {{")
    for qctx in range(4):
        print("   {")
        for tx in range(5):
            print("      {")
            for ctx in range(4):
                cdf = sel[qctx][tx][ctx]
                print(f"         {{{cdf[0]}, {cdf[1]}, {cdf[2]}, {cdf[3]}}},")
            print("      },")
        print("   },")
    print("};")


if __name__ == "__main__":
    main()
