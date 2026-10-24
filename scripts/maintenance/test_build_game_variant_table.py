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


if __name__ == "__main__":
    unittest.main()
