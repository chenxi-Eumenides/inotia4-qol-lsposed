#!/usr/bin/env python3
"""Unit tests for the owner-fact HTTP route synchronizer."""

import unittest
from collections import Counter
from pathlib import Path

import sync_http_routes as sync


class RouteSyncTests(unittest.TestCase):
    def test_repository_route_sources_match_frozen_inventory(self) -> None:
        plugin_facts, resident_facts = sync.load_route_facts()
        facts = sync.validate_facts(plugin_facts, resident_facts)

        self.assertEqual(len(plugin_facts), 140)
        self.assertEqual(len(resident_facts), 7)
        self.assertEqual(len(facts), 147)
        self.assertEqual(Counter(fact.method for fact in facts), {"GET": 68, "POST": 79})
        self.assertEqual(sum(fact.op_gated for fact in facts), 21)
        aggregate = sync.AGGREGATE.read_text(encoding="utf-8")
        self.assertEqual(sync.render_aggregate(facts, aggregate), aggregate)

    def test_malformed_owner_fact_fails_closed(self) -> None:
        with self.assertRaises(sync.RouteSyncError):
            sync.parse_route_line(
                'QOL_HTTP_ROUTE(kPatch, "/api/example", "owner", 0, 1)',
                "QOL_HTTP_ROUTE",
                "plugins/owner/routes.inc",
                3,
            )

    def test_duplicate_route_facts_fail_closed(self) -> None:
        first = sync.RouteFact("GET", "/api/example", "owner", 0, 1, "a/routes.inc", 1)
        second = sync.RouteFact("GET", "/api/example", "owner", 0, 1, "b/routes.inc", 4)
        with self.assertRaisesRegex(sync.RouteSyncError, "duplicate route facts"):
            sync.validate_facts([first, second], [])

    def test_missing_owner_sources_fail_closed(self) -> None:
        with self.assertRaisesRegex(sync.RouteSyncError, "route source inventory mismatch"):
            sync.owner_sources(Path("/path/that/does/not/exist"))

    def test_aggregate_order_uses_existing_keys_but_owner_facts(self) -> None:
        facts = [
            sync.RouteFact("GET", "/api/a", "owner.a", 0, 1, "a/routes.inc", 1),
            sync.RouteFact("POST", "/api/b", "owner.b", 1, 1, "b/routes.inc", 1),
        ]
        old_aggregate = (
            'QOL_HTTP_ROUTE(kPost, "/api/b", "stale", 0, 0)\n'
            'QOL_HTTP_ROUTE(kGet, "/api/a", "stale", 1, 0)\n'
        )
        self.assertEqual(
            sync.render_aggregate(facts, old_aggregate),
            sync.AGGREGATE_HEADER
            + 'QOL_HTTP_ROUTE(kPost, "/api/b", "owner.b", 1, 1)\n'
            + 'QOL_HTTP_ROUTE(kGet, "/api/a", "owner.a", 0, 1)\n',
        )

    def test_new_keys_append_in_deterministic_order(self) -> None:
        facts = [
            sync.RouteFact("GET", "/api/z", "owner.z", 0, 1, "z/routes.inc", 1),
            sync.RouteFact("GET", "/api/a", "owner.a", 0, 1, "a/routes.inc", 1),
        ]
        existing = 'QOL_HTTP_ROUTE(kGet, "/api/old", "old", 0, 1)\n'
        self.assertEqual(
            sync.render_aggregate(facts, existing),
            sync.AGGREGATE_HEADER
            + 'QOL_HTTP_ROUTE(kGet, "/api/a", "owner.a", 0, 1)\n'
            + 'QOL_HTTP_ROUTE(kGet, "/api/z", "owner.z", 0, 1)\n',
        )


if __name__ == "__main__":
    unittest.main()
