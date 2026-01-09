#!/usr/bin/env python3
"""Verify m3b.3: tile groups embedded in OBU_FRAME.

This runs the existing generated AVIF corpus through:
- m2 extractor (./build/avif_extract_av1)
- m3b frame header tool (./build/av1_framehdr)

and asserts that av1_framehdr successfully locates and prints a tile group when
it is embedded in an OBU_FRAME.

This is intentionally strict on exit codes but tolerant on the exact sizes,
since those depend on the encoder.
"""

from __future__ import annotations

import subprocess
from pathlib import Path


def _run(cmd: list[str]) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    avifs = sorted((root / "testFiles" / "generated" / "avif").glob("*.avif"))
    if not avifs:
        raise SystemExit("no generated AVIFs found")

    extractor = root / "build" / "avif_extract_av1"
    framehdr = root / "build" / "av1_framehdr"

    failures: list[str] = []
    checked = 0

    out_av1 = Path("/tmp/out.av1")

    for avif in avifs:
        r1 = _run([str(extractor), str(avif), str(out_av1)])
        if r1.returncode != 0:
            failures.append(f"extract failed: {avif}\n{r1.stderr.decode('utf-8', errors='replace')}")
            continue

        r2 = _run([str(framehdr), str(out_av1)])
        checked += 1
        if r2.returncode != 0:
            failures.append(
                f"av1_framehdr failed: {avif}\n{r2.stderr.decode('utf-8', errors='replace')}"
            )
            continue

        out = (r2.stdout or b"").decode("utf-8", errors="replace")
        if "tile_group:" not in out:
            failures.append(f"missing tile_group output: {avif}")
            continue

        # The primary goal of this verifier: embedded case should no longer be reported as unsupported.
        if "unsupported when tiles are embedded in OBU_FRAME" in out:
            failures.append(f"still reports embedded case unsupported: {avif}")
            continue

    print("m3b embedded tile-group check")
    print("  generated avifs:", len(avifs))
    print("  checked:", checked)
    print("  failures:", len(failures))

    if failures:
        for msg in failures[:10]:
            print("\nFAIL:\n" + msg)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
