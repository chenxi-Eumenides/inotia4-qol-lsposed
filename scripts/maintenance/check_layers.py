#!/usr/bin/env python3
"""层依赖方向与 include 环校验（ARCHITECTURE.md §2.2 依赖矩阵 / §9.2）。

规则：
- layer_direction：源文件所属层必须允许依赖目标文件所属层（矩阵见 arch_common.allows）；
- plugin_boundary：插件只能 include `contracts/` 或同一插件目录；
- include_cycle：已映射层的文件之间不得存在 include 环（SCC > 1）；
- unknown_layer：七个目标目录下无法映射的文件硬失败；
- unknown_legacy_path（警告，不失败）：legacy 路径无法映射时只提示迁移债。

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
            root = rel.split("/", 1)[0]
            rule = "unknown_layer" if root in ac.BASE_LAYERS else "unknown_legacy_path"
            detail = "七个目标目录下无法映射到目标层" if rule == "unknown_layer" else "legacy 路径无法映射"
            violations.append(ac.Violation(rule, rel, "", detail))
            continue
        for target in graph.includes_of(rel):
            dst_layer = graph.layer(target)
            if dst_layer in ("unknown", "tests"):
                continue
            if src_layer.startswith("plugins/") and not (
                dst_layer == "contracts" or dst_layer == src_layer
            ):
                violations.append(
                    ac.Violation(
                        "plugin_boundary",
                        rel,
                        target,
                        f"{src_layer} 只能依赖 contracts 或自身目录，实际为 {dst_layer}",
                    )
                )
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
        rules=("layer_direction", "plugin_boundary", "include_cycle", "unknown_layer", "unknown_legacy_path"),
    )


if __name__ == "__main__":
    sys.exit(main())
