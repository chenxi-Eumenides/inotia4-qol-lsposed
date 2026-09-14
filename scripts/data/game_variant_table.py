#!/usr/bin/env python3
"""离线生成 libgame.so 校验值 → 游戏变体/能力对照表。

用法：
    uv run python scripts/data/game_variant_table.py

输入：
    apk/game-apk/*.apk
    apk/game-apk/history/*.apk
    apk/game-apk/history/*.xapk

输出：
    module/app/src/main/cpp/data/native/game_variant_table.inc

设计要点：
  * 主键为 arm64 `lib/arm64-v8a/libgame.so` 的 md5（16 字节）。
  * `.xapk` 的 arm64 库位于内部 `config.arm64_v8a.apk`，需先打开内层 zip。
  * 无 arm64 库的 APK 记为 unsupported：打印警告，不写入表。
  * 客观能力位由 ELF 结构事实计算；语义能力位由 `CURATION` 显式登记（含依据注释）。
  * 输出确定性排序（系列 → 版本标签 → md5），不含时间戳，重复运行结果一致。
  * 脚本输出仅允许写入 apk/、output/、archive/、.tmp/ 或生成的 .inc 文件。
"""

from __future__ import annotations

import hashlib
import io
import re
import struct
import sys
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# 路径解析（本文件位于 <root>/scripts/data/）
# ---------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "module/app/src/main/cpp/data/native/game_variant_table.inc"

INPUT_GLOBS = [
    "apk/game-apk/*.apk",
    "apk/game-apk/history/*.apk",
    "apk/game-apk/history/*.xapk",
]

LIB_PATH = "lib/arm64-v8a/libgame.so"
XAPK_INNER = "config.arm64_v8a.apk"

# ---------------------------------------------------------------------------
# ELF64 常量与结构事实
# ---------------------------------------------------------------------------
PT_LOAD = 1
PF_X = 0x1
HIDDEN_SEGMENT_VADDR_MIN = 0x700000  # 隐藏注入段起始 vaddr 下限
WH4_MARKER = b"WH4JRN01"             # monster 隐藏段内的仓库容器 magic

# 能力位（与 core/native/game_variant.h 保持一致）
CAP_HIDDEN_SEGMENT = 1 << 0   # 存在隐藏可执行 PT_LOAD 段
CAP_WAREHOUSE = 1 << 1        # 隐藏段内包含 ASCII WH4JRN01
CAP_WAREHOUSE_INLINE = 1 << 2 # block3 内嵌 WH96v002 仓库段（语义位）

# 语义位登记：客观位由脚本从 ELF 计算，语义位在此按文件名显式登记。
# 每条必须写明依据；未登记者语义位保持 0（表示“未验证”，不是“确定没有”）。
CURATION = {
    # 依据：真机/反汇编实测（backlog 2026-09-14/15，block3 校验器 0x7413b4 调用链）
    # 仅有 Inotia4_v1.3.2_monster_v23.apk 的 block3 内嵌 WH96v002 仓库段；其余版本未验证。
    "Inotia4_v1.3.2_monster_v23.apk": CAP_WAREHOUSE_INLINE,
}


# ---------------------------------------------------------------------------
# 文件名解析
# ---------------------------------------------------------------------------
def parse_series(name: str) -> str:
    """系列：monster / overhaul(盗版大修) / original(原版) / unknown。"""
    if "monster" in name.lower():
        return "monster"
    if "盗版大修" in name:
        return "overhaul"
    if "原版" in name:
        return "original"
    return "unknown"


def parse_version_label(name: str) -> str:
    """版本标签：优先 8 位日期（大修构建），否则取最后一个 v<版本>；取不到留空。"""
    date = re.search(r"(20\d{6})", name)
    if date:
        return date.group(1)
    versions = re.findall(r"v(\d+(?:\.\d+)*)", name)
    if versions:
        return "v" + versions[-1]
    return ""


# ---------------------------------------------------------------------------
# 提取与 ELF 解析
# ---------------------------------------------------------------------------
def read_libgame(apk_path: Path) -> bytes | None:
    """提取 arm64 libgame.so；无 arm64 库返回 None。"""
    try:
        with zipfile.ZipFile(apk_path) as zf:
            if apk_path.suffix.lower() == ".xapk":
                inner = zf.read(XAPK_INNER)
                with zipfile.ZipFile(io.BytesIO(inner)) as inner_zf:
                    try:
                        return inner_zf.read(LIB_PATH)
                    except KeyError:
                        return None
            try:
                return zf.read(LIB_PATH)
            except KeyError:
                return None
    except (zipfile.BadZipFile, KeyError, OSError):
        return None


def parse_elf_facts(data: bytes) -> tuple[bool, int, bool]:
    """返回 (是否有隐藏可执行段, 隐藏段 p_filesz, 隐藏段是否含 WH4JRN01)。

    仅支持 ELF64 小端（arm64）。解析失败按无隐藏段处理。
    """
    if len(data) < 0x40 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return False, 0, False
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    has_hidden = False
    hidden_size = 0
    has_marker = False
    for i in range(phnum):
        base = phoff + i * phentsize
        if base + 56 > len(data):
            break
        # 字段顺序：p_type(4) p_flags(4) p_offset(8) p_vaddr(8)
        #           p_paddr(8) p_filesz(8) p_memsz(8) p_align(8)
        p_type, p_flags = struct.unpack_from("<II", data, base)
        p_offset = struct.unpack_from("<Q", data, base + 8)[0]
        p_vaddr = struct.unpack_from("<Q", data, base + 16)[0]
        p_filesz = struct.unpack_from("<Q", data, base + 32)[0]
        if p_type != PT_LOAD or not (p_flags & PF_X):
            continue
        if p_vaddr < HIDDEN_SEGMENT_VADDR_MIN:
            continue
        has_hidden = True
        hidden_size = p_filesz
        segment = data[p_offset:p_offset + p_filesz]
        if WH4_MARKER in segment:
            has_marker = True
    return has_hidden, hidden_size, has_marker


def capabilities_for(name: str, has_hidden: bool, has_marker: bool) -> int:
    caps = 0
    if has_hidden:
        caps |= CAP_HIDDEN_SEGMENT
    if has_marker:
        caps |= CAP_WAREHOUSE
    caps |= CURATION.get(name, 0)
    return caps


# ---------------------------------------------------------------------------
# 生成
# ---------------------------------------------------------------------------
def collect() -> tuple[list[dict], list[str]]:
    entries: list[dict] = []
    sources: list[str] = []
    unsupported: list[str] = []
    for pattern in INPUT_GLOBS:
        for apk_path in sorted(ROOT.glob(pattern)):
            rel = apk_path.relative_to(ROOT).as_posix()
            sources.append(rel)
            name = apk_path.name
            data = read_libgame(apk_path)
            if data is None:
                unsupported.append(rel)
                print(f"[unsupported] {rel}: 无 arm64 libgame.so，跳过", file=sys.stderr)
                continue
            has_hidden, hidden_size, has_marker = parse_elf_facts(data)
            entries.append({
                "name": name,
                "source": rel,
                "series": parse_series(name),
                "version": parse_version_label(name),
                "md5": hashlib.md5(data).hexdigest(),
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "hidden": has_hidden,
                "hidden_size": hidden_size,
                "marker": has_marker,
                "caps": capabilities_for(name, has_hidden, has_marker),
            })
    return entries, sources


SERIES_ORDER = {"original": 0, "overhaul": 1, "monster": 2, "unknown": 3}


def md5_bytes_literal(hex32: str) -> str:
    pairs = [hex32[i:i + 2] for i in range(0, 32, 2)]
    return ", ".join(f"0x{p}" for p in pairs)


def render(entries: list[dict], sources: list[str]) -> str:
    ordered = sorted(entries, key=lambda e: (SERIES_ORDER[e["series"]], e["version"], e["md5"]))
    lines: list[str] = []
    lines.append("// 本文件由 scripts/data/game_variant_table.py 生成，请勿手改。")
    lines.append("// 重新生成：uv run python scripts/data/game_variant_table.py")
    lines.append("//")
    lines.append("// 源文件清单（确定性排序，无 arm64 libgame.so 的 APK 不在此表内）：")
    for src in sources:
        lines.append(f"//   {src}")
    lines.append("//")
    lines.append("// 排序：系列(original/overhaul/monster/unknown) → 版本标签 → md5")
    lines.append("// 能力位：bit0 kCapHiddenSegment / bit1 kCapWarehouse / bit2 kCapWarehouseInline")
    lines.append("// 条目为 qol::GameVariant 聚合初始化；known 由运行时命中后置位。")
    lines.append("")
    lines.append("static const GameVariant kGameVariantTable[] = {")
    for e in ordered:
        lines.append(
            f"    {{GameSeries::k{e['series'].capitalize()}, \"{e['version']}\", "
            f"0x{e['caps']:08x}u, {{{md5_bytes_literal(e['md5'])}}}}},"
        )
    lines.append("};")
    lines.append("")
    lines.append("static const size_t kGameVariantTableSize =")
    lines.append("    sizeof(kGameVariantTable) / sizeof(kGameVariantTable[0]);")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    entries, sources = collect()
    md5s = [e["md5"] for e in entries]
    if len(md5s) != len(set(md5s)):
        print("[error] md5 不唯一，拒绝生成", file=sys.stderr)
        return 1
    text = render(entries, sources)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(text, encoding="utf-8")
    print(f"生成 {OUTPUT.relative_to(ROOT)}：{len(entries)} 条")
    for e in sorted(entries, key=lambda x: (SERIES_ORDER[x["series"]], x["version"], x["md5"])):
        print(
            f"  {e['series']:<8} {e['version'] or '-':<10} caps=0x{e['caps']:02x} "
            f"hidden={'Y' if e['hidden'] else 'N'}(0x{e['hidden_size']:x}) "
            f"marker={'Y' if e['marker'] else 'N'} md5={e['md5']} {e['name']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
