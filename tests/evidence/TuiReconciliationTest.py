#!/usr/bin/env python3
"""Boundary regressions for the #1182 TUI route-batch index.

The verification packet reconciles every supported `cch_tui` row with either a whole-row delivery
receipt or exactly one bounded comparison batch. These tests exercise the public build/check
boundary of `tui_route_batches.py`: the index is deterministic, it is a partition of the supported
rows that the delivery receipts do not already cover, and a batch that loses its owner, caller,
scenario, discriminating case, dependency or implementation obligation is rejected instead of
quietly leaving work unnamed.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "fixtures" / "pi-v1.3.0" / "consolidation-1058" / "tui_route_batches.py"
INDEX = ROOT / "docs" / "research" / "pi-v1.3.0" / "1182-tui-route-batches.json"

SPEC = importlib.util.spec_from_file_location("tui_route_batches", SCRIPT)
batches = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(batches)


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


class TuiRouteBatchTest(unittest.TestCase):
    def setUp(self) -> None:
        self.ledger = load(batches.LEDGER_PATH)
        self.deliveries = load(batches.DELIVERIES_PATH)
        self.index = load(INDEX)

    def test_index_matches_a_deterministic_rebuild(self) -> None:
        self.assertEqual(self.index, batches.build_index(self.ledger, self.deliveries))

    def test_every_supported_row_is_batched_or_delivered(self) -> None:
        rows = batches.supported_rows(self.ledger)
        delivered = batches.delivered_rows(self.deliveries) & {row["id"] for row in rows}
        batched = [row_id for batch in self.index["batches"] for row_id in batch["rows"]]
        self.assertEqual(len(batched), len(set(batched)), "a row appears in more than one batch")
        self.assertEqual(set(batched) | delivered, {row["id"] for row in rows})
        self.assertEqual(len(rows), self.index["supportedRows"])
        self.assertEqual(set(self.index["deliveredRows"]), delivered)
        notes = batches.validate(self.index, self.ledger, self.deliveries)
        self.assertTrue(any("batched" in note for note in notes))

    def test_every_batch_names_its_bounded_work(self) -> None:
        for batch in self.index["batches"]:
            with self.subTest(batch=batch["id"]):
                self.assertEqual(batch["owner"], batches.OWNER)
                self.assertTrue(batch["rows"])
                self.assertTrue(batch["captureFamilies"])
                self.assertTrue(batch["normalCase"].strip())
                self.assertTrue(batch["discriminatingCase"].strip())
                self.assertTrue(batch["dependency"].strip())
                self.assertEqual(bool(batch["implementationRows"]), bool(batch["implementationObligation"]))

    def test_rejects_a_row_dropped_from_the_index(self) -> None:
        index = copy.deepcopy(self.index)
        dropped = index["batches"][0]["rows"].pop()
        implementation = index["batches"][0]["implementationRows"]
        if dropped in implementation:
            implementation.remove(dropped)
        with self.assertRaisesRegex(ValueError, "without a batch"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_rejects_a_row_claimed_by_two_batches(self) -> None:
        index = copy.deepcopy(self.index)
        row_id = index["batches"][0]["rows"][0]
        index["batches"][1]["rows"].append(row_id)
        with self.assertRaisesRegex(ValueError, "assigned to"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_rejects_a_delivered_row_inside_a_batch(self) -> None:
        delivered = batches.delivered_rows(self.deliveries) & {
            row["id"] for row in batches.supported_rows(self.ledger)
        }
        if not delivered:  # pragma: no cover - the packet delivers whole rows by construction
            self.skipTest("no supported cch_tui row carries a delivery receipt yet")
        index = copy.deepcopy(self.index)
        row_id = sorted(delivered)[0]
        index["batches"][0]["rows"].append(row_id)
        with self.assertRaisesRegex(ValueError, "already carries a delivery receipt"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_rejects_a_batch_without_a_discriminating_case(self) -> None:
        index = copy.deepcopy(self.index)
        index["batches"][0]["discriminatingCase"] = " "
        with self.assertRaisesRegex(ValueError, "missing discriminatingCase"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_rejects_an_implementation_row_without_an_obligation(self) -> None:
        index = copy.deepcopy(self.index)
        batch = next(item for item in index["batches"] if item["implementationRows"])
        batch["implementationObligation"] = ""
        with self.assertRaisesRegex(ValueError, "implementation obligation"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_rejects_an_unknown_or_foreign_row(self) -> None:
        index = copy.deepcopy(self.index)
        index["batches"][0]["rows"].append("cch_tui:not-a-capability")
        with self.assertRaisesRegex(ValueError, "is not a supported cch_tui row"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_rejects_a_batch_that_loses_a_recorded_scenario_label(self) -> None:
        index = copy.deepcopy(self.index)
        batch = next(item for item in index["batches"] if item["scenarioLabels"])
        batch["scenarioLabels"] = []
        with self.assertRaisesRegex(ValueError, "scenario labels"):
            batches.validate(index, self.ledger, self.deliveries)

    def test_check_command_passes(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "check"], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("tui route batches check: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
