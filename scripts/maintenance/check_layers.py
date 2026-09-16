#!/usr/bin/env python3
"""层依赖方向与 include 环校验（ARCHITECTURE.md §2.2 依赖矩阵 / §9.2）。

规则：
- layer_direction：源文件所属层必须允许依赖目标文件所属层（矩阵见 arch_common.allows）；
- include_cycle：已映射层的文件之间不得存在 include 环（SCC > 1）；
- unknown_layer（警告，不失败）：扫描范围内无法映射到目标层的文件。

用法：
    uv run python scripts/maintenance/check_layers.py            # 棘轮校验
    uv run python scripts/maintenance/check_layers.py --freeze   # 冻结基线（只允许收缩）
    uv run python scripts/maintenance/check_layers.py -v --json  # 明细 + JSON 汇总
"""
from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arch_common as ac  # noqa: E402


def main() -> int:
    args = ac.parse_common_args(__doc__ or "层依赖校验")

    graph = ac.IncludeGraph(ac.iter_sources())
    violations: list[ac.Violation] = []

    for rel in sorted(graph.paths):
        src_layer = graph.layer(rel)
        if src_layer == "unknown":
            violations.append(ac.Violation("unknown_layer", rel, "", "无法映射到目标层"))
            continue
        for target in graph.includes_of(rel):
            dst_layer = graph.layer(target)
            if dst_layer in ("unknown", "tests"):
                continue
            if ac.allows(src_layer, dst_layer):
                continue
            violations.append(
                ac.Violation("layer_direction", rel, target, f"{src_layer} → {dst_layer}")
            )

    for component in graph.sccs():
        violations.append(
            ac.Violation(
                "include_cycle",
                " ".join(sorted(component)),
                "",
                f"{len(component)} 文件成环",
            )
        )

    layer_counts = Counter(graph.layer(rel) for rel in graph.paths)
    edge_counts = Counter(
        (graph.layer(rel), graph.layer(target))
        for rel in graph.paths
        for target in graph.includes_of(rel)
        if graph.layer(target) not in ("unknown", "tests") and graph.layer(rel) != "unknown"
    )
    summary = [
        f"  文件 {len(graph.paths)} 个；层分布："
        + "、".join(f"{layer}={count}" for layer, count in sorted(layer_counts.items())),
        "  跨层依赖 Top10：",
    ]
    for (src, dst), count in edge_counts.most_common(10):
        if src != dst:
            summary.append(f"    {src} → {dst}: {count}")

    return ac.emit(
        "check_layers",
        violations,
        args,
        "层依赖方向与 include 环",
        summary,
        rules=("layer_direction", "include_cycle", "unknown_layer"),
    )


if __name__ == "__main__":
    sys.exit(main())
