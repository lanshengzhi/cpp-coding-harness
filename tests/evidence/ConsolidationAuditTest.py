#!/usr/bin/env python3
"""Boundary regressions for the #1058 consolidated capability ledger.

The consolidation tool applies a curated corrections index to the frozen source pointers of each
audit row and reports scenario coverage. These tests exercise the public build/check boundary: a
replacement rewrites only the row in its scope and only a pointer that is exactly equal to the
recorded spelling, so an already-qualified pointer, a pointer with different symbols on the same
path, and a row outside the scope keep the lane's recorded provenance. The committed ledger and
the checker's printed coverage note are the observable results.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "fixtures" / "pi-v1.3.0" / "consolidation-1058" / "consolidate_audits.py"
PLAN = ROOT / "fixtures" / "pi-v1.3.0" / "consolidation-1058" / "consolidation-plan.json"
LEDGER = ROOT / "docs" / "research" / "pi-v1.3.0" / "1058-consolidated-capability-ledger.json"

SPEC = importlib.util.spec_from_file_location("consolidate_audits", SCRIPT)
consolidate = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(consolidate)


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def correction(plan: dict, correction_id: str) -> dict:
    return next(item for item in plan["corrections"] if item["id"] == correction_id)


class CorrectionScopeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.plan = load(PLAN)

    def test_exact_pointer_rewrites_only_the_row_in_scope(self) -> None:
        recorded = correction(self.plan, "C1051-1")["recorded"]
        actual = correction(self.plan, "C1051-1")["actual"]

        sources, applied = consolidate.apply_corrections(
            "1051", ("cch_coding_agent:output-pad",), [recorded], self.plan
        )
        self.assertEqual(sources, [actual])
        self.assertEqual(applied, ["C1051-1"])

        untouched, applied = consolidate.apply_corrections(
            "1051", ("cch_coding_agent:startup-header-help",), [recorded], self.plan
        )
        self.assertEqual(untouched, [recorded])
        self.assertEqual(applied, [])

    def test_already_qualified_symbol_survives_a_file_shorthand_correction(self) -> None:
        recorded = "packages/durable/src/harness/submissions.ts:Submissions.status"
        sources, applied = consolidate.apply_corrections(
            "1056", ("cch_durable:inspect-submissions",), [recorded], self.plan
        )
        self.assertEqual(sources, [recorded])
        self.assertEqual(applied, [])

    def test_different_symbol_on_the_same_path_is_not_widened(self) -> None:
        recorded = "packages/durable/src/tool-search/tool-search.ts:createToolSearchTool"
        sources, applied = consolidate.apply_corrections(
            "1056", ("cch_durable:toolsearch-no-match",), [recorded], self.plan
        )
        self.assertEqual(sources, [recorded])
        self.assertEqual(applied, [])

    def test_record_only_correction_never_rewrites_a_pointer(self) -> None:
        recorded = correction(self.plan, "C1054-9")["recorded"]
        sources, applied = consolidate.apply_corrections(
            "1054", ("cch_durable:Storage:jsonl:poison",), [recorded], self.plan
        )
        self.assertEqual(sources, [recorded])
        self.assertEqual(applied, [])


class CommittedLedgerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.plan = load(PLAN)
        self.ledger = load(LEDGER)
        self.rows = {row["id"]: row for row in self.ledger["rows"]}

    def test_keeps_the_observed_submission_status_symbol(self) -> None:
        row = self.rows["cch_durable:inspect-submissions"]
        self.assertIn("packages/durable/src/harness/submissions.ts:Submissions.status", row["source"])
        self.assertEqual(row["appliedCorrections"], [])

    def test_does_not_add_an_unrecorded_tool_search_export(self) -> None:
        row = self.rows["cch_durable:toolsearch-no-match"]
        self.assertEqual(
            row["source"],
            ["packages/durable/src/tool-search/tool-search.ts:createToolSearchTool"],
        )
        self.assertEqual(row["appliedCorrections"], [])

    def test_records_only_the_replacements_that_actually_rewrote_a_pointer(self) -> None:
        record_only = {
            item["id"] for item in self.plan["corrections"] if item["kind"] == "record-only"
        }
        applied = {
            correction_id
            for row in self.ledger["rows"]
            for correction_id in row["appliedCorrections"]
        }
        self.assertTrue(applied)
        self.assertFalse(applied & record_only)
        for item in self.plan["corrections"]:
            if item["kind"] != "replace":
                continue
            self.assertIn(item["id"], applied, f"{item['id']} rewrote no pointer")

    def test_applied_corrections_reproduce_each_rows_recorded_pointers(self) -> None:
        sources = {
            source["ticket"]: load(ROOT / source["ledger"]) for source in self.plan["sources"]
        }
        for row in self.ledger["rows"]:
            primary = row["sourceRows"][0]
            raw = next(
                item
                for item in sources[primary["ticket"]]["rows"]
                if item["id"] == primary["rowId"]
            )
            recorded, _ = consolidate.extract_sources(primary["ticket"], raw)
            corrected, applied = consolidate.apply_corrections(
                primary["ticket"], (row["id"], primary["rowId"]), recorded, self.plan
            )
            self.assertEqual(corrected, row["source"], row["id"])
            self.assertEqual(applied, row["appliedCorrections"], row["id"])


class ScenarioCoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.ledger = load(LEDGER)

    def test_build_reads_each_bundle_manifest_once(self) -> None:
        plan = load(PLAN)
        bundles = consolidate.build_bundle_index()
        scenario_index = consolidate.build_scenario_index(bundles)
        reads: list[str] = []
        original = consolidate.load_json

        def counting(path: Path):
            if str(path).endswith("manifest.json"):
                reads.append(str(path))
            return original(path)

        consolidate.load_json = counting
        try:
            consolidate.build_ledger(plan, bundles, scenario_index)
        finally:
            consolidate.load_json = original
        self.assertEqual(reads, [])
        registered = {
            name for artifacts in bundles.values() for names in artifacts.values() for name in names
        }
        self.assertEqual(set(scenario_index), registered)

    def test_reports_the_registered_orphans_and_no_unrouted_scenario(self) -> None:
        coverage = self.ledger["counts"]["scenarioCoverage"]
        self.assertEqual(len(coverage["orphaned"]), 3)
        self.assertEqual(coverage["uncovered"], [])
        note = consolidate.coverage_note(coverage)
        self.assertIn("3 explicit orphan", note)
        self.assertIn("0 uncovered", note)

    def test_check_command_prints_the_derived_orphan_count(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "check"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3 explicit orphan", result.stdout)
        self.assertIn("0 uncovered", result.stdout)


class ProductRoutingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.ledger = load(LEDGER)
        self.rows = {row["id"]: row for row in self.ledger["rows"]}
        self.overlap = next(
            item
            for item in self.ledger["overlapResolutions"]
            if item["id"] == "overlap-toolkit-product"
        )

    def test_names_the_adjacent_1051_rows_and_leaves_five_rows_unmatched(self) -> None:
        self.assertEqual(
            set(self.overlap["adjacentRows"]),
            {
                "cch_coding_agent:mode-switch-renderer",
                "cch_coding_agent:exit-restoration",
                "cch_tui:hardware-cursor",
            },
        )
        routing = self.overlap["rowRouting"]
        self.assertEqual({item["row"] for item in routing}, set(self.overlap["rows"]))
        self.assertEqual(len([item for item in routing if not item["adjacent"]]), 5)

    def test_routed_product_rows_stay_not_audited_and_visible(self) -> None:
        for item in self.overlap["rowRouting"]:
            row = self.rows[item["row"]]
            self.assertEqual(row["evidence"]["route"], "not-audited", item["row"])
            self.assertEqual(row["verification"], "unverified", item["row"])
            self.assertTrue(row["missingObservation"], item["row"])
            self.assertTrue(item["observation"], item["row"])
            for adjacent in item["adjacent"]:
                self.assertIn(adjacent, self.overlap["adjacentRows"], item["row"])


class CheckerRejectionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.plan = load(PLAN)
        self.bundles = consolidate.build_bundle_index()
        self.scenario_index = consolidate.build_scenario_index(self.bundles)
        self.ledger = load(LEDGER)

    def rows(self) -> dict[str, dict]:
        return {row["id"]: row for row in self.ledger["rows"]}

    def check(self) -> list[str]:
        return consolidate.check_ledger(
            self.plan, self.ledger, self.bundles, self.scenario_index, None
        )

    def test_accepts_the_committed_ledger(self) -> None:
        self.check()

    def test_rejects_a_correction_applied_to_a_row_outside_its_scope(self) -> None:
        row = self.rows()["cch_coding_agent:startup-header-help"]
        item = correction(self.plan, "C1051-1")
        row["source"][-1] = item["actual"]
        row["appliedCorrections"] = [item["id"]]
        with self.assertRaisesRegex(consolidate.ConsolidationError, "outside its scope"):
            self.check()

    def test_rejects_a_correction_marked_applied_without_its_corrected_pointer(self) -> None:
        row = self.rows()["cch_coding_agent:output-pad"]
        item = correction(self.plan, "C1051-1")
        row["source"][-1] = f"{item['actual']},padOutputLines"
        row["appliedCorrections"] = [item["id"]]
        with self.assertRaisesRegex(consolidate.ConsolidationError, "does not carry the corrected pointer"):
            self.check()

    def test_rejects_a_longer_qualified_pointer_that_survives_its_correction(self) -> None:
        row = self.rows()["cch_coding_agent:output-pad"]
        item = correction(self.plan, "C1051-1")
        row["source"][-1] = f"vendor/{item['recorded']}"
        row["appliedCorrections"] = []
        with self.assertRaisesRegex(consolidate.ConsolidationError, "survived the corrections index"):
            self.check()

    def test_rejects_a_recorded_pointer_that_survives_its_correction(self) -> None:
        row = self.rows()["cch_coding_agent:output-pad"]
        row["source"][-1] = correction(self.plan, "C1051-1")["recorded"]
        row["appliedCorrections"] = []
        with self.assertRaisesRegex(consolidate.ConsolidationError, "survived the corrections index"):
            self.check()

    def test_rejects_a_replacement_that_rewrote_nothing(self) -> None:
        for row in self.ledger["rows"]:
            row["appliedCorrections"] = [
                item for item in row["appliedCorrections"] if item != "C1055-2"
            ]
        with self.assertRaisesRegex(consolidate.ConsolidationError, "C1055-2"):
            self.check()

    def test_rejects_product_routing_to_an_unrelated_row(self) -> None:
        overlap = next(
            item
            for item in self.ledger["overlapResolutions"]
            if item["id"] == "overlap-toolkit-product"
        )
        overlap["adjacentRows"] = ["cch_coding_agent:mode-switch-renderer"]
        overlap["rowRouting"][0]["adjacent"] = ["cch_coding_agent:mode-switch-renderer"]
        with self.assertRaisesRegex(consolidate.ConsolidationError, "not a named 1051 counterpart"):
            self.check()

    def test_rejects_a_product_row_that_claims_its_counterpart_as_evidence(self) -> None:
        row = self.rows()["cch_coding_agent:product.showHardwareCursor"]
        row["verification"] = "upstream-captured"
        with self.assertRaises(consolidate.ConsolidationError):
            self.check()


if __name__ == "__main__":
    unittest.main()
