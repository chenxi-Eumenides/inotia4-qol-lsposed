#!/usr/bin/env python3
"""把 qol-crash.log 里的绝对地址还原成 `模块+偏移`，可选调用 llvm-symbolizer 出函数名。

崩溃捕获只写寄存器与 /proc/self/maps（见 module/app/src/main/cpp/infra/log/qol_crash.cpp），
不做符号化：符号化需要 ELF 与符号表，属于离线步骤。

用法：
    uv run python scripts/analysis/symbolize_crash.py <qol-crash.log> \
        [--obj-dir module/app/build] [--symbolizer tools/ndk/.../llvm-symbolizer]

只读输入，结果打印到 stdout；不写文件。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OBJ_DIR = ROOT / "module" / "app" / "build"
SYMBOLIZER_GLOB = "tools/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-symbolizer"

MAP_LINE = re.compile(
    r"^([0-9a-f]+)-([0-9a-f]+)\s+([rwxps-]{4})\s+([0-9a-f]+)\s+\S+\s+\d+\s*(.*)$"
)
REG_LINE = re.compile(r"^([a-z][a-z0-9]*)=(0x[0-9a-f]+|-?\d+)$")
SIGNAL_LINE = re.compile(r"^signal=(\d+) (\S+) code=(-?\d+) \((\S+)\) fault=(0x[0-9a-f]+)$")

# 优先符号化的寄存器：PC 必然在代码里，LR/FP 命中代码段时也有意义。
PREFERRED_REGS = ("pc", "lr", "fp", "sp")


def register_sort_key(name: str) -> tuple[int, int, str]:
    """输出顺序：btN 按帧号紧随 PREFERRED_REGS，其后通用寄存器，最后其余键。"""
    if name.startswith("bt") and name[2:].isdigit():
        return (1, int(name[2:]), "")
    if name.startswith(("x", "r")):
        return (2, 0, name)
    return (3, 0, name)


@dataclass
class Mapping:
    start: int
    end: int
    perms: str
    offset: int
    path: str

    @property
    def executable(self) -> bool:
        return "x" in self.perms


def parse_crash(path: Path) -> tuple[dict[str, int], list[Mapping], dict[str, str]]:
    regs: dict[str, int] = {}
    maps: list[Mapping] = []
    header: dict[str, str] = {}
    in_maps = False
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if line.startswith("--- maps ---"):
                in_maps = True
                continue
            if line.startswith("--- log tail ---"):
                in_maps = False
                continue
            if in_maps:
                match = MAP_LINE.match(line)
                if match:
                    start, end, perms, offset, name = match.groups()
                    maps.append(
                        Mapping(
                            start=int(start, 16),
                            end=int(end, 16),
                            perms=perms,
                            offset=int(offset, 16),
                            path=name.strip(),
                        )
                    )
                continue
            signal = SIGNAL_LINE.match(line)
            if signal:
                header["signo"], header["signame"], header["code"], header["codename"], header["fault"] = signal.groups()
                continue
            if line.startswith(("pid=", "crash_at=")):
                header.update(dict(part.split("=", 1) for part in line.split() if "=" in part))
                continue
            match = REG_LINE.match(line)
            if match:
                name, value = match.groups()
                regs[name] = int(value, 16) if value.startswith("0x") else int(value)
    return regs, maps, header


def find_map(maps: list[Mapping], addr: int) -> Mapping | None:
    for mapping in maps:
        if mapping.start <= addr < mapping.end:
            return mapping
    return None


def load_bias(maps: list[Mapping], mapping: Mapping) -> int:
    """同一文件所有映射里 (start - offset) 的最小值即加载基址。"""
    if not mapping.path:
        return mapping.start - mapping.offset
    candidates = [
        item.start - item.offset
        for item in maps
        if item.path == mapping.path
    ]
    return min(candidates) if candidates else mapping.start - mapping.offset


def find_object(mapping: Mapping, obj_dirs: list[Path]) -> Path | None:
    if not mapping.path:
        return None
    name = Path(mapping.path).name
    for obj_dir in obj_dirs:
        if not obj_dir.exists():
            continue
        direct = obj_dir / name
        if direct.is_file():
            return direct
        for candidate in obj_dir.rglob(name):
            if candidate.is_file():
                return candidate
    return None


def symbolize(symbolizer: Path | None, obj: Path, offset: int) -> str:
    if symbolizer is None or not symbolizer.is_file():
        return ""
    try:
        completed = subprocess.run(
            [str(symbolizer), f"--obj={obj}", hex(offset)],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    return " ".join(lines[:2])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("crash_log", type=Path)
    parser.add_argument(
        "--obj-dir",
        action="append",
        type=Path,
        default=None,
        help="从哪个目录按文件名查找 .so（可重复，默认 module/app/build）",
    )
    parser.add_argument("--symbolizer", type=Path, default=None, help="llvm-symbolizer 路径")
    args = parser.parse_args()

    if not args.crash_log.is_file():
        print(f"crash log not found: {args.crash_log}", file=sys.stderr)
        return 2

    regs, maps, header = parse_crash(args.crash_log)
    obj_dirs = args.obj_dir or [DEFAULT_OBJ_DIR]
    symbolizer = args.symbolizer
    if symbolizer is None:
        matches = sorted(ROOT.glob(SYMBOLIZER_GLOB))
        symbolizer = matches[0] if matches else None

    print(f"file: {args.crash_log}")
    if "signame" in header:
        fault = int(header["fault"], 16)
        fault_map = find_map(maps, fault)
        where = f"{fault_map.path or '[anon]'} perms={fault_map.perms}" if fault_map else "unmapped"
        print(
            f"signal: {header.get('signo')} {header['signame']} "
            f"code={header.get('code')} ({header.get('codename')}) "
            f"fault={header['fault']} -> {where}"
        )
    if "frame" in header:
        print(f"frame: {header['frame']}")
    print()

    order = list(PREFERRED_REGS) + sorted(
        (name for name in regs if name not in PREFERRED_REGS),
        key=register_sort_key,
    )
    seen: set[str] = set()
    for name in order:
        if name in seen or name not in regs:
            continue
        seen.add(name)
        addr = regs[name]
        mapping = find_map(maps, addr)
        if mapping is None:
            print(f"{name:4s} 0x{addr:012x}  <unmapped>")
            continue
        if not mapping.path:
            print(f"{name:4s} 0x{addr:012x}  [anon] perms={mapping.perms}")
            continue
        offset = addr - load_bias(maps, mapping)
        obj = find_object(mapping, obj_dirs)
        detail = ""
        if mapping.executable:
            detail = symbolize(symbolizer, obj, offset) if obj else ""
        suffix = f"  {detail}" if detail else ""
        print(f"{name:4s} 0x{addr:012x}  {mapping.path}+0x{offset:x}{suffix}")

    if symbolizer is None or not symbolizer.is_file():
        print()
        print("提示：未找到 llvm-symbolizer，只输出 模块+偏移（可用 NDK 的 llvm-symbolizer 手工解析）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
