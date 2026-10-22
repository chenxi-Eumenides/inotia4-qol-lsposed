#!/usr/bin/env python3
"""维护检查器详细输出开关的一致性回归测试。"""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from argparse import Namespace
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arch_common as ac  # noqa: E402
import check_arch  # noqa: E402
import check_resources as cr  # noqa: E402


DETAIL_FLAGS = ("--all", "--verbose", "-v")


class CommonDetailFlagTests(unittest.TestCase):
    def test_common_parser_aliases_are_equivalent(self) -> None:
        parsed = []
        for flag in DETAIL_FLAGS:
            with self.subTest(flag=flag), mock.patch.object(sys, "argv", ["checker", flag]):
                args = ac.parse_common_args("test checker")
                self.assertTrue(args.verbose)
                parsed.append(vars(args))
        self.assertEqual(parsed[0], parsed[1])
        self.assertEqual(parsed[0], parsed[2])

    def test_default_emit_keeps_summary_and_actionable_findings_visible(self) -> None:
        args = Namespace(freeze=False, force_freeze=False, json=False, verbose=False)
        findings = [
            ac.Violation("layer_direction", "game/a.cpp", "infra/b.h", "game → infra"),
            ac.Violation("unknown_layer", "legacy/a.cpp", "", "路径未映射"),
        ]
        output = io.StringIO()
        with mock.patch.object(ac, "load_baseline", return_value=set()), mock.patch.object(
            ac, "load_exemptions", return_value=[]
        ), contextlib.redirect_stdout(output):
            result = ac.emit("test", findings, args, "测试校验", rules=("layer_direction", "unknown_layer"))

        self.assertEqual(result, 1)
        text = output.getvalue()
        self.assertIn("违规 2 条", text)
        self.assertIn("❌ layer_direction: game/a.cpp → infra/b.h", text)
        self.assertIn("⚠️  unknown_layer: legacy/a.cpp", text)

    def test_all_alias_exposes_resolved_baseline_entries(self) -> None:
        resolved = ac.Violation("layer_direction", "game/a.cpp", "infra/b.h", "game → infra")
        output = io.StringIO()
        with mock.patch.object(ac, "load_baseline", return_value={resolved.key()}), mock.patch.object(
            ac, "load_exemptions", return_value=[]
        ), contextlib.redirect_stdout(output):
            result = ac.emit(
                "test",
                [],
                Namespace(freeze=False, force_freeze=False, json=False, verbose=True),
                "测试校验",
                rules=("layer_direction",),
            )

        self.assertEqual(result, 0)
        self.assertIn("✅ 已消除", output.getvalue())


class ResourcesDetailFlagTests(unittest.TestCase):
    def _run_resources(self, flag: str | None):
        argv = ["check_resources.py"] + ([flag] if flag else [])
        points = [cr.InstallPoint("entry hook", "plugins/demo/hook.cpp", 3, "F_DEMO_HOOK_VMA")]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(cr, "scan", return_value=points), mock.patch.object(
            cr, "build_violations", return_value=[]
        ), mock.patch.object(ac, "emit", return_value=0) as emit:
            result = cr.main()
        args = emit.call_args.args[2]
        lines = emit.call_args.args[4]
        return result, args.verbose, lines

    def test_custom_parser_aliases_are_equivalent(self) -> None:
        results = [self._run_resources(flag) for flag in DETAIL_FLAGS]
        for result, verbose, lines in results:
            self.assertEqual(result, 0)
            self.assertTrue(verbose)
            self.assertTrue(any("去重资源清单 1 条：" in line for line in lines))
            self.assertTrue(any("F_DEMO_HOOK_VMA" in line for line in lines))
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0], results[2])

    def test_default_resources_output_omits_only_optional_full_inventory(self) -> None:
        result, verbose, lines = self._run_resources(None)
        self.assertEqual(result, 0)
        self.assertFalse(verbose)
        self.assertTrue(any("安装点 1 个" in line for line in lines))
        self.assertTrue(any("重复安装资源 0 个" in line for line in lines))
        self.assertFalse(any("去重资源清单" in line for line in lines))


class AggregateDetailFlagTests(unittest.TestCase):
    def test_only_accepts_script_name_with_prefix(self) -> None:
        calls = []
        with mock.patch.object(
            sys, "argv", ["check_arch.py", "--only", "check_jni", "--all"]
        ), mock.patch.object(
            check_arch.subprocess,
            "run",
            side_effect=lambda command, **kwargs: calls.append(command)
            or SimpleNamespace(returncode=0),
        ), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_arch.main(), 0)

        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], sys.executable)
        self.assertIn("check_jni.py", calls[0][1])
        self.assertIn("--all", calls[0])

    def test_aliases_forward_canonical_all_to_child(self) -> None:
        calls = []
        for flag in DETAIL_FLAGS:
            with self.subTest(flag=flag), mock.patch.object(
                sys, "argv", ["check_arch.py", flag]
            ), mock.patch.object(
                check_arch.subprocess,
                "run",
                side_effect=lambda command, **kwargs: calls.append(command) or SimpleNamespace(returncode=0),
            ), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check_arch.main(), 0)

        per_alias = len(check_arch.SCRIPTS)
        self.assertEqual(len(calls), len(DETAIL_FLAGS) * per_alias)
        groups = [calls[start : start + per_alias] for start in range(0, len(calls), per_alias)]
        for group in groups:
            for command in group:
                self.assertIn("--all", command)
                self.assertNotIn("--verbose", command)
                self.assertNotIn("-v", command)
        self.assertEqual(groups[0], groups[1])
        self.assertEqual(groups[0], groups[2])


if __name__ == "__main__":
    unittest.main()
