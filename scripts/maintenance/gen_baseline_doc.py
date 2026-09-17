#!/usr/bin/env python3
"""现状基线清单文档生成器（重构执行期的机器事实快照）。

把分散的机器事实源汇总成一份**确定性**的 Markdown 现状清单，避免手工维护形成双重事实源。
本脚本只读既有事实源、不修改它们：

- `scripts/maintenance/arch_baseline.json`   基线棘轮快照（`violations` 字符串数组）
- `scripts/maintenance/arch_common.py`       层映射、include 图、分类（同源 `Check` 脚本）
- `scripts/maintenance/check_resources.py`   资源安装点清单（`--inventory` 写出 JSON）
- `scripts/maintenance/check_jni.py`         JNI 面差异（`--json` 输出尾部 JSON 汇总）

生成结果不含时间戳，所有列表排序，重复运行字节一致。

用法：
    uv run python scripts/maintenance/gen_baseline_doc.py            # 写出 docs/refactor/baseline.md
    uv run python scripts/maintenance/gen_baseline_doc.py --check    # 只校验一致，不一致 rc=2
    uv run python scripts/maintenance/gen_baseline_doc.py --verbose  # 列全部去重资源清单
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import arch_common as ac  # noqa: E402

SCRIPT_NAME = Path(__file__).name
DEFAULT_OUTPUT = ac.ROOT / "docs" / "refactor" / "baseline.md"
TMP_DIR = ac.ROOT / ".tmp" / "gen_baseline_doc"
INVENTORY_PATH = TMP_DIR / "resource_inventory.json"

# 数据来源清单（文档头部展示；路径相对仓库根）。
DATA_SOURCES = (
    "scripts/maintenance/arch_baseline.json",
    "scripts/maintenance/arch_common.py",
    "scripts/maintenance/check_layers.py",
    "scripts/maintenance/check_resources.py",
    "scripts/maintenance/check_jni.py",
    "module/app/src/main/cpp/feature/*/",
)

MECHANISM_ORDER = ("entry hook", "call patch", "ptr slot")
CROSS_LAYER_TOP = 15
BASELINE_SAMPLE = 20
JNI_SAMPLE = 20
SYMBOL_SAMPLE = 30


# --------------------------------------------------------------------------
# 数据收集
# --------------------------------------------------------------------------


def _run_python(script_name: str, *extra: str) -> subprocess.CompletedProcess[str]:
    script = HERE / script_name
    return subprocess.run(
        [sys.executable, str(script), *extra],
        cwd=str(ac.ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def collect_layer_counts(graph: ac.IncludeGraph) -> tuple[Counter, int]:
    counts = Counter(graph.layer(rel) for rel in graph.paths)
    return counts, len(graph.paths)


def collect_cross_edges(graph: ac.IncludeGraph) -> list[tuple[tuple[str, str], int]]:
    edges = Counter(
        (graph.layer(rel), graph.layer(target))
        for rel in graph.paths
        for target in graph.includes_of(rel)
        if graph.layer(rel) not in ("unknown", "tests")
        and graph.layer(target) not in ("unknown", "tests")
    )
    cross = [((src, dst), count) for (src, dst), count in edges.items() if src != dst]
    cross.sort(key=lambda item: (-item[1], item[0]))
    return cross


def collect_cycles(graph: ac.IncludeGraph) -> list[list[str]]:
    return sorted((sorted(component) for component in graph.sccs()), key=lambda c: c[0])


def collect_baseline() -> dict[str, list[str]]:
    data = json.loads(ac.BASELINE_PATH.read_text(encoding="utf-8"))
    by_rule: dict[str, list[str]] = defaultdict(list)
    for key in data.get("violations", []):
        by_rule[key.split("|", 1)[0]].append(key)
    return {rule: sorted(items) for rule, items in by_rule.items()}


def collect_resources() -> dict:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    try:
        result = _run_python("check_resources.py", "--inventory", str(INVENTORY_PATH))
        if not INVENTORY_PATH.exists():
            raise RuntimeError(
                "check_resources.py 未写出安装点清单："
                + (result.stderr.strip() or result.stdout.strip())
            )
        return json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    finally:
        INVENTORY_PATH.unlink(missing_ok=True)


def collect_jni() -> dict:
    result = _run_python("check_jni.py", "--json")
    stdout = result.stdout
    counts = re.search(r"Kotlin external=(\d+)，native 导出=(\d+)", stdout)
    if counts is None:
        raise RuntimeError("check_jni.py 输出缺少 external/native 计数：" + stdout)
    payload_line = next((line for line in reversed(stdout.splitlines()) if line.startswith("{")), None)
    if payload_line is None:
        raise RuntimeError("check_jni.py --json 输出缺少 JSON 汇总：" + stdout)
    payload = json.loads(payload_line)
    return {
        "externals": int(counts.group(1)),
        "exports": int(counts.group(2)),
        "total": int(payload.get("total", 0)),
        "new": list(payload.get("new", [])),
    }


def collect_features(graph: ac.IncludeGraph) -> list[tuple[str, str, int]]:
    counts: Counter[str] = Counter()
    for rel in graph.paths:
        parts = rel.split("/")
        if rel.startswith("feature/") and len(parts) > 2:
            counts[parts[1]] += 1
    out: list[tuple[str, str, int]] = []
    for name in sorted(counts):
        layer = ""
        for rel in sorted(graph.paths):
            if rel.startswith(f"feature/{name}/"):
                layer = graph.layer(rel)
                break
        out.append((name, layer, counts[name]))
    return out


# --------------------------------------------------------------------------
# 渲染
# --------------------------------------------------------------------------


def _bullet_map(title: str, mapping: dict[str, str], current: str, target: str) -> list[str]:
    lines = [f"**{title}**", "", f"| {current} | {target} |", "| --- | --- |"]
    for key in sorted(mapping):
        lines.append(f"| `{key}` | {mapping[key]} |")
    lines.append("")
    return lines


def render_header() -> list[str]:
    lines = [
        "<!-- 本文件由脚本生成，禁止手工编辑。 -->",
        "",
        "# 重构现状基线清单",
        "",
        "本文由脚本生成，**禁止手工编辑**；手工改动会在 `--check` 下被判定为不一致。",
        "",
        f"- 生成命令：`uv run python scripts/maintenance/{SCRIPT_NAME}`",
        f"- 一致性校验：`uv run python scripts/maintenance/{SCRIPT_NAME} --check`",
        "- 机器事实源：",
    ]
    lines += [f"  - `{path}`" for path in DATA_SOURCES]
    lines.append("")
    return lines


def render_mapping() -> list[str]:
    lines = ["## 1. 层分布与迁移期映射", "", "### 1.1 层分布", ""]
    graph = ac.IncludeGraph(ac.iter_sources())
    counts, total = collect_layer_counts(graph)
    lines += ["| 目标层 | 文件数 |", "| --- | --- |"]
    for layer in sorted(counts):
        lines.append(f"| `{layer}` | {counts[layer]} |")
    lines += [f"| **合计** | **{total}** |", ""]

    lines += ["### 1.2 迁移期映射摘要", ""]
    lines += [
        "映射常量直接 `import arch_common` 读取私有表（同目录模块，脚本只读、不修改该库），",
        "避免在本文档复制一份映射形成第二事实源。",
        "",
    ]
    lines += _bullet_map(
        "新架构目标目录（`arch_common._TARGET_PREFIXES`，按序匹配）",
        {prefix: layer for prefix, layer in ac._TARGET_PREFIXES},
        "当前路径前缀",
        "目标层",
    )
    lines += _bullet_map(
        "重构期根目录覆盖（`arch_common._ROOT_OVERRIDES`）",
        ac._ROOT_OVERRIDES,
        "当前文件（cpp 根）",
        "目标层",
    )
    lines += _bullet_map(
        "重构期 `core/native/` 覆盖（`arch_common._CORE_OVERRIDES`）",
        ac._CORE_OVERRIDES,
        "当前文件名",
        "目标层",
    )
    lines += _bullet_map(
        "重构期 `api/native/` 文件名前缀 → 域（`arch_common._DOMAIN_BY_STEM`）",
        {f"{stem}*": f"domains/{domain}" for stem, domain in ac._DOMAIN_BY_STEM.items()},
        "当前文件名前缀",
        "目标层",
    )
    lines += [
        "> 说明：`_TARGET_PREFIXES` 为有序元组，`features/` 必须先于 `feature/` 匹配；",
        "> 其余无法映射的路径落入 `unknown`（由 `check_layers` 以警告上报）。",
        "",
    ]
    return lines


def render_dependencies(graph: ac.IncludeGraph) -> list[str]:
    lines = ["## 2. 跨层依赖与 include 环", "", "### 2.1 跨层依赖对 Top 15", ""]
    cross = collect_cross_edges(graph)
    lines += [f"跨层依赖对共 {len(cross)} 个；下表按计数降序、同计数按层名升序取前 {CROSS_LAYER_TOP} 个。", ""]
    if cross:
        lines += ["| 源层 | 目标层 | 计数 |", "| --- | --- | --- |"]
        for (src, dst), count in cross[:CROSS_LAYER_TOP]:
            lines.append(f"| `{src}` | `{dst}` | {count} |")
    else:
        lines.append("（无）")
    lines.append("")

    cycles = collect_cycles(graph)
    lines += ["### 2.2 include 环", ""]
    if cycles:
        for component in cycles:
            lines.append(f"- {len(component)} 文件成环：" + "、".join(f"`{rel}`" for rel in component))
        lines.append("")
    else:
        lines += ["当前无 include 环。", ""]
    return lines


def render_baseline() -> list[str]:
    groups = collect_baseline()
    total = sum(len(items) for items in groups.values())
    lines = ["## 3. 基线债务", "", f"基线共 {len(groups)} 组、{total} 条。", ""]
    if not groups:
        lines += ["（基线为空）", ""]
    for rule in sorted(groups):
        items = groups[rule]
        lines += [f"### {rule}（{len(items)} 条）", ""]
        for key in items[:BASELINE_SAMPLE]:
            lines.append(f"- `{key}`")
        if len(items) > BASELINE_SAMPLE:
            lines.append(f"- …（其余 {len(items) - BASELINE_SAMPLE} 条省略）")
        lines.append("")
    lines += ["基线只允许收缩，新增违规即失败", ""]
    return lines


def render_resources(inventory: dict, verbose: bool) -> list[str]:
    counts = inventory.get("counts", {})
    mechanisms = inventory.get("mechanisms", {})
    lines = [
        "## 4. 资源安装点清单",
        "",
        f"扫描合计：安装点 {counts.get('points', 0)} 个 / 文件 {counts.get('files', 0)} 个 / "
        f"去重符号 {counts.get('symbols', 0)} 个 / 无法归属 {counts.get('unknown', 0)} 个。",
        "",
        "| 机制 | 文件数 | 安装点数 | 去重符号数 |",
        "| --- | --- | --- | --- |",
    ]
    for mechanism in MECHANISM_ORDER:
        stat = mechanisms.get(mechanism, {})
        lines.append(
            f"| {mechanism} | {stat.get('files', 0)} | {stat.get('points', 0)} | {stat.get('symbols', 0)} |"
        )
    lines.append("")

    symbols: dict[str, set[str]] = defaultdict(set)
    for item in inventory.get("unique_resources", []):
        symbols[item["symbol"]].update(item.get("files", []))
    ordered = sorted(symbols)
    limit = len(ordered) if verbose else SYMBOL_SAMPLE
    if verbose:
        lines += [f"去重符号 → 文件（全部 {len(ordered)} 条）：", ""]
    else:
        lines += [f"去重符号 → 文件（前 {min(limit, len(ordered))} 条，共 {len(ordered)} 条；`--verbose` 列全部）：", ""]
    for symbol in ordered[:limit]:
        files = "、".join(f"`{rel}`" for rel in sorted(symbols[symbol]))
        lines.append(f"- `{symbol}` → {files}")
    lines.append("")
    return lines


def render_jni(jni: dict) -> list[str]:
    lines = [
        "## 5. JNI 面",
        "",
        f"- Kotlin `external fun`：{jni['externals']} 个",
        f"- native 导出：{jni['exports']} 个",
        f"- 差异条数：{jni['total']} 条",
        "",
    ]
    samples = sorted(jni["new"])
    lines += [f"差异样例（前 {min(JNI_SAMPLE, len(samples))} 条，共 {len(samples)} 条）：", ""]
    if samples:
        for key in samples[:JNI_SAMPLE]:
            lines.append(f"- `{key}`")
    else:
        lines.append("（无）")
    lines.append("")
    return lines


def render_features(graph: ac.IncludeGraph) -> list[str]:
    features = collect_features(graph)
    lines = ["## 6. 功能域清单", "", f"`feature/*` 共 {len(features)} 个域。", ""]
    lines += ["| 域 | 目标层 | 文件数 |", "| --- | --- | --- |"]
    for name, layer, count in features:
        lines.append(f"| `{name}` | `{layer}` | {count} |")
    lines.append("")
    return lines


def generate(verbose: bool) -> str:
    graph = ac.IncludeGraph(ac.iter_sources())
    lines: list[str] = []
    lines += render_header()
    lines += render_mapping()
    lines += render_dependencies(graph)
    lines += render_baseline()
    lines += render_resources(collect_resources(), verbose)
    lines += render_jni(collect_jni())
    lines += render_features(graph)
    lines += [
        "## 校验入口",
        "",
        "```",
        "uv run python scripts/maintenance/check_arch.py",
        "```",
    ]
    while lines and not lines[-1].strip():  # 末尾恰好一个换行，避免 git diff --check 报 EOF 空行
        lines.pop()
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只校验生成结果与磁盘一致（不一致返回 2）")
    parser.add_argument("--verbose", "-v", action="store_true", help="列出全部去重资源清单")
    args = parser.parse_args()

    content = generate(args.verbose)

    if args.check:
        if not DEFAULT_OUTPUT.exists():
            print(f"不一致：{DEFAULT_OUTPUT} 不存在", file=sys.stderr)
            return 2
        existing = DEFAULT_OUTPUT.read_text(encoding="utf-8")
        if existing != content:
            print(
                f"不一致：{DEFAULT_OUTPUT} 与生成结果不同（请运行 "
                f"`uv run python scripts/maintenance/{SCRIPT_NAME}` 重新生成）",
                file=sys.stderr,
            )
            return 2
        print(f"一致：{DEFAULT_OUTPUT}")
        return 0

    DEFAULT_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    DEFAULT_OUTPUT.write_text(content, encoding="utf-8")
    print(f"已写出：{DEFAULT_OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
