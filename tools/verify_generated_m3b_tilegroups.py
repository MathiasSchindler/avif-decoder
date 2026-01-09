#!/usr/bin/env python3
"""Verify m3b tile_info + OBU_TILE_GROUP boundary parsing on synthetic AV1 vectors.

This is the "b)" approach: generate AV1 streams that already contain
OBU_FRAME_HEADER + OBU_TILE_GROUP, so we can test tile-group boundary discovery
without needing to split tiles out of OBU_FRAME.

Exit code:
- 0 on success
- 1 on mismatch or tool failure
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

RE_TILE_COLS_ROWS = re.compile(r"^\s*tile_cols=(\d+)\s+tile_rows=(\d+)\s*$")
RE_TILE_SIZE_BYTES = re.compile(r"^\s*tile_size_bytes=(\d+)\s*$")
RE_TILE_LINE = re.compile(r"^\s*tile\[(\d+)\].*\bsize=(\d+)\s*$")
RE_TG_LINE = re.compile(r"^\s*tile_group:\s+tg_start=(\d+)\s+tg_end=(\d+).*$")


def run_capture(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def parse_tile_info(output: str) -> tuple[int, int, int]:
    cols = rows = None
    tsb = None
    for line in output.splitlines():
        m = RE_TILE_COLS_ROWS.match(line)
        if m:
            cols, rows = int(m.group(1)), int(m.group(2))
        m = RE_TILE_SIZE_BYTES.match(line)
        if m:
            tsb = int(m.group(1))
    if cols is None or rows is None or tsb is None:
        raise RuntimeError("failed to parse tile info")
    return cols, rows, tsb


def parse_tile_group_tiles(output: str) -> tuple[tuple[int, int] | None, list[tuple[int, int]]]:
    tg = None
    tiles: list[tuple[int, int]] = []
    for line in output.splitlines():
        m = RE_TG_LINE.match(line)
        if m:
            tg = (int(m.group(1)), int(m.group(2)))
        m = RE_TILE_LINE.match(line)
        if m:
            tiles.append((int(m.group(1)), int(m.group(2))))
    return tg, tiles


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    av1_dir = root / "testFiles" / "generated" / "av1"

    framehdr = root / "build" / "av1_framehdr"
    if not framehdr.exists():
        print("missing build output; run `make build-m3b`", file=sys.stderr)
        return 1

    files = sorted(av1_dir.glob("*.av1"))
    if not files:
        print(f"no generated .av1 files found in {av1_dir} (run tools/gen_av1_tilegroup_vectors.py)", file=sys.stderr)
        return 1

    expected = {
        "m3b_tilegroup_1tile.av1": {
            "tile_info": (1, 1, 0),
            "tg": (0, 0),
            "tiles": [(0, 4)],
        },
        "m3b_tilegroup_1tile_trailingonly.av1": {
            "tile_info": (1, 1, 0),
            "tg": (0, 0),
            "tiles": [(0, 1)],
        },
        "m3b_tilegroup_1tile_exit1bool.av1": {
            "tile_info": (1, 1, 0),
            "tg": (0, 0),
            "tiles": [(0, 2)],
        },
        "m3b_tilegroup_1tile_exit8bool.av1": {
            "tile_info": (1, 1, 0),
            "tg": (0, 0),
            "tiles": [(0, 2)],
        },
        "m3b_tilegroup_2x2_alltiles_flag0.av1": {
            "tile_info": (2, 2, 1),
            "tg": (0, 3),
            "tiles": [(0, 3), (1, 2), (2, 1), (3, 4)],
        },
        "m3b_tilegroup_2x2_alltiles_flag0_trailingonly.av1": {
            "tile_info": (2, 2, 1),
            "tg": (0, 3),
            "tiles": [(0, 1), (1, 2), (2, 3), (3, 1)],
        },
        "m3b_tilegroup_2x2_alltiles_flag0_exit8bool.av1": {
            "tile_info": (2, 2, 1),
            "tg": (0, 3),
            "tiles": [(0, 2), (1, 2), (2, 2), (3, 2)],
        },
        "m3b_tilegroup_2x2_subset_flag1.av1": {
            "tile_info": (2, 2, 1),
            "tg": (1, 2),
            "tiles": [(1, 2), (2, 5)],
        },
        "m3b_tilegroup_2x2_subset_flag1_trailingonly.av1": {
            "tile_info": (2, 2, 1),
            "tg": (1, 2),
            "tiles": [(1, 2), (2, 4)],
        },
    }

    failures: list[str] = []

    for f in files:
        name = f.name
        if name not in expected:
            failures.append(f"{name}: unexpected file (no expectations defined)")
            continue

        rc, out = run_capture([str(framehdr), str(f)])
        if rc != 0:
            msg = (out.strip().splitlines() or ["(no output)"])[0]
            failures.append(f"{name}: av1_framehdr failed: {msg}")
            continue

        try:
            cols, rows, tsb = parse_tile_info(out)
        except Exception as e:
            failures.append(f"{name}: {e}")
            continue

        exp_cols, exp_rows, exp_tsb = expected[name]["tile_info"]
        if (cols, rows, tsb) != (exp_cols, exp_rows, exp_tsb):
            failures.append(f"{name}: tile_info mismatch got={(cols, rows, tsb)} want={(exp_cols, exp_rows, exp_tsb)}")
            continue

        tg, tiles = parse_tile_group_tiles(out)
        if tg != expected[name]["tg"]:
            failures.append(f"{name}: tg_start/tg_end mismatch got={tg} want={expected[name]['tg']}")
            continue

        if tiles != expected[name]["tiles"]:
            failures.append(f"{name}: tile list mismatch got={tiles} want={expected[name]['tiles']}")
            continue

    total = len(files)
    ok = total - len(failures)

    print(f"tile-group vectors checked: {total}")
    print(f"passed: {ok}")
    print(f"failures: {len(failures)}")

    if failures:
        print("\nFailures:")
        for line in failures[:50]:
            print("-", line)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
