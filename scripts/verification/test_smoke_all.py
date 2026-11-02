"""Host-only tests for smoke_all output modes and report completeness."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import smoke_all  # noqa: E402


OUTPUT_DIR = ROOT / ".tmp" / "smoke-all-tests"
TEST_ROUTES = [
    {"m": "GET", "p": "/pass"},
    {"m": "GET", "p": "/warn"},
    {"m": "GET", "p": "/fail"},
]
TEST_RESPONSES = {
    "/pass": {
        "status": 200,
        "body": '{"ok":true}',
        "elapsed": 0.01,
        "error": None,
    },
    "/warn": {
        "status": 404,
        "body": '{"error":"missing"}',
        "elapsed": 0.02,
        "error": None,
    },
    "/fail": {
        "status": None,
        "body": "",
        "elapsed": 0.03,
        "error": "ConnectionRefusedError: mocked failure",
    },
}


class SmokeAllOutputTests(unittest.TestCase):
    def setUp(self):
        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    def run_smoke(self, argv):
        output = io.StringIO()

        def fake_fetch(base, route):
            return {
                "url": smoke_all.build_url(base, route),
                **TEST_RESPONSES[route["p"]],
            }

        with (
            patch.object(smoke_all, "ROUTES", TEST_ROUTES),
            patch.object(smoke_all, "validate_route_coverage") as coverage,
            patch.object(smoke_all, "fetch", side_effect=fake_fetch),
            patch.object(smoke_all.time, "sleep") as sleep,
            redirect_stdout(output),
        ):
            result = smoke_all.main(argv)

        coverage.assert_called_once_with()
        self.assertEqual(sleep.call_count, len(TEST_ROUTES))
        return result, output.getvalue()

    def test_default_output_keeps_warn_and_fail_details_and_complete_json(self):
        report_path = OUTPUT_DIR / "default.json"
        result, output = self.run_smoke(["http://mock.invalid", str(report_path)])

        self.assertEqual(result, 1)
        self.assertNotIn(" PASS ", output)
        self.assertIn(" warn GET  /warn", output)
        self.assertIn("FAIL GET  /fail", output)
        self.assertIn("状态码 404", output)
        self.assertIn("ConnectionRefusedError: mocked failure", output)
        self.assertIn("摘要: total=3 pass=1 warn=1 fail=1", output)

        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["summary"], {"pass": 1, "warn": 1, "fail": 1, "total": 3})
        self.assertEqual([route["path"] for route in report["routes"]], [
            "/pass", "/warn", "/fail",
        ])
        self.assertEqual([route["verdict"] for route in report["routes"]], [
            "pass", "warn", "fail",
        ])

    def test_all_prints_pass_routes_and_json_matches_default_report(self):
        default_path = OUTPUT_DIR / "comparison-default.json"
        all_path = OUTPUT_DIR / "all.json"
        self.assertEqual(
            self.run_smoke(["http://mock.invalid", str(default_path)])[0], 1
        )
        result, output = self.run_smoke([
            "http://mock.invalid", "--all", str(all_path),
        ])

        self.assertEqual(result, 1)
        self.assertIn(" PASS GET  /pass", output)
        self.assertIn(" warn GET  /warn", output)
        self.assertIn("FAIL GET  /fail", output)
        default_report = json.loads(default_path.read_text(encoding="utf-8"))
        all_report = json.loads(all_path.read_text(encoding="utf-8"))
        self.assertEqual(all_report["summary"], default_report["summary"])
        self.assertEqual(all_report["routes"], default_report["routes"])

    def test_argparse_accepts_all_between_legacy_positionals(self):
        parsed = smoke_all.parse_args([
            "http://mock.invalid", "--all", str(OUTPUT_DIR / "mixed.json"),
        ])

        self.assertEqual(parsed.base_url, "http://mock.invalid")
        self.assertEqual(parsed.output_json, OUTPUT_DIR / "mixed.json")
        self.assertTrue(parsed.all)


if __name__ == "__main__":
    unittest.main()
