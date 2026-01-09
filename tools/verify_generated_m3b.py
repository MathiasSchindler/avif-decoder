#!/usr/bin/env python3
"""Oracle-driven sanity check for m3b-step1 on generated vectors.

Checks (per generated .avif):
- m2 extraction succeeds
- m3b-step1 parser succeeds on extracted sample
- Coded dimensions match `avifdec --info` Resolution

Exit code:
- 0 on success
- 1 on mismatch or tool failure
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RE_INFO_RES = re.compile(r"^\s*\*\s*Resolution\s*:\s*(\d+)x(\d+)\s*$")
RE_M3B_W = re.compile(r"^\s*frame_width=(\d+)\s*$")
RE_M3B_UW = re.compile(r"^\s*upscaled_width=(\d+)\s*$")
RE_M3B_H = re.compile(r"^\s*frame_height=(\d+)\s*$")


def run_capture(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def parse_avifdec_res(output: str) -> tuple[int, int]:
    for line in output.splitlines():
        m = RE_INFO_RES.match(line)
        if m:
            return int(m.group(1)), int(m.group(2))
    raise RuntimeError("failed to parse avifdec --info Resolution")


def parse_m3b_dims(output: str) -> tuple[int, int]:
    w = None
    uw = None
    h = None
    for line in output.splitlines():
        m = RE_M3B_W.match(line)
        if m:
            w = int(m.group(1))
        m = RE_M3B_UW.match(line)
        if m:
            uw = int(m.group(1))
        m = RE_M3B_H.match(line)
        if m:
            h = int(m.group(1))
    if (w is None and uw is None) or h is None:
        raise RuntimeError("failed to parse av1_framehdr output")
    return (uw if uw is not None else w), h


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    gen_avif_dir = root / "testFiles" / "generated" / "avif"

    files = sorted(gen_avif_dir.glob("*.avif"))
    if not files:
        print(f"no generated AVIFs found in {gen_avif_dir}", file=sys.stderr)
        return 1

    extract = root / "build" / "avif_extract_av1"
    framehdr = root / "build" / "av1_framehdr"
    if not extract.exists() or not framehdr.exists():
        print("missing build outputs; run `make all`", file=sys.stderr)
        return 1

    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="avif-decoder-m3b-") as td:
        tmp_av1 = os.path.join(td, "out.av1")

        for avif in files:
            rc, info_out = run_capture(["avifdec", "--info", str(avif)])
            if rc != 0:
                failures.append(f"{avif}: avifdec --info failed")
                continue
            try:
                ow, oh = parse_avifdec_res(info_out)
            except Exception as e:
                failures.append(f"{avif}: {e}")
                continue

            rc, out = run_capture([str(extract), str(avif), tmp_av1])
            if rc != 0:
                msg = (out.strip().splitlines() or ["(no output)"])[0]
                failures.append(f"{avif}: m2 extract failed: {msg}")
                continue

            rc, m3b_out = run_capture([str(framehdr), tmp_av1])
            if rc != 0:
                msg = (m3b_out.strip().splitlines() or ["(no output)"])[0]
                failures.append(f"{avif}: m3b-step1 failed: {msg}")
                continue

            try:
                w, h = parse_m3b_dims(m3b_out)
            except Exception as e:
                failures.append(f"{avif}: {e}")
                continue

            if (w, h) != (ow, oh):
                failures.append(f"{avif}: dimension mismatch (m3b={w}x{h}, avifdec={ow}x{oh})")

    total = len(files)
    ok = total - len({f.split(":", 1)[0] for f in failures})

    print(f"generated vectors checked: {total}")
    print(f"passed: {ok}")
    print(f"failures: {len(failures)}")

    if failures:
        print("\nFirst failures:")
        for line in failures[:20]:
            print("-", line)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
