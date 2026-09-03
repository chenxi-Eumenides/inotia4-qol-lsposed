#!/usr/bin/env python3
"""离线解析 416 个 map 文件，输出通行矩阵 JSON + 出口目标 JSON。

按 MAP_LoadBase (0x112060) 反汇编逻辑：
- 11-bit tile ID 编码：(byte1 & 0x7) << 8 | byte2
- matrix 字节：(byte1 >> 4) | (0x40 if blocking)
- 阻挡 tile ID 列表从 0x11210c-0x11225c cmp 指令提取
- matrix 64x64 (MAP_IsBlocking y*64+x)
- 文件结构：byte 0 skip, byte 1-4 header (byte3=width, byte4=height)

出口条目 6 字节（2026-08-16 逆向验证，data-sources.md §3.2 格式记载 + 双向交叉验证）：
- byte 0-1: 当前地图出口 tile 坐标 (x, y)
- byte 2-3: 目标点坐标 (targetX, targetY)，86% 落在目标地图尺寸内
- byte 4-5: u16 LE，高字节 byte5 = 目标地图 ID（MAPINFOBASE 索引 0-415，
  全量 3077 条无一越界；双向出口中 93.5% 的目标点距对方指回出口 ≤2 格）

输出：
- apk/static-data/json/maps/tiles.json   （瓦片矩阵，结构不变）
- apk/static-data/json/maps/exits.json   （出口目标：mapId/x/y/targetMapId/targetX/targetY）

格式：
tiles.json: {"m0": {"mapId":0,"width":..,"height":..,"blockingCount":..,"tiles":"<base64 of 4096 bytes>"}, ...}
exits.json: {"m20": {"mapId":20,"width":..,"height":..,"exits":[{"x":..,"y":..,"targetMapId":..,"targetX":..,"targetY":..}, ...]}, ...}

运行：uv run python scripts/parse/export_map_tiles.py
"""
from __future__ import annotations
import base64
import json
import sys
from pathlib import Path


BLOCKING_TILES_EXACT = frozenset({0xa1, 0xa8, 0xaf, 0xb2, 0x259, 0x264, 0x267, 0x273, 0x276, 0x6f6, 0x758})
BLOCKING_TILES_RANGES = (
    (0x8d, 0x8f), (0x95, 0x95), (0x99, 0x9e), (0xaa, 0xad),
    (0xb5, 0xb6), (0xb8, 0xbb), (0x23d, 0x23d), (0x24a, 0x24b),
)


def is_blocking_tile(tile_id: int) -> bool:
    if tile_id in BLOCKING_TILES_EXACT:
        return True
    for lo, hi in BLOCKING_TILES_RANGES:
        if lo <= tile_id <= hi:
            return True
    return False


def parse_map_tiles(data: bytes) -> tuple[int, int, bytearray, int, list[dict]]:
    """解析单个 map 文件，返回 (width, height, 64x64 matrix, blocking_count, exits)。

    文件结构（MAP_Load 0x1149d4 + MAP_LoadBase 0x112060 + MAP_LoadLayer 0x11467c 反汇编，
    2026-08-12 真机 frida 验证 m31 100% 一致）：
    - byte 0-1: skip；byte 2: width；byte 3: height
    - base layer: width*height*2 bytes（每 cell 2 字节，11-bit tile ID）
    - MAP_LoadLayer: 5 u16 layer configs + 1 u8 section count + sections
      （每 section: 1 u8 hdr + 1 u16 count + count*4 bytes features）
    - exit count: 1 u8；exits: 6 bytes each，matrix[y*64+x] |= 0x80 (bit7=exit)

    matrix_byte = (byte1 >> 4) | (0x40 if blocking) | (0x80 if exit)

    exit 条目 6 字节语义（2026-08-16 逆向验证）：
    [x, y, targetX, targetY, u16_lo, targetMapId]，targetMapId = byte5 = MAPINFOBASE 索引。
    """
    if len(data) < 5:
        return 0, 0, bytearray(64 * 64), 0, []

    width = data[2]
    height = data[3]
    pos = 4

    matrix = bytearray(64 * 64)
    blocking_count = 0

    for y in range(min(height, 64)):
        for x in range(min(width, 64)):
            if pos + 2 > len(data):
                return width, height, matrix, blocking_count, []
            byte1 = data[pos]
            byte2 = data[pos + 1]
            pos += 2

            tile_id = ((byte1 & 0x7) << 8) | byte2
            if is_blocking_tile(tile_id):
                matrix[y * 64 + x] = (byte1 >> 4) | 0x40
                blocking_count += 1
            else:
                matrix[y * 64 + x] = (byte1 >> 4)

    pos += 2 * width * (height - min(height, 64))

    pos += 10
    pos += 1
    if pos < len(data):
        section_count = data[pos - 1]
        for _ in range(section_count):
            if pos + 3 > len(data):
                break
            pos += 1
            cnt = data[pos] | (data[pos + 1] << 8)
            pos += 2 + cnt * 4

    exits: list[dict] = []
    if pos < len(data):
        exit_count = data[pos]
        pos += 1
        for _ in range(exit_count):
            if pos + 6 > len(data):
                break
            x, y = data[pos], data[pos + 1]
            tx, ty = data[pos + 2], data[pos + 3]
            target_map_id = data[pos + 5]  # u16 高字节 = 目标地图 ID（MAPINFOBASE 索引）
            pos += 6
            if y < 64 and x < 64:
                matrix[y * 64 + x] |= 0x80
            exits.append({
                "x": x,
                "y": y,
                "targetMapId": target_map_id,
                "targetX": tx,
                "targetY": ty,
            })

    return width, height, matrix, blocking_count, exits


def main() -> None:
    raw_dir = Path("apk/static-data/raw")
    out_path = Path("apk/static-data/json/maps/tiles.json")
    exits_path = Path("apk/static-data/json/maps/exits.json")
    if not raw_dir.exists():
        print(f"ERROR: {raw_dir} not found", file=sys.stderr)
        sys.exit(1)

    out_path.parent.mkdir(parents=True, exist_ok=True)

    files = sorted(f for f in raw_dir.glob("m*.dat.bin") if f.stem[1:].split(".")[0].isdigit())
    print(f"Parsing {len(files)} map files...")

    result: dict[str, dict] = {}
    exits_result: dict[str, dict] = {}
    blocking_total = 0
    skip_count = 0
    exit_total = 0

    for f in files:
        map_id = int(f.stem[1:].split(".")[0])
        data = f.read_bytes()
        width, height, matrix, blocking_count, exits = parse_map_tiles(data)

        if width == 0 and height == 0:
            skip_count += 1

        blocking_total += blocking_count
        exit_total += len(exits)
        result[f"m{map_id}"] = {
            "mapId": map_id,
            "width": width,
            "height": height,
            "blockingCount": blocking_count,
            "tiles": base64.b64encode(bytes(matrix)).decode("ascii"),
        }
        if exits:
            exits_result[f"m{map_id}"] = {
                "mapId": map_id,
                "width": width,
                "height": height,
                "exits": exits,
            }

    out_path.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    exits_path.write_text(json.dumps(exits_result, ensure_ascii=False, separators=(",", ":")))
    size_kb = out_path.stat().st_size / 1024
    exits_kb = exits_path.stat().st_size / 1024
    print(f"OK: {len(result)} maps, {blocking_total} total blocking tiles, {exit_total} exits")
    print(f"Output: {out_path} ({size_kb:.1f} KB)")
    print(f"Output: {exits_path} ({exits_kb:.1f} KB, {len(exits_result)} maps with exits)")


if __name__ == "__main__":
    main()
