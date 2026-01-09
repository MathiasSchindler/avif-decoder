#!/usr/bin/env python3
"""Spec-driven sanity check for m1 on generated vectors.

Checks (per generated .avif):
- m1 metadump runs
- If both `pixi` and `av1C` are present on the primary item:
  - `pixi_channels` matches channels derived from av1C (mono => 1 else 3)
  - all `pixi_depths` match bit depth derived from av1C (8/10/12)

This follows AVIF v1.2.0 §2.2.1: bit depth + number of channels derived from
`av1C` shall match `pixi` if present.

Exit code:
- 0 on success
- 1 on mismatch or tool failure
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

RE_PROP = re.compile(r"^\s*-\s+prop_index=(\d+)\s+type='(.{4})'\s+essential=(true|false)(.*)$")
RE_PIXI_CHANNELS = re.compile(r"\bpixi_channels=(\d+)\b")
RE_PIXI_DEPTHS = re.compile(r"\bpixi_depths=([0-9,]+)\b")
RE_AV1C = re.compile(
    r"\bav1C\(profile=(\d+)\s+level=(\d+)\s+tier=(\d+)\s+hb=(\d+)\s+tb=(\d+)\s+mono=(\d+)\s+subsamp=(\d)(\d)\)"
)


def run_capture(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def av1c_derived_bit_depth(hb: int, tb: int) -> int:
    # AV1CodecConfigurationRecord: hb=0 => 8bpc; hb=1 and tb=0 => 10bpc; hb=1 and tb=1 => 12bpc.
    if hb == 0:
        return 8
    return 12 if tb else 10


def av1c_derived_channels(mono: int) -> int:
    return 1 if mono else 3


def parse_primary_props(metadump_out: str) -> tuple[tuple[int, list[int]] | None, tuple[int, int] | None]:
    """Returns (pixi, av1c) where:

    pixi = (channels, depths)
    av1c = (bit_depth, channels)
    """

    in_primary = False
    pixi: tuple[int, list[int]] | None = None
    av1c: tuple[int, int] | None = None

    for line in metadump_out.splitlines():
        if line.strip() == "primary item properties:":
            in_primary = True
            continue
        if in_primary and line and not line.startswith("    -") and not line.startswith("      "):
            # End of primary item properties block (next item line).
            in_primary = False

        if not in_primary:
            continue

        m = RE_PROP.match(line)
        if not m:
            continue

        typ = m.group(2)
        tail = m.group(4) or ""

        if typ == "pixi":
            mc = RE_PIXI_CHANNELS.search(tail)
            md = RE_PIXI_DEPTHS.search(tail)
            if not mc or not md:
                # Old builds might not print depths; treat as missing data.
                continue
            channels = int(mc.group(1))
            depths = [int(x) for x in md.group(1).split(",") if x]
            pixi = (channels, depths)

        if typ == "av1C":
            ma = RE_AV1C.search(tail)
            if not ma:
                continue
            hb = int(ma.group(4))
            tb = int(ma.group(5))
            mono = int(ma.group(6))
            bit_depth = av1c_derived_bit_depth(hb, tb)
            channels = av1c_derived_channels(mono)
            av1c = (bit_depth, channels)

    return pixi, av1c


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    gen_avif_dir = root / "testFiles" / "generated" / "avif"

    files = sorted(gen_avif_dir.glob("*.avif"))
    if not files:
        print(f"no generated AVIFs found in {gen_avif_dir}", file=sys.stderr)
        return 1

    metadump = root / "build" / "avif_metadump"
    if not metadump.exists():
        print("missing build outputs; run `make all`", file=sys.stderr)
        return 1

    failures: list[str] = []

    for avif in files:
        rc, out = run_capture([str(metadump), str(avif)])
        if rc != 0:
            msg = (out.strip().splitlines() or ["(no output)"])[0]
            failures.append(f"{avif}: avif_metadump failed: {msg}")
            continue

        pixi, av1c = parse_primary_props(out)

        # Spec constraint applies only when pixi is present.
        if pixi is None:
            continue
        if av1c is None:
            failures.append(f"{avif}: pixi present but av1C not found on primary item")
            continue

        pixi_channels, pixi_depths = pixi
        av1c_bit_depth, av1c_channels = av1c

        if pixi_channels != av1c_channels:
            failures.append(
                f"{avif}: pixi_channels mismatch (pixi={pixi_channels}, av1C-derived={av1c_channels})"
            )
            continue

        if len(pixi_depths) < pixi_channels:
            failures.append(f"{avif}: pixi_depths missing entries (have {len(pixi_depths)}, need {pixi_channels})")
            continue

        for i in range(pixi_channels):
            if pixi_depths[i] != av1c_bit_depth:
                failures.append(
                    f"{avif}: pixi_depth[{i}] mismatch (pixi={pixi_depths[i]}, av1C-derived={av1c_bit_depth})"
                )
                break

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
