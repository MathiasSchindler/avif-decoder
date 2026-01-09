#!/usr/bin/env python3
import re
from pathlib import Path


def reshape(flat: list[int], dims: list[int]):
    if not dims:
        return flat[0]
    d = dims[0]
    rest = dims[1:]
    stride = 1
    for x in rest:
        stride *= x
    return [reshape(flat[i * stride : (i + 1) * stride], rest) for i in range(d)]


def extract_table(html: str, name: str, dims: list[int]):
    # Find the last occurrence of the table name (the code block in the HTML).
    idx = html.rfind(name)
    if idx < 0:
        raise SystemExit(f"missing table: {name}")

    chunk = html[idx : idx + 900_000]

    # Parse numeric initializer data only (avoid picking up dimension numbers from the declaration).
    # The spec shows e.g. `Default_Eob_Pt_16_Cdf[...] = { ... }` inside an HTML-highlighted C block.
    eq = chunk.find("=")
    if eq < 0:
        raise SystemExit(f"missing '=' for: {name}")
    brace = chunk.find("{", eq)
    if brace < 0:
        raise SystemExit(f"missing '{{' initializer for: {name}")

    nums = list(map(int, re.findall(r"\b\d+\b", chunk[brace:])))

    need = 1
    for d in dims:
        need *= d
    if len(nums) < need:
        raise SystemExit(f"{name}: need {need} nums, found {len(nums)}")

    nums = nums[:need]
    return reshape(nums, dims)


def main() -> None:
    html = Path("av1bitstream.html").read_text(encoding="utf-8", errors="ignore")

    Q = 4
    P = 2  # PLANE_TYPES

    tables_with_ctx = [
        ("Default_Eob_Pt_16_Cdf", [Q, P, 2, 6], "kDefaultEobPt16CdfLuma"),
        ("Default_Eob_Pt_32_Cdf", [Q, P, 2, 7], "kDefaultEobPt32CdfLuma"),
        ("Default_Eob_Pt_64_Cdf", [Q, P, 2, 8], "kDefaultEobPt64CdfLuma"),
        ("Default_Eob_Pt_128_Cdf", [Q, P, 2, 9], "kDefaultEobPt128CdfLuma"),
        ("Default_Eob_Pt_256_Cdf", [Q, P, 2, 10], "kDefaultEobPt256CdfLuma"),
    ]

    for name, dims, out in tables_with_ctx:
        arr = extract_table(html, name, dims)
        L = len(arr[0][0][0])
        print(f"static const uint16_t {out}[4][2][{L}] = {{")
        for q in range(4):
            print("   {")
            for ctx in range(2):
                vals = arr[q][0][ctx]  # plane 0 == luma
                print("      {" + ", ".join(map(str, vals)) + "},")
            print("   },")
        print("};\n")

    # No ctx dimension for these.
    for name, dims, out in [
        ("Default_Eob_Pt_512_Cdf", [Q, P, 11], "kDefaultEobPt512CdfLuma"),
        ("Default_Eob_Pt_1024_Cdf", [Q, P, 12], "kDefaultEobPt1024CdfLuma"),
    ]:
        arr = extract_table(html, name, dims)
        L = len(arr[0][0])
        print(f"static const uint16_t {out}[4][{L}] = {{")
        for q in range(4):
            vals = arr[q][0]  # plane 0 == luma
            print("   {" + ", ".join(map(str, vals)) + "},")
        print("};\n")


if __name__ == "__main__":
    main()
