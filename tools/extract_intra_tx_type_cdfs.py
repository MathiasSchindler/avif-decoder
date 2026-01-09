#!/usr/bin/env python3
"""Extract Default_Intra_Tx_Type_Set{1,2}_Cdf from local av1bitstream.html.

Outputs C initializers:
  static const uint16_t kDefaultIntraTxTypeSet1Cdf[2][13][8] = { ... };
  static const uint16_t kDefaultIntraTxTypeSet2Cdf[3][13][6] = { ... };

The spec tables include the trailing count slot (last element is 0).
"""

from __future__ import annotations

import os
import re
import sys
from typing import List


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def slice_initializer(text: str, needle: str) -> str:
    i = text.find(needle)
    if i < 0:
        die(f"did not find initializer anchor for {needle}")
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
    return text[i:end]


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
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    html_path = os.path.join(repo_root, "av1bitstream.html")

    try:
        text = open(html_path, "r", encoding="utf-8").read()
    except OSError as e:
        die(f"failed to read {html_path}: {e}")

    init1 = slice_initializer(text, 'Default_Intra_Tx_Type_Set1_Cdf</span><span class="p">[')
    nums1 = [int(m.group(0)) for m in re.finditer(r"\b\d+\b", init1)]
    dims1 = [2, 13, 8]
    expected1 = 1
    for d in dims1:
        expected1 *= d
    if len(nums1) != expected1:
        die(f"set1: expected {expected1} integers, found {len(nums1)}")
    arr1 = reshape(nums1, dims1)  # type: ignore[assignment]

    init2 = slice_initializer(text, 'Default_Intra_Tx_Type_Set2_Cdf</span><span class="p">[')
    nums2 = [int(m.group(0)) for m in re.finditer(r"\b\d+\b", init2)]
    dims2 = [3, 13, 6]
    expected2 = 1
    for d in dims2:
        expected2 *= d
    if len(nums2) != expected2:
        die(f"set2: expected {expected2} integers, found {len(nums2)}")
    arr2 = reshape(nums2, dims2)  # type: ignore[assignment]

    print("static const uint16_t kDefaultIntraTxTypeSet1Cdf[2][13][8] = {")
    for a in range(2):
        print("   {")
        for m in range(13):
            row = arr1[a][m]
            print(f"      {{{', '.join(str(x) for x in row)}}},")
        print("   },")
    print("};")
    print("")
    print("static const uint16_t kDefaultIntraTxTypeSet2Cdf[3][13][6] = {")
    for a in range(3):
        print("   {")
        for m in range(13):
            row = arr2[a][m]
            print(f"      {{{', '.join(str(x) for x in row)}}},")
        print("   },")
    print("};")


if __name__ == "__main__":
    main()
