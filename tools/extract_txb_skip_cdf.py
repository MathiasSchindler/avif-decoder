#!/usr/bin/env python3
"""Extract Default_Txb_Skip_Cdf from av1bitstream.html.

Writes src/m3b-av1-decode/av1_default_cdfs_txb_skip.inc containing only the
C initializer (the {...} part).

This is a tiny dev helper; it is not used at runtime.
"""

from __future__ import annotations

import html
import re
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    spec_path = repo_root / "av1bitstream.html"
    out_path = repo_root / "src/m3b-av1-decode/av1_default_cdfs_txb_skip.inc"

    raw = spec_path.read_text(encoding="utf-8", errors="ignore")
    txt = re.sub(r"<[^>]+>", "", raw)
    txt = html.unescape(txt)

    # Find the table definition header in the stripped text.
    header_pat = (
        r"Default_Txb_Skip_Cdf\s*\[\s*COEFF_CDF_Q_CTXS\s*\]"
        r"\s*\[\s*TX_SIZES\s*\]\s*\[\s*TXB_SKIP_CONTEXTS\s*\]"
        r"\s*\[\s*3\s*\]\s*=\s*\{"  # '=' then opening brace
    )
    m = re.search(header_pat, txt)
    if not m:
        raise SystemExit("Could not find Default_Txb_Skip_Cdf header")

    # Start parsing from the first '{' after '='.
    eq = txt.find("=", m.start())
    if eq == -1:
        raise SystemExit("Malformed header (missing '=')")
    brace_start = txt.find("{", eq)
    if brace_start == -1:
        raise SystemExit("Malformed header (missing '{')")

    # Scan forward to find the matching closing brace.
    depth = 0
    end = None
    for i in range(brace_start, len(txt)):
        ch = txt[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        raise SystemExit("Could not find matching '}'")

    init = txt[brace_start:end].strip() + "\n"
    out_path.write_text(init, encoding="utf-8")
    print(f"wrote {out_path} ({len(init)} chars)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
