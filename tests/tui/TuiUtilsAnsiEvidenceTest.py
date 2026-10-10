#!/usr/bin/env python3
"""#957: verify the utils-ansi named evidence against frozen pi-v1.0.4.

The bundle is read through the named runner, so a wrong-version, missing, or
digest-mismatched artifact fails instead of falling back to an older baseline.
The cases asserted here are the discriminating ones: a stand-in that keeps a
staged control on the row a wrap break pushes, pads outside an open span, keeps
a control that only styles dropped text, or rewrites a hyperlink terminator
cannot satisfy them.
"""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts/tui/evidence.py"
BUNDLE = ROOT / "fixtures/pi-tui/bundles/pi-v1.0.4/utils-ansi.json"
REVISION = "7c10bd4337495ee613f2224843ecdf349b80d1df"


def scenario() -> dict:
    payload = json.loads(BUNDLE.read_text())
    assert payload["revision"] == REVISION, payload["revision"]
    scenarios = payload["scenarios"]
    assert [entry["name"] for entry in scenarios] == ["ansi-boundary-order-and-hyperlinks"], scenarios
    return scenarios[0]


def rows(name: str) -> dict[str, dict]:
    return {row["name"]: row for row in scenario()["expected"][name]}


class TuiUtilsAnsiEvidenceTest(unittest.TestCase):
    def test_named_bundle_verifies_and_includes_utils_ansi_family(self) -> None:
        result = subprocess.run(
            [sys.executable, str(RUNNER), "verify", "--baseline", "pi-v1.0.4"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verified 6 TUI artifacts", result.stdout)

    def test_wrong_baseline_is_rejected(self) -> None:
        result = subprocess.run(
            [sys.executable, str(RUNNER), "verify", "--baseline", "pi-v1.0.0"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown TUI baseline", result.stderr)

    def test_wrap_case_excludes_the_staged_control_from_the_pushed_row(self) -> None:
        row = rows("wrap")["staged-control-after-content"]
        self.assertEqual(row["output"], ["中文", "\x1b[31mABCD", "\x1b[31mEFGH", "\x1b[31mIJ"])
        self.assertNotIn("\x1b[31m", row["output"][0])

    def test_wrap_cases_preserve_each_link_terminator(self) -> None:
        wrap = rows("wrap")
        self.assertEqual(
            wrap["staged-hyperlink-after-content"]["output"],
            ["中文", "\x1b]8;;u\x07ABCD\x1b]8;;\x07", "\x1b]8;;u\x07EFGH"],
        )
        self.assertEqual(
            wrap["st-terminated-link-across-break"]["output"],
            ["\x1b]8;;u\x1b\\hello\x1b]8;;\x1b\\", "\x1b]8;;u\x1b\\world\x1b]8;;\x1b\\"],
        )

    def test_slice_case_pins_the_slice_start_code_order(self) -> None:
        row = rows("slice")["slice-start-code-order"]
        self.assertEqual(row["output"], "\x1b[32m\x1b[39m bar")
        self.assertTrue(row["output"].startswith("\x1b[32m\x1b[39m"))

    def test_slice_inside_an_open_link_keeps_the_link_open(self) -> None:
        row = rows("slice")["slice-inside-open-link"]
        self.assertEqual(row["output"], "\x1b]8;;u\x07cde")
        self.assertNotIn("\x1b]8;;\x07", row["output"])

    def test_truncate_pads_inside_an_open_span(self) -> None:
        row = rows("truncate")["fits-open-underline-pads-inside"]
        self.assertEqual(row["output"], "\x1b[4mabc     ")
        self.assertNotIn("\x1b[24m", row["output"])

    def test_truncate_drops_controls_that_only_style_dropped_text(self) -> None:
        truncate = rows("truncate")
        for name in ("pending-control-styles-dropped-text", "pending-link-styles-dropped-text"):
            with self.subTest(name=name):
                self.assertEqual(truncate[name]["output"], "\x1b[4ma\x1b[0m...\x1b[0m")
        self.assertNotIn("\x1b]8;;\x07", truncate["pending-link-styles-dropped-text"]["output"])

    def test_truncate_closes_the_link_with_its_own_terminator(self) -> None:
        self.assertEqual(
            rows("truncate")["st-terminated-link-closes-before-reset"]["output"],
            "\x1b]8;;u\x1b\\a\x1b]8;;\x1b\\\x1b[0m...\x1b[0m",
        )

    def test_link_column_cases_cover_every_cell_of_a_wide_grapheme(self) -> None:
        link = rows("linkColumns")
        self.assertEqual(link["cjk-link-first-cell"]["link"], "https://x")
        self.assertEqual(link["cjk-link-second-cell"]["link"], "https://x")
        self.assertEqual(link["cjk-link-first-cell"]["input"], link["cjk-link-second-cell"]["input"])
        self.assertEqual(link["cjk-link-first-cell"]["column"] + 1, link["cjk-link-second-cell"]["column"])
        self.assertIsNone(link["cell-after-link-close"]["link"])


if __name__ == "__main__":
    unittest.main()