#!/usr/bin/env python3
"""#948: ledger completeness against frozen pi index exports and classification rules."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

import os

ROOT = Path(__file__).resolve().parents[2]
LEDGER = ROOT / "docs/research/tui-v1.0.4-capability-ledger.json"
# Linked worktrees may not sit beside pi; prefer PI_CHECKOUT, then common siblings.
_CANDIDATES = []
if os.environ.get("PI_CHECKOUT"):
    _CANDIDATES.append(Path(os.environ["PI_CHECKOUT"]))
_CANDIDATES.extend(
    [
        ROOT.parent / "pi",
        Path("/home/lansy/Work/github/coding-agent/pi"),
    ]
)
PI = next((path for path in _CANDIDATES if (path / "packages/tui/src/index.ts").is_file()), _CANDIDATES[0])
REV = "7c10bd4337495ee613f2224843ecdf349b80d1df"


def index_exports(index_text: str) -> set[str]:
    names: set[str] = set()
    for match in re.finditer(r"export\s+(?:type\s+)?\{([^}]+)\}\s+from\s+\"([^\"]+)\"", index_text, re.S):
        for raw in match.group(1).split(","):
            raw = raw.strip()
            if not raw:
                continue
            raw = re.sub(r"^type\s+", "", raw).strip()
            if " as " in raw:
                raw = raw.split(" as ")[-1].strip()
            names.add(raw)
    return names


class CapabilityLedgerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ledger = json.loads(LEDGER.read_text())
        cls.export_rows = [
            row
            for row in cls.ledger["rows"]
            if row.get("member") is None
            and row["kind"] not in {"product-entry", "diagnostic-capability", "internal-helper"}
        ]

    def test_ledger_targets_frozen_revision(self) -> None:
        self.assertEqual(self.ledger["baseline"], "pi-v1.0.4")
        self.assertEqual(self.ledger["revision"], REV)

    def test_every_index_export_is_accounted(self) -> None:
        index = (PI / "packages/tui/src/index.ts").read_text()
        expected = index_exports(index)
        observed = {row["export"] for row in self.export_rows}
        self.assertEqual(expected, observed)
        self.assertEqual(len(expected), 173)

    def test_classifications_are_only_allowed_values(self) -> None:
        allowed = {"included", "cpp-representation", "approved-exclusion"}
        for row in self.ledger["rows"]:
            self.assertIn(row["classification"], allowed, row["id"])

    def test_no_included_row_is_deferred_or_closeout_only(self) -> None:
        for row in self.ledger["rows"]:
            if row["classification"] != "included":
                continue
            blob = json.dumps(row)
            self.assertNotIn("Deferred", blob, row["id"])
            self.assertNotEqual(row["tickets"], ["#1025"], row["id"])
            self.assertTrue(row["tickets"], row["id"])

    def test_required_cross_references_are_recorded_without_issue_mutation(self) -> None:
        refs = self.ledger["crossReferences"]
        for key in ("831", "811", "809", "749", "830"):
            self.assertIn(key, refs)
            self.assertIn("none", refs[key]["issueMutation"].lower())
            self.assertTrue(refs[key]["tickets"])

    def test_product_entry_points_and_exclusions_exist(self) -> None:
        ids = {row["id"] for row in self.ledger["rows"]}
        for required in (
            "product.tuiMode",
            "product.--tui-mode",
            "product.runInteractiveMode",
            "Marked",
            "isAppleTerminalSession",
            "diag.PI_TUI_WRITE_LOG",
        ):
            self.assertIn(required, ids)
        marked = next(row for row in self.ledger["rows"] if row["id"] == "Marked")
        self.assertEqual(marked["classification"], "approved-exclusion")

    def test_major_classes_expose_public_operations(self) -> None:
        for name, minimum in (
            ("Editor", 20),
            ("Input", 10),
            ("ProcessTerminal", 10),
            ("StdinBuffer", 5),
            ("TuiAltScreen", 20),
            ("SelectList", 10),
        ):
            row = next(item for item in self.export_rows if item["export"] == name)
            self.assertGreaterEqual(len(row["public_operations"]), minimum, name)
            self.assertEqual(row["classification"], "included")


if __name__ == "__main__":
    unittest.main()
