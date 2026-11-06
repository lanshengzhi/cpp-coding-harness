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

import copy
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
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


def orphan_keys(plan: dict) -> list[str]:
    """The orphan coverage keys the plan registers, in plan order.

    The orphan count is derived from `orphanScenarios`, so the tests read the expected value from
    the plan instead of restating a number that changes whenever a lane registers a scenario the
    consolidation does not route. The ledger's own output is compared against this derivation.
    """
    return [
        f"{item['family']}/{item['artifact']}:{item['scenario']}"
        for item in plan["orphanScenarios"]
    ]


class DeliveryEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        files = {
            "src/Example.cpp": "implementation\n",
            "tests/ExampleTest.cpp": "public comparison and negative control\n",
            "docs/delivery.md": "Reviewed comparison mapping\n",
            "docs/result.xml": '<testsuite tests="2" failures="0"><testcase name="compare" status="run"/>'
                               '<testcase name="control" status="run"/></testsuite>',
        }
        for relative, value in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value)
        self.pin = lambda relative: {
            "path": relative, "sha256": hashlib.sha256((self.root / relative).read_bytes()).hexdigest()
        }
        self.rows = [{"id": "cch_mcp:example", "membership": "supported", "comparisonScenarios": ["preserves null"]}]
        self.delivery = {
            "schema": "pike/pi-v1.3.0-capability-deliveries/1",
            "baseline": "pi-v1.3.0", "revision": consolidate.REVISION,
            "records": [{
                "id": "example", "ticket": "1101", "implementationCommits": ["a" * 40],
                "report": "docs/delivery.md",
                "inputs": [self.pin("src/Example.cpp"), self.pin("tests/ExampleTest.cpp")],
                "validation": {
                    "command": ["ctest", "--preset", "vcpkg"], "testedRevision": "b" * 40,
                    "recordedAt": "2026-10-25T00:00:00Z", "selected": 2,
                    "junit": self.pin("docs/result.xml"),
                },
                "rows": [{
                    "id": "cch_mcp:example", "remainingBoundary": [],
                    "comparisons": [{"artifact": "audit-1101/example.json", "scenario": "preserves null",
                                     "test": "compare", "testSource": "tests/ExampleTest.cpp"}],
                    "discriminatingCase": {"test": "control", "testSource": "tests/ExampleTest.cpp",
                                           "reason": "An absent field and a JSON null produce distinct results."},
                }],
            }],
        }
        self.record = self.delivery["records"][0]
        self.claim = self.record["rows"][0]
        self.artifacts = {"audit-1101/example.json": json.dumps({"scenarios": [{"name": "preserves null"}]}).encode()}
        self.record["routingSha256"] = consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts)
        self.root_patch = patch.object(consolidate, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.verify_patch = patch.object(consolidate.audit, "verify", return_value=(
            {"revision": consolidate.REVISION}, self.artifacts, "verified upstream"))
        self.verify_patch.start()
        self.addCleanup(self.verify_patch.stop)

    def check(self):
        updates, _bounded = consolidate.delivery_updates(self.rows, self.delivery)
        return updates

    def check_bounded(self):
        return consolidate.delivery_updates(self.rows, self.delivery)

    def test_routing_digest_is_stable_and_binds_row_scenario_and_artifact(self) -> None:
        digest = consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts)
        self.assertEqual(digest, consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts))
        self.artifacts["audit-9999/unrelated.json"] = b'{"scenarios":[{"name":"unrelated"}]}'
        self.assertEqual(digest, consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts))
        self.artifacts["audit-9999/unrelated.json"] = b'{"scenarios":[{"name":"preserves null"}]}'
        with self.assertRaisesRegex(consolidate.ConsolidationError, "ambiguous"):
            consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts)
        del self.artifacts["audit-9999/unrelated.json"]
        self.rows[0]["id"] = self.claim["id"] = "cch_mcp:other"
        self.assertNotEqual(digest, consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts))
        self.rows[0]["id"] = self.claim["id"] = "cch_mcp:example"
        self.rows[0]["comparisonScenarios"] = ["other"]
        self.artifacts["audit-1101/example.json"] = b'{"scenarios":[{"name":"other"}]}'
        self.assertNotEqual(digest, consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts))
        self.rows[0]["comparisonScenarios"] = ["preserves null"]
        self.artifacts.clear()
        self.artifacts["audit-1101/other.json"] = b'{"scenarios":[{"name":"preserves null"}]}'
        self.assertNotEqual(digest, consolidate.delivery_routing_digest(self.rows, self.record["rows"], self.artifacts))

    def test_complete_delivery_promotes_only_its_row(self) -> None:
        self.rows.append({"id": "cch_mcp:unrelated", "membership": "supported", "comparisonScenarios": ["other"]})
        before = copy.deepcopy(self.rows)
        updates = self.check()
        self.assertEqual(set(updates), {"cch_mcp:example"})
        self.assertEqual(updates["cch_mcp:example"]["verification"], "verified")
        self.assertEqual(self.rows, before)

    def test_rejects_wrong_baseline_or_revision(self) -> None:
        for key in ("baseline", "revision"):
            with self.subTest(key=key):
                saved = self.delivery[key]
                self.delivery[key] = "historical"
                with self.assertRaisesRegex(consolidate.ConsolidationError, "baseline/revision"):
                    self.check()
                self.delivery[key] = saved

    def test_changed_source_or_test_invalidates_a_previously_green_result(self) -> None:
        for relative in ("src/Example.cpp", "tests/ExampleTest.cpp", "docs/result.xml"):
            with self.subTest(path=relative):
                path = self.root / relative
                saved = path.read_bytes()
                try:
                    path.write_bytes(saved + b"changed")
                    with self.assertRaisesRegex(
                        consolidate.ConsolidationError,
                        rf"example: delivery digest mismatch: {re.escape(relative)}.*1058-delivery-reconciliation\.md",
                    ):
                        self.check()
                finally:
                    path.write_bytes(saved)

    def test_missing_report_or_result_cannot_fall_back(self) -> None:
        for relative in ("docs/result.xml", "docs/delivery.md"):
            with self.subTest(path=relative):
                path = self.root / relative
                saved = path.read_bytes()
                path.unlink()
                with self.assertRaises(consolidate.audit.EvidenceError):
                    self.check()
                path.write_bytes(saved)

    def test_skipped_or_failed_case_cannot_verify_a_row_even_with_a_new_digest(self) -> None:
        for child in ("skipped", "failure", "error"):
            with self.subTest(child=child):
                (self.root / "docs/result.xml").write_text(
                    '<testsuite tests="2"><testcase name="compare" status="run"><' + child + '/></testcase>'
                    '<testcase name="control" status="run"/></testsuite>')
                self.record["validation"]["junit"] = self.pin("docs/result.xml")
                with self.assertRaisesRegex(consolidate.ConsolidationError, "did not pass"):
                    self.check()

    def test_rejects_partial_selection_or_unrun_comparison(self) -> None:
        self.record["validation"]["selected"] = 3
        with self.assertRaisesRegex(consolidate.ConsolidationError, "selected count"):
            self.check()
        self.record["validation"]["selected"] = 2
        self.claim["comparisons"][0]["test"] = "not selected"
        with self.assertRaisesRegex(consolidate.ConsolidationError, "did not run"):
            self.check()

    def test_rejects_incomplete_coverage(self) -> None:
        self.rows[0]["comparisonScenarios"].append("uncompared error")
        with self.assertRaisesRegex(consolidate.ConsolidationError, "audit obligations"):
            self.check()
        self.rows[0]["comparisonScenarios"].pop()

    def test_bounded_claim_validates_but_never_promotes_its_row(self) -> None:
        """A non-empty remainingBoundary bounds the claim: validated, not promoted (#1229)."""
        self.claim["remainingBoundary"] = ["unverified input"]
        updates, bounded = self.check_bounded()
        self.assertEqual(updates, {})
        self.assertEqual(bounded, {"cch_mcp:example"})
        # The bounded claim stays in the receipt with its boundary text.
        self.assertEqual(self.claim["remainingBoundary"], ["unverified input"])

    def test_malformed_remaining_boundary_fails_closed(self) -> None:
        for bad in ("unverified input", [""], [1]):
            with self.subTest(boundary=bad):
                self.claim["remainingBoundary"] = bad
                with self.assertRaisesRegex(consolidate.ConsolidationError, "remainingBoundary"):
                    self.check_bounded()
        self.claim["remainingBoundary"] = []

    def test_rejects_unregistered_scenario_and_missing_negative_control(self) -> None:
        self.artifacts.clear()
        with self.assertRaisesRegex(consolidate.ConsolidationError, "not registered"):
            self.check()
        self.artifacts["audit-1101/example.json"] = b'{"scenarios": [{"name": "another"}]}'
        with self.assertRaisesRegex(consolidate.ConsolidationError, "scenario is missing"):
            self.check()
        self.artifacts["audit-1101/example.json"] = b'{"scenarios": [{"name": "preserves null"}]}'
        self.claim["discriminatingCase"]["test"] = "not selected"
        with self.assertRaisesRegex(consolidate.ConsolidationError, "discriminating case"):
            self.check()

    def test_cannot_promote_excluded_or_duplicate_rows(self) -> None:
        self.rows[0]["membership"] = "excluded"
        with self.assertRaisesRegex(consolidate.ConsolidationError, "membership"):
            self.check()
        self.rows[0]["membership"] = "supported"
        self.record["rows"].append(copy.deepcopy(self.claim))
        with self.assertRaisesRegex(consolidate.ConsolidationError, "duplicate delivered row"):
            self.check()


class WithdrawnClaimsTest(unittest.TestCase):
    """The #1202 per-claim withdrawal channel: exactly the named claims subtract from promotion.

    The temp-root harness claims two rows in one receipt, so a withdrawal of one claim must
    leave its sibling intact, and a withdrawn comparison must drop from its claim while the
    claim stays validated.
    """

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        files = {
            "src/Example.cpp": "implementation\n",
            "tests/ExampleTest.cpp": "public comparison and negative control\n",
            "docs/delivery.md": "Reviewed comparison mapping\n",
            "docs/result.xml": '<testsuite tests="2" failures="0"><testcase name="compare" status="run"/>'
                               '<testcase name="control" status="run"/></testsuite>',
        }
        for relative, value in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value)
        self.pin = lambda relative: {
            "path": relative, "sha256": hashlib.sha256((self.root / relative).read_bytes()).hexdigest()
        }
        self.rows = [
            {"id": "cch_mcp:example", "membership": "supported", "comparisonScenarios": ["preserves null"]},
            {"id": "cch_mcp:sibling", "membership": "supported", "comparisonScenarios": ["keeps order"]},
        ]

        def claim(row_id: str, scenario: str) -> dict:
            return {
                "id": row_id, "remainingBoundary": [],
                "comparisons": [{"artifact": f"audit-1101/{scenario.replace(' ', '-')}.json",
                                 "scenario": scenario, "test": "compare",
                                 "testSource": "tests/ExampleTest.cpp"}],
                "discriminatingCase": {"test": "control", "testSource": "tests/ExampleTest.cpp",
                                       "reason": "An absent field and a JSON null produce distinct results."},
            }

        self.delivery = {
            "schema": "pike/pi-v1.3.0-capability-deliveries/1",
            "baseline": "pi-v1.3.0", "revision": consolidate.REVISION,
            "records": [{
                "id": "example", "ticket": "1101", "implementationCommits": ["a" * 40],
                "report": "docs/delivery.md",
                "inputs": [self.pin("src/Example.cpp"), self.pin("tests/ExampleTest.cpp")],
                "validation": {
                    "command": ["ctest", "--preset", "vcpkg"], "testedRevision": "b" * 40,
                    "recordedAt": "2026-10-25T00:00:00Z", "selected": 2,
                    "junit": self.pin("docs/result.xml"),
                },
                "rows": [claim("cch_mcp:example", "preserves null"),
                         claim("cch_mcp:sibling", "keeps order")],
            }],
        }
        self.record = self.delivery["records"][0]
        self.artifacts = {
            "audit-1101/preserves-null.json": json.dumps({"scenarios": [{"name": "preserves null"}]}).encode(),
            "audit-1101/keeps-order.json": json.dumps({"scenarios": [{"name": "keeps order"}]}).encode(),
        }
        self.record["routingSha256"] = consolidate.delivery_routing_digest(
            self.rows, self.record["rows"], self.artifacts)
        self.root_patch = patch.object(consolidate, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.verify_patch = patch.object(consolidate.audit, "verify", return_value=(
            {"revision": consolidate.REVISION}, self.artifacts, "verified upstream"))
        self.verify_patch.start()
        self.addCleanup(self.verify_patch.stop)

    def updates(self, withdrawn=None):
        updates, _bounded = consolidate.delivery_updates(self.rows, self.delivery, withdrawn)
        return updates

    def test_withdrawal_of_one_claim_leaves_sibling_intact(self) -> None:
        withdrawn = {("example", "cch_mcp:example"): []}
        updates = self.updates(withdrawn)
        self.assertEqual(set(updates), {"cch_mcp:sibling"})
        self.assertEqual(updates["cch_mcp:sibling"]["verification"], "verified")
        effective = consolidate.apply_withdrawals(self.delivery, withdrawn)
        example = next(r for r in effective["records"] if r["id"] == "example")
        self.assertEqual([claim["id"] for claim in example["rows"]], ["cch_mcp:sibling"])
        self.assertEqual(len(next(r for r in self.delivery["records"] if r["id"] == "example")["rows"]), 2)

    def test_withdrawn_comparison_drops_from_the_claim_and_reverts_the_row(self) -> None:
        withdrawn = {("example", "cch_mcp:example"): ["preserves null"]}
        updates = self.updates(withdrawn)
        self.assertEqual(set(updates), {"cch_mcp:sibling"})
        effective = consolidate.apply_withdrawals(self.delivery, withdrawn)
        example = next(c for r in effective["records"] if r["id"] == "example"
                       for c in r["rows"] if c["id"] == "cch_mcp:example")
        self.assertEqual(example["comparisons"], [])
        sibling = next(c for r in effective["records"] if r["id"] == "example"
                       for c in r["rows"] if c["id"] == "cch_mcp:sibling")
        self.assertEqual(len(sibling["comparisons"]), 1)

    def test_unknown_receipt_claim_or_comparison_fails_closed(self) -> None:
        full = {"records": self.delivery["records"]}
        with self.assertRaisesRegex(consolidate.ConsolidationError, "unknown receipt"):
            consolidate.withdrawn_reference_map(
                {("example-missing", "#1202"): {"claims": [{"id": "cch_mcp:example"}]}}, full)
        with self.assertRaisesRegex(consolidate.ConsolidationError, "unknown claim"):
            consolidate.withdrawn_reference_map(
                {("example", "#1202"): {"claims": [{"id": "cch_mcp:absent"}]}}, full)
        with self.assertRaisesRegex(consolidate.ConsolidationError, "unknown comparison"):
            consolidate.withdrawn_reference_map(
                {("example", "#1202"): {"claims": [{"id": "cch_mcp:example",
                                           "withdrawnComparisons": ["not a scenario"]}]}}, full)

    def test_two_records_withdrawing_the_same_claim_fail_closed(self) -> None:
        full = {"records": self.delivery["records"]}
        duplicate = {
            ("example", "#1202"): {"claims": [{"id": "cch_mcp:example"}]},
            ("example", "#1229"): {"claims": [{"id": "cch_mcp:example"}]},
        }
        with self.assertRaisesRegex(consolidate.ConsolidationError, "withdrawn twice"):
            consolidate.withdrawn_reference_map(duplicate, full)

    def test_malformed_withdrawal_records_fail_closed(self) -> None:
        research = self.root / "docs" / "research" / "pi-v1.3.0"
        research.mkdir(parents=True)
        deliveries_path = research / "1058-deliveries.json"
        path_patch = patch.object(consolidate, "DELIVERIES_PATH", deliveries_path)
        path_patch.start()
        self.addCleanup(path_patch.stop)

        def write(name: str, record) -> None:
            (research / name).write_text(json.dumps(record), encoding="utf-8")

        write("1202-withdrawn-example-claims.json", {
            "issue": "#1202", "receipt": "example", "withdrawnAt": "2026-11-05",
            "reason": "withdrawn", "claims": [{"id": "cch_mcp:example"}],
        })
        self.assertEqual(set(consolidate.load_withdrawn_claims()), {("example", "#1202")})
        (research / "1202-withdrawn-example-claims.json").unlink()
        for key in ("issue", "receipt", "withdrawnAt", "reason", "claims"):
            record = {"issue": "#1202", "receipt": "example", "withdrawnAt": "2026-11-05",
                      "reason": "withdrawn", "claims": [{"id": "cch_mcp:example"}]}
            record.pop(key)
            with self.subTest(missing=key):
                write("1202-withdrawn-example-claims.json", record)
                with self.assertRaisesRegex(consolidate.ConsolidationError, "withdrawal record"):
                    consolidate.load_withdrawn_claims()
                (research / "1202-withdrawn-example-claims.json").unlink()

    def test_records_are_keyed_by_receipt_and_issue(self) -> None:
        """One receipt may carry withdrawals from several issues, never two from one (#1229)."""
        research = self.root / "docs" / "research" / "pi-v1.3.0"
        research.mkdir(parents=True)
        deliveries_path = research / "1058-deliveries.json"
        path_patch = patch.object(consolidate, "DELIVERIES_PATH", deliveries_path)
        path_patch.start()
        self.addCleanup(path_patch.stop)

        def write(name: str, issue: str) -> None:
            (research / name).write_text(json.dumps({
                "issue": issue, "receipt": "example", "withdrawnAt": "2026-11-06",
                "reason": "withdrawn", "claims": [{"id": "cch_mcp:example"}],
            }), encoding="utf-8")

        write("1202-withdrawn-example-claims.json", "#1202")
        write("1229-withdrawn-example-claims.json", "#1229")
        self.assertEqual(
            set(consolidate.load_withdrawn_claims()),
            {("example", "#1202"), ("example", "#1229")},
        )
        write("1202-withdrawn-example-claims.json", "#1229")
        with self.assertRaisesRegex(consolidate.ConsolidationError, "duplicate withdrawal record"):
            consolidate.load_withdrawn_claims()

    def test_issue_must_be_a_ticket_reference(self) -> None:
        research = self.root / "docs" / "research" / "pi-v1.3.0"
        research.mkdir(parents=True)
        deliveries_path = research / "1058-deliveries.json"
        path_patch = patch.object(consolidate, "DELIVERIES_PATH", deliveries_path)
        path_patch.start()
        self.addCleanup(path_patch.stop)
        (research / "1229-withdrawn-example-claims.json").write_text(json.dumps({
            "issue": "1229", "receipt": "example", "withdrawnAt": "2026-11-06",
            "reason": "withdrawn", "claims": [{"id": "cch_mcp:example"}],
        }), encoding="utf-8")
        with self.assertRaisesRegex(consolidate.ConsolidationError, "issue is not a #<issue>"):
            consolidate.load_withdrawn_claims()

    def test_empty_withdrawals_leave_the_deliveries_unchanged(self) -> None:
        self.assertIs(consolidate.apply_withdrawals(self.delivery, {}), self.delivery)
        self.assertEqual(consolidate.json_bytes(consolidate.apply_withdrawals(self.delivery, {})),
                         consolidate.json_bytes(self.delivery))
        self.assertEqual(self.updates(), self.updates({}))
        self.assertEqual(set(self.updates()), {"cch_mcp:example", "cch_mcp:sibling"})


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

    def test_source_row_id_scope_rewrites_only_its_own_lane_row(self) -> None:
        """A scope naming the lane's source row id applies there and nowhere else."""
        plan = load(PLAN)
        item = correction(plan, "C1051-1")
        item["appliesTo"] = ["output-pad"]
        identities = consolidate.scope_ids("cch_coding_agent:output-pad", ["output-pad"])

        sources, applied = consolidate.apply_corrections(
            "1051", identities, [item["recorded"]], plan
        )
        self.assertEqual(sources, [item["actual"]])
        self.assertEqual(applied, ["C1051-1"])

        other_lane, applied = consolidate.apply_corrections(
            "1056", identities, [item["recorded"]], plan
        )
        self.assertEqual(other_lane, [item["recorded"]])
        self.assertEqual(applied, [])


class TicketMappingTest(unittest.TestCase):
    """The #1183 E1 ticket-mapping channel: the checked-in plan section retargets rows.

    A mapping rewrites only the ticket reference of a row whose recorded ticket is null
    or the `#1058-draft` placeholder, after the delivery updates; a mapping that names an
    unknown row, appears twice, or collides with a delivery-backed ticket fails closed.
    An absent or empty section rewrites nothing (byte-identical output).
    """

    def setUp(self) -> None:
        self.rows = [
            {"id": "cch_mcp:example", "membership": "supported", "implementation": "missing",
             "verification": "unverified", "implementation_ticket": None, "gaps": ["gap"],
             "notes": "notes", "evidence": {"route": "unobserved"}},
            {"id": "cch_mcp:draft", "membership": "supported", "implementation": "partial",
             "verification": "unverified", "implementation_ticket": "#1058-draft", "gaps": [],
             "notes": "notes", "evidence": {"route": "unobserved"}},
            {"id": "cch_mcp:delivered", "membership": "supported", "implementation": "implemented",
             "verification": "verified", "implementation_ticket": "#1101", "gaps": [],
             "notes": "notes", "evidence": {"route": "captured"}, "delivery": "example"},
        ]

    def entry(self, row_id: str, ticket: str = "#1180", history: bool = False) -> dict:
        return {"id": f"TM-test-{row_id}", "ticket": ticket, "history": history,
                "note": "test mapping", "rows": [row_id]}

    def apply(self, mappings: dict) -> tuple[int, int]:
        return consolidate.apply_ticket_mappings(self.rows, mappings)

    def test_mapping_applies_the_ticket_and_leaves_every_status_untouched(self) -> None:
        before = copy.deepcopy(self.rows)
        applied, history_applied = self.apply({
            "cch_mcp:example": self.entry("cch_mcp:example", "#1180", history=True),
            "cch_mcp:draft": self.entry("cch_mcp:draft", "#1206"),
        })
        self.assertEqual((applied, history_applied), (2, 1))
        self.assertEqual(self.rows[0]["implementation_ticket"], "#1180")
        self.assertEqual(self.rows[1]["implementation_ticket"], "#1206")
        for row, original in zip(self.rows, before):
            for key in ("membership", "implementation", "verification", "gaps", "notes", "evidence"):
                self.assertEqual(row[key], original[key])

    def test_mapping_conflicting_with_a_delivery_ticket_fails(self) -> None:
        with self.assertRaisesRegex(consolidate.ConsolidationError, "delivery-backed ticket"):
            self.apply({"cch_mcp:delivered": self.entry("cch_mcp:delivered", "#1206")})

    def test_unknown_mapping_row_fails_closed(self) -> None:
        with self.assertRaisesRegex(consolidate.ConsolidationError, "unknown row"):
            self.apply({"cch_mcp:absent": self.entry("cch_mcp:absent")})

    def test_malformed_mapping_entries_fail_closed(self) -> None:
        base = self.entry("cch_mcp:example")
        cases = [
            ({"id": "", **{k: v for k, v in base.items() if k != "id"}}, "id"),
            ({**base, "ticket": "1180"}, "ticket"),
            ({**base, "ticket": "#draft"}, "ticket"),
            ({**base, "history": "yes"}, "history"),
            ({**base, "note": " "}, "note"),
            ({**base, "rows": []}, "rows"),
        ]
        for entry, _ in cases:
            with self.subTest(entry=entry):
                with self.assertRaises(consolidate.ConsolidationError):
                    consolidate.load_ticket_mappings({"ticketMappings": {"entries": [entry]}})
        other_id = {**self.entry("cch_mcp:other"), "rows": ["cch_mcp:example"]}
        duplicate = {"ticketMappings": {"entries": [base, other_id]}}
        with self.assertRaisesRegex(consolidate.ConsolidationError, "more than one"):
            consolidate.load_ticket_mappings(duplicate)

    def test_absent_and_empty_mapping_sections_are_inert_and_byte_identical(self) -> None:
        """The compatibility contract: no section and an empty section rewrite nothing.

        Both variants must produce byte-identical ledgers, and that output differs from
        the full plan's build exactly on the mapped rows' ticket references.
        """
        plan = load(PLAN)
        bundles = consolidate.build_bundle_index()
        scenario_index = consolidate.build_scenario_index(bundles)
        without_section = copy.deepcopy(plan)
        without_section.pop("ticketMappings")
        with_empty = copy.deepcopy(plan)
        with_empty["ticketMappings"] = {"entries": []}
        bare_bytes = consolidate.json_bytes(consolidate.build_ledger(without_section, bundles, scenario_index))
        self.assertEqual(
            consolidate.json_bytes(consolidate.build_ledger(with_empty, bundles, scenario_index)),
            bare_bytes,
        )
        full_bytes = consolidate.json_bytes(consolidate.build_ledger(plan, bundles, scenario_index))
        self.assertNotEqual(full_bytes, bare_bytes)
        mapped = {
            row_id: entry["ticket"]
            for entry in plan["ticketMappings"]["entries"]
            for row_id in entry["rows"]
        }
        bare = json.loads(bare_bytes)
        full = json.loads(full_bytes)
        for bare_row, full_row in zip(bare["rows"], full["rows"]):
            self.assertEqual(bare_row["id"], full_row["id"])
            if full_row["id"] in mapped:
                self.assertIn(bare_row["implementation_ticket"], (None, "#1058-draft"))
                self.assertEqual(full_row["implementation_ticket"], mapped[full_row["id"]])
            else:
                self.assertEqual(bare_row["implementation_ticket"], full_row["implementation_ticket"])


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


class ReanchoredLedgerTest(unittest.TestCase):
    """The 13 rows #1202 disputed are re-anchored and verified again.

    The 2026-11-05 re-anchor (commit 71064c7dc) made the cited C++ comparisons and
    discriminating cases read the registered artifact, so the four receipts
    (1090-durable-images, 1118-codemode-catalog, 1179-codemode-comparisons,
    1181-mcp-comparisons) pass scripts/evidence/receipt_boundary.py and the
    per-claim withdrawal records were removed: the full claims of those 13 rows
    promote again and the rows are delivery-backed verified. The #1104/#1189 whole-record
    withdrawals are unaffected and stay withdrawn. The #1226 withdrawal of
    cch_mcp:mcp-request-auth stays withdrawn: the effective deliveries embedded
    in the ledger carry that claim absent. The 182 claims #1236 withdrew across the
    seven 1201-1213 receipts are superseded by the #1229 bounded-claim disposition:
    each claim keeps its full comparisons but carries a non-empty remainingBoundary
    entry, so it stays validated yet does not promote its row (the seven
    1202-withdrawn-<receipt>-claims.json records were removed in the same slice;
    their content is preserved in git history).
    """

    REANCHORED = {
        "cch_coding_agent:codemode-application-namespace": "1118-codemode-catalog",
        "cch_coding_agent:codemode-description-catalog": "1118-codemode-catalog",
        "cch_coding_agent:codemode-description-loadout": "1118-codemode-catalog",
        "cch_codemode:codemode-source-timeout-option": "1179-codemode-comparisons",
        "cch_codemode:codemode-worker-admission": "1179-codemode-comparisons",
        "cch_codemode:codemode-worker-repeated-cancellation": "1179-codemode-comparisons",
        "cch_codemode:codemode-worker-construction-failure": "1179-codemode-comparisons",
        "cch_mcp:mcp-resources-list-read": "1181-mcp-comparisons",
        "cch_mcp:mcp-oauth-discovery": "1181-mcp-comparisons",
        "cch_mcp:mcp-oauth-flow-cancellation": "1181-mcp-comparisons",
        "cch_mcp:mcp-oauth-registration-signin": "1181-mcp-comparisons",
        "cch_durable:image-processor": "1090-durable-images",
        "cch_durable:read-image": "1090-durable-images",
    }

    SIBLINGS = {
        "cch_coding_agent:codemode-models-description-gate": "1118-codemode-catalog",
        "cch_durable:image-capability-absent": "1090-durable-images",
        "cch_durable:image-orientation-conversion": "1090-durable-images",
        "cch_mcp:mcp-http-transport": "1181-mcp-comparisons",
        "cch_mcp:mcp-oauth-issuer": "1181-mcp-comparisons",
    }

    def setUp(self) -> None:
        self.ledger = load(LEDGER)
        self.rows = {row["id"]: row for row in self.ledger["rows"]}
        self.deliveries = {
            record["id"]: record for record in self.ledger["deliveries"]["records"]
        }

    def test_reanchored_rows_are_verified_by_their_receipts(self) -> None:
        plan = load(PLAN)
        mapped = {
            row_id
            for entry in consolidate.load_ticket_mappings(plan)
            for row_id in entry["rows"]
        }
        for row_id, receipt in self.REANCHORED.items():
            with self.subTest(row=row_id):
                row = self.rows[row_id]
                self.assertEqual(row["implementation"], "implemented")
                self.assertEqual(row["verification"], "verified")
                self.assertEqual(row["delivery"], receipt)
                # The re-anchored row carries its delivery ticket again, so it
                # must not be retargeted by a ticket mapping.
                self.assertNotIn(row_id, mapped)

    def test_unaffected_siblings_stay_verified_by_their_receipts(self) -> None:
        for row_id, receipt in self.SIBLINGS.items():
            with self.subTest(row=row_id):
                row = self.rows[row_id]
                self.assertEqual(row["verification"], "verified")
                self.assertEqual(row["delivery"], receipt)

    def test_ledger_embeds_the_full_reanchored_deliveries(self) -> None:
        self.assertEqual(
            [claim["id"] for claim in self.deliveries["1118-codemode-catalog"]["rows"]],
            [
                "cch_coding_agent:codemode-description-catalog",
                "cch_coding_agent:codemode-description-loadout",
                "cch_coding_agent:codemode-application-namespace",
                "cch_coding_agent:codemode-models-description-gate",
            ],
        )
        self.assertEqual(len(self.deliveries["1179-codemode-comparisons"]["rows"]), 39)
        # 1181-mcp-comparisons embeds six effective rows: #1226 withdraws
        # cch_mcp:mcp-request-auth (sham discriminating case), so the claim stays
        # in the full receipt but drops from the ledger's effective deliveries.
        self.assertEqual(len(self.deliveries["1181-mcp-comparisons"]["rows"]), 6)
        image_processor = next(
            claim
            for claim in self.deliveries["1090-durable-images"]["rows"]
            if claim["id"] == "cch_durable:image-processor"
        )
        self.assertEqual(
            [comparison["artifact"] for comparison in image_processor["comparisons"]],
            ["audit-1090/images.json", "audit-1056/environment.json"],
        )
        read_image = next(
            claim
            for claim in self.deliveries["1090-durable-images"]["rows"]
            if claim["id"] == "cch_durable:read-image"
        )
        self.assertEqual(len(read_image["comparisons"]), 4)

    def test_ledger_embeds_the_1229_bounded_and_withdrawn_claims(self) -> None:
        """The seven 1201-1213 receipts embed their full claims: the 182 bounded
        claims keep their comparisons with a remainingBoundary entry, and the
        fourteen #1229-withdrawn 1209 claims drop from the effective receipt."""
        bounded_counts = {
            "1201-terminal-primitives": (25, 30),
            "1203-durable-captures": (3, 3),
            "1209-root-tui-lifecycle": (70, 99),
            "1210-regular-renderer": (9, 15),
            "1211-alt-screen-viewport": (49, 57),
            "1212-transcript-selection": (23, 39),
            "1213-mouse-wheel-routing": (3, 21),
        }
        for receipt, (bounded, total) in bounded_counts.items():
            with self.subTest(receipt=receipt):
                record = self.deliveries[receipt]
                self.assertEqual(len(record["rows"]), total)
                self.assertEqual(
                    sum(1 for claim in record["rows"] if claim["remainingBoundary"]), bounded
                )
        root_tui_ids = {claim["id"] for claim in self.deliveries["1209-root-tui-lifecycle"]["rows"]}
        for row_id in Withdrawn1229LedgerTest.WITHDRAWN_1229:
            self.assertNotIn(row_id, root_tui_ids)

    def test_every_verified_row_is_delivery_backed(self) -> None:
        delivered = [row["id"] for row in self.ledger["rows"] if "delivery" in row]
        # Tripwire on the delivery-backed row count: 392 before the #1236
        # per-claim withdrawals (182 claims across the seven 1201-1213 receipts),
        # 209 after the #1226 withdrawal of cch_mcp:mcp-request-auth, and 195 after
        # #1229 withdraws the fourteen unexercised ScreenStateGoldenTest claims from
        # 1209-root-tui-lifecycle (the 182 discriminating-case rows were already
        # reverted and stay reverted as bounded claims). #1215 (route batch B7)
        # receipted 80 editor rows whole-row: 275. #1217 (route batch B9)
        # receipted 51 markdown/latex rows whole-row: 356. #1218 (route batch B10)
        # receipted 49 color rows whole-row: 405. #1219 (route batch B11)
        # receipted 84 select-list/settings-list rows whole-row: 489. #1220 (route batch B12)
        # receipted 104 layout/scroll/small-text rows whole-row: 593.
        # #1221 (route batch B13 slice A) receipted 9 key identity/parse-match
        # rows whole-row: 602.
        self.assertEqual(len(delivered), 602)
        for row in self.ledger["rows"]:
            if row["verification"] == "verified":
                self.assertIn(row["id"], delivered)

    def test_committed_receipts_keep_their_full_claims(self) -> None:
        """The deliveries file preserves the full receipts; the ledger embeds the effective set."""
        full = load(ROOT / "docs" / "research" / "pi-v1.3.0" / "1058-deliveries.json")
        full_records = {record["id"]: record for record in full["records"]}
        self.assertEqual(len(full_records["1118-codemode-catalog"]["rows"]), 4)
        self.assertEqual(len(full_records["1179-codemode-comparisons"]["rows"]), 39)
        self.assertEqual(len(full_records["1181-mcp-comparisons"]["rows"]), 7)
        image_processor = next(
            claim
            for claim in full_records["1090-durable-images"]["rows"]
            if claim["id"] == "cch_durable:image-processor"
        )
        self.assertEqual(len(image_processor["comparisons"]), 2)

    def test_no_identical_discriminating_case_without_a_boundary(self) -> None:
        """The #1229 stop condition: across the seven 1201-1213 receipts no claim has
        a discriminatingCase.test string-equal to one of its comparisons[].test unless
        the claim records the limitation in its remainingBoundary."""
        full = load(ROOT / "docs" / "research" / "pi-v1.3.0" / "1058-deliveries.json")
        tickets = ("1201", "1203", "1209", "1210", "1211", "1212", "1213")
        checked = 0
        for record in full["records"]:
            if record["ticket"] not in tickets:
                continue
            for claim in record["rows"]:
                dc = claim.get("discriminatingCase")
                if not dc:
                    continue
                checked += 1
                if dc["test"] in {c["test"] for c in claim["comparisons"]}:
                    self.assertTrue(
                        claim["remainingBoundary"],
                        f"{record['id']}:{claim['id']} cites its happy-path test as the "
                        "discriminating case without a remainingBoundary disposition",
                    )
        self.assertEqual(checked, 278)

    def test_committed_ledger_matches_a_fresh_build(self) -> None:
        plan = load(PLAN)
        bundles = consolidate.build_bundle_index()
        scenario_index = consolidate.build_scenario_index(bundles)
        fresh = consolidate.build_ledger(plan, bundles, scenario_index)
        self.assertEqual(consolidate.json_bytes(fresh), LEDGER.read_bytes())


class WithdrawnClaimsLedgerTest(unittest.TestCase):
    """#1226 withdraws cch_mcp:mcp-request-auth from 1181-mcp-comparisons.

    The full receipt in 1058-deliveries.json keeps the claim (its pins stay valid
    and receipt_boundary.py checks it like every other claim); the ledger embeds
    the effective deliveries with the claim absent and the row reverts to its
    source-audit status (ADR 0075 clauses 2 and 6; ADR 0076: a withdrawn claim
    must not be silently reinstated).
    """

    def setUp(self) -> None:
        self.ledger = load(LEDGER)
        self.rows = {row["id"]: row for row in self.ledger["rows"]}
        self.full = load(
            ROOT / "docs" / "research" / "pi-v1.3.0" / "1058-deliveries.json"
        )

    def test_withdrawn_row_reverts_to_source_audit_status(self) -> None:
        row = self.rows["cch_mcp:mcp-request-auth"]
        self.assertEqual(row["implementation"], "partial")
        self.assertEqual(row["verification"], "upstream-captured")
        self.assertIsNone(row["implementation_ticket"])
        self.assertNotIn("delivery", row)
        self.assertNotIn("auditStatus", row)
        self.assertIn(
            "C++ comparison against the registered artifact is not run",
            row["remainingWork"],
        )

    def test_full_receipt_keeps_the_claim_and_the_effective_drops_it(self) -> None:
        full_record = next(
            record
            for record in self.full["records"]
            if record["id"] == "1181-mcp-comparisons"
        )
        self.assertIn(
            "cch_mcp:mcp-request-auth",
            [claim["id"] for claim in full_record["rows"]],
        )
        effective = {record["id"]: record for record in self.ledger["deliveries"]["records"]}
        self.assertNotIn(
            "cch_mcp:mcp-request-auth",
            [claim["id"] for claim in effective["1181-mcp-comparisons"]["rows"]],
        )

    def test_withdrawal_record_names_the_claim(self) -> None:
        record = load(
            ROOT
            / "docs"
            / "research"
            / "pi-v1.3.0"
            / "1202-withdrawn-1181-mcp-comparisons-claims.json"
        )
        self.assertEqual(record["issue"], "#1226")
        self.assertEqual(record["receipt"], "1181-mcp-comparisons")
        self.assertEqual(
            [claim["id"] for claim in record["claims"]],
            ["cch_mcp:mcp-request-auth"],
        )


class Withdrawn1229LedgerTest(unittest.TestCase):
    """#1229 disposes of the two evidence-integrity findings on the 1201-1213 receipts.

    Fix 1: fourteen ScreenStateGoldenTest-cited root-TUI-lifecycle claims whose cited
    tests never exercise the row's specific behavior (preserveScreen/alternate-screen
    exits, renderNow, mouse handling, mode/terminal queries, invalidate, focus queries)
    are withdrawn through the per-claim channel
    (1229-withdrawn-1209-root-tui-lifecycle-claims.json, keyed alongside the removed
    #1236 record by (receipt, issue)). The rows revert to source-audit status.

    Fix 2: the 182 non-discriminating discriminatingCase citations across the seven
    receipts are recorded as bounded claims — a non-empty remainingBoundary entry in the
    receipt itself — superseding the #1236 withdrawal records; the claims stay validated
    but do not promote their rows, which remain at their reverted source-audit status
    until a genuinely discriminating case is delivered.
    """

    WITHDRAWN_1229 = [
        "cch_tui:TUI.renderNow",
        "cch_tui:TUI.mode",
        "cch_tui:TUI.terminal",
        "cch_tui:Container.render",
        "cch_tui:Container.invalidate",
        "cch_tui:Container.handleMouse",
        "cch_tui:Container.mouseLayout",
        "cch_tui:Component.handleInput",
        "cch_tui:Component.invalidate",
        "cch_tui:Component.render",
        "cch_tui:Component.handleMouse",
        "cch_tui:Focusable",
        "cch_tui:Focusable.focused",
        "cch_tui:TuiMode",
    ]

    BOUNDED_182 = {
        "1201-terminal-primitives": 25,
        "1203-durable-captures": 3,
        "1209-root-tui-lifecycle": 70,
        "1210-regular-renderer": 9,
        "1211-alt-screen-viewport": 49,
        "1212-transcript-selection": 23,
        "1213-mouse-wheel-routing": 3,
    }

    def setUp(self) -> None:
        self.ledger = load(LEDGER)
        self.rows = {row["id"]: row for row in self.ledger["rows"]}
        self.full = load(
            ROOT / "docs" / "research" / "pi-v1.3.0" / "1058-deliveries.json"
        )

    def test_withdrawn_rows_revert_to_source_audit_status(self) -> None:
        for row_id in self.WITHDRAWN_1229:
            with self.subTest(row=row_id):
                row = self.rows[row_id]
                self.assertEqual(row["verification"], "unverified")
                self.assertEqual(row["implementation"], "implemented")
                self.assertIsNone(row["implementation_ticket"])
                self.assertNotIn("delivery", row)
                self.assertNotIn("auditStatus", row)

    def test_full_receipt_keeps_the_claim_and_the_effective_drops_it(self) -> None:
        full_record = next(
            record
            for record in self.full["records"]
            if record["id"] == "1209-root-tui-lifecycle"
        )
        self.assertEqual(len(full_record["rows"]), 113)
        self.assertIn(
            "cch_tui:TUI.renderNow",
            [claim["id"] for claim in full_record["rows"]],
        )
        effective = {record["id"]: record for record in self.ledger["deliveries"]["records"]}
        self.assertEqual(len(effective["1209-root-tui-lifecycle"]["rows"]), 99)
        self.assertNotIn(
            "cch_tui:TUI.renderNow",
            [claim["id"] for claim in effective["1209-root-tui-lifecycle"]["rows"]],
        )

    def test_withdrawal_record_names_the_claims(self) -> None:
        record = load(
            ROOT
            / "docs"
            / "research"
            / "pi-v1.3.0"
            / "1229-withdrawn-1209-root-tui-lifecycle-claims.json"
        )
        self.assertEqual(record["issue"], "#1229")
        self.assertEqual(record["receipt"], "1209-root-tui-lifecycle")
        self.assertEqual(
            [claim["id"] for claim in record["claims"]], self.WITHDRAWN_1229
        )

    def test_bounded_rows_stay_at_their_source_audit_status(self) -> None:
        """The 182 bounded rows were reverted by #1236 and stay reverted: no delivery
        association, no verified label, and no ticket mapping conflicts."""
        full_records = {record["id"]: record for record in self.full["records"]}
        for receipt, count in self.BOUNDED_182.items():
            record = full_records[receipt]
            bounded = [
                claim
                for claim in record["rows"]
                if claim.get("remainingBoundary")
            ]
            self.assertEqual(len(bounded), count, receipt)
            for claim in bounded:
                with self.subTest(receipt=receipt, row=claim["id"]):
                    self.assertTrue(claim["remainingBoundary"][0].startswith("#1229"))
                    row = self.rows[claim["id"]]
                    self.assertNotIn("delivery", row)
                    self.assertNotEqual(row["verification"], "verified")

    def test_the_1236_withdrawal_records_are_removed(self) -> None:
        """The limitation now lives in the receipts' remainingBoundary entries."""
        research = ROOT / "docs" / "research" / "pi-v1.3.0"
        for receipt in self.BOUNDED_182:
            self.assertFalse(
                (research / f"1202-withdrawn-{receipt}-claims.json").exists(), receipt
            )


class TicketMappedLedgerTest(unittest.TestCase):
    """The committed ledger carries the #1183 E1 plan mappings exactly.

    Every mapped row's ticket is its mapped issue (live reference or history reference),
    no delivery-backed row carries a mapping (the conflict rule), and the rows the
    execution report left unmapped per the reported ambiguity sets now carry the
    owner's 2026-11-05 rulings: the proposal group-2 abort-family and admission-family
    candidates map to #1148, and the seven implemented Session rows whose per-row
    delivering slice is unresolvable map to live verification owner #1180.
    """

    RULED_ABORT_ADMISSION_1148 = [
        "cch_durable:busy-admission-modes",
        "cch_durable:conversation-abort",
        "cch_durable:conversation-abort-effects",
        "cch_durable:generation-admission",
        "cch_durable:idle-input-admission",
        "cch_durable:passive-write-admission",
        "cch_durable:recovery-abort-mark",
        "cch_durable:recovery-after-admission",
        "cch_durable:submission-abort",
        "cch_durable:wait-cancellation-is-not-abort",
    ]
    RULED_DRAFT_IMPLEMENTED_1180 = [
        "cch_coding_agent:AgentSession:lifecycle:close",
        "cch_coding_agent:Session:assembly:createAgentSession",
        "cch_coding_agent:Session:event-commitment",
        "cch_coding_agent:Session:fork:branch-file",
        "cch_coding_agent:Session:history:tree-ops",
        "cch_coding_agent:Session:publication:deferred-jsonl",
        "cch_coding_agent:Session:tree:live-store",
    ]

    def setUp(self) -> None:
        self.plan = load(PLAN)
        self.ledger = load(LEDGER)
        self.rows = {row["id"]: row for row in self.ledger["rows"]}
        self.entries = consolidate.load_ticket_mappings(self.plan)

    def test_every_mapping_is_applied_to_its_row(self) -> None:
        self.assertTrue(self.entries)
        for entry in self.entries:
            for row_id in entry["rows"]:
                with self.subTest(entry=entry["id"], row=row_id):
                    row = self.rows[row_id]
                    self.assertEqual(row["implementation_ticket"], entry["ticket"])
                    self.assertNotIn("delivery", row)

    def test_no_delivery_backed_row_carries_a_mapping(self) -> None:
        mapped = {
            row_id for entry in self.entries for row_id in entry["rows"]
        }
        for row in self.ledger["rows"]:
            if "delivery" in row:
                self.assertNotIn(row["id"], mapped)

    WITHDRAWN_UNREFERENCED = [
        "cch_durable:background-ownership-boundary",
        "cch_durable:harness-settings-defaults",
        "cch_durable:submission-queues",
        "cch_mcp:mcp-request-auth",
    ]
    UNREFERENCED_1058_DRAFT = [
        "cch_tui:CURSOR_MARKER",
        "cch_tui:TUI",
        "cch_tui:TUI.showOverlay",
        "cch_tui:TuiAltScreen",
        "cch_tui:TuiAltScreen.clearTextSelection",
        "cch_tui:TuiAltScreen.lastClick",
        "cch_tui:TuiAltScreen.render",
        "cch_tui:TuiAltScreen.resetTextSelection",
        "cch_tui:TuiMainScreen.doRender",
        "cch_tui:renderFakeCursor",
        "cch_tui:tui:fake-cursor-marker-constants",
        "cch_tui:tui:resolveFakeCursors",
    ]

    def test_unreferenced_rows_match_the_recorded_sets_and_the_rulings_are_applied(self) -> None:
        null_refs, draft_refs = consolidate.unreferenced_ticket_rows(self.ledger["rows"])
        # Reverted rows carry no delivery ticket until a genuine re-delivery: the
        # #1229 bounded durable-captures rows and the #1226 mcp-request-auth
        # withdrawal stay unreferenced; the #1058-draft TUI rows remain as the
        # check reports them.
        self.assertEqual(null_refs, self.WITHDRAWN_UNREFERENCED)
        self.assertEqual(draft_refs, self.UNREFERENCED_1058_DRAFT)
        for row_id in self.RULED_ABORT_ADMISSION_1148:
            with self.subTest(row=row_id):
                row = self.rows[row_id]
                self.assertEqual(row["implementation_ticket"], "#1148")
                self.assertIn(row["implementation"], ("missing", "partial"))
        draft_implemented = sorted(
            row["id"]
            for row in self.ledger["rows"]
            if row["implementation_ticket"] == "#1058-draft"
            and row["implementation"] == "implemented"
        )
        self.assertEqual(draft_implemented, [])
        for row_id in self.RULED_DRAFT_IMPLEMENTED_1180:
            with self.subTest(row=row_id):
                row = self.rows[row_id]
                self.assertEqual(row["implementation_ticket"], "#1180")
                self.assertEqual(row["implementation"], "implemented")


class ScenarioCoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.plan = load(PLAN)
        self.ledger = load(LEDGER)

    def test_indexes_a_registered_variant_family(self) -> None:
        bundles = {"audit-1085-replay": {"harness.json": ["resumes after reopen"]}}
        self.assertEqual(
            consolidate.family_for_artifact(
                "fixtures/pi-v1.3.0/audit-1085-replay/harness.json", bundles
            ),
            ("audit-1085-replay", "harness.json"),
        )
        self.assertIsNone(
            consolidate.family_for_artifact("fixtures/pi-v1.3.0/audit-1085-replay/other.json", bundles)
        )

    def test_frontier_accepts_a_variant_family_capture(self) -> None:
        frontier = copy.deepcopy(load(consolidate.FRONTIER_PATH))
        bundles = consolidate.build_bundle_index()
        scenario = next(iter(bundles["audit-1085-replay"]["harness.json"]))
        draft = frontier["drafts"][0]
        draft.setdefault("evidence", {})["captures"] = [
            f"fixtures/pi-v1.3.0/audit-1085-replay/harness.json: {scenario}"
        ]
        notes = consolidate.check_frontier(frontier, self.ledger, bundles)
        self.assertTrue(notes)

    def test_build_reuses_the_supplied_bundle_scenario_index(self) -> None:
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
            consolidate.build_ledger(self.plan, bundles, scenario_index)
        finally:
            consolidate.load_json = original
        self.assertEqual(reads, [])
        registered = {
            name for artifacts in bundles.values() for names in artifacts.values() for name in names
        }
        self.assertEqual(set(scenario_index), registered)

    def test_reports_the_registered_orphans_and_no_unrouted_scenario(self) -> None:
        coverage = self.ledger["counts"]["scenarioCoverage"]
        expected = orphan_keys(self.plan)
        self.assertTrue(expected)
        self.assertEqual(coverage["orphaned"], expected)
        self.assertEqual(coverage["uncovered"], [])
        note = consolidate.coverage_note(coverage)
        self.assertIn(f"{len(expected)} explicit orphan", note)
        self.assertIn("0 uncovered", note)

    def test_orphans_never_double_count_a_named_or_artifact_covered_scenario(self) -> None:
        """The derived count is only meaningful when every scenario has exactly one route."""
        coverage = self.ledger["counts"]["scenarioCoverage"]
        named = {
            scenario for row in self.ledger["rows"] for scenario in row["comparisonScenarios"]
        }
        orphaned = {item["scenario"] for item in self.plan["orphanScenarios"]}
        self.assertFalse(orphaned & named)
        bundles = consolidate.build_bundle_index()
        total = sum(
            len(scenarios)
            for family, artifacts in bundles.items()
            if family != "audit-1046"
            for scenarios in artifacts.values()
        )
        self.assertEqual(coverage["scenarios"], total)
        self.assertEqual(
            coverage["scenarioNamed"]
            + coverage["artifactCovered"]
            + len(coverage["orphaned"])
            + len(coverage["uncovered"]),
            total,
        )

    def test_check_command_prints_the_derived_orphan_count(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "check"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{len(orphan_keys(self.plan))} explicit orphan", result.stdout)
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

    def test_lane_gaps_state_the_missing_observation_instead_of_a_1051_audit(self) -> None:
        """The inherited 1050 gap must match the routing: two adjacent rows, five with no row."""
        for item in self.overlap["rowRouting"]:
            gap_text = " ".join(self.rows[item["row"]]["gaps"])
            self.assertNotIn("audited by #1051", gap_text, item["row"])
            self.assertIn("unaudited", gap_text, item["row"])
            if item["adjacent"]:
                self.assertIn("does not observe this setting", gap_text, item["row"])
                for adjacent in item["adjacent"]:
                    self.assertIn(adjacent.split(":")[-1], gap_text, item["row"])
            else:
                self.assertIn("no #1051 row observes", gap_text, item["row"])


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

    def test_rejects_verification_without_a_whole_row_delivery(self) -> None:
        row = self.rows()["cch_coding_agent:startup-header-help"]
        row["verification"] = "verified"
        row["implementation"] = "implemented"
        row["remainingWork"] = None
        row["missingObservation"] = None
        row["gaps"] = []
        with self.assertRaisesRegex(consolidate.ConsolidationError, "validated delivery"):
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

    def test_accepts_a_scope_naming_the_lane_source_row_id(self) -> None:
        """The checker must accept the same scope identities the builder accepts."""
        item = correction(self.plan, "C1051-1")
        item["appliesTo"] = ["output-pad"]
        index = next(entry for entry in self.ledger["correctionsIndex"] if entry["id"] == "C1051-1")
        index["appliesTo"] = ["output-pad"]
        self.check()

    def test_still_rejects_a_surviving_pointer_under_a_source_row_id_scope(self) -> None:
        """A source-row-id scope must reach the survived-pointer check, not skip the row."""
        item = correction(self.plan, "C1051-1")
        item["appliesTo"] = ["output-pad"]
        index = next(entry for entry in self.ledger["correctionsIndex"] if entry["id"] == "C1051-1")
        index["appliesTo"] = ["output-pad"]
        row = self.rows()["cch_coding_agent:output-pad"]
        row["source"][-1] = item["recorded"]
        row["appliedCorrections"] = []
        with self.assertRaisesRegex(consolidate.ConsolidationError, "survived the corrections index"):
            self.check()

    def test_rejects_a_source_row_id_scope_from_another_lane(self) -> None:
        item = correction(self.plan, "C1051-1")
        item["appliesTo"] = ["output-pad"]
        index = next(entry for entry in self.ledger["correctionsIndex"] if entry["id"] == "C1051-1")
        index["appliesTo"] = ["output-pad"]
        index["ticket"] = "1056"
        with self.assertRaises(consolidate.ConsolidationError):
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

    def test_rejects_a_product_gap_that_claims_a_1051_audit(self) -> None:
        row = self.rows()["cch_coding_agent:product.tuiMode"]
        row["gaps"][-1] = (
            "application product row; v1.3.0 product behavior is audited by #1051 "
            "and is not carried as unchanged"
        )
        with self.assertRaisesRegex(consolidate.ConsolidationError, "still claims #1051 audits"):
            self.check()

    def test_rejects_an_adjacent_gap_that_omits_its_counterpart(self) -> None:
        row = self.rows()["cch_coding_agent:product.showHardwareCursor"]
        row["gaps"][-1] = "no #1051 row observes this product setting; v1.3.0 product behavior is unaudited"
        with self.assertRaisesRegex(consolidate.ConsolidationError, "does not name its adjacent"):
            self.check()


if __name__ == "__main__":
    unittest.main()
