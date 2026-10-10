#!/usr/bin/env python3
"""#958: verify fuzzy named evidence against frozen pi observations."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts/tui/evidence.py"
BUNDLE = ROOT / "fixtures/pi-tui/bundles/pi-v1.0.4/fuzzy.json"


def load_scenario() -> dict:
    payload = json.loads(BUNDLE.read_text())
    scenarios = payload["scenarios"]
    assert len(scenarios) == 1
    return scenarios[0]


class TuiFuzzyEvidenceTest(unittest.TestCase):
    def test_named_bundle_includes_fuzzy_family(self) -> None:
        result = subprocess.run(
            [sys.executable, str(RUNNER), "verify", "--baseline", "pi-v1.0.4"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # The family set grows with every ticket that appends evidence, so this
        # asserts the verified bundle reports a family count and still serves the
        # fuzzy artifact rather than pinning one number.
        self.assertRegex(result.stdout, r"^verified [1-9][0-9]* TUI artifacts\n$")
        served = subprocess.run(
            [sys.executable, str(RUNNER), "read", "--baseline", "pi-v1.0.4", "--artifact", "fuzzy.json"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(served.returncode, 0, served.stderr)
        self.assertEqual(served.stdout, BUNDLE.read_text())

    def test_match_rows_carry_both_index_spaces(self) -> None:
        scenario = load_scenario()
        rows = scenario["inputs"]["match"]
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row["utf16Length"], len(row["text"].encode("utf-16-le")) // 2, row["name"])
            self.assertEqual(row["utf8Length"], len(row["text"].encode()), row["name"])
        # Byte-indexed scoring can only agree where both index spaces agree, so
        # the distinguishing rows must exist in the frozen corpus.
        differing = [row["name"] for row in rows if row["utf16Length"] != row["utf8Length"]]
        self.assertIn("supplementary-prefix", differing)
        self.assertIn("accented-prefix", differing)

    def test_sharp_s_rows_are_frozen_nonmatches(self) -> None:
        scenario = load_scenario()
        expected = {row["name"]: row for row in scenario["expected"]["match"]}
        self.assertFalse(expected["sharp-s-query"]["matches"])
        self.assertFalse(expected["sharp-s-text"]["matches"])
        self.assertEqual(scenario["inputs"]["match"][0]["name"], "empty-query")

    def test_score_rows_pin_utf16_positions(self) -> None:
        scenario = load_scenario()
        expected = {row["name"]: row for row in scenario["expected"]["match"]}
        self.assertEqual(expected["accented-prefix"]["score"], 0.1)
        self.assertEqual(expected["ascii-prefix"]["score"], 0.2)
        self.assertEqual(expected["supplementary-prefix"]["score"], 0.2)
        self.assertEqual(expected["dotted-capital-prefix"]["score"], 0.2)
        self.assertEqual(expected["combining-prefix"]["score"], 0.2)
        self.assertEqual(expected["accented-pair"]["score"], -4.7)
        self.assertEqual(expected["no-break-space-boundary"]["score"], -9.8)
        self.assertEqual(expected["final-sigma-terminal"]["matches"], False)
        self.assertEqual(expected["final-sigma-initial"]["score"], -15)

    def test_filter_rows_pin_order_not_membership(self) -> None:
        scenario = load_scenario()
        expected = {row["name"]: row for row in scenario["expected"]["filter"]}
        ranked = expected["utf16-rank-with-ties"]["output"]
        self.assertEqual(ranked, ["a", "aa", "éa", "xxa", "\U0001f600a"])
        self.assertEqual(expected["non-ascii-membership"]["output"], ["éa", "xxa", "\U0001f600a"])
        self.assertEqual(expected["final-sigma-filter"]["output"], ["ΣΑ"])
        self.assertEqual(expected["no-break-space-token"]["output"], ["a/b/c"])
        self.assertEqual(expected["no-break-space-trim"]["output"], ["alpha"])


if __name__ == "__main__":
    unittest.main()
