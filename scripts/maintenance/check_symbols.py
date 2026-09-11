#!/usr/bin/env python3
"""分类并校验 libgame.so 符号 VMA 与 game_symbols.h 硬编码常量。

两段式：
1. 分类：解析 game_symbols.h 全部 `constexpr <type> <NAME> = <expr>;`，按类型与
   symbol_registry.h 注册关系分三类：
   - A 地址·具名：type == uintptr_t 且宏名已注册 → 可按 .dynsym 名动态解析；
   - B 地址·静态：type == uintptr_t 但未注册（宏名或行尾注释含 GOT 者为 GOT 槽，
     运行时走 .rela.dyn RELATIVE 反查；其余为其他静态地址）；
   - C 布局/枚举：其余 type（size_t/int/uint32_t/uint8_t 等）→ 结构体偏移/枚举，
     非地址，不参与符号比对。
2. 校验：仅对 A 类，用注册符号名在 SO 符号表查地址并与常量比对。

映射单一来源：cpp/symbol_registry.h（X-macro 注册表，SYM(宏名, 符号名) 行）。
用法：uv run python scripts/maintenance/check_symbols.py [libgame.so 路径]
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SO = ROOT / "apk" / "decoded" / "overhaul" / "lib" / "arm64-v8a" / "libgame.so"
HEADER = ROOT / "module" / "app" / "src" / "main" / "cpp" / "data" / "native" / "game_symbols.h"
REGISTRY = ROOT / "module" / "app" / "src" / "main" / "cpp" / "data" / "native" / "symbol_registry.h"
READELF = (
    ROOT / "tools" / "ndk" / "android-ndk-r26d" / "toolchains" / "llvm"
    / "prebuilt" / "linux-x86_64" / "bin" / "llvm-readelf"
)

_SYM_RE = re.compile(r"SYM\(([A-Z0-9_]+),\s*([A-Za-z0-9_]+)\)")
_CONST_RE = re.compile(
    r"constexpr\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Z0-9_]+)\s*=\s*([^;]+);(.*)$"
)
# 表达式求值：先替换常量引用，再用字符白名单校验，最后才 eval（禁止对任意输入直接 eval）。
_IDENT_RE = re.compile(r"(?<![0-9A-Za-z_])[A-Za-z_][A-Za-z0-9_]*")
_WHITELIST_RE = re.compile(r"[0-9a-fA-FxX+\-\s]+")

CAT_A = "A 地址·具名"
CAT_B_GOT = "B 地址·静态(GOT槽)"
CAT_B_STATIC = "B 地址·静态(其他)"
CAT_C = "C 布局/枚举"


class Constant(NamedTuple):
    """game_symbols.h 中的一条 constexpr 常量。"""

    name: str
    ctype: str
    expr: str
    comment: str
    value: int | None


def load_registry() -> dict[str, list[str]]:
    """读取 symbol_registry.h，返回 {符号名: [宏名, ...]}（同一符号保留全部注册宏）。"""
    mapping: dict[str, list[str]] = {}
    for m in _SYM_RE.finditer(REGISTRY.read_text()):
        macro, symbol = m.group(1), m.group(2)
        macros = mapping.setdefault(symbol, [])
        if macro not in macros:
            macros.append(macro)
    return mapping


def _eval_expr(
    expr: str,
    raw: dict[str, str],
    cache: dict[str, int],
    stack: tuple[str, ...],
) -> int:
    """解析 `=` 右侧表达式（十六进制/十进制字面量 + `+`/`-`，可引用其他常量）。

    先递归替换常量标识符，再用 `_WHITELIST_RE` 校验替换结果，最后在受限命名空间
    eval。任何未知标识符或非法字符都抛出 ValueError。
    """

    def repl(match: re.Match[str]) -> str:
        ref = match.group(0)
        if ref not in raw:
            raise ValueError(f"表达式含未知标识符 {ref!r}: {expr!r}")
        if ref not in cache:
            if ref in stack:
                raise ValueError(f"常量循环引用: {' -> '.join((*stack, ref))}")
            cache[ref] = _eval_expr(raw[ref], raw, cache, (*stack, ref))
        return str(cache[ref])

    substituted = _IDENT_RE.sub(repl, expr)
    if not _WHITELIST_RE.fullmatch(substituted):
        raise ValueError(f"表达式含非法字符: {expr!r}")
    return int(eval(substituted, {"__builtins__": {}}, {}))


def read_constants(header: Path) -> list[Constant]:
    """解析全部 constexpr 常量，保持文件声明顺序。"""
    raw: dict[str, str] = {}
    parsed: list[tuple[str, str, str, str]] = []
    for line in header.read_text().splitlines():
        m = _CONST_RE.search(line)
        if not m:
            continue
        ctype, name = m.group(1), m.group(2)
        expr, comment = m.group(3).strip(), m.group(4)
        raw[name] = expr
        parsed.append((name, ctype, expr, comment))

    cache: dict[str, int] = {}
    consts: list[Constant] = []
    for name, ctype, expr, comment in parsed:
        try:
            value: int | None = _eval_expr(expr, raw, cache, (name,))
        except ValueError as exc:
            print(f"⚠️ 常量 {name} 求值失败: {exc}", file=sys.stderr)
            value = None
        consts.append(Constant(name, ctype, expr, comment, value))
    return consts


def read_symbols(so: Path) -> dict[str, int]:
    out = subprocess.run([str(READELF), "-s", str(so)], capture_output=True, text=True).stdout
    syms: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[0].rstrip(":").isdigit() and parts[1] != "0000000000000000":
            syms[parts[-1]] = int(parts[1], 16)
    return syms


def classify(const: Constant, registered_macros: set[str]) -> str:
    if const.ctype != "uintptr_t":
        return CAT_C
    if const.name in registered_macros:
        return CAT_A
    if "GOT" in const.name or "GOT" in const.comment:
        return CAT_B_GOT
    return CAT_B_STATIC


def fmt_value(const: Constant) -> str:
    return "无法求值" if const.value is None else f"0x{const.value:x}"


def main() -> None:
    so = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SO
    constants = read_constants(HEADER)
    registry = load_registry()
    registered_macros = {macro for macros in registry.values() for macro in macros}
    const_by_name = {c.name: c for c in constants}

    counts: dict[str, int] = {CAT_A: 0, CAT_B_GOT: 0, CAT_B_STATIC: 0, CAT_C: 0}
    for const in constants:
        counts[classify(const, registered_macros)] += 1
    count_b = counts[CAT_B_GOT] + counts[CAT_B_STATIC]

    print("== 分类汇总 ==")
    print(f"A 地址·具名（可动态解析）: {counts[CAT_A]}")
    print(f"B 地址·静态（无符号名）: {count_b}"
          f"（GOT 槽 {counts[CAT_B_GOT]} + 其他静态地址 {counts[CAT_B_STATIC]}）")
    print(f"C 布局/枚举（非地址，不参与符号比对）: {counts[CAT_C]}")
    print(f"合计: {counts[CAT_A] + count_b + counts[CAT_C]}")
    print()

    print("== 分类明细 ==")
    for const in constants:
        print(f"[{classify(const, registered_macros)}] {const.name} = {fmt_value(const)} ({const.ctype})")

    if not so.exists():
        print()
        print(f"libgame.so 不存在: {so}")
        print("分类已完成；符号校验需提供有效 libgame.so 路径。")
        sys.exit(1)

    syms = read_symbols(so)

    duplicates = {symbol: macros for symbol, macros in registry.items() if len(macros) > 1}
    if duplicates:
        print()
        for symbol, macros in duplicates.items():
            print(f"⚠️ 符号 {symbol} 被多个宏注册: {', '.join(macros)}")

    print()
    print(f"== A 类符号校验（libgame.so: {so}，符号表 {len(syms)} 个）==")
    print(f"{'符号':28s} {'cpp 当前':>12s} {'新版本':>12s} {'状态'}")
    changed = 0
    for symbol, macros in registry.items():
        new_addr = syms.get(symbol)
        for macro in macros:
            const = const_by_name.get(macro)
            if const is None or const.value is None:
                print(f"{symbol:28s} {'缺失':>12s} {'-':>12s} ❌ 常量缺失({macro})")
                continue
            cur_addr = const.value
            if new_addr is None:
                print(f"{symbol:28s} {'0x%x' % cur_addr:>12s} {'-':>12s} ❌ 符号不存在")
                continue
            if cur_addr != new_addr:
                changed += 1
                print(f"{symbol:28s} {'0x%x' % cur_addr:>12s} {'0x%x' % new_addr:>12s} ⚠️ 需更新")
            else:
                print(f"{symbol:28s} {'0x%x' % cur_addr:>12s} {'0x%x' % new_addr:>12s} ✅ 一致")
    if changed:
        print(f"\n⚠️ {changed} 个符号地址变化：更新 game_symbols.h 中对应 _VMA 常量后重新构建")


if __name__ == "__main__":
    main()
