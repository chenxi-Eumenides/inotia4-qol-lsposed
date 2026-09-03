#!/usr/bin/env python3
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path
from typing import Any

import lzma

ROOT = Path(__file__).resolve().parents[2]
RAW_DIR = ROOT / "apk" / "static-data" / "raw"
GAME_RES = ROOT / "apk" / "decoded" / "assets" / "common" / "game_res"
OUT_DIR = ROOT / "apk" / "static-data" / "json" / "snasys"


def props_to_lclppb(prop: int) -> tuple[int, int, int] | None:
    if prop >= 225:
        return None
    pb = prop // 45
    rem = prop % 45
    lp = rem // 9
    lc = rem % 9
    return lc, lp, pb


def decode_raw_with_limit(comp: bytes, filters: list[dict[str, Any]], out_size: int) -> bytes:
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
    out = bytearray()
    data = comp
    while len(out) < out_size:
        chunk = dec.decompress(data, max_length=out_size - len(out))
        out.extend(chunk)
        data = b""
        if len(out) >= out_size:
            break
        if not chunk and dec.needs_input:
            break
    return bytes(out)


def decode_inner_segment(segment: bytes) -> bytes:
    if len(segment) < 15:
        return segment
    for prop_off, dict_off, size_off, payload_off in ((1, 2, 6, 14), (2, 3, 7, 15)):
        if len(segment) <= payload_off:
            continue
        props = props_to_lclppb(segment[prop_off])
        if props is None:
            continue
        lc, lp, pb = props
        dict_size = int.from_bytes(segment[dict_off:dict_off + 4], "little")
        out_size = int.from_bytes(segment[size_off:size_off + 4], "little")
        try:
            decoded = decode_raw_with_limit(
                segment[payload_off:],
                [{"id": lzma.FILTER_LZMA1, "dict_size": max(dict_size, 4096), "lc": lc, "lp": lp, "pb": pb}],
                out_size,
            )
        except lzma.LZMAError:
            continue
        if len(decoded) == out_size:
            return decoded
    return segment


def decode_snasys_entries(blob: bytes) -> list[bytes]:
    if len(blob) < 24:
        raise ValueError("blob too small")
    entry_count = struct.unpack_from("<I", blob, 0)[0]
    if not (0 < entry_count < 10000):
        raise ValueError(f"invalid entry count {entry_count}")
    table_base = -1
    raw_entries: list[int] = []
    for base in (18, 19, 20):
        if base + (entry_count + 1) * 3 > len(blob):
            continue
        values: list[int] = []
        previous = -1
        valid = True
        for index in range(entry_count + 1):
            value = int.from_bytes(blob[base + index * 3:base + index * 3 + 3], "little")
            masked = value & 0x7FFFFF
            if masked < previous or masked > len(blob):
                valid = False
                break
            values.append(value)
            previous = masked
        if valid:
            table_base = base
            raw_entries = values
            break
    if table_base < 0:
        raise ValueError("no monotonic SNASYS offset table")
    decoded: list[bytes] = []
    for index in range(len(raw_entries) - 1):
        start = raw_entries[index] & 0x7FFFFF
        end = raw_entries[index + 1] & 0x7FFFFF
        segment = blob[start:end]
        decoded.append(decode_inner_segment(segment) if raw_entries[index] >> 23 else segment)
    return decoded


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    targets = {
        "i_tile": RAW_DIR / "i_tile.dat.bin",
        "i_mapfeature": RAW_DIR / "i_mapfeature.dat.bin",
        "i_worldmap": GAME_RES / "i_worldmap.dat.jpg",
    }
    for name, src in targets.items():
        if not src.exists():
            print(f"missing {name}: {src}")
            continue
        blob = src.read_bytes()
        try:
            entries = decode_snasys_entries(blob)
            payload = {
                "source": src.name,
                "entry_count": len(entries),
                "entries": [
                    {"index": i, "size": len(e), "hex": e.hex()}
                    for i, e in enumerate(entries)
                ],
            }
            (OUT_DIR / f"{name}.json").write_text(json.dumps(payload))
            sizes = [len(e) for e in entries]
            print(f"{name}: {len(entries)} entries, size_range={min(sizes)}..{max(sizes)}, total={sum(sizes):,}")
        except Exception as e:  # noqa: BLE001
            print(f"{name}: FAILED - {e}")


if __name__ == "__main__":
    main()
