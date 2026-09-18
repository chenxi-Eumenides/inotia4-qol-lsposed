#!/usr/bin/env python3
"""新架构实现地图的生成统计区维护脚本。

只重写 `docs/refactor/map.md` 的 markers 区间：

    <!-- BEGIN GENERATED:stats --> … <!-- END GENERATED:stats -->

人工导航区、过渡依赖区与验证附录原样保留（本脚本不读也不改）。统计区数据源：

- `module/app/src/main/cpp/{contracts,game,infra,hooks,patch,domains,plugins,host}`
  新架构目录的文件数与行数（`wc -l` 口径，含空行与注释；不含 `bridge/`、`tests/` 与 legacy 路径）
- `module/app/src/main/cpp/plugins/*`  已迁移插件
- `module/app/src/main/cpp/tests/CMakeLists.txt`  CTest target 总数与分类

CTest 分类规则（显式映射，不按源文件路径推断）：
    `MIGRATED_TEST_GROUPS` 白名单以 `tests/` 下的测试源文件（去扩展名）列出新架构/已迁移
    target，分「机制/宿主」与「已迁移功能」两类；其余 target 一律归 legacy。判据是测试对象
    归属，而非 target 编译的完整源文件集合——host/plugin 类 target 会夹带 legacy 依赖
    （如 `core/native/qol_log.cpp`），按路径启发式会被误判。

生成结果不含时间戳，列表排序，重复运行字节一致。

用法：
    uv run python scripts/maintenance/gen_map_doc.py          # 重写 markers 区间
    uv run python scripts/maintenance/gen_map_doc.py --check  # 只校验一致，不一致 rc=2
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import arch_common as ac  # noqa: E402

SCRIPT_NAME = Path(__file__).name
DEFAULT_OUTPUT = ac.ROOT / "docs" / "refactor" / "map.md"
TESTS_CMAKE = ac.CPP_ROOT / "tests" / "CMakeLists.txt"
PLUGINS_DIR = ac.CPP_ROOT / "plugins"

BEGIN_MARKER = "<!-- BEGIN GENERATED:stats -->"
END_MARKER = "<!-- END GENERATED:stats -->"

# 新架构顶层目录（ARCHITECTURE.md §0：八个顶层目录，plugins/ 单层，ADR-0004）。
NEW_ARCH_DIRS = ("contracts", "game", "infra", "hooks", "patch", "domains", "plugins", "host")

# --------------------------------------------------------------------------
# CTest 分类白名单（显式映射）
# --------------------------------------------------------------------------
#
# 键为 `tests/` 下测试源文件名（去 `.cpp` 后缀）。分类依据是测试对象归属，不按 target
# 编译的完整源文件路径集合推断：host/plugin 类 target 会夹带 legacy 依赖
# （如 `core/native/qol_log.cpp`），按路径启发式会被误判为 legacy。
#
# 反例：`test_infra_data` 的测试对象是 `core/native/frame_task` 与 `data/native/item_class`，
# 仍属 legacy，故不进入映射。
MECHANISM_HOST_TESTS: tuple[str, ...] = (
    "test_infra_snapshot",
    "test_infra_command_queue",
    "test_infra_events",
    "test_infra_capabilities_status",
    "test_infra_config_commands",
    "test_infra_scheduler",
    "test_hooks_registry",
    "test_hooks_transaction",
    "test_hooks_dispatch",
    "test_patch_layer",
    "test_plugin_registry",
    "test_plugin_lifecycle",
    "test_capability_lifecycle",
    "test_hooks_popup_state",
    "test_store_sell_rules",
    "test_infra_store",
)

MIGRATED_FEATURE_TESTS: tuple[str, ...] = (
    "test_simple_mode_rules",
    "test_push_all_rules",
    "test_attribute_range",
    "test_module_text",
    "test_special_equip_rules",
    "test_autosell_rules",
    "test_autosell_store",
    "test_save_warehouse_validate",
    "test_custom_recipe",
    "test_native_choice",
)

MIGRATED_TEST_GROUPS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("机制/宿主", MECHANISM_HOST_TESTS),
    ("已迁移功能", MIGRATED_FEATURE_TESTS),
)

# `tests/CMakeLists.txt` 里的源码根变量在预处理时替换为该哨兵，便于区分项目源文件与测试 harness。
_CPP_SENTINEL = "@CPP@"
_PROJECT_PREFIX = _CPP_SENTINEL + "/"


# --------------------------------------------------------------------------
# CMake 解析（仅识别 set / foreach / add_executable，不执行配置）
# --------------------------------------------------------------------------

_SET_RE = re.compile(r"\bset\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+([^)]*)\)")
_FOREACH_RE = re.compile(r"\bforeach\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+([^)]*)\)")
_ENDFOREACH_RE = re.compile(r"\bendforeach\(\s*\)")
_ADD_EXE_RE = re.compile(r"\badd_executable\(\s*([^\s()]*)([^)]*)\)")
_VAR_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def _strip_comments(text: str) -> str:
    return "\n".join(re.sub(r"#.*$", "", line) for line in text.splitlines())


def _expand_tokens(tokens: list[str], variables: dict[str, list[str]]) -> list[str]:
    out: list[str] = []
    for token in tokens:
        match = _VAR_RE.fullmatch(token)
        if match is not None and match.group(1) in variables:
            out.extend(variables[match.group(1)])
        else:
            out.append(token)
    return out


def _expand_name(name: str, variables: dict[str, list[str]]) -> str:
    for key, value in variables.items():
        name = name.replace("${" + key + "}", " ".join(value))
    return name


def _collect_executables(
    text: str, variables: dict[str, list[str]], targets: dict[str, list[str]]
) -> None:
    for match in _ADD_EXE_RE.finditer(text):
        name = _expand_name(match.group(1), variables).strip()
        args = _expand_tokens(match.group(2).split(), variables)
        sources = [arg for arg in args if arg.endswith((".cpp", ".cc", ".cxx"))]
        if name:
            targets[name] = sources


def parse_test_targets(cmake_path: Path) -> dict[str, list[str]]:
    """返回 {target 名: 源文件 token 列表}（foreach 展开，变量展开）。"""
    text = _strip_comments(cmake_path.read_text(encoding="utf-8"))
    text = text.replace("${CPP_DIR}", _CPP_SENTINEL)

    variables: dict[str, list[str]] = {}
    for match in _SET_RE.finditer(text):
        variables[match.group(1)] = match.group(2).split()

    targets: dict[str, list[str]] = {}
    remaining = text
    while True:
        match = _FOREACH_RE.search(remaining)
        if match is None:
            break
        end = _ENDFOREACH_RE.search(remaining, match.end())
        if end is None:
            raise SystemExit(f"{cmake_path}: foreach 未闭合")
        body = remaining[match.end(): end.start()]
        for item in match.group(2).split():
            _collect_executables(body.replace("${" + match.group(1) + "}", item), variables, targets)
        remaining = remaining[: match.start()] + remaining[end.end():]
    _collect_executables(remaining, variables, targets)
    return targets


def _test_source_stem(token: str) -> str | None:
    """裸 `test_*.cpp` 源文件的 stem；`${CPP_DIR}/**` 项目源与其它 token 返回 None。"""
    if token.startswith(_PROJECT_PREFIX):
        return None
    name = Path(token).name
    if not name.startswith("test_") or not name.endswith((".cpp", ".cc", ".cxx")):
        return None
    return name.rsplit(".", 1)[0]


def build_test_source_index(targets: dict[str, list[str]]) -> dict[str, list[str]]:
    """{测试源文件 stem: [编译它的 target]}，用于把白名单映射回 target 名。"""
    index: dict[str, list[str]] = {}
    for target, sources in targets.items():
        for token in sources:
            stem = _test_source_stem(token)
            if stem is not None:
                index.setdefault(stem, []).append(target)
    return index


def classify_targets(targets: dict[str, list[str]]) -> dict[str, str]:
    """返回 {target: 类别}；未列入白名单的 target 不在结果中（归 legacy）。

    自检失败直接抛 `SystemExit`（非零退出），不生成可能错误的分类结果：
    1. 白名单中的每个测试源文件必须出现在 `tests/CMakeLists.txt` 的某个 target；
    2. 同名测试源不得同时出现在两类；
    3. 不同测试源不得把同一 target 归入不同类别。
    """
    index = build_test_source_index(targets)
    mapped = [stem for _, stems in MIGRATED_TEST_GROUPS for stem in stems]
    duplicates = sorted({stem for stem in mapped if mapped.count(stem) > 1})
    if duplicates:
        raise SystemExit(f"分类映射错误：测试源同时出现在两类：{'、'.join(duplicates)}")

    classified: dict[str, str] = {}
    for group, stems in MIGRATED_TEST_GROUPS:
        for stem in stems:
            matches = index.get(stem)
            if not matches:
                raise SystemExit(
                    f"分类映射错误：{stem}.cpp 不在 tests/CMakeLists.txt 的任何 target 中"
                )
            if len(matches) > 1:
                raise SystemExit(
                    f"分类映射错误：{stem}.cpp 命中多个 target：{'、'.join(sorted(matches))}"
                )
            target = matches[0]
            if target in classified and classified[target] != group:
                raise SystemExit(
                    f"分类映射错误：target {target} 同时归入「{classified[target]}」与「{group}」"
                )
            classified[target] = group
    return classified


# --------------------------------------------------------------------------
# 数据收集
# --------------------------------------------------------------------------


def _line_count(path: Path) -> int:
    return path.read_bytes().count(b"\n")  # wc -l 口径：只数换行符


def collect_dir_stats() -> tuple[list[tuple[str, int, int]], int, int]:
    rows: list[tuple[str, int, int]] = []
    total_files = 0
    total_lines = 0
    for name in NEW_ARCH_DIRS:
        base = ac.CPP_ROOT / name
        files = sorted(
            path
            for path in base.rglob("*")
            if path.is_file() and path.suffix in ac._SOURCE_SUFFIXES
        )
        lines = sum(_line_count(path) for path in files)
        rows.append((name, len(files), lines))
        total_files += len(files)
        total_lines += lines
    return rows, total_files, total_lines


def collect_features() -> list[tuple[str, int, int]]:
    out: list[tuple[str, int, int]] = []
    for directory in sorted(path for path in PLUGINS_DIR.iterdir() if path.is_dir()):
        files = sorted(
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix in ac._SOURCE_SUFFIXES
        )
        if not files:
            continue
        out.append((directory.name, len(files), sum(_line_count(path) for path in files)))
    return out


# --------------------------------------------------------------------------
# 渲染
# --------------------------------------------------------------------------


def collect_classification() -> tuple[dict[str, str], dict[str, list[str]], list[str], int]:
    """解析 CTest target 并按白名单分类。

    返回 (target→类别, 类别→target 列表, legacy 列表, target 总数)。
    """
    targets = parse_test_targets(TESTS_CMAKE)
    classified = classify_targets(targets)
    group_targets = {
        group: sorted(target for target, name in classified.items() if name == group)
        for group, _ in MIGRATED_TEST_GROUPS
    }
    legacy_targets = sorted(set(targets) - set(classified))
    return classified, group_targets, legacy_targets, len(targets)


def _group_counts(group_targets: dict[str, list[str]]) -> str:
    return " + ".join(
        f"{group} {len(group_targets[group])}" for group, _ in MIGRATED_TEST_GROUPS
    )


def emit_verbose(
    targets_total: int, group_targets: dict[str, list[str]], legacy_targets: list[str]
) -> None:
    migrated = sum(len(names) for names in group_targets.values())
    print("CTest target 分类（--verbose）")
    print(f"- 全仓 target 总数：{targets_total}")
    print(f"- 新架构/已迁移：{migrated}（{_group_counts(group_targets)}）")
    for group, _ in MIGRATED_TEST_GROUPS:
        names = group_targets[group]
        listed = "、".join(names) if names else "（无）"
        print(f"- {group}（{len(names)}）：{listed}")
    listed = "、".join(legacy_targets) if legacy_targets else "（无）"
    print(f"- legacy（{len(legacy_targets)}）：{listed}")


def render_stats() -> str:
    dir_rows, total_files, total_lines = collect_dir_stats()
    features = collect_features()
    _, group_targets, legacy_targets, targets_total = collect_classification()
    migrated_targets = sorted(
        target for group, _ in MIGRATED_TEST_GROUPS for target in group_targets[group]
    )

    lines: list[str] = [
        "## 生成统计",
        "",
        "> 本节由 `scripts/maintenance/gen_map_doc.py` 生成，**禁止手工编辑**；",
        "> 手工改动会在 `--check` 下被判定为不一致。",
        "",
        "- 数据源：`module/app/src/main/cpp/{contracts,game,infra,hooks,patch,domains,plugins,host}`、"
        "`module/app/src/main/cpp/plugins/*`、`module/app/src/main/cpp/tests/CMakeLists.txt`",
        "- 口径：行数按 `wc -l`（换行符计数，含空行与注释）；文件限 `.cpp`/`.h`/`.inc`；"
        "不含 `bridge/`、`tests/` 与 legacy 路径",
        "- CTest 分类规则：`gen_map_doc.py` 的显式白名单（测试源文件 → 机制/宿主或已迁移功能）"
        "确定新架构/已迁移 target，其余归 legacy；白名单条目须存在于 `tests/CMakeLists.txt`，两类互斥",
        "",
        "### 新架构目录",
        "",
        "| 目录 | 文件数 | 行数 |",
        "| --- | --- | --- |",
    ]
    for name, files, line_count in dir_rows:
        lines.append(f"| `{name}/` | {files} | {line_count} |")
    lines += [f"| **合计** | **{total_files}** | **{total_lines}** |", ""]

    lines += ["### 已迁移插件（`plugins/*`）", ""]
    if features:
        for name, files, line_count in features:
            lines.append(f"- `plugins/{name}/`：{files} 文件 / {line_count} 行")
    else:
        lines.append("（无）")
    lines.append("")

    lines += [
        "### CTest target 分类",
        "",
        f"- 全仓 target 总数：**{targets_total}**",
        f"- 新架构/已迁移：**{len(migrated_targets)}**（{_group_counts(group_targets)}）",
        f"- legacy：**{len(legacy_targets)}**",
        "",
    ]
    if migrated_targets:
        lines.append(f"新架构/已迁移（{len(migrated_targets)}）：")
        for group, _ in MIGRATED_TEST_GROUPS:
            names = group_targets[group]
            if names:
                lines.append(
                    f"- {group}（{len(names)}）：" + "、".join(f"`{name}`" for name in names)
                )
            else:
                lines.append(f"- {group}（0）：（无）")
    else:
        lines.append("新架构/已迁移（0）：（无）")
    lines.append("")
    if legacy_targets:
        lines.append(
            f"legacy（{len(legacy_targets)}）：" + "、".join(f"`{name}`" for name in legacy_targets)
        )
    else:
        lines.append("legacy（0）：（无）")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------


def splice(existing: str, body: str) -> str:
    begin = existing.find(BEGIN_MARKER)
    end = existing.find(END_MARKER)
    if begin < 0 or end < 0 or end < begin:
        raise SystemExit(f"{DEFAULT_OUTPUT} 缺少 markers：{BEGIN_MARKER} … {END_MARKER}")
    return existing[: begin + len(BEGIN_MARKER)] + "\n" + body + "\n" + existing[end:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只校验生成结果与磁盘一致（不一致返回 2）")
    parser.add_argument(
        "--verbose", action="store_true", help="打印完整 CTest 分类（含 legacy 列表与计数）"
    )
    args = parser.parse_args()

    if not DEFAULT_OUTPUT.exists():
        print(f"不一致：{DEFAULT_OUTPUT} 不存在", file=sys.stderr)
        return 2
    existing = DEFAULT_OUTPUT.read_text(encoding="utf-8")
    content = splice(existing, render_stats())

    if args.check:
        if existing != content:
            print(
                f"不一致：{DEFAULT_OUTPUT} 的生成统计区与磁盘不同（请运行 "
                f"`uv run python scripts/maintenance/{SCRIPT_NAME}` 重新生成）",
                file=sys.stderr,
            )
            return 2
        print(f"一致：{DEFAULT_OUTPUT}")
    else:
        DEFAULT_OUTPUT.write_text(content, encoding="utf-8")
        print(f"已更新生成统计区：{DEFAULT_OUTPUT}")

    if args.verbose:
        _, group_targets, legacy_targets, targets_total = collect_classification()
        emit_verbose(targets_total, group_targets, legacy_targets)
    return 0


if __name__ == "__main__":
    sys.exit(main())
