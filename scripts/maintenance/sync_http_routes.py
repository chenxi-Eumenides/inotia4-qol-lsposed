#!/usr/bin/env python3
"""Validate owner route facts and regenerate the central HTTP route aggregate."""

import argparse
from collections import Counter
from dataclasses import dataclass
from difflib import unified_diff
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PLUGIN_ROOT = ROOT / "module/app/src/main/cpp/plugins"
HOST_ROUTES = ROOT / "module/app/src/main/cpp/host/http_resident_routes.inc"
AGGREGATE = PLUGIN_ROOT / "core.api/routes.inc"
AGGREGATE_HEADER = (
    "// 由 scripts/maintenance/sync_http_routes.py 从 plugin owner 与 Host resident route facts 生成。\n"
    "// 不要直接编辑；事实源位于各 owner 的 routes.inc / core.api/owned_routes.inc 与\n"
    "// host/http_resident_routes.inc。字段顺序：method, path, owner label, op gate, route lease。\n"
    "\n"
)

SOURCE_ROUTE_COUNTS = {
    "autosell": 2,
    "core.api": 5,
    "core.character": 41,
    "core.craft": 1,
    "core.custom_ui": 3,
    "core.data": 12,
    "core.feature": 1,
    "core.item": 21,
    "core.quest": 8,
    "core.save": 3,
    "core.system": 5,
    "core.ui_state": 9,
    "core.world": 12,
    "extension_bag": 8,
    "save_backup": 4,
    "settings": 5,
}
EXPECTED_OWNER_COUNTS = {
    **SOURCE_ROUTE_COUNTS,
    "core.api": 6,
    "legacy.debug": 3,
    "settings": 8,
}
EXPECTED_PLUGIN_FACT_COUNT = 140
EXPECTED_RESIDENT_FACT_COUNT = 7
EXPECTED_ROUTE_COUNT = 147
EXPECTED_METHOD_COUNTS = {"GET": 68, "POST": 79}
EXPECTED_OP_GATED_COUNT = 21

PLUGIN_SOURCE_COUNTS = {
    "autosell/routes.inc": ("autosell", 2),
    "core.api/owned_routes.inc": ("core.api", 5),
    "core.character/routes.inc": ("core.character", 41),
    "core.craft/routes.inc": ("core.craft", 1),
    "core.custom_ui/routes.inc": ("core.custom_ui", 3),
    "core.data/routes.inc": ("core.data", 12),
    "core.feature/routes.inc": ("core.feature", 1),
    "core.item/routes.inc": ("core.item", 21),
    "core.quest/routes.inc": ("core.quest", 8),
    "core.save/routes.inc": ("core.save", 3),
    "core.system/routes.inc": ("core.system", 5),
    "core.ui_state/routes.inc": ("core.ui_state", 9),
    "core.world/routes.inc": ("core.world", 12),
    "extension_bag/routes.inc": ("extension_bag", 8),
    "save_backup/routes.inc": ("save_backup", 4),
    "settings/routes.inc": ("settings", 5),
}

ROUTE_PATTERNS = {
    "QOL_HTTP_ROUTE": re.compile(
        r'^\s*QOL_HTTP_ROUTE\(\s*(kGet|kPost)\s*,\s*"([^"\n]+)"\s*,\s*'
        r'"([^"\n]+)"\s*,\s*([01])\s*,\s*([01])\s*\)\s*(?://.*)?$'
    ),
    "QOL_HTTP_RESIDENT_ROUTE": re.compile(
        r'^\s*QOL_HTTP_RESIDENT_ROUTE\(\s*(kGet|kPost)\s*,\s*"([^"\n]+)"\s*,\s*'
        r'"([^"\n]+)"\s*,\s*([01])\s*,\s*([01])\s*\)\s*(?://.*)?$'
    ),
}


class RouteSyncError(Exception):
    """Raised when route facts are missing, malformed, or inconsistent."""


@dataclass(frozen=True)
class RouteFact:
    method: str
    path: str
    owner: str
    op_gated: int
    lease_required: int
    source: str
    line: int

    @property
    def key(self) -> tuple[str, str]:
        return self.method, self.path

    def aggregate_line(self) -> str:
        method = "kGet" if self.method == "GET" else "kPost"
        return (
            f'QOL_HTTP_ROUTE({method}, "{self.path}", "{self.owner}", '
            f"{self.op_gated}, {self.lease_required})"
        )


def parse_route_line(line: str, macro: str, source: str, line_number: int) -> RouteFact | None:
    stripped = line.strip()
    if not stripped or stripped.startswith("//"):
        return None
    match = ROUTE_PATTERNS[macro].fullmatch(line)
    if match is None:
        raise RouteSyncError(f"{source}:{line_number}: malformed {macro} fact")
    method, route_path, owner, op_gated, lease_required = match.groups()
    if not route_path.startswith("/api/"):
        raise RouteSyncError(f"{source}:{line_number}: route path must start with /api/")
    return RouteFact(
        "GET" if method == "kGet" else "POST",
        route_path,
        owner,
        int(op_gated),
        int(lease_required),
        source,
        line_number,
    )


def parse_source(path: Path, macro: str, root: Path) -> list[RouteFact]:
    relative = path.relative_to(root).as_posix()
    facts: list[RouteFact] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fact = parse_route_line(line, macro, relative, line_number)
        if fact is not None:
            facts.append(fact)
    return facts


def owner_sources(root: Path = ROOT) -> list[Path]:
    plugin_root = root / "module/app/src/main/cpp/plugins"
    discovered = {
        path.relative_to(plugin_root).as_posix()
        for path in plugin_root.glob("*/routes.inc")
        if path != plugin_root / "core.api/routes.inc"
    }
    if (plugin_root / "core.api/owned_routes.inc").is_file():
        discovered.add("core.api/owned_routes.inc")
    expected = set(PLUGIN_SOURCE_COUNTS)
    if discovered != expected:
        missing = sorted(expected - discovered)
        extra = sorted(discovered - expected)
        raise RouteSyncError(f"plugin route source inventory mismatch: missing={missing}, extra={extra}")
    return [plugin_root / relative for relative in sorted(expected)]


def load_route_facts(root: Path = ROOT) -> tuple[list[RouteFact], list[RouteFact]]:
    plugin_root = root / "module/app/src/main/cpp/plugins"
    plugin_facts: list[RouteFact] = []
    for source in owner_sources(root):
        relative = source.relative_to(plugin_root).as_posix()
        expected_owner, expected_count = PLUGIN_SOURCE_COUNTS[relative]
        facts = parse_source(source, "QOL_HTTP_ROUTE", root)
        if len(facts) != expected_count:
            raise RouteSyncError(
                f"{relative}: expected {expected_count} facts, found {len(facts)}"
            )
        for fact in facts:
            if fact.owner != expected_owner:
                raise RouteSyncError(
                    f"{relative}:{fact.line}: owner {fact.owner!r} does not match "
                    f"source owner {expected_owner!r}"
                )
        plugin_facts.extend(facts)

    resident_facts = parse_source(
        root / "module/app/src/main/cpp/host/http_resident_routes.inc",
        "QOL_HTTP_RESIDENT_ROUTE",
        root,
    )
    if len(resident_facts) != EXPECTED_RESIDENT_FACT_COUNT:
        raise RouteSyncError(
            f"host/http_resident_routes.inc: expected {EXPECTED_RESIDENT_FACT_COUNT} facts, "
            f"found {len(resident_facts)}"
        )
    return plugin_facts, resident_facts


def validate_facts(plugin_facts: list[RouteFact], resident_facts: list[RouteFact]) -> list[RouteFact]:
    facts = plugin_facts + resident_facts
    seen: dict[tuple[str, str], RouteFact] = {}
    duplicates: list[str] = []
    for fact in facts:
        previous = seen.get(fact.key)
        if previous is not None:
            duplicates.append(
                f"{fact.method} {fact.path}: {previous.source}:{previous.line} and "
                f"{fact.source}:{fact.line}"
            )
        else:
            seen[fact.key] = fact
    if duplicates:
        raise RouteSyncError("duplicate route facts:\n  " + "\n  ".join(duplicates))

    methods = Counter(fact.method for fact in facts)
    owners = Counter(fact.owner for fact in facts)
    op_gated_count = sum(fact.op_gated for fact in facts)
    if len(plugin_facts) != EXPECTED_PLUGIN_FACT_COUNT:
        raise RouteSyncError(
            f"plugin route fact count mismatch: expected {EXPECTED_PLUGIN_FACT_COUNT}, "
            f"found {len(plugin_facts)}"
        )
    if len(facts) != EXPECTED_ROUTE_COUNT:
        raise RouteSyncError(
            f"total route count mismatch: expected {EXPECTED_ROUTE_COUNT}, found {len(facts)}"
        )
    if dict(methods) != EXPECTED_METHOD_COUNTS:
        raise RouteSyncError(
            f"method counts mismatch: expected {EXPECTED_METHOD_COUNTS}, found {dict(methods)}"
        )
    if op_gated_count != EXPECTED_OP_GATED_COUNT:
        raise RouteSyncError(
            f"op_gated count mismatch: expected {EXPECTED_OP_GATED_COUNT}, found {op_gated_count}"
        )
    if dict(owners) != EXPECTED_OWNER_COUNTS:
        raise RouteSyncError(
            f"owner counts mismatch: expected {dict(sorted(EXPECTED_OWNER_COUNTS.items()))}, "
            f"found {dict(sorted(owners.items()))}"
        )
    return facts


def aggregate_keys(text: str) -> list[tuple[str, str]]:
    keys: list[tuple[str, str]] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        match = ROUTE_PATTERNS["QOL_HTTP_ROUTE"].fullmatch(line)
        if match is None:
            raise RouteSyncError(f"core.api/routes.inc:{line_number}: malformed aggregate row")
        method, path, _owner, _op_gated, _lease_required = match.groups()
        keys.append(("GET" if method == "kGet" else "POST", path))
    return keys


def render_aggregate(facts: list[RouteFact], existing_text: str) -> str:
    by_key = {fact.key: fact for fact in facts}
    ordered_keys: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for key in aggregate_keys(existing_text):
        if key in by_key and key not in seen:
            ordered_keys.append(key)
            seen.add(key)
    ordered_keys.extend(sorted(set(by_key) - seen))
    return AGGREGATE_HEADER + "".join(by_key[key].aggregate_line() + "\n" for key in ordered_keys)


def facts_summary(plugin_facts: list[RouteFact], resident_facts: list[RouteFact]) -> str:
    facts = plugin_facts + resident_facts
    methods = Counter(fact.method for fact in facts)
    owners = Counter(fact.owner for fact in facts)
    op_gated_count = sum(fact.op_gated for fact in facts)
    return (
        f"route facts: plugin={len(plugin_facts)} resident={len(resident_facts)} "
        f"total={len(facts)} GET={methods['GET']} POST={methods['POST']} "
        f"op_gated={op_gated_count}\n"
        f"owner distribution: {dict(sorted(owners.items()))}"
    )


def aggregate_diff(actual: str, expected: str) -> str:
    return "".join(
        unified_diff(
            actual.splitlines(keepends=True),
            expected.splitlines(keepends=True),
            fromfile="core.api/routes.inc (current)",
            tofile="core.api/routes.inc (generated)",
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="check aggregate without writing (default)")
    mode.add_argument("--write", action="store_true", help="regenerate core.api/routes.inc")
    args = parser.parse_args()

    try:
        plugin_facts, resident_facts = load_route_facts()
        facts = validate_facts(plugin_facts, resident_facts)
        if not AGGREGATE.exists():
            raise RouteSyncError("central aggregate is missing; stable route order baseline unavailable")
        existing = AGGREGATE.read_text(encoding="utf-8")
        expected = render_aggregate(facts, existing)
    except (OSError, RouteSyncError) as error:
        print(f"HTTP route source validation failed: {error}", file=sys.stderr)
        return 2

    if args.write:
        try:
            AGGREGATE.write_text(expected, encoding="utf-8")
        except OSError as error:
            print(f"cannot write aggregate: {error}", file=sys.stderr)
            return 2
        print(f"generated {AGGREGATE.relative_to(ROOT)} ({len(facts)} routes)")
        return 0

    print(facts_summary(plugin_facts, resident_facts))
    if existing != expected:
        print("aggregate mismatch:")
        print(aggregate_diff(existing, expected), end="")
        return 1
    print(f"aggregate: PASS ({len(facts)} exact source facts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
