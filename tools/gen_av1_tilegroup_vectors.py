#!/usr/bin/env python3
"""Generate minimal AV1 OBU streams for testing m3b tile-group boundary parsing.

These vectors are *not* intended to be fully decodable AV1 bitstreams.
They are crafted to be parseable by our current m3b implementation:
- `parse_seq_hdr_min()` (reduced still-picture sequence header subset)
- `parse_uncompressed_header_reduced_still()` up through `tile_info()`
- `parse_tile_group_obu_and_print()` for OBU_TILE_GROUP payload boundary discovery

Outputs:
- testFiles/generated/av1/*.av1

No external dependencies (stdlib only).
"""

from __future__ import annotations

import os
from dataclasses import dataclass

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_DIR = os.path.join(ROOT, "testFiles", "generated", "av1")


class BitWriter:
    def __init__(self) -> None:
        self._buf = bytearray()
        self._cur = 0
        self._n = 0  # bits in _cur

    def write_bit(self, b: int) -> None:
        b &= 1
        self._cur = (self._cur << 1) | b
        self._n += 1
        if self._n == 8:
            self._buf.append(self._cur & 0xFF)
            self._cur = 0
            self._n = 0

    def write_bits(self, n: int, v: int) -> None:
        if n < 0:
            raise ValueError("n<0")
        for i in range(n - 1, -1, -1):
            self.write_bit((v >> i) & 1)

    def align_to_byte_zero(self) -> None:
        while self._n != 0:
            self.write_bit(0)

    def finish(self) -> bytes:
        self.align_to_byte_zero()
        return bytes(self._buf)


def leb128_u64(n: int) -> bytes:
    if n < 0:
        raise ValueError("negative")
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        if n:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            break
    return bytes(out)


def obu(type_: int, payload: bytes) -> bytes:
    # obu_header(): forbidden=0, type=type_, extension_flag=0, has_size_field=1, reserved=0
    header = ((type_ & 0x0F) << 3) | (1 << 1)
    return bytes([header]) + leb128_u64(len(payload)) + payload


def bits_for_max_minus_1(max_minus_1: int) -> int:
    # Returns n such that max_minus_1 fits in n bits.
    if max_minus_1 < 0:
        raise ValueError("negative")
    n = 0
    v = max_minus_1
    while v:
        n += 1
        v >>= 1
    return max(1, n)


def make_seq_hdr_reduced_still(max_w: int, max_h: int, use_128: int = 0) -> bytes:
    if max_w <= 0 or max_h <= 0:
        raise ValueError("invalid dims")

    bw = BitWriter()
    seq_profile = 0
    still_picture = 1
    reduced_still_picture_header = 1

    bw.write_bits(3, seq_profile)
    bw.write_bit(still_picture)
    bw.write_bit(reduced_still_picture_header)

    # seq_level_idx[0]
    bw.write_bits(5, 0)

    # frame_width_bits_minus_1 / frame_height_bits_minus_1
    mw_minus_1 = max_w - 1
    mh_minus_1 = max_h - 1
    wbits = bits_for_max_minus_1(mw_minus_1)
    hbits = bits_for_max_minus_1(mh_minus_1)
    if wbits > 16 or hbits > 16:
        raise ValueError("dims too large for these vectors")

    bw.write_bits(4, wbits - 1)
    bw.write_bits(4, hbits - 1)
    bw.write_bits(wbits, mw_minus_1)
    bw.write_bits(hbits, mh_minus_1)

    # use_128x128_superblock
    bw.write_bit(1 if use_128 else 0)
    # enable_filter_intra, enable_intra_edge_filter
    bw.write_bit(0)
    bw.write_bit(0)

    # reduced still => enable_order_hint etc are forced (no bits)

    # enable_superres, enable_cdef, enable_restoration
    bw.write_bit(0)
    bw.write_bit(0)
    bw.write_bit(0)

    # color_config() (minimal, seq_profile=0)
    # high_bitdepth
    bw.write_bit(0)
    # mono_chrome (present when seq_profile != 1)
    bw.write_bit(0)
    # color_description_present_flag
    bw.write_bit(0)
    # color_range
    bw.write_bit(0)
    # For seq_profile=0, subsampling is fixed to 4:2:0 => chroma_sample_position is present.
    bw.write_bits(2, 0)
    # separate_uv_delta_q
    bw.write_bit(0)

    # film_grain_params_present
    bw.write_bit(0)

    return bw.finish()


def make_frame_hdr_reduced_still(max_w: int, max_h: int, *, tile_cols_log2: int, tile_rows_log2: int) -> bytes:
    """Creates an OBU_FRAME_HEADER payload parseable by our reduced-still parser.

    For our m3b subset, we rely on:
    - seq max_w/max_h for frame_size
    - enable_superres=0
    - render_and_frame_size_different=0
    - uniform_tile_spacing_flag=1
    """

    bw = BitWriter()

    # disable_cdf_update
    bw.write_bit(1)

    # allow_screen_content_tools (seq_force_screen_content_tools == SELECT => bit present)
    bw.write_bit(0)

    # frame_size_override_flag absent for reduced still.
    # order_hint absent (enable_order_hint=0)
    # decoder_model_info_present_flag=0 => nothing

    # render_and_frame_size_different
    bw.write_bit(0)

    # tile_info(): uniform_tile_spacing_flag
    bw.write_bit(1)

    # increment_tile_cols_log2 loop: write 1 for each increment to reach desired log2, then stop with 0 if needed.
    for i in range(tile_cols_log2):
        bw.write_bit(1)
    # If the loop is entered but we don't want further increments, we must terminate with a 0.
    # For our chosen dimensions, maxLog2TileCols == tile_cols_log2, so no explicit terminator bit is needed.

    for i in range(tile_rows_log2):
        bw.write_bit(1)

    if tile_cols_log2 > 0 or tile_rows_log2 > 0:
        tile_bits = tile_cols_log2 + tile_rows_log2
        # context_update_tile_id
        bw.write_bits(tile_bits, 0)
        # tile_size_bytes_minus_1 => 0 => TileSizeBytes=1
        bw.write_bits(2, 0)

    return bw.finish()


@dataclass(frozen=True)
class TileGroupSpec:
    present_flag: int
    tg_start: int
    tg_end: int
    tile_size_bytes: int
    # For non-last tiles, explicit sizes; last tile consumes remaining bytes.
    non_last_sizes: list[int]
    payload_bytes: bytes


def make_tile_group_payload(*, num_tiles: int, tile_cols_log2: int, tile_rows_log2: int, spec: TileGroupSpec) -> bytes:
    bw = BitWriter()

    if num_tiles <= 0:
        raise ValueError("num_tiles")

    # tile_start_and_end_present_flag only present when NumTiles > 1
    if num_tiles > 1:
        bw.write_bit(1 if spec.present_flag else 0)

    if num_tiles == 1 or not spec.present_flag:
        tg_start = 0
        tg_end = num_tiles - 1
    else:
        tile_bits = tile_cols_log2 + tile_rows_log2
        bw.write_bits(tile_bits, spec.tg_start)
        bw.write_bits(tile_bits, spec.tg_end)
        tg_start = spec.tg_start
        tg_end = spec.tg_end

    bw.align_to_byte_zero()
    header = bw.finish()

    if tg_start > tg_end:
        raise ValueError("tg range")

    # Build the byte payload after the header.
    body = bytearray()

    tiles_in_group = tg_end - tg_start + 1
    if tiles_in_group == 1:
        # single tile consumes all remaining payload bytes
        body.extend(spec.payload_bytes)
        return header + bytes(body)

    if spec.tile_size_bytes < 1 or spec.tile_size_bytes > 4:
        raise ValueError("tile_size_bytes")

    # We expect exactly (tiles_in_group - 1) explicit sizes.
    if len(spec.non_last_sizes) != (tiles_in_group - 1):
        raise ValueError("non_last_sizes length")

    # For each tile except last: write tile_size_minus_1 (le(TileSizeBytes)), then tile payload bytes.
    off = 0
    for tile_idx in range(tiles_in_group):
        last = tile_idx == (tiles_in_group - 1)
        if last:
            body.extend(spec.payload_bytes[off:])
            break

        size = spec.non_last_sizes[tile_idx]
        if size <= 0:
            raise ValueError("size <= 0")
        minus_1 = size - 1
        for i in range(spec.tile_size_bytes):
            body.append((minus_1 >> (8 * i)) & 0xFF)

        body.extend(spec.payload_bytes[off : off + size])
        off += size

    return header + bytes(body)


def write_file(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)

    def tile_trailing_only(size_bytes: int) -> bytes:
        if size_bytes <= 0:
            raise ValueError("size_bytes")
        return bytes([0x80]) + bytes([0x00] * (size_bytes - 1))

    def _floor_log2_u32(n: int) -> int:
        r = 0
        while n >= 2:
            n >>= 1
            r += 1
        return r

    def _get_bit(data: bytes, bitpos: int) -> int:
        byte = data[bitpos >> 3]
        shift = 7 - (bitpos & 7)
        return (byte >> shift) & 1

    def _read_bits(data: bytes, bitpos: int, n: int) -> tuple[int, int] | None:
        if n == 0:
            return 0, bitpos
        if bitpos + n > len(data) * 8:
            return None
        v = 0
        for _ in range(n):
            v = (v << 1) | _get_bit(data, bitpos)
            bitpos += 1
        return v, bitpos

    # Minimal Python model of our C symbol decoder, sufficient to brute-force small payloads.
    def _init_symbol(data: bytes) -> dict:
        size_bytes = len(data)
        num_bits = min(size_bytes * 8, 15)
        rb = _read_bits(data, 0, num_bits)
        if rb is None:
            raise RuntimeError("truncated")
        buf, bitpos = rb
        padded = buf << (15 - num_bits)
        return {
            "data": data,
            "size": size_bytes,
            "bitpos": bitpos,
            "symbol_value": ((1 << 15) - 1) ^ padded,
            "symbol_range": 1 << 15,
            "symbol_max_bits": (size_bytes * 8) - 15,
        }

    def _read_symbol_2sym(sd: dict) -> int | None:
        # Equivalent to av1_symbol_read_bool() calling read_symbol() with cdf[0]=1<<14, cdf[1]=1<<15.
        n = 2
        cdf0 = 1 << 14
        cur = sd["symbol_range"]
        prev = 0
        symbol = -1
        while True:
            symbol += 1
            if symbol >= n:
                return None
            prev = cur
            f = (1 << 15) - (cdf0 if symbol == 0 else (1 << 15))
            t = ((sd["symbol_range"] >> 8) * (f >> 6)) >> (7 - 6)
            t += 4 * (n - symbol - 1)
            cur = t
            if sd["symbol_value"] >= cur:
                break

        sd["symbol_range"] = prev - cur
        sd["symbol_value"] -= cur

        if sd["symbol_range"] == 0:
            return None
        bits = 15 - _floor_log2_u32(sd["symbol_range"])
        sd["symbol_range"] <<= bits
        max_readable = max(sd["symbol_max_bits"], 0)
        num_bits = min(bits, max_readable)
        new_data = 0
        if num_bits:
            rb = _read_bits(sd["data"], sd["bitpos"], num_bits)
            if rb is None:
                return None
            new_data, sd["bitpos"] = rb
        padded = new_data << (bits - num_bits)
        sd["symbol_value"] = padded ^ (((sd["symbol_value"] + 1) << bits) - 1)
        sd["symbol_max_bits"] -= bits
        return symbol

    def _exit_symbol_ok(sd: dict) -> bool:
        smb = sd["symbol_max_bits"]
        if smb < -14:
            return False
        smb_plus_15 = smb + 15
        minv = 15
        if smb_plus_15 < 15:
            minv = 0 if smb_plus_15 < 0 else smb_plus_15
        if sd["bitpos"] < minv:
            return False
        trailing_pos = sd["bitpos"] - minv
        if smb > 0:
            sd["bitpos"] += smb
        if (sd["bitpos"] & 7) != 0:
            return False
        if sd["bitpos"] > len(sd["data"]) * 8:
            return False

        # trailing bit must be 1
        if _get_bit(sd["data"], trailing_pos) != 1:
            return False
        # all bits after until paddingEndPosition must be 0
        for pos in range(trailing_pos + 1, sd["bitpos"]):
            if _get_bit(sd["data"], pos) != 0:
                return False
        return True

    def find_payload_exit_after_n_bools(n_bools: int) -> bytes:
        if n_bools <= 0:
            raise ValueError("n_bools")

        # Deterministic brute-force over 16-bit space.
        for x in range(0x10000):
            data = bytes([(x >> 8) & 0xFF, x & 0xFF])
            sd = _init_symbol(data)
            ok = True
            for _ in range(n_bools):
                sym = _read_symbol_2sym(sd)
                if sym is None:
                    ok = False
                    break
            if not ok:
                continue
            if _exit_symbol_ok(sd):
                return data
        raise RuntimeError(f"failed to find 2-byte payload that exits after {n_bools} bool(s)")

    # Vector 1: 1 tile (64x64), single-tile group.
    seq1 = make_seq_hdr_reduced_still(64, 64, use_128=0)
    fh1 = make_frame_hdr_reduced_still(64, 64, tile_cols_log2=0, tile_rows_log2=0)
    tg1_payload = bytes([0x11, 0x22, 0x33, 0x44])
    tg1 = make_tile_group_payload(
        num_tiles=1,
        tile_cols_log2=0,
        tile_rows_log2=0,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=0,
            tile_size_bytes=0,
            non_last_sizes=[],
            payload_bytes=tg1_payload,
        ),
    )
    stream1 = obu(1, seq1) + obu(3, fh1) + obu(4, tg1)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_1tile.av1"), stream1)

    # Vector 1b: 1 tile where the tile payload is *only* valid trailing bits.
    # This should satisfy init/exit_symbol without decoding any syntax.
    tg1b_payload = tile_trailing_only(1)
    tg1b = make_tile_group_payload(
        num_tiles=1,
        tile_cols_log2=0,
        tile_rows_log2=0,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=0,
            tile_size_bytes=0,
            non_last_sizes=[],
            payload_bytes=tg1b_payload,
        ),
    )
    stream1b = obu(1, seq1) + obu(3, fh1) + obu(4, tg1b)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_1tile_trailingonly.av1"), stream1b)

    # Vector 1c: 1 tile where we can decode 1 bool symbol and still satisfy exit_symbol().
    payload_1c = find_payload_exit_after_n_bools(1)
    tg1c = make_tile_group_payload(
        num_tiles=1,
        tile_cols_log2=0,
        tile_rows_log2=0,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=0,
            tile_size_bytes=0,
            non_last_sizes=[],
            payload_bytes=payload_1c,
        ),
    )
    stream1c = obu(1, seq1) + obu(3, fh1) + obu(4, tg1c)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_1tile_exit1bool.av1"), stream1c)

    # Vector 1d: 1 tile where we can decode 8 bool symbols and still satisfy exit_symbol().
    payload_1d = find_payload_exit_after_n_bools(8)
    tg1d = make_tile_group_payload(
        num_tiles=1,
        tile_cols_log2=0,
        tile_rows_log2=0,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=0,
            tile_size_bytes=0,
            non_last_sizes=[],
            payload_bytes=payload_1d,
        ),
    )
    stream1d = obu(1, seq1) + obu(3, fh1) + obu(4, tg1d)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_1tile_exit8bool.av1"), stream1d)

    # Vector 2: 2x2 tiles (128x128), tile_start_and_end_present_flag = 0.
    seq2 = make_seq_hdr_reduced_still(128, 128, use_128=0)
    fh2 = make_frame_hdr_reduced_still(128, 128, tile_cols_log2=1, tile_rows_log2=1)
    # tile sizes: 3,2,1, last=4
    payload2 = bytes([0xAA, 0xAB, 0xAC, 0xBA, 0xBB, 0xCA, 0xDA, 0xDB, 0xDC, 0xDD])
    tg2 = make_tile_group_payload(
        num_tiles=4,
        tile_cols_log2=1,
        tile_rows_log2=1,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=3,
            tile_size_bytes=1,
            non_last_sizes=[3, 2, 1],
            payload_bytes=payload2,
        ),
    )
    stream2 = obu(1, seq2) + obu(3, fh2) + obu(4, tg2)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_2x2_alltiles_flag0.av1"), stream2)

    # Vector 2b: 2x2 tiles, each tile payload is trailing-only.
    # We vary per-tile sizes to exercise size accounting:
    # tile[0]=1B, tile[1]=2B, tile[2]=3B, tile[3]=1B (last consumes remainder).
    payload2b = tile_trailing_only(1) + tile_trailing_only(2) + tile_trailing_only(3) + tile_trailing_only(1)
    tg2b = make_tile_group_payload(
        num_tiles=4,
        tile_cols_log2=1,
        tile_rows_log2=1,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=3,
            tile_size_bytes=1,
            non_last_sizes=[1, 2, 3],
            payload_bytes=payload2b,
        ),
    )
    stream2b = obu(1, seq2) + obu(3, fh2) + obu(4, tg2b)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_2x2_alltiles_flag0_trailingonly.av1"), stream2b)

    # Vector 2c: 2x2 tiles, each tile payload supports decoding 8 bools then exit_symbol().
    # Use identical 2-byte payloads for all tiles.
    payload2c_tile = payload_1d
    payload2c = payload2c_tile * 4
    tg2c = make_tile_group_payload(
        num_tiles=4,
        tile_cols_log2=1,
        tile_rows_log2=1,
        spec=TileGroupSpec(
            present_flag=0,
            tg_start=0,
            tg_end=3,
            tile_size_bytes=1,
            non_last_sizes=[2, 2, 2],
            payload_bytes=payload2c,
        ),
    )
    stream2c = obu(1, seq2) + obu(3, fh2) + obu(4, tg2c)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_2x2_alltiles_flag0_exit8bool.av1"), stream2c)

    # Vector 3: 2x2 tiles (128x128), tile_start_and_end_present_flag = 1, subset tiles [1..2].
    payload3 = bytes([0x21, 0x22, 0x31, 0x32, 0x33, 0x34, 0x35])  # 2 bytes for tile[1], 5 bytes for tile[2]
    tg3 = make_tile_group_payload(
        num_tiles=4,
        tile_cols_log2=1,
        tile_rows_log2=1,
        spec=TileGroupSpec(
            present_flag=1,
            tg_start=1,
            tg_end=2,
            tile_size_bytes=1,
            non_last_sizes=[2],
            payload_bytes=payload3,
        ),
    )
    stream3 = obu(1, seq2) + obu(3, fh2) + obu(4, tg3)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_2x2_subset_flag1.av1"), stream3)

    # Vector 3b: subset tiles [1..2], trailing-only payloads.
    # tile[1]=2B explicit, tile[2]=4B last consumes remainder.
    payload3b = tile_trailing_only(2) + tile_trailing_only(4)
    tg3b = make_tile_group_payload(
        num_tiles=4,
        tile_cols_log2=1,
        tile_rows_log2=1,
        spec=TileGroupSpec(
            present_flag=1,
            tg_start=1,
            tg_end=2,
            tile_size_bytes=1,
            non_last_sizes=[2],
            payload_bytes=payload3b,
        ),
    )
    stream3b = obu(1, seq2) + obu(3, fh2) + obu(4, tg3b)
    write_file(os.path.join(OUT_DIR, "m3b_tilegroup_2x2_subset_flag1_trailingonly.av1"), stream3b)

    print(f"wrote AV1 tile-group vectors to {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
