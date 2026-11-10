#!/usr/bin/env python3
"""内容签名驱动的 C18 原生背包布局候选能力测试。"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_game_variant_table as variants  # noqa: E402


class C18NativeBagLayoutCandidateTests(unittest.TestCase):
    def test_requires_both_library_markers(self) -> None:
        self.assertTrue(variants.has_c18_native_bag_layout_candidate_signature(b"C18BAG01+C18Lbag.dat"))
        self.assertFalse(variants.has_c18_native_bag_layout_candidate_signature(b"C18BAG01"))
        self.assertFalse(variants.has_c18_native_bag_layout_candidate_signature(b"C18Lbag.dat"))
        self.assertFalse(variants.has_c18_native_bag_layout_candidate_signature(b"C18BAG01+unrelated.dat"))

    def test_detection_does_not_depend_on_release_filename(self) -> None:
        self.assertFalse(any(name.startswith("艾诺迪亚4_C18L_") for name in variants.CURATION))
        self.assertEqual(
            variants.capabilities_for(
                "future_c18l_release.apk",
                has_hidden=False,
                has_marker=False,
                has_c18_native_bag_layout_candidate=variants.has_c18_native_bag_layout_candidate_signature(
                    b"C18BAG01+C18Lbag.dat"
                ),
            ),
            variants.CAP_C18_NATIVE_BAG_LAYOUT_CANDIDATE,
        )


class MergePreservesHistoryTests(unittest.TestCase):
    EXISTING_ROW = (
        '    {GameSeries::kMonster, "v22", 0x00000003u, '
        '{0x2e, 0x25, 0x87, 0xb4, 0x73, 0x9f, 0xfe, 0x77, 0x05, 0x9b, 0x58, 0xbc, 0x00, 0x72, 0x0d, 0x69}},'
    )

    def test_parse_table_rows_restores_entry(self) -> None:
        entries = variants.parse_table_rows(self.EXISTING_ROW)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["series"], "monster")
        self.assertEqual(entries[0]["version"], "v22")
        self.assertEqual(entries[0]["caps"], variants.CAP_HIDDEN_SEGMENT | variants.CAP_WAREHOUSE)
        self.assertEqual(entries[0]["md5"], "2e2587b4739ffe77059b58bc00720d69")

    def test_missing_source_keeps_entry_and_live_source_refreshes_same_md5(self) -> None:
        existing = variants.parse_table_rows(self.EXISTING_ROW)
        live = [{
            "name": "艾诺迪亚4_monster_v22.apk",
            "source": "apk/game-apk/history/艾诺迪亚4_monster_v22.apk",
            "series": "monster",
            "version": "v22",
            "md5": "2e2587b4739ffe77059b58bc00720d69",
            "sha256": "",
            "size": 0,
            "hidden": True,
            "hidden_size": 0,
            "marker": True,
            "c18_native_bag_layout_candidate": False,
            "caps": variants.CAP_HIDDEN_SEGMENT | variants.CAP_WAREHOUSE | variants.CAP_WAREHOUSE_INLINE,
        }]
        kept, retained = variants.merge_with_existing([], existing)
        self.assertEqual(len(kept), 1)
        self.assertEqual(retained, 1)

        refreshed, retained = variants.merge_with_existing(live, existing)
        self.assertEqual(len(refreshed), 1)
        self.assertEqual(retained, 0)
        self.assertEqual(refreshed[0]["source"], "apk/game-apk/history/艾诺迪亚4_monster_v22.apk")
        self.assertEqual(
            refreshed[0]["caps"],
            variants.CAP_HIDDEN_SEGMENT | variants.CAP_WAREHOUSE | variants.CAP_WAREHOUSE_INLINE,
        )

    def test_rendered_table_round_trips(self) -> None:
        entries, _ = variants.merge_with_existing([], variants.parse_table_rows(self.EXISTING_ROW))
        text = variants.render(entries, ["apk/game-apk/history/艾诺迪亚4_monster_v22.apk"])
        self.assertEqual(variants.parse_table_rows(text), entries)
        self.assertEqual(
            variants.parse_source_list(text),
            ["apk/game-apk/history/艾诺迪亚4_monster_v22.apk"],
        )

    def test_source_list_keeps_archived_entries(self) -> None:
        previous = ["apk/game-apk/history/艾诺迪亚4_monster_v22.apk"]
        merged = variants.merge_sources(["apk/game-apk/艾诺迪亚4_monster_v29.apk"], previous)
        self.assertEqual(
            merged,
            ["apk/game-apk/history/艾诺迪亚4_monster_v22.apk", "apk/game-apk/艾诺迪亚4_monster_v29.apk"],
        )
        self.assertEqual(variants.merge_sources([], previous), previous)


if __name__ == "__main__":
    unittest.main()
