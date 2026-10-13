#!/usr/bin/env python3
"""核对 Kotlin controller、API 参考、native 冻结路由集合与固定 owner。"""

from collections import Counter
from pathlib import Path
import re
import sys

import sync_http_routes


ROOT = Path(__file__).resolve().parents[2]
CONTROLLERS = ROOT / "module/app/src/main/java/com/inotia4/qol/api/controller"
REFERENCE = ROOT / "docs/reference/api-reference.md"
ROUTES = ROOT / "module/app/src/main/cpp/plugins/core.api/routes.inc"
EXPECTED_ROUTE_COUNT = sync_http_routes.EXPECTED_ROUTE_COUNT
EXPECTED_SETTINGS_ROUTE_COUNT = 8
EXPECTED_CONFIG_ROUTE_OWNERS = {
    ("GET", "/api/config/list"): "settings",
    ("POST", "/api/config/set"): "settings",
    ("GET", "/api/config/receipt/{id}"): "settings",
}


def normalize(method: str, path: str) -> tuple[str, str]:
    return method.upper(), path.split("?", 1)[0]


def controller_routes() -> set[tuple[str, str]]:
    pattern = re.compile(r'@(GetMapping|PostMapping)\("([^"]+)"\)')
    routes: set[tuple[str, str]] = set()
    for source in CONTROLLERS.glob("*.kt"):
        for match in pattern.finditer(source.read_text(encoding="utf-8")):
            routes.add(normalize(match.group(1).replace("Mapping", ""), match.group(2)))
    return routes


def reference_routes() -> set[tuple[str, str]]:
    pattern = re.compile(r"`(GET|POST) ([^`\s]+)`")
    routes: set[tuple[str, str]] = set()
    for match in pattern.finditer(REFERENCE.read_text(encoding="utf-8")):
        method, path = normalize(match.group(1), match.group(2))
        if path.startswith("/api/"):
            routes.add((method, path))
    return routes


def native_routes() -> tuple[list[tuple[str, str]], Counter[str], dict[tuple[str, str], str]]:
    pattern = re.compile(
        r'QOL_HTTP_ROUTE\((kGet|kPost),\s*"([^"]+)",\s*"([^"]+)",\s*([01]),\s*([01])\)'
    )
    routes: list[tuple[str, str]] = []
    owners: Counter[str] = Counter()
    route_owners: dict[tuple[str, str], str] = {}
    for match in pattern.finditer(ROUTES.read_text(encoding="utf-8")):
        method = "GET" if match.group(1) == "kGet" else "POST"
        path = match.group(2)
        key = (method, path)
        routes.append(key)
        owners[match.group(3)] += 1
        route_owners[key] = match.group(3)
    return routes, owners, route_owners


def print_set(label: str, routes: set[tuple[str, str]]) -> None:
    print(f"{label}: {len(routes)}")


def main() -> int:
    source_failures = False
    try:
        plugin_facts, resident_facts = sync_http_routes.load_route_facts(ROOT)
        source_facts = sync_http_routes.validate_facts(plugin_facts, resident_facts)
        aggregate = ROUTES.read_text(encoding="utf-8")
        generated = sync_http_routes.render_aggregate(source_facts, aggregate)
        print(sync_http_routes.facts_summary(plugin_facts, resident_facts))
        if aggregate != generated:
            source_failures = True
            print("owner source facts -> generated aggregate mismatch")
            print(sync_http_routes.aggregate_diff(aggregate, generated), end="")
    except (OSError, sync_http_routes.RouteSyncError) as error:
        source_failures = True
        print(f"owner source validation failed: {error}")

    controllers = controller_routes()
    reference = reference_routes()
    native_list, owners, route_owners = native_routes()
    native = set(native_list)
    duplicates = [route for route, count in Counter(native_list).items() if count > 1]

    print_set("controller unique", controllers)
    print_set("api-reference unique", reference)
    print(f"native frozen: {len(native_list)} (unique {len(native)})")
    print(f"native route count baseline: {EXPECTED_ROUTE_COUNT}")
    print(f"owner distribution: {dict(sorted(owners.items()))}")
    print(f"duplicates: {len(duplicates)}")
    print(f"controller -> native missing: {len(controllers - native)}")
    print(f"native -> controller extra: {len(native - controllers)}")
    print(f"api-reference -> native missing: {len(reference - native)}")
    print(f"native -> api-reference extra: {len(native - reference)}")

    failures = source_failures
    if len(native_list) != EXPECTED_ROUTE_COUNT:
        failures = True
        print(f"native route count mismatch: expected {EXPECTED_ROUTE_COUNT}, got {len(native_list)}")
    if owners["settings"] != EXPECTED_SETTINGS_ROUTE_COUNT:
        failures = True
        print(
            f"settings route owner count mismatch: expected {EXPECTED_SETTINGS_ROUTE_COUNT}, "
            f"got {owners['settings']}"
        )
    if owners["core.settings"] != 0:
        failures = True
        print(f"obsolete route owner core.settings remains on {owners['core.settings']} route(s)")
    for label, routes in (
        ("controller -> native missing", controllers - native),
        ("native -> controller extra", native - controllers),
        ("api-reference -> native missing", reference - native),
        ("native -> api-reference extra", native - reference),
    ):
        if routes:
            failures = True
            print(label + ":")
            for method, path in sorted(routes):
                print(f"  {method} {path}")
    if duplicates:
        failures = True
        print("duplicate native routes:")
        for method, path in duplicates:
            print(f"  {method} {path}")
    for route, expected_owner in EXPECTED_CONFIG_ROUTE_OWNERS.items():
        actual_owner = route_owners.get(route)
        if actual_owner != expected_owner:
            failures = True
            print(
                f"route owner mismatch: {route[0]} {route[1]} "
                f"expected {expected_owner!r}, got {actual_owner!r}"
            )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
