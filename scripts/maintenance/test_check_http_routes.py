#!/usr/bin/env python3
"""Unit tests for the HTTP route checker output modes."""

from collections import Counter
from contextlib import ExitStack, redirect_stdout
from io import StringIO
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import check_http_routes as checker


CONFIG_ROUTE = ("GET", "/api/config/list")


class CheckHttpRoutesTests(unittest.TestCase):
    def run_checker(
        self,
        args: list[str],
        controllers: set[tuple[str, str]],
        reference: set[tuple[str, str]],
        native_list: list[tuple[str, str]],
        owners: Counter[str],
        route_owners: dict[tuple[str, str], str],
        *,
        source_error: str | None = None,
    ) -> tuple[int, str]:
        output = StringIO()
        with ExitStack() as stack:
            stack.enter_context(patch.object(checker, "controller_routes", return_value=controllers))
            stack.enter_context(patch.object(checker, "reference_routes", return_value=reference))
            stack.enter_context(
                patch.object(
                    checker,
                    "native_routes",
                    return_value=(native_list, owners, route_owners),
                )
            )
            stack.enter_context(
                patch.object(
                    checker,
                    "ROUTES",
                    SimpleNamespace(read_text=lambda encoding: "aggregate"),
                )
            )
            stack.enter_context(patch.object(checker, "EXPECTED_ROUTE_COUNT", len(native_list)))
            stack.enter_context(patch.object(checker, "EXPECTED_SETTINGS_ROUTE_COUNT", 1))
            stack.enter_context(
                patch.object(checker, "EXPECTED_CONFIG_ROUTE_OWNERS", {CONFIG_ROUTE: "settings"})
            )
            stack.enter_context(patch.object(checker.sync_http_routes, "facts_summary", return_value="facts"))
            stack.enter_context(patch.object(checker.sync_http_routes, "validate_facts", return_value=[]))
            stack.enter_context(
                patch.object(checker.sync_http_routes, "render_aggregate", return_value="aggregate")
            )
            if source_error is None:
                stack.enter_context(
                    patch.object(checker.sync_http_routes, "load_route_facts", return_value=([], []))
                )
            else:
                stack.enter_context(
                    patch.object(
                        checker.sync_http_routes,
                        "load_route_facts",
                        side_effect=checker.sync_http_routes.RouteSyncError(source_error),
                    )
                )
            with redirect_stdout(output):
                result = checker.main(args)
        return result, output.getvalue()

    def test_default_hides_healthy_route_inventory(self) -> None:
        owners = Counter({"settings": 1})
        result, output = self.run_checker(
            [],
            {CONFIG_ROUTE},
            {CONFIG_ROUTE},
            [CONFIG_ROUTE],
            owners,
            {CONFIG_ROUTE: "settings"},
        )

        self.assertEqual(result, 0)
        self.assertNotIn("all routes:", output)
        self.assertNotIn(CONFIG_ROUTE[1], output)

    def test_all_lists_routes_with_presence_and_native_owner(self) -> None:
        controllers = {CONFIG_ROUTE, ("GET", "/controller-only")}
        reference = {CONFIG_ROUTE, ("GET", "/reference-only")}
        native_list = [CONFIG_ROUTE, ("GET", "/native-only")]
        owners = Counter({"settings": 1, "native.owner": 1})
        route_owners = {CONFIG_ROUTE: "settings", native_list[1]: "native.owner"}
        result, output = self.run_checker(
            ["--all"], controllers, reference, native_list, owners, route_owners
        )

        self.assertEqual(result, 1)
        self.assertIn(
            "GET /api/config/list controller=yes api-reference=yes native=yes owner=settings",
            output,
        )
        self.assertIn(
            "GET /controller-only controller=yes api-reference=no native=no owner=-",
            output,
        )
        self.assertIn(
            "GET /native-only controller=no api-reference=no native=yes owner=native.owner",
            output,
        )
        self.assertIn(
            "GET /reference-only controller=no api-reference=yes native=no owner=-",
            output,
        )

    def test_actionable_error_details_are_kept_in_both_modes(self) -> None:
        controllers = {CONFIG_ROUTE, ("GET", "/controller-only")}
        reference = {CONFIG_ROUTE, ("GET", "/reference-only")}
        native_list = [CONFIG_ROUTE, ("GET", "/native-only"), ("GET", "/native-only")]
        owners = Counter({"wrong_owner": 1, "native.owner": 2})
        route_owners = {CONFIG_ROUTE: "wrong_owner", native_list[1]: "native.owner"}

        for args in ([], ["--all"]):
            with self.subTest(args=args):
                result, output = self.run_checker(
                    args,
                    controllers,
                    reference,
                    native_list,
                    owners,
                    route_owners,
                    source_error="invalid owner facts",
                )

                self.assertEqual(result, 1)
                self.assertIn("owner source validation failed: invalid owner facts", output)
                self.assertIn("controller -> native missing:", output)
                self.assertIn("  GET /controller-only", output)
                self.assertIn("api-reference -> native missing:", output)
                self.assertIn("  GET /reference-only", output)
                self.assertIn("native -> controller extra:", output)
                self.assertIn("  GET /native-only", output)
                self.assertIn("duplicate native routes:", output)
                self.assertIn("route owner mismatch: GET /api/config/list", output)
                self.assertIn("settings route owner count mismatch:", output)


if __name__ == "__main__":
    unittest.main()
