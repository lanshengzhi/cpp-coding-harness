#!/usr/bin/env python3
"""#956: verify utils-width named evidence against frozen pi observations."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts/tui/evidence.py"
BUNDLE = ROOT / "fixtures/pi-tui/bundles/pi-v1.0.4/utils-width.json"


def load_scenario() -> dict:
    payload = json.loads(BUNDLE.read_text())
    scenarios = payload["scenarios"]
    assert len(scenarios) == 1
    return scenarios[0]


class TuiUtilsWidthEvidenceTest(unittest.TestCase):
    def test_named_bundle_includes_utils_width_family(self) -> None:
        result = subprocess.run(
            [sys.executable, str(RUNNER), "verify", "--baseline", "pi-v1.0.4"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # The family set grows with every ticket that appends evidence, so this
        # asserts the verified bundle reports a family count and still serves the
        # utils-width artifact rather than pinning one number.
        self.assertRegex(result.stdout, r"^verified [1-9][0-9]* TUI artifacts\n$")
        served = subprocess.run(
            [sys.executable, str(RUNNER), "read", "--baseline", "pi-v1.0.4", "--artifact", "utils-width.json"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(served.returncode, 0, served.stderr)
        self.assertEqual(served.stdout, BUNDLE.read_text())

    def test_visible_width_cases_match_frozen_pi(self) -> None:
        scenario = load_scenario()
        expected = {row["name"]: row for row in scenario["expected"]["visibleWidth"]}
        inputs = {row["name"]: row for row in scenario["inputs"]["visibleWidth"]}
        self.assertEqual(set(expected), set(inputs))
        for name, row in expected.items():
            self.assertEqual(row["output"], inputs[name]["output"], name)
            self.assertGreater(row["output"], 0, name)

    def test_newline_sum_case_is_distinguishing(self) -> None:
        scenario = load_scenario()
        row = next(r for r in scenario["expected"]["visibleWidth"] if r["name"] == "ka-newline-ksha")
        self.assertEqual(row["output"], 4)
        self.assertIn("\n", row["input"])

    def test_narrow_text_and_input_observations_present(self) -> None:
        scenario = load_scenario()
        self.assertEqual(scenario["expected"]["narrowText"]["width2"], ["का", "क्ष"])
        self.assertEqual(scenario["expected"]["narrowText"]["width4"], ["काक्ष"])
        self.assertTrue(scenario["expected"]["narrowInput"]["width6"][0].startswith("> "))


if __name__ == "__main__":
    unittest.main()
