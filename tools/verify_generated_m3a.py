#!/usr/bin/env python3
"""Oracle-driven verification for m3a on the generated vectors.

Checks (per generated .avif):
- m2 extraction succeeds (avif_extract_av1)
- m3a parses extracted AV1 sample (av1_parse)
- Parsed AV1 bitstream fields that *should* match the oracle do match:
  - Bit depth
  - Chroma format (YUV420/422/444 or monochrome)

Notes:
- We intentionally do NOT compare CICP/range here because AVIF often signals
  these via container properties (colr/nclx) rather than via AV1 bitstream
  defaults (and av1_parse currently parses only the AV1 Sequence Header).

Exit code:
- 0 on success
- 1 on any mismatch or tool failure
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


RE_INFO_BITDEPTH = re.compile(r"^\s*\*\s*Bit Depth\s*:\s*(\d+)\s*$")
RE_INFO_FORMAT = re.compile(r"^\s*\*\s*Format\s*:\s*([A-Za-z0-9]+)\s*$")

RE_M3A_BITDEPTH = re.compile(r"^\s*bit_depth=(\d+)\s*$")
RE_M3A_MONO = re.compile(r"^\s*monochrome=(\d+)\s*$")
RE_M3A_SX = re.compile(r"^\s*subsampling_x=(\d+)\s*$")
RE_M3A_SY = re.compile(r"^\s*subsampling_y=(\d+)\s*$")


def run_capture(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def require_executable(name: str) -> None:
    r, _ = run_capture(["/usr/bin/env", name, "--help"])
    if r != 0:
        raise RuntimeError(f"required tool not found or not runnable: {name}")


def parse_avifdec_info(output: str) -> tuple[int, str]:
    bit_depth: int | None = None
    fmt: str | None = None

    for line in output.splitlines():
        m = RE_INFO_BITDEPTH.match(line)
        if m:
            bit_depth = int(m.group(1))
            continue
        m = RE_INFO_FORMAT.match(line)
        if m:
            fmt = m.group(1)
            continue

    if bit_depth is None or fmt is None:
        raise RuntimeError("failed to parse avifdec --info output")

    # Normalize formats seen in the wild.
    fmt = fmt.upper()
    if fmt == "YUV400":
        fmt = "YUV400"
    elif fmt in ("YUV420", "YUV422", "YUV444"):
        pass
    else:
        # Keep the raw token for reporting.
        pass

    return bit_depth, fmt


def parse_m3a_summary(output: str) -> tuple[int, str]:
    bit_depth: int | None = None
    monochrome: int | None = None
    sx: int | None = None
    sy: int | None = None

    for line in output.splitlines():
        m = RE_M3A_BITDEPTH.match(line)
        if m:
            bit_depth = int(m.group(1))
            continue
        m = RE_M3A_MONO.match(line)
        if m:
            monochrome = int(m.group(1))
            continue
        m = RE_M3A_SX.match(line)
        if m:
            sx = int(m.group(1))
            continue
        m = RE_M3A_SY.match(line)
        if m:
            sy = int(m.group(1))
            continue

    if bit_depth is None or monochrome is None or sx is None or sy is None:
        raise RuntimeError("failed to parse av1_parse output (missing fields)")

    if monochrome:
        fmt = "YUV400"
    else:
        if (sx, sy) == (0, 0):
            fmt = "YUV444"
        elif (sx, sy) == (1, 0):
            fmt = "YUV422"
        elif (sx, sy) == (1, 1):
            fmt = "YUV420"
        else:
            fmt = f"YUV(unsupported subsampling {sx},{sy})"

    return bit_depth, fmt


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    gen_avif_dir = root / "testFiles" / "generated" / "avif"

    if not gen_avif_dir.is_dir():
        print(f"missing generated AVIF dir: {gen_avif_dir}", file=sys.stderr)
        return 1

    # External oracle tooling.
    try:
        require_executable("avifdec")
    except Exception as e:
        print(str(e), file=sys.stderr)
        return 1

    # Built tools.
    extract = root / "build" / "avif_extract_av1"
    parse = root / "build" / "av1_parse"
    if not extract.exists() or not parse.exists():
        print("missing build outputs; run `make all`", file=sys.stderr)
        return 1

    files = sorted(gen_avif_dir.glob("*.avif"))
    if not files:
        print(f"no generated AVIFs found in {gen_avif_dir}", file=sys.stderr)
        return 1

    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="avif-decoder-m3a-") as td:
        tmp_av1 = os.path.join(td, "out.av1")

        for avif in files:
            # Oracle: avifdec --info
            rc, info_out = run_capture(["avifdec", "--info", str(avif)])
            if rc != 0:
                failures.append(f"{avif}: avifdec --info failed")
                continue

            try:
                oracle_depth, oracle_fmt = parse_avifdec_info(info_out)
            except Exception as e:
                failures.append(f"{avif}: {e}")
                continue

            # m2 extract
            rc, out = run_capture([str(extract), str(avif), tmp_av1])
            if rc != 0:
                msg = (out.strip().splitlines() or ["(no output)"])[0]
                failures.append(f"{avif}: m2 extract failed: {msg}")
                continue

            # m3a parse
            rc, m3a_out = run_capture([str(parse), tmp_av1])
            if rc != 0:
                msg = (m3a_out.strip().splitlines() or ["(no output)"])[0]
                failures.append(f"{avif}: m3a parse failed: {msg}")
                continue

            try:
                m3a_depth, m3a_fmt = parse_m3a_summary(m3a_out)
            except Exception as e:
                failures.append(f"{avif}: {e}")
                continue

            if m3a_depth != oracle_depth:
                failures.append(
                    f"{avif}: bit depth mismatch (m3a={m3a_depth}, avifdec={oracle_depth})"
                )

            # Oracle uses YUV444/YUV422/YUV420 and sometimes YUV400.
            # Our m3a only supports those formats.
            if m3a_fmt != oracle_fmt:
                failures.append(
                    f"{avif}: format mismatch (m3a={m3a_fmt}, avifdec={oracle_fmt})"
                )

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
