#!/usr/bin/env python3
"""Extract Coeff_Base_Ctx_Offset[TX_SIZES_ALL][5][5] from local av1bitstream.html.

Outputs a C include snippet defining:
  static const uint8_t kCoeffBaseCtxOffset[AV1_TX_SIZES_ALL][5][5] = { ... };

This keeps large, spec-derived tables out of hand-edited code.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


def _strip_html(code: str) -> str:
    # We only need to unescape common entities used in the spec HTML code blocks.
    return (
        code.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&amp;", "&")
        .replace("&nbsp;", " ")
    )


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    html_path = repo_root / "av1bitstream.html"
    if not html_path.exists():
        print(f"error: missing {html_path}", file=sys.stderr)
        return 2

    html = html_path.read_text("utf-8", errors="replace")

    # The spec HTML wraps most tokens in <span> tags, so search in a tag-stripped view.
    plain = _strip_html(re.sub(r"<[^>]+>", "", html))

    m = re.search(
        r"Coeff_Base_Ctx_Offset\s*\[\s*TX_SIZES_ALL\s*\]\s*\[\s*5\s*\]\s*\[\s*5\s*\]\s*=\s*\{",
        plain,
    )
    if not m:
        print("error: failed to locate Coeff_Base_Ctx_Offset header", file=sys.stderr)
        return 2

    # Extract a window starting at the initializer and ending at the matching closing brace.
    start = m.start()
    i = plain.find("{", m.end() - 1)
    if i == -1:
        print("error: failed to find opening brace", file=sys.stderr)
        return 2

    depth = 0
    end = -1
    for j in range(i, len(plain)):
        ch = plain[j]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = j + 1
                break
    if end == -1:
        print("error: failed to find matching closing brace", file=sys.stderr)
        return 2

    block = plain[i:end]

    # Extract all integers (including negatives, though we don't expect them here).
    nums = [int(x) for x in re.findall(r"-?\d+", block)]

    expected = 19 * 5 * 5
    if len(nums) != expected:
        print(
            f"error: expected {expected} ints (19*5*5), got {len(nums)}",
            file=sys.stderr,
        )
        return 2

    it = iter(nums)

    out_lines: list[str] = []
    out_lines.append("static const uint8_t kCoeffBaseCtxOffset[AV1_TX_SIZES_ALL][5][5] = {")
    for tx in range(19):
        out_lines.append("   {")
        for r in range(5):
            row = [next(it) for _ in range(5)]
            row_str = ", ".join(str(v) for v in row)
            out_lines.append(f"      {{ {row_str} }},")
        out_lines.append("   },")
    out_lines.append("};")

    sys.stdout.write("\n".join(out_lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
