#!/usr/bin/env python3
"""架构校验公共库（ARCHITECTURE.md §2.2 / §9.2 / §9.4 的唯一机械实现）。

提供：
- 层映射：目标目录（新架构）与重构期映射（现存路径 → 目标层，随迁移逐步删除）；
- 依赖矩阵：`allows(src_layer, dst_layer)`；
- C++ include 图解析与文件索引；
- 违规模型、基线棘轮（只允许收缩）、临时豁免登记；
- 统一 CLI 收尾与输出（文本 / JSON）。

只依赖标准库。被 `scripts/maintenance/check_*.py` 复用。
违规路径一律相对 `module/app/src/main/cpp`。
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

ARCH_DIR = Path(__file__).resolve().parent
ROOT = ARCH_DIR.parents[1]
CPP_ROOT = ROOT / "module" / "app" / "src" / "main" / "cpp"
BASELINE_PATH = ARCH_DIR / "arch_baseline.json"
EXEMPTIONS_PATH = ARCH_DIR / "arch_exemptions.json"

# --------------------------------------------------------------------------
# 层与依赖矩阵（与 ARCHITECTURE.md §2.2 表格逐行一致）
# --------------------------------------------------------------------------

BASE_LAYERS = frozenset({"contracts", "game", "infra", "hooks", "patch", "plugins", "host"})

# 同层内部允许互相 include（plugins 除外，见 allows()）
_SIMPLE_ALLOW = {
    "contracts": {"contracts"},
    "game": {"contracts", "game"},
    "infra": {"contracts", "infra"},
    "hooks": {"contracts", "game", "infra", "hooks"},
    "patch": {"contracts", "game", "infra", "patch"},
}


def base_of(layer: str) -> str:
    return layer.split("/", 1)[0]


def allows(src_layer: str, dst_layer: str) -> bool:
    """目标层 src 是否允许依赖目标层 dst（同文件家族内部 include 另由调用方放行）。"""
    sb, db = base_of(src_layer), base_of(dst_layer)
    if sb == "host":
        return True
    if db in _SIMPLE_ALLOW.get(sb, set()):
        return True
    if sb == "legacy":
        if db in {"contracts", "game", "infra"}:
            return True
        if db == "host":
            # game_save_preflight 是宿主层的纯逻辑判定；存档 API 仍在迁移期，
            # 允许 legacy/save 通过这一稳定、零游戏依赖的宿主工具完成预检。
            return src_layer == "legacy/save"
        if db == "legacy":
            # 旧路径仅保留迁移期兼容语义；域间水平互调仅 system 聚合方向允许。
            return src_layer == dst_layer or src_layer == "legacy/system"
        return False
    if sb == "plugins":
        if db == "contracts":
            return True
        if db == "plugins":
            return src_layer == dst_layer  # 同一插件内部允许
        return False
    return False


# --------------------------------------------------------------------------
# 重构期映射：现存 legacy 路径 → 目标层或迁移期标签。
# 说明：这些表不是架构例外。core/、api/ 被七目录吸收后删除对应映射；
# 清除条件是目录内文件全部迁入目标目录、include 与构建边界通过校验，并完成基线收缩。
# --------------------------------------------------------------------------

_SKIP_DIRS = {"tests", "build"}

_ROOT_OVERRIDES = {
    "game_system.cpp": "legacy/system",
    "game_system.h": "legacy/system",
    "game_dialog.cpp": "legacy/dialog",
    "game_dialog.h": "legacy/dialog",
    "game_dialog_content.inc": "legacy/dialog",
    "game_dialog_operations.inc": "legacy/dialog",
}

# 暂留 legacy core/native：这些实现仍直接依赖未吸收的原版访问面，依赖吸收完成后再迁入目标层。
_CORE_OVERRIDES = {
    "game_cache.cpp": "infra",
    "game_cache.h": "infra",
    "module_save.cpp": "legacy/save",
    "module_save_port.h": "contracts",
    "inventory_trade.cpp": "legacy/inventory",
    "inventory_trade.h": "legacy/inventory",
}

_TARGET_PREFIXES = (
    ("contracts/", "contracts"),
    ("game/", "game"),
    ("infra/", "infra"),
    ("hooks/", "hooks"),
    ("patch/", "patch"),
    ("plugins/", "plugins"),
    ("host/", "host"),
)


def classify(rel: str) -> str:
    """把 cpp 根下的相对路径映射到目标层；无法映射返回 'unknown'。

    目标目录（新架构）优先；其余走重构期映射（_classify_transition）。
    """
    for prefix, layer in _TARGET_PREFIXES:
        if rel.startswith(prefix):
            if prefix == "plugins/" and rel.count("/") > 1:
                return layer + "/" + rel.split("/", 2)[1]
            return layer
    return _classify_transition(rel)


def _classify_transition(rel: str) -> str:
    if rel in _ROOT_OVERRIDES:
        return _ROOT_OVERRIDES[rel]
    if "/" not in rel:
        return "unknown"
    if rel.startswith("bridge/native/"):
        return "host"
    if rel.startswith("core/native/"):
        name = rel.rsplit("/", 1)[1]
        return _CORE_OVERRIDES.get(name, "infra")
    if rel.startswith("feature/"):
        parts = rel.split("/")
        return "plugins/" + parts[1] if len(parts) > 2 else "plugins"
    if rel.startswith("tests/"):
        return "tests"
    return "unknown"


# --------------------------------------------------------------------------
# 文件扫描与 include 解析
# --------------------------------------------------------------------------

_SOURCE_SUFFIXES = {".cpp", ".h", ".inc"}
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def iter_sources() -> list[tuple[str, Path]]:
    """返回 (cpp 相对路径, 绝对路径)，按路径排序；跳过 tests/ 与 build/。"""
    out: list[tuple[str, Path]] = []
    for path in sorted(CPP_ROOT.rglob("*")):
        if path.suffix not in _SOURCE_SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(CPP_ROOT).as_posix()
        if any(part in _SKIP_DIRS for part in rel.split("/")[:-1]):
            continue
        out.append((rel, path))
    return out


def build_index(sources: list[tuple[str, Path]]) -> dict[str, list[str]]:
    index: dict[str, list[str]] = {}
    for rel, _ in sources:
        index.setdefault(Path(rel).name, []).append(rel)
    return index


def resolve_include(
    raw: str, src_rel: str, index: dict[str, list[str]], all_rels: set[str] | None = None
) -> str | None:
    """解析 include 目标：先相对目录，再相对 cpp 根，最后按文件名后缀匹配。"""
    if all_rels is None:
        all_rels = {rel for rels in index.values() for rel in rels}
    src_dir = Path(src_rel).parent
    for candidate in ((src_dir / raw).as_posix(), raw):
        if candidate in all_rels:
            return candidate
    matches = index.get(Path(raw).name)
    if not matches:
        return None
    if src_rel.startswith("plugins/"):
        plugin_parts = src_rel.split("/", 2)
        if len(plugin_parts) >= 2:
            plugin_prefix = "/".join(plugin_parts[:2]) + "/"
            plugin_matches = [rel for rel in matches if rel.startswith(plugin_prefix)]
            if len(plugin_matches) == 1:
                return plugin_matches[0]
    for rel in matches:
        if rel.endswith(raw):
            return rel
    return matches[0]


def parse_includes(
    rel: str, path: Path, index: dict[str, list[str]], all_rels: set[str] | None = None
) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    resolved: list[str] = []
    for match in _INCLUDE_RE.finditer(text):
        target = resolve_include(match.group(1), rel, index, all_rels)
        if target is not None:
            resolved.append(target)
    return resolved


class IncludeGraph:
    """一次构建、多次查询的 include 图（层映射 + 解析 + 边）。"""

    def __init__(self, sources: list[tuple[str, Path]]):
        self.sources = sources
        self.paths = dict(sources)
        self.index = build_index(sources)
        self.all_rels = {rel for rel, _ in sources}
        self.layers = {rel: classify(rel) for rel, _ in sources}
        self._edges: dict[str, list[str]] | None = None

    def layer(self, rel: str) -> str:
        return self.layers.get(rel, "unknown")

    def includes_of(self, rel: str) -> list[str]:
        path = self.paths.get(rel)
        if path is None:
            return []
        return parse_includes(rel, path, self.index, self.all_rels)

    def edges(self) -> dict[str, list[str]]:
        if self._edges is None:
            self._edges = {rel: self.includes_of(rel) for rel, _ in self.sources}
        return self._edges

    def sccs(self) -> list[list[str]]:
        """Tarjan（迭代实现），仅遍历已映射层且非 tests 的节点。"""
        graph = {
            rel: [t for t in targets if self.layer(t) not in ("unknown", "tests")]
            for rel, targets in self.edges().items()
            if self.layer(rel) not in ("unknown", "tests")
        }
        return tarjan_scc(graph)


def tarjan_scc(graph: dict[str, list[str]]) -> list[list[str]]:
    index_counter = 0
    stack: list[str] = []
    low: dict[str, int] = {}
    idx: dict[str, int] = {}
    on_stack: set[str] = set()
    result: list[list[str]] = []

    for start in sorted(graph):
        if start in idx:
            continue
        work = [(start, 0)]
        while work:
            node, child_i = work[-1]
            if child_i == 0:
                idx[node] = low[node] = index_counter
                index_counter += 1
                stack.append(node)
                on_stack.add(node)
            children = graph.get(node, [])
            if child_i < len(children):
                child = children[child_i]
                work[-1] = (node, child_i + 1)
                if child not in idx:
                    work.append((child, 0))
                elif child in on_stack:
                    low[node] = min(low[node], idx[child])
            else:
                work.pop()
                if work:
                    parent = work[-1][0]
                    low[parent] = min(low[parent], low[node])
                if low[node] == idx[node]:
                    component: list[str] = []
                    while True:
                        member = stack.pop()
                        on_stack.discard(member)
                        component.append(member)
                        if member == node:
                            break
                    if len(component) > 1:
                        result.append(component)
    return result


# --------------------------------------------------------------------------
# 违规模型、基线棘轮与豁免
# --------------------------------------------------------------------------

WARN_RULES = frozenset({"unknown_layer"})


@dataclass(frozen=True)
class Violation:
    rule: str
    src: str
    dst: str = ""
    detail: str = ""

    def key(self) -> str:
        return "|".join((self.rule, self.src, self.dst, self.detail))


def _load_json(path: Path, default):
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:  # 配置损坏必须显式失败
        raise SystemExit(f"配置损坏：{path}: {exc}") from exc


def load_baseline() -> set[str]:
    data = _load_json(BASELINE_PATH, {"violations": []})
    return set(data.get("violations", []))


def save_baseline(keys: set[str]) -> None:
    payload = {
        "说明": "架构校验基线快照（棘轮）：只允许收缩。新增违规即失败；消除后重新冻结。",
        "生成命令": "uv run python scripts/maintenance/check_arch.py --freeze",
        "violations": sorted(keys),
    }
    BASELINE_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def load_exemptions() -> list[dict]:
    data = _load_json(EXEMPTIONS_PATH, {"exemptions": []})
    return list(data.get("exemptions", []))


def _exempt(violation: Violation, exemptions: list[dict]) -> dict | None:
    for item in exemptions:
        if not fnmatch.fnmatch(violation.rule, item.get("rule", "*")):
            continue
        if not fnmatch.fnmatch(violation.src, item.get("src", "*")):
            continue
        dst_pattern = item.get("dst")
        if dst_pattern and not fnmatch.fnmatch(violation.dst, dst_pattern):
            continue
        return item
    return None


def emit(
    rule: str,
    violations: list[Violation],
    args,
    title: str,
    extra_lines=(),
    rules: "Iterable[str] | None" = None,
) -> int:
    """统一收尾：--freeze 写基线；否则按棘轮判定；返回进程退出码。

    `rules` 为本脚本可产出的**全部违规规则名**，用于基线分区与收缩判定；
    缺省从当前违规推导——此时已完全消除的规则无法被自动收缩，需显式传入。
    """
    violations = sorted(set(violations), key=lambda v: v.key())
    keys = {v.key() for v in violations}
    owned = tuple(sorted(set(rules))) if rules else tuple(sorted({v.rule for v in violations}))
    prefixes = tuple(name + "|" for name in owned) or (rule + "|",)

    for line in extra_lines:
        print(line)

    if args.freeze:
        baseline = load_baseline()
        rule_baseline = {key for key in baseline if key.startswith(prefixes)}
        rule_keys = {key for key in keys if key.startswith(prefixes)}
        added = rule_keys - rule_baseline
        if rule_baseline and added and not getattr(args, "force_freeze", False):
            print(f"[{rule}] 拒绝冻结：相比基线新增 {len(added)} 条（基线只允许收缩）。")
            for key in sorted(added)[:10]:
                print(f"  + {key}")
            print("  如属有意变更：先改设计并走评审，或登记 arch_exemptions.json；确需扩容用 --force-freeze。")
            return 1
        removed = rule_baseline - rule_keys
        merged = (baseline - rule_baseline) | rule_keys
        save_baseline(merged)
        print(
            f"[{rule}] 基线已冻结：本规则 {len(rule_keys)} 条"
            f"（移除已消除 {len(removed)} 条；共享基线共 {len(merged)} 条）→ {BASELINE_PATH}"
        )
        return 0

    baseline = load_baseline()
    exemptions = load_exemptions()
    new: list[Violation] = []
    exempted: list[tuple[Violation, dict]] = []
    for violation in violations:
        item = _exempt(violation, exemptions)
        if item is not None:
            exempted.append((violation, item))
        elif violation.key() not in baseline:
            new.append(violation)

    resolved = sorted(baseline - keys)
    warns = [v for v in new if v.rule in WARN_RULES]
    fail = [v for v in new if v.rule not in WARN_RULES]

    print(f"== {title} ==")
    print(f"  违规 {len(violations)} 条（基线 {len(baseline)} 条 / 豁免 {len(exempted)} 条 / 新增 {len(new)} 条）")
    for violation in fail:
        print(f"  ❌ {violation.rule}: {violation.src}" + (f" → {violation.dst}" if violation.dst else "")
              + (f"  [{violation.detail}]" if violation.detail else ""))
    for violation in warns:
        print(f"  ⚠️  {violation.rule}: {violation.src}" + (f"  [{violation.detail}]" if violation.detail else ""))
    if args.verbose:
        for violation, item in exempted:
            print(f"  ➖ 豁免({item.get('anchor', '?')}): {violation.key()}")
        for key in resolved:
            print(f"  ✅ 已消除（可在下次 --freeze 移除）：{key}")

    if args.json:
        print(json.dumps(
            {
                "rule": rule,
                "total": len(violations),
                "baseline": len(baseline),
                "exempted": len(exempted),
                "new": [v.key() for v in new],
                "resolved": resolved,
            },
            ensure_ascii=False,
        ))

    if fail:
        print(f"\n[{rule}] 失败：{len(fail)} 条新增违规（基线只允许收缩；如属有意变更请改设计并重新冻结）")
        return 1
    print(f"[{rule}] 通过")
    return 0


def parse_common_args(description: str) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--freeze", action="store_true", help="把当前违规写入基线快照（只允许收缩）")
    parser.add_argument(
        "--force-freeze",
        action="store_true",
        help="允许在冻结时扩容基线（仅用于首次建立或经评审的有意变更）",
    )
    parser.add_argument("--json", action="store_true", help="额外输出 JSON 汇总")
    parser.add_argument("--verbose", "-v", action="store_true", help="打印豁免与已消除条目")
    args = parser.parse_args()
    if args.force_freeze:
        args.freeze = True
    return args


def read_text(rel: str, path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def sources_by_rel() -> dict[str, Path]:
    return dict(iter_sources())


def main() -> int:  # pragma: no cover - 直接运行本库时给出用法
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
