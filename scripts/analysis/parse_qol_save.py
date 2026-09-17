#!/usr/bin/env python3
"""把 `.qol_save` 备份 bundle 完整解析为可读 JSON。

解析层次（格式依据 archive/docs/development/features/save-backup.md §3、docs/reference/game/save.md）：
  1. bundle 容器：magic/formatVersion/分段边界/CRC32/checksum（sha256(origPlain‖module‖wh) 前 12 位）。
  2. metaJson 元数据。
  3. 原版存档明文 origPlain：块表、块 0 Information、块 3 Inventory（含内嵌 WH96v002 段检测）。
  4. 模块 sidecar（MSAV 容器）：generation、各 section 名/版本/payload CRC（payload 本体不透明，输出摘要）。
  5. 个人仓库 warehouseBlob：v3 逐项 suffix/flags；flags=1（明文）时解析 WH4JRN01 头并校验
     +0x38 FNV-1a 64 完整性值；flags=0（密文）仅输出摘要（需设备密钥，无法离线解析）。

用法：
  uv run scripts/analysis/parse_qol_save.py <file.qol_save> [-o out.json]

stdout 输出纯 JSON（人类提示走 stderr）；解析失败输出 {"ok": false, "error": ...} 并退出码 1。
"""

import argparse
import hashlib
import json
import struct
import sys
import zlib
from datetime import datetime
from pathlib import Path

BUNDLE_MAGIC = 0x51534231  # "QSB1"（大端）
BUNDLE_SUFFIX = ".qol_save"
WH4_PLAIN_MAGIC = b"WH4JRN01"
WH96_MAGIC = b"WH96v002"
MSAV_MAGIC = 0x4D534156  # "MSAV"（大端）

# wh4 明文固定头布局（docs/reference/game/save.md §1.1，多字节字段小端）
WH4_HDR_LEN = 0x40
FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


class ParseError(Exception):
    """bundle 结构无法按契约解析时抛出。"""


def fnv1a64(data: bytes) -> int:
    """FNV-1a 64（wh4 +0x38 完整性校验算法）。"""
    h = FNV_BASIS
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def iso_ms(ms: int) -> str:
    """毫秒时间戳 → ISO8601 本地时间；异常时返回空串。"""
    try:
        return datetime.fromtimestamp(ms / 1000).isoformat(timespec="seconds")
    except (OverflowError, OSError, ValueError):
        return ""


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def read_bundle(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 4 + 4:
        raise ParseError(f"file too small: {len(data)} bytes")
    return data


def split_bundle(data: bytes) -> dict:
    """按 save-backup.md §3 大端布局切段并校验 CRC，兼容 v1/v2/v3。"""
    stored_crc = struct.unpack_from(">I", data, len(data) - 4)[0]
    computed_crc = crc32(data[:-4])
    payload_end = len(data) - 4
    off = 0

    def need(n: int, what: str) -> None:
        nonlocal off
        if off + n > payload_end:
            raise ParseError(f"truncated before {what}: need {n} bytes at offset {off}, payload ends {payload_end}")

    magic, version, source_slot = struct.unpack_from(">IHB", data, off)
    off += 7
    if magic != BUNDLE_MAGIC:
        raise ParseError(f"bad magic {magic:#010x}, expected {BUNDLE_MAGIC:#010x}")
    need(11, "reserved+exportTime")
    reserved = data[off:off + 3]
    off += 3
    export_time_ms = struct.unpack_from(">Q", data, off)[0]
    off += 8

    def take_u32(what: str) -> int:
        nonlocal off
        need(4, what)
        (n,) = struct.unpack_from(">I", data, off)
        off += 4
        return n

    orig_len = take_u32("origLen")
    need(orig_len, "origPlain")
    orig = data[off:off + orig_len]
    off += orig_len
    module_len = take_u32("moduleLen")
    need(module_len, "module")
    module = data[off:off + module_len]
    off += module_len

    warehouse_len = 0
    warehouse = b""
    if version >= 2:
        warehouse_len = take_u32("warehouseLen")
        need(warehouse_len, "warehouse")
        warehouse = data[off:off + warehouse_len]
        off += warehouse_len

    need(2, "metaLen")
    (meta_len,) = struct.unpack_from(">H", data, off)
    off += 2
    need(meta_len, "metaJson")
    meta_raw = data[off:off + meta_len]
    off += meta_len
    if off != payload_end:
        raise ParseError(f"segment boundary mismatch: consumed {off}, payload end {payload_end}")

    try:
        meta = json.loads(meta_raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ParseError(f"metaJson unreadable: {e}") from e

    return {
        "format_version": version,
        "source_slot": source_slot,
        "reserved_hex": reserved.hex(),
        "export_time_ms": export_time_ms,
        "export_time_iso": iso_ms(export_time_ms),
        "orig_plain": orig,
        "module": module,
        "warehouse": warehouse,
        "meta": meta,
        "crc32": {"stored": f"{stored_crc:08x}", "computed": f"{computed_crc:08x}", "ok": stored_crc == computed_crc},
    }


def parse_block_table(plain: bytes) -> dict:
    """明文块表：[0..8) 头，[8+i*4) 每项 u16 offset_rel + u16 len，块数据 = plain + offset_rel + 8。

    块数按 C++ 剥离实现同一规则精确得出：块 0 数据紧跟块表 ⇒ offset_rel0 == 表字节数，
    block_count = offset_rel0 / 4（与 save_backup_bundle.cpp warehouse_strip_inline_segment 一致）。
    """
    if len(plain) < 12:
        raise ParseError("plain too small for block table")
    (off0,) = struct.unpack_from("<H", plain, 8)
    if off0 == 0 or off0 % 4 != 0:
        raise ParseError(f"block0 offset_rel invalid: {off0}")
    block_count = off0 // 4
    table_end = 8 + block_count * 4
    if table_end > len(plain):
        raise ParseError(f"block table end {table_end} beyond plain {len(plain)}")
    entries = []
    covered_end = table_end
    contiguous = True
    for i in range(block_count):
        offset_rel, length = struct.unpack_from("<HH", plain, 8 + i * 4)
        data_abs = 8 + offset_rel
        if data_abs < table_end or data_abs + length > len(plain):
            raise ParseError(f"block {i} out of bounds: abs={data_abs} len={length}")
        if data_abs != covered_end:
            contiguous = False
        covered_end = max(covered_end, data_abs + length)
        entries.append({"index": i, "offset_rel": offset_rel, "len": length, "data_abs": data_abs})
    return {
        "entries": entries,
        "blocks_cover_fully": contiguous and covered_end == len(plain),
        "cover_note": "无重叠无空隙铺满表后区间（导出剥离内嵌段的前提，C++ 同款校验）",
    }


def parse_block0_information(plain: bytes, entries: list[dict]) -> dict:
    """块 0 Information（save.md §3：+0 u8 slot / +13 u64 save_time / +29 u32 version 等）。"""
    e0 = entries[0]
    base = e0["data_abs"]
    b = plain[base:base + e0["len"]]
    if len(b) < 33:
        return {"raw_len": len(b), "too_short": True}
    save_time_ms = struct.unpack_from("<Q", b, 13)[0]
    return {
        "slot_raw": b[0],
        "slot_note": "0xFF = 导出时归一化（bundle 内约定），导入时改写为目标槽",
        "unk_01_u32": struct.unpack_from("<I", b, 1)[0],
        "unk_05_u64": struct.unpack_from("<Q", b, 5)[0],
        "save_time_ms": save_time_ms,
        "save_time_iso": iso_ms(save_time_ms),
        "unk_15_u32": struct.unpack_from("<I", b, 21)[0],
        "unk_19_u32": struct.unpack_from("<I", b, 25)[0],
        "version": struct.unpack_from("<I", b, 29)[0],
    }


def parse_inline_warehouse(block: bytes) -> dict | None:
    """block3 内嵌 WH96v002 段检测：定位最后一个段，报告段长与段后是否纯零填充。"""
    pos = block.rfind(WH96_MAGIC)
    if pos < 0:
        return {"found": False}
    (seg_len,) = struct.unpack_from("<I", block, pos + 8)
    total = 8 + seg_len
    tail = block[pos + total:]
    return {
        "found": True,
        "offset_in_block": pos,
        "seg_len": seg_len,
        "total_len": total,
        "trailing_bytes": len(tail),
        "trailing_zero_only": all(b == 0 for b in tail),
        "seg_head_hex": block[pos:min(pos + 32, len(block))].hex(),
    }


def parse_original_save(plain: bytes) -> dict:
    """origPlain：块表 + 块 0 Information + 块 3 Inventory。"""
    table = parse_block_table(plain)
    entries = table["entries"]
    info = parse_block0_information(plain, entries)
    inv = None
    if len(entries) > 3:
        e3 = entries[3]
        block3 = plain[e3["data_abs"]:e3["data_abs"] + e3["len"]]
        inv = {"len": e3["len"], "inline_warehouse": parse_inline_warehouse(block3)}
    return {
        "orig_len": len(plain),
        "header_8_hex": plain[:8].hex(),
        "header_note": "前 8 字节为块指针基址保留头（LoadBlock +8 跳过）",
        "block_count": len(entries),
        "block_table": entries,
        "blocks_cover_fully": table["blocks_cover_fully"],
        "blocks_cover_note": table["cover_note"],
        "block0_information": info,
        "block3_inventory": inv,
        "sha256": hashlib.sha256(plain).hexdigest(),
    }


def parse_msav(module: bytes) -> dict:
    """MSAV sidecar 容器（ModuleSaveStore.kt encode 布局，DataOutputStream 大端）。"""
    out: dict = {"module_len": len(module)}
    if not module:
        out["note"] = "空 module = 仅原版备份"
        return out
    if len(module) < 4 + 2 + 1 + 8 + 2 + 4:
        out["magic_ok"] = False
        out["note"] = "短于 MSAV 最小容器长度，仅输出摘要"
        return out
    magic, fmt_ver = struct.unpack_from(">IH", module, 0)
    out["magic_ok"] = magic == MSAV_MAGIC
    if not out["magic_ok"]:
        out["note"] = "非 MSAV 容器，仅输出摘要"
        return out
    out["format_version"] = fmt_ver
    out["slot"] = module[6]
    (generation,) = struct.unpack_from(">Q", module, 7)
    (count,) = struct.unpack_from(">H", module, 15)
    out["generation"] = generation
    out["section_count"] = count
    off = 17
    sections = []
    for i in range(count):
        if off + 1 > len(module):
            out["note"] = f"truncated at section {i}"
            break
        name_len = module[off]
        off += 1
        if off + name_len + 6 > len(module):
            out["note"] = f"truncated in section {i} header"
            break
        name = module[off:off + name_len].decode("ascii", "replace")
        off += name_len
        (sec_ver,) = struct.unpack_from(">H", module, off)
        (payload_len,) = struct.unpack_from(">I", module, off + 2)
        off += 6
        if off + payload_len + 4 > len(module):
            out["note"] = f"truncated in section {i} payload"
            break
        payload = module[off:off + payload_len]
        off += payload_len
        (stored_crc,) = struct.unpack_from(">I", module, off)
        off += 4
        sections.append({
            "name": name,
            "version": sec_ver,
            "payload_len": payload_len,
            "payload_crc32": {"stored": f"{stored_crc:08x}", "computed": f"{crc32(payload):08x}", "ok": stored_crc == crc32(payload)},
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
            "payload_head_hex": payload[:32].hex(),
        })
    out["sections"] = sections
    out["sections_end_offset"] = off
    (stored_crc,) = struct.unpack_from(">I", module, len(module) - 4)
    body = module[:-4]
    out["container_crc32"] = {"stored": f"{stored_crc:08x}", "computed": f"{crc32(body):08x}", "ok": stored_crc == crc32(body)}
    out["trailing_bytes_after_body_crc"] = max(0, len(module) - 4 - off)
    out["sha256"] = hashlib.sha256(module).hexdigest()
    return out


def parse_wh96_entries(buf: bytes) -> dict:
    """wh4 内的 WH96v002 段：magic(8) + u32 LE 条目区长(288) + u32 LE chk + 条目区。

    注意与 block3 内嵌段（C++ 剥离目标）语义不同：block3 段无独立 chk、len 含自身 4B、
    段总长 = 8 + len；wh4 段有独立 chk、len 仅条目区、段总长 = 16 + len（真机样本实测）。
    条目为游戏内部道具记录（sub_1764 变长记录，未解析业务格式），输出长度/SHA-256/前缀 hex。
    """
    (seg_len, seg_chk) = struct.unpack_from("<II", buf, 8)
    entries = buf[16:16 + seg_len] if len(buf) >= 16 + seg_len else buf[16:]
    return {
        "magic_ok": buf[:8] == WH96_MAGIC,
        "entries_len": seg_len,
        "seg_chk": f"{seg_chk:08x}",
        "segment_total": 16 + seg_len,
        "entries_sha256": hashlib.sha256(entries).hexdigest(),
        "entries_head_hex": entries[:64].hex(),
    }


def parse_wh4_plain(blob: bytes, suffix: str) -> dict:
    """flags=1 的 wh4 明文：WH4JRN01 头 + +0x38 FNV-1a 64 校验 + 0x40 起两份 WH96v002 段。"""
    out: dict = {"plain_magic_ok": blob[:8] == WH4_PLAIN_MAGIC}
    if len(blob) < WH4_HDR_LEN or not out["plain_magic_ok"]:
        out["note"] = "头部不符合 WH4JRN01 布局，仅输出摘要"
        return out
    warehouse_id = struct.unpack_from("<Q", blob, 0x08)[0]
    stored_fnv = struct.unpack_from("<Q", blob, 0x38)[0]
    covered = blob[0x00:0x38] + blob[0x40:]
    computed_fnv = fnv1a64(covered)
    base_id = suffix.removesuffix(".bak")
    out.update({
        "warehouse_id": f"{warehouse_id:016x}",
        "warehouse_id_matches_suffix": base_id == f".wh4-{warehouse_id:016x}",
        "const_1": struct.unpack_from("<I", blob, 0x10)[0],
        "slot": struct.unpack_from("<I", blob, 0x14)[0],
        "hash_a": f"{struct.unpack_from('<Q', blob, 0x18)[0]:016x}",
        "hash_b": f"{struct.unpack_from('<Q', blob, 0x20)[0]:016x}",
        "declared_seg_len_a": struct.unpack_from("<I", blob, 0x28)[0],
        "declared_seg_len_b": struct.unpack_from("<I", blob, 0x2C)[0],
        "declared_note": "+0x28/+0x2c = 每段完整长度（16 + 条目区长）",
        "fnv_0x38": {"stored": f"{stored_fnv:016x}", "computed": f"{computed_fnv:016x}", "ok": stored_fnv == computed_fnv},
    })
    segments = []
    off = WH4_HDR_LEN
    while off + 16 <= len(blob) and blob[off:off + 8] == WH96_MAGIC:
        (seg_len,) = struct.unpack_from("<I", blob, off + 8)
        seg = parse_wh96_entries(blob[off:])
        seg["offset"] = off
        seg["matches_declared"] = 16 + seg_len in (
            out["declared_seg_len_a"], out["declared_seg_len_b"])
        segments.append(seg)
        off += 16 + seg_len
    out["wh96_segments"] = segments
    out["wh96_segment_count"] = len(segments)
    out["wh96_note"] = "同一份仓库内容按约定重复两段（副本）；.bak 与主文件各含一份"
    out["trailing_bytes_after_segments"] = len(blob) - off
    return out


def parse_warehouse(wh: bytes, format_version: int) -> dict:
    """warehouseBlob：v3 = u16 count + 每项 u16 suffixLen|suffix|u32 dataLen|u8 flags|data；
    v2 = 同结构但无 flags 字节；空 blob = 无仓库。"""
    out: dict = {"warehouse_len": len(wh), "sha256": hashlib.sha256(wh).hexdigest()}
    if not wh:
        out["count"] = 0
        return out
    (count,) = struct.unpack_from(">H", wh, 0)
    out["count"] = count
    has_flags = format_version >= 3
    off = 2
    items = []
    for i in range(count):
        if off + 2 > len(wh):
            out["note"] = f"truncated before item {i}"
            break
        (suffix_len,) = struct.unpack_from(">H", wh, off)
        off += 2
        if off + suffix_len + 4 > len(wh):
            out["note"] = f"truncated in item {i} header"
            break
        suffix = wh[off:off + suffix_len].decode("ascii", "replace")
        off += suffix_len
        (data_len,) = struct.unpack_from(">I", wh, off)
        off += 4
        if has_flags:
            if off + 1 > len(wh):
                out["note"] = f"truncated before item {i} flags"
                break
            flags = wh[off]
            off += 1
        if off + data_len > len(wh):
            out["note"] = f"truncated in item {i} data"
            break
        blob = wh[off:off + data_len]
        off += data_len
        item: dict = {
            "index": i,
            "suffix": suffix,
            "flags": flags if has_flags else None,
            "flags_note": None if has_flags else "v2 无 flags 字节：按密文兜底处理",
            "data_len": data_len,
            "data_sha256": hashlib.sha256(blob).hexdigest(),
        }
        if has_flags and flags & 0x01:
            item["plaintext"] = parse_wh4_plain(blob, suffix)
            if flags & ~0x01:
                item["undefined_flags_bits"] = f"{flags:#04x}"
        else:
            item["note"] = "密文兜底（需源设备密钥），无法离线解析内容"
        items.append(item)
    out["items"] = items
    out["decoded_end_offset"] = off
    out["fully_consumed"] = off == len(wh)
    return out


def parse_checksum(parts: list[bytes], meta: dict, file_name: str) -> dict:
    computed = hashlib.sha256(b"".join(parts)).hexdigest()[:12]
    expected_suffix = f"_{computed}{BUNDLE_SUFFIX}"
    return {
        "computed": computed,
        "matches_meta": meta.get("checksum") == computed,
        "filename_locatable": file_name.endswith(expected_suffix),
        "filename_note": "导入按 `_<checksum>.qol_save` 文件名后缀定位；浏览器重名后缀如 `(1)` 会让定位失败（backup not found）",
    }


def parse_qol_save(path: Path) -> dict:
    """入口：完整解析并组装输出 JSON。"""
    raw = read_bundle(path)
    bundle = split_bundle(raw)
    meta = bundle["meta"]
    return {
        "ok": True,
        "file": {"name": path.name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()},
        "container": {
            "format_version": bundle["format_version"],
            "source_slot": bundle["source_slot"],
            "reserved_hex": bundle["reserved_hex"],
            "export_time_ms": bundle["export_time_ms"],
            "export_time_iso": bundle["export_time_iso"],
            "crc32": bundle["crc32"],
        },
        "checksum": parse_checksum([bundle["orig_plain"], bundle["module"], bundle["warehouse"]], meta, path.name),
        "meta_json": meta,
        "original_save": parse_original_save(bundle["orig_plain"]),
        "module_sidecar": parse_msav(bundle["module"]),
        "warehouse": parse_warehouse(bundle["warehouse"], bundle["format_version"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="解析 .qol_save 备份为可读 JSON")
    parser.add_argument("bundle", type=Path, help=".qol_save 文件路径")
    parser.add_argument("-o", "--output", type=Path, default=None, help="输出 JSON 路径（缺省打印 stdout）")
    args = parser.parse_args()

    try:
        result = parse_qol_save(args.bundle)
    except (ParseError, struct.error) as e:
        result = {"ok": False, "error": str(e), "file": {"name": args.bundle.name}}
    except OSError as e:
        result = {"ok": False, "error": f"io error: {e}", "file": {"name": args.bundle.name}}

    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output is not None:
        args.output.write_text(text + "\n", encoding="utf-8")
        print(f"written: {args.output}", file=sys.stderr)
    else:
        print(text)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
