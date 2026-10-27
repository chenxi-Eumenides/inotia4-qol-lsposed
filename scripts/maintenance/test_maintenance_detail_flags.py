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
import check_symbols  # noqa: E402


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
    def _mock_successful_steps(self, calls):
        return mock.patch.object(
            check_arch.subprocess,
            "run",
            side_effect=lambda command, **kwargs: calls.append(command)
            or SimpleNamespace(returncode=0),
        )

    def test_default_runs_every_candidate_in_order(self) -> None:
        calls = []
        expected = (
            "build_game_variant_table",
            "check_layers",
            "check_offsets",
            "check_jni",
            "check_resources",
            "check_symbols",
            "check_http_routes",
            "check_log_policy",
        )
        with mock.patch.object(sys, "argv", ["check_arch.py"]), self._mock_successful_steps(
            calls
        ), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_arch.main(), 0)

        self.assertEqual(check_arch.SCRIPTS, expected)
        self.assertEqual(
            [Path(command[1]).stem for command in calls],
            [name for name in expected if name != "check_log_policy"]
            + ["check_log_policy"],
        )
        self.assertEqual(calls[0][1], str(check_arch.SCRIPT_PATHS["build_game_variant_table"]))

    def test_list_prints_all_steps_without_running_them(self) -> None:
        output = io.StringIO()
        with mock.patch.object(sys, "argv", ["check_arch.py", "--list"]), mock.patch.object(
            check_arch.subprocess, "run"
        ) as run, contextlib.redirect_stdout(output):
            self.assertEqual(check_arch.main(), 0)

        self.assertEqual(output.getvalue().splitlines(), list(check_arch.SCRIPTS))
        run.assert_not_called()

    def test_only_accepts_script_name_with_prefix(self) -> None:
        calls = []
        with mock.patch.object(
            sys, "argv", ["check_arch.py", "--only", "check_jni", "--all"]
        ), self._mock_successful_steps(calls), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_arch.main(), 0)

        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], sys.executable)
        self.assertIn("check_jni.py", calls[0][1])
        self.assertIn("--all", calls[0])
        self.assertNotIn("build_game_variant_table.py", calls[0][1])

    def test_aliases_forward_canonical_all_to_child(self) -> None:
        calls = []
        for flag in DETAIL_FLAGS:
            with self.subTest(flag=flag), mock.patch.object(
                sys, "argv", ["check_arch.py", flag]
            ), self._mock_successful_steps(calls), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check_arch.main(), 0)

        per_alias = len(check_arch.SCRIPTS)
        self.assertEqual(len(calls), len(DETAIL_FLAGS) * per_alias)
        groups = [calls[start : start + per_alias] for start in range(0, len(calls), per_alias)]
        for group in groups:
            for command in group:
                script = Path(command[1]).stem
                if script in check_arch.ALL_CAPABLE:
                    self.assertIn("--all", command)
                else:
                    self.assertNotIn("--all", command)
                self.assertNotIn("--verbose", command)
                self.assertNotIn("-v", command)
        self.assertEqual(groups[0], groups[1])
        self.assertEqual(groups[0], groups[2])

    def test_freeze_json_and_all_are_routed_only_to_capable_children(self) -> None:
        calls = []
        with mock.patch.object(
            sys,
            "argv",
            ["check_arch.py", "--freeze", "--json", "--all"],
        ), self._mock_successful_steps(calls), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_arch.main(), 0)

        by_name = {Path(command[1]).stem: command[2:] for command in calls}
        self.assertEqual(by_name["build_game_variant_table"], ["--all"])
        for name in check_arch.ARCH_CHECKS:
            self.assertEqual(by_name[name], ["--freeze", "--json", "--all"])
        self.assertEqual(by_name["check_symbols"], ["--all"])
        self.assertEqual(by_name["check_http_routes"], ["--all"])
        self.assertEqual(by_name["check_log_policy"], [])

    def test_failures_are_aggregated_and_later_steps_still_run(self) -> None:
        calls = []
        results = iter((SimpleNamespace(returncode=2), SimpleNamespace(returncode=0), SimpleNamespace(returncode=1)))
        output = io.StringIO()
        with mock.patch.object(
            sys, "argv", ["check_arch.py", "--only", "layers,jni,resources"]
        ), mock.patch.object(
            check_arch.subprocess,
            "run",
            side_effect=lambda command, **kwargs: calls.append(command) or next(results),
        ), contextlib.redirect_stdout(output):
            self.assertEqual(check_arch.main(), 1)

        self.assertEqual([Path(command[1]).stem for command in calls], ["check_layers", "check_jni", "check_resources"])
        self.assertIn("失败 2 项", output.getvalue())


class CheckSymbolsGateTests(unittest.TestCase):
    def _run_main(self, symbols):
        constants = [check_symbols.Constant("F_TEST_VMA", "uintptr_t", "0x1000", "", 0x1000)]
        output = io.StringIO()
        with mock.patch.object(sys, "argv", ["check_symbols.py"]), mock.patch.object(
            check_symbols, "read_constants", return_value=constants
        ), mock.patch.object(check_symbols, "load_registry", return_value={"test_symbol": ["F_TEST_VMA"]}), mock.patch.object(
            check_symbols, "read_symbols", return_value=symbols
        ), mock.patch.object(Path, "exists", return_value=True), contextlib.redirect_stdout(output):
            result = check_symbols.main()
        return result, output.getvalue()

    def test_address_mismatch_fails_gate(self) -> None:
        result, output = self._run_main({"test_symbol": 0x2000})
        self.assertEqual(result, 1)
        self.assertIn("地址不一致 1", output)

    def test_missing_symbol_fails_gate(self) -> None:
        result, output = self._run_main({})
        self.assertEqual(result, 1)
        self.assertIn("缺失 1", output)

    def test_constant_expression_and_duplicate_registration_warnings_do_not_fail_gate(self) -> None:
        constants = [
            check_symbols.Constant("F_TEST_VMA", "uintptr_t", "0x1000", "", 0x1000),
            check_symbols.Constant("F_ALIAS_VMA", "uintptr_t", "0x1000", "", 0x1000),
            check_symbols.Constant("F_UNREGISTERED_VMA", "uintptr_t", "BAD", "", None),
        ]
        output = io.StringIO()
        errors = io.StringIO()

        def read_with_warning(_header):
            print("⚠️ 常量 F_UNREGISTERED_VMA 求值失败", file=sys.stderr)
            return constants

        with mock.patch.object(sys, "argv", ["check_symbols.py"]), mock.patch.object(
            check_symbols, "read_constants", side_effect=read_with_warning
        ), mock.patch.object(
            check_symbols, "load_registry", return_value={"test_symbol": ["F_TEST_VMA", "F_ALIAS_VMA"]}
        ), mock.patch.object(
            check_symbols, "read_symbols", return_value={"test_symbol": 0x1000}
        ), mock.patch.object(Path, "exists", return_value=True), contextlib.redirect_stdout(
            output
        ), contextlib.redirect_stderr(errors):
            result = check_symbols.main()

        self.assertEqual(result, 0)
        self.assertIn("被多个宏注册", output.getvalue())
        self.assertIn("求值失败", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
