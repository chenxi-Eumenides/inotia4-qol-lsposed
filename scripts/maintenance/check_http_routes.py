#!/usr/bin/env python3
"""核对 Kotlin controller、API 参考和 core.api native 冻结路由集合。"""

from collections import Counter
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
CONTROLLERS = ROOT / "module/app/src/main/java/com/inotia4/qol/api/controller"
REFERENCE = ROOT / "docs/reference/api-reference.md"
ROUTES = ROOT / "module/app/src/main/cpp/plugins/core.api/routes.inc"


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


def native_routes() -> tuple[list[tuple[str, str]], Counter[str]]:
    pattern = re.compile(
        r'QOL_HTTP_ROUTE\((kGet|kPost),\s*"([^"]+)",\s*"([^"]+)",\s*([01]),\s*([01])\)'
    )
    routes: list[tuple[str, str]] = []
    owners: Counter[str] = Counter()
    for match in pattern.finditer(ROUTES.read_text(encoding="utf-8")):
        method = "GET" if match.group(1) == "kGet" else "POST"
        path = match.group(2)
        routes.append((method, path))
        owners[match.group(3)] += 1
    return routes, owners


def print_set(label: str, routes: set[tuple[str, str]]) -> None:
    print(f"{label}: {len(routes)}")


def main() -> int:
    controllers = controller_routes()
    reference = reference_routes()
    native_list, owners = native_routes()
    native = set(native_list)
    duplicates = [route for route, count in Counter(native_list).items() if count > 1]

    print_set("controller unique", controllers)
    print_set("api-reference unique", reference)
    print(f"native frozen: {len(native_list)} (unique {len(native)})")
    print(f"owner distribution: {dict(sorted(owners.items()))}")
    print(f"duplicates: {len(duplicates)}")
    print(f"controller -> native missing: {len(controllers - native)}")
    print(f"native -> controller extra: {len(native - controllers)}")
    print(f"api-reference -> native missing: {len(reference - native)}")
    print(f"native -> api-reference extra: {len(native - reference)}")

    failures = False
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
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
