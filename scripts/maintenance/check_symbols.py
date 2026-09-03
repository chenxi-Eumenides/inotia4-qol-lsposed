#!/usr/bin/env python3
"""校验 libgame.so 符号 VMA 与 game_symbols.h 硬编码常量是否一致。

换游戏版本后运行：检测哪些符号地址变了，输出更新后的常量。
映射单一来源：cpp/symbol_registry.h（X-macro 注册表，SYM(宏名, 符号名) 行）。
用法：uv run python scripts/maintenance/check_symbols.py [libgame.so 路径]
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SO = ROOT / "apk" / "decoded" / "lib" / "arm64-v8a" / "libgame.so"
HEADER = ROOT / "module" / "app" / "src" / "main" / "cpp" / "game_symbols.h"
REGISTRY = ROOT / "module" / "app" / "src" / "main" / "cpp" / "symbol_registry.h"
READELF = (
    ROOT / "tools" / "ndk" / "android-ndk-r26d" / "toolchains" / "llvm"
    / "prebuilt" / "linux-x86_64" / "bin" / "llvm-readelf"
)


def load_registry() -> dict[str, str]:
    """从 symbol_registry.h 读取 SYM(宏名, 符号名) → {符号名: 宏名}。"""
    mapping: dict[str, str] = {}
    for m in re.finditer(r"SYM\(([A-Z0-9_]+), ([A-Za-z0-9_]+)\)", REGISTRY.read_text()):
        mapping[m.group(2)] = m.group(1)
    return mapping


SYMBOL_TO_MACRO = load_registry()


def read_symbols(so: Path) -> dict[str, int]:
    out = subprocess.run([str(READELF), "-s", str(so)], capture_output=True, text=True).stdout
    syms: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[0].rstrip(":").isdigit() and parts[1] != "0000000000000000":
            syms[parts[-1]] = int(parts[1], 16)
    return syms


def read_cpp_constants(header: Path) -> dict[str, int]:
    text = header.read_text()
    consts: dict[str, int] = {}
    for m in re.finditer(r"constexpr uintptr_t ([A-Z0-9_]+) = (0x[0-9a-fA-F]+)", text):
        consts[m.group(1)] = int(m.group(2), 16)
    return consts


def main() -> None:
    so = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SO
    if not so.exists():
        print(f"libgame.so 不存在: {so}")
        sys.exit(1)
    syms = read_symbols(so)
    consts = read_cpp_constants(HEADER)
    print(f"libgame.so: {so}（符号表 {len(syms)} 个）")
    print(f"{'符号':28s} {'cpp 当前':>12s} {'新版本':>12s} {'状态'}")
    changed = 0
    for symbol, macro in SYMBOL_TO_MACRO.items():
        new_addr = syms.get(symbol)
        cur_addr = consts.get(macro)
        if new_addr is None:
            print(f"{symbol:28s} {'缺失':>12s} ❌ 符号不存在")
            continue
        if cur_addr != new_addr:
            changed += 1
            print(f"{symbol:28s} {'0x%x' % cur_addr:>12s} {'0x%x' % new_addr:>12s} ⚠️ 需更新")
        else:
            print(f"{symbol:28s} {'0x%x' % cur_addr:>12s} {'0x%x' % new_addr:>12s} ✅ 一致")
    if changed:
        print(f"\n⚠️ {changed} 个符号地址变化：用 sed 更新 game_symbols.h 中对应 _VMA 常量后重新构建")


if __name__ == "__main__":
    main()
