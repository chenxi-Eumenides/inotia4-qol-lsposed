#!/usr/bin/env python3
"""裸地址与符号头越权校验（ARCHITECTURE.md §5「只有 game/ 允许 include game/symbols/* 并出现十六进制地址」）。

规则：
- offsets_symbols_header（结构性阻止）：非 game 层的文件禁止 include
  `game_symbols.h` / `symbol_registry.h` / `symbol_resolver.h`；
- offsets_bare_address（文本扫描）：非 game 层的源码行禁止出现明显裸地址——
  1. `g_base` / `image_base` / `base` 后跟 `+ 0x...` 的十六进制字面量；
  2. `F_/G_/C_/I_/O_/S_/M_` 前缀且名称含 `VMA`/`ADDR`/`OFFSET` 的常量被赋值为十六进制；
  3. 名称含 `VMA`/`ADDR`/`ADDRESS`/`OFFSET`（大写 token）的标识符被赋值为 ≥4 位十六进制字面量。

能力边界（本脚本只能做补充文本扫描，不能替代结构性约束）：
- 宏展开（预处理器）产生的地址、由多个变量间接计算的地址、字符串拼接/编码后的地址无法覆盖；
- 只扫描去掉 `//` 行注释后的代码；块注释 `/* ... */` 未剥离，跨行拼接的表达式不解析；
- 关键词按大写 token 匹配以避免 `kWh4FnvOffsetBasis` 之类哈希常量误报；
- 仅覆盖「明显裸地址」，覆盖不到的越权由 game 层的符号头包含关系（规则 1）结构性阻断。

用法：
    uv run python scripts/maintenance/check_offsets.py            # 棘轮校验
    uv run python scripts/maintenance/check_offsets.py --freeze   # 冻结基线（只允许收缩）
    uv run python scripts/maintenance/check_offsets.py -v --json  # 明细 + JSON 汇总
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arch_common as ac  # noqa: E402

# 规则 1：符号头的 basename（game 层内部互相引用允许）
_SYMBOL_HEADERS = frozenset({"game_symbols.h", "symbol_registry.h", "symbol_resolver.h"})

# 规则 2.1：基址加十六进制字面量
_BASE_ADD_RE = re.compile(r"\b(?:g_base|image_base|base)\s*\+\s*0x[0-9a-fA-F]+")

# 规则 2.2：宏风格 VMA/ADDR/OFFSET 常量定义（前缀 F_/G_/C_/I_/O_/S_/M_）
_VMA_CONST_RE = re.compile(
    r"\b[FGCIOSM]_[A-Z0-9_]*(?:VMA|ADDR|ADDRESS|OFFSET)[A-Z0-9_]*\s*=\s*0x[0-9a-fA-F]+"
)

# 规则 2.3：名称含地址关键词的标识符被赋值为 ≥4 位十六进制
_ASSIGN_HEX_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x[0-9a-fA-F]{4,}")
_ADDR_KEYWORDS = ("VMA", "ADDR", "ADDRESS", "OFFSET")


def _scan_line(line: str) -> str | None:
    """返回该行命中的裸地址原文片段；无命中返回 None。"""
    code = line.split("//", 1)[0]
    match = _BASE_ADD_RE.search(code)
    if match is not None:
        return match.group(0)
    match = _VMA_CONST_RE.search(code)
    if match is not None:
        return match.group(0)
    for match in _ASSIGN_HEX_RE.finditer(code):
        if any(keyword in match.group(1) for keyword in _ADDR_KEYWORDS):
            return match.group(0)
    return None


def main() -> int:
    args = ac.parse_common_args(__doc__ or "裸地址与符号头越权校验")

    graph = ac.IncludeGraph(ac.iter_sources())
    violations: list[ac.Violation] = []

    for rel in sorted(graph.paths):
        src_layer = graph.layer(rel)

        # 规则 1：非 game 层禁止 include 符号头
        if src_layer != "game":
            for target in graph.includes_of(rel):
                if Path(target).name in _SYMBOL_HEADERS:
                    violations.append(
                        ac.Violation(
                            "offsets_symbols_header", rel, target, f"{src_layer} 层引用符号头"
                        )
                    )

        # 规则 2：非 game 层禁止明显裸地址
        if src_layer == "game":
            continue
        text = ac.read_text(rel, graph.paths[rel])
        for lineno, line in enumerate(text.splitlines(), start=1):
            snippet = _scan_line(line)
            if snippet is not None:
                violations.append(
                    ac.Violation("offsets_bare_address", rel, "", f"L{lineno}: {snippet}")
                )

    game_files = sum(1 for rel in graph.paths if graph.layer(rel) == "game")
    hit_by_layer = Counter(
        graph.layer(v.src) for v in violations if graph.layer(v.src) != "game"
    )
    extra_lines = [
        f"  game 层豁免文件 {game_files} 个；命中 "
        + ("、".join(f"{layer}={count}" for layer, count in sorted(hit_by_layer.items())) or "无")
    ]

    return ac.emit(
        "check_offsets",
        violations,
        args,
        "裸地址与符号头越权",
        extra_lines,
        rules=("offsets_symbols_header", "offsets_bare_address"),
    )


if __name__ == "__main__":
    sys.exit(main())
