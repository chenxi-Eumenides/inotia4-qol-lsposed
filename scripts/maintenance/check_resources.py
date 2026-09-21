#!/usr/bin/env python3
"""资源安装点清单与重复安装检测（ARCHITECTURE.md §4.1 一资源一安装者 / §9.2）。

背景：项目内所有 hook 安装都必须唯一——LSPosed NativeHook 对同一地址二次挂载会失败
（rc=-1）。当前尚无资源登记表，本脚本在 P0 阶段先做「安装点清单 + 重复安装检测」，
产出后续 P3/P4 资源登记表的输入。

扫描与判定：
- 对 `module/app/src/main/cpp` 下每个 `.cpp/.h/.inc` 逐行扫描三类安装机制：
    entry hook：`native_hook_func(` 或 `hook_func(`
    call patch：`call_patch`、`patch_apply(`、`patch_revert(`、`instr_patch`
    ptr slot  ：`PtrHook`、`ptr_hook`、`game_ptr_hook`、`ptr_slot`
- 每个命中行在前后各 25 行内按「由近及远」抽取目标符号：优先 `fn_resolve("F_XXX_VMA")`
  的字符串实参，其次裸 `F_[A-Z0-9_]+_VMA` 标识符；取不到记为 `<unknown>`。
- `resource_duplicate_owner`：同一 `机制|符号` 出现在两个不同文件中（同文件内重复不算）。
- `resource_unknown_symbol`：命中安装点但抽取不到符号，交人工登记。

不构成安装点的行不计入：空行、纯注释行、`#include`、机制原语自身的实现/声明文件
（`_MECHANISM_FILES`），以及机制取函数指针的 API 声明/定义行（`_MECH_API_LINE`）。这些
是机制本体而不是「某游戏符号被安装」的资源登记，混入只会制造无法归属的噪声。

用法：
    uv run python scripts/maintenance/check_resources.py            # 棘轮校验
    uv run python scripts/maintenance/check_resources.py -v         # 明细 + 资源清单
    uv run python scripts/maintenance/check_resources.py --inventory .tmp/res.json
        # 仅在显式传参时写出完整 JSON 清单；路径必须位于项目内，默认不写任何文件

CLI 说明：`arch_common.parse_common_args()` 内部自建 parser、无法追加自定义参数，且
`emit()` 只依赖 args 的 `freeze/json/verbose` 三个属性；因此这里按同样语义自建 parser 并
复用 `emit`，避免改动 arch_common（保持公共库稳定）。
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arch_common as ac  # noqa: E402

# --------------------------------------------------------------------------
# 扫描配置
# --------------------------------------------------------------------------

_MECHANISMS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("entry hook", re.compile(r"native_hook_func\(|hook_func\(")),
    ("call patch", re.compile(r"call_patch|patch_apply\(|patch_revert\(|instr_patch")),
    ("ptr slot", re.compile(r"PtrHook|ptr_hook|game_ptr_hook|ptr_slot")),
)

# 机制原语自身的实现/声明文件：其中的命中是机制定义而非资源安装点。
_MECHANISM_FILES = frozenset(
    {
        "patch/call_patch.cpp",
        "patch/call_patch.h",
        "patch/game_ptr_hook.h",
        "patch/ptr_slot.cpp",
        "patch/ptr_slot.h",
        "game/access/ptr_slots.cpp",
        "game/access/ptr_slots.h",
    }
)

# LSPosed hook 函数指针获取器自身的声明/定义（`NativeHookFunType native_hook_func();`
# 及 `native_hook_func() { return g_hook_func; }`），不是「某符号被安装」的登记。
_MECH_API_LINE = re.compile(r"\bNative(Hook|Unhook)FunType\s+(?:native_)?(?:un)?hook_func\s*\(")

# 机制原语注入：host 装配期取 call_patch 原语地址交给 patch/ 层（`&call_patch_install_bl` 等）。
# 这是把机制本体注入机制层，不是某个游戏符号的安装点；若无此排除会记为无法归属资源。
# `ptr_slot_site(` 同理：host/plugin_registry 按 kPtrSlot 声明解析槽站点，是机制分派而非安装点
# （真实安装点在插件的 ResourceClaim，文本扫描无法识别该声明式形态）。
_MECH_WIRING_LINE = re.compile(
    r"&\s*call_patch_(?:install|revert)_(?:bl|word|branch)\b"
    r"|&\s*(?:qol::patch::)?ptr_slot_(?:write|restore)_raw\b"
    r"|\bptr_slot_site\s*\("
)

_SKIP_LINE_PREFIXES = ("//", "*", "/*", "#include")
_SYMBOL_STRING = re.compile(r'fn_resolve\(\s*"(F_[A-Z0-9_]+_VMA)"')
_SYMBOL_BARE = re.compile(r"\b(F_[A-Z0-9_]+_VMA)\b")
_WINDOW = 25
UNKNOWN_SYMBOL = "<unknown>"
UNKNOWN_DETAIL = "无法归属符号"


@dataclass(frozen=True)
class InstallPoint:
    mechanism: str
    rel: str
    line: int
    symbol: str


# --------------------------------------------------------------------------
# 扫描
# --------------------------------------------------------------------------


def _extract_symbol(lines: list[str], index: int) -> str:
    """在命中行前后各 _WINDOW 行内由近及远抽取 F_XXX_VMA 符号。"""
    lo = max(0, index - _WINDOW)
    hi = min(len(lines) - 1, index + _WINDOW)
    order = list(range(index, lo - 1, -1)) + list(range(index + 1, hi + 1))
    for line_no in order:
        match = _SYMBOL_STRING.search(lines[line_no])
        if match:
            return match.group(1)
    for line_no in order:
        match = _SYMBOL_BARE.search(lines[line_no])
        if match:
            return match.group(1)
    return UNKNOWN_SYMBOL


def scan() -> list[InstallPoint]:
    points: list[InstallPoint] = []
    for rel, path in ac.iter_sources():
        if rel in _MECHANISM_FILES:
            continue
        lines = ac.read_text(rel, path).splitlines()
        for index, raw in enumerate(lines):
            stripped = raw.strip()
            if not stripped or stripped.startswith(_SKIP_LINE_PREFIXES):
                continue
            if _MECH_API_LINE.search(raw) or _MECH_WIRING_LINE.search(raw):
                continue
            for mechanism, pattern in _MECHANISMS:
                if pattern.search(raw):
                    points.append(
                        InstallPoint(mechanism, rel, index + 1, _extract_symbol(lines, index))
                    )
    return points


# --------------------------------------------------------------------------
# 违规
# --------------------------------------------------------------------------


def build_violations(points: list[InstallPoint]) -> list[ac.Violation]:
    violations: list[ac.Violation] = []
    owners: dict[tuple[str, str], set[str]] = {}
    for point in points:
        if point.symbol == UNKNOWN_SYMBOL:
            violations.append(
                ac.Violation(
                    "resource_unknown_symbol",
                    f"{point.rel}:{point.line}",
                    "",
                    f"{point.mechanism} {UNKNOWN_DETAIL}",
                )
            )
            continue
        owners.setdefault((point.mechanism, point.symbol), set()).add(point.rel)
    for (mechanism, symbol), files in owners.items():
        if len(files) < 2:
            continue
        detail = f"{mechanism}|{symbol} 被 {len(files)} 个文件安装"
        for rel in sorted(files):
            violations.append(ac.Violation("resource_duplicate_owner", rel, "", detail))
    return violations


# --------------------------------------------------------------------------
# 摘要与清单输出
# --------------------------------------------------------------------------


def _mechanism_stats(points: list[InstallPoint]) -> dict[str, dict[str, int]]:
    stats: dict[str, dict[str, int]] = {
        mechanism: {"files": 0, "points": 0, "symbols": 0} for mechanism, _ in _MECHANISMS
    }
    files: dict[str, set[str]] = {mechanism: set() for mechanism, _ in _MECHANISMS}
    symbols: dict[str, set[str]] = {mechanism: set() for mechanism, _ in _MECHANISMS}
    for point in points:
        stats[point.mechanism]["points"] += 1
        files[point.mechanism].add(point.rel)
        symbols[point.mechanism].add(point.symbol)
    for mechanism, _ in _MECHANISMS:
        stats[mechanism]["files"] = len(files[mechanism])
        stats[mechanism]["symbols"] = len(symbols[mechanism])
    return stats


def _unique_resources(points: list[InstallPoint]) -> list[tuple[str, str, list[str]]]:
    owners: dict[tuple[str, str], set[str]] = {}
    for point in points:
        if point.symbol == UNKNOWN_SYMBOL:
            continue
        owners.setdefault((point.mechanism, point.symbol), set()).add(point.rel)
    return [(mechanism, symbol, sorted(files)) for (mechanism, symbol), files in sorted(owners.items())]


def build_extra_lines(points: list[InstallPoint], verbose: bool) -> list[str]:
    stats = _mechanism_stats(points)
    unknown = [p for p in points if p.symbol == UNKNOWN_SYMBOL]
    resources = _unique_resources(points)
    duplicates = [item for item in resources if len(item[2]) > 1]
    total_files = len({p.rel for p in points})
    ratio = len(unknown) / len(points) if points else 0.0

    lines = [
        f"  安装点 {len(points)} 个 / 文件 {total_files} 个 / 去重符号 "
        f"{len({p.symbol for p in points if p.symbol != UNKNOWN_SYMBOL})} 个 / "
        f"无法归属 {len(unknown)} 个（{ratio:.0%}）",
    ]
    for mechanism, _ in _MECHANISMS:
        stat = stats[mechanism]
        lines.append(
            f"  {mechanism}: {stat['files']} 文件 / {stat['points']} 安装点 / {stat['symbols']} 符号"
        )
    lines.append(f"  重复安装资源 {len(duplicates)} 个（机制|符号 → 安装文件）：")
    for mechanism, symbol, files in duplicates:
        lines.append(f"    {mechanism}|{symbol} → {'、'.join(files)}")
    if not duplicates:
        lines.append("    （无）")
    if verbose:
        lines.append(f"  去重资源清单 {len(resources)} 条：")
        for mechanism, symbol, files in resources:
            lines.append(f"    {mechanism}|{symbol} → {'、'.join(files)}")
    return lines


def _inventory_payload(points: list[InstallPoint]) -> dict:
    stats = _mechanism_stats(points)
    unknown = [p for p in points if p.symbol == UNKNOWN_SYMBOL]
    resources = _unique_resources(points)
    duplicates = [
        {"mechanism": mechanism, "symbol": symbol, "files": files}
        for mechanism, symbol, files in resources
        if len(files) > 1
    ]
    ordered = sorted(points, key=lambda p: (p.mechanism, p.rel, p.line))
    return {
        "generated_by": "scripts/maintenance/check_resources.py",
        "counts": {
            "points": len(points),
            "files": len({p.rel for p in points}),
            "symbols": len({p.symbol for p in points if p.symbol != UNKNOWN_SYMBOL}),
            "unknown": len(unknown),
        },
        "mechanisms": {mechanism: stats[mechanism] for mechanism, _ in _MECHANISMS},
        "install_points": [
            {
                "mechanism": p.mechanism,
                "file": p.rel,
                "line": p.line,
                "symbol": p.symbol,
            }
            for p in ordered
        ],
        "unique_resources": [
            {"mechanism": mechanism, "symbol": symbol, "files": files}
            for mechanism, symbol, files in resources
        ],
        "duplicate_owners": duplicates,
    }


def write_inventory(target_arg: str, points: list[InstallPoint]) -> bool:
    target = Path(target_arg)
    if not target.is_absolute():
        target = Path.cwd() / target
    resolved = target.resolve()
    root = ac.ROOT.resolve()
    if not resolved.is_relative_to(root):
        print(f"错误：--inventory 路径必须位于项目内（{root}）：{resolved}", file=sys.stderr)
        return False
    try:
        resolved.parent.mkdir(parents=True, exist_ok=True)
        resolved.write_text(
            json.dumps(_inventory_payload(points), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        print(f"错误：写入清单失败 {resolved}: {exc}", file=sys.stderr)
        return False
    print(f"  清单已写出：{resolved}")
    return True


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze", action="store_true", help="把当前违规写入基线快照（只允许收缩）")
    parser.add_argument(
        "--force-freeze",
        action="store_true",
        help="允许在冻结时扩容基线（仅用于首次建立或经评审的有意变更）",
    )
    parser.add_argument("--json", action="store_true", help="额外输出 JSON 汇总")
    parser.add_argument("--verbose", "-v", action="store_true", help="打印完整去重资源清单")
    parser.add_argument(
        "--inventory",
        metavar="PATH",
        default=None,
        help="把完整安装点清单写成 JSON（仅显式传参时写出；路径必须位于项目内）",
    )
    args = parser.parse_args()

    points = scan()
    violations = build_violations(points)
    extra_lines = build_extra_lines(points, args.verbose)
    if args.inventory is not None and not write_inventory(args.inventory, points):
        return 2
    return ac.emit(
        "check_resources",
        violations,
        args,
        "资源安装点与重复安装检测",
        extra_lines,
        rules=("resource_duplicate_owner", "resource_unknown_symbol"),
    )


if __name__ == "__main__":
    sys.exit(main())
