#!/usr/bin/env python3
"""Sweep m2+m3a over the third-party corpus.

Runs:
- m2 extractor (./build/avif_extract_av1) to /tmp/out.av1
- m3a parser (./build/av1_parse) on the extracted sample

Goal: robustness + quick visibility into unsupported AV1 features.
"""

from __future__ import annotations

import subprocess
from collections import Counter
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    test_root = root / "testFiles"
    generated = test_root / "generated"

    files = [p for p in test_root.rglob("*.avif") if not str(p).startswith(str(generated))]
    files.sort()

    extract_ok = 0
    extract_fail = 0
    parse_ok = 0
    parse_fail: list[tuple[str, str]] = []

    for avif in files:
        r = subprocess.run(
            [str(root / "build" / "avif_extract_av1"), str(avif), "/tmp/out.av1"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        if r.returncode != 0:
            extract_fail += 1
            continue
        extract_ok += 1

        r2 = subprocess.run(
            [str(root / "build" / "av1_parse"), "/tmp/out.av1"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        if r2.returncode == 0:
            parse_ok += 1
        else:
            stderr_text = (r2.stderr or b"").decode("utf-8", errors="replace")
            msg = ((stderr_text.strip().splitlines()) or ["(no stderr)"])[0]
            parse_fail.append((str(avif), msg))

    print("third-party AVIF count:", len(files))
    print("m2 extract ok:", extract_ok)
    print("m2 extract fail:", extract_fail)
    print("m3a parse ok:", parse_ok)
    print("m3a parse fail:", len(parse_fail))

    if parse_fail:
        c = Counter(m for _, m in parse_fail)
        print("\nTop m3a failure reasons:")
        for reason, n in c.most_common(10):
            print(f"  {n:3d}  {reason}")

        print("\nSample failures:")
        for p, m in parse_fail[:10]:
            print("  ", p)
            print("     ", m)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
