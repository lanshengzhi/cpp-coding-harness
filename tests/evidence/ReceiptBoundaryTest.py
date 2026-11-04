#!/usr/bin/env python3
"""Boundary regressions for the #1202 receipt artifact-anchoring check.

A delivery receipt claims its cited C++ test reads the frozen artifact its rows point at. These
tests exercise ``scripts/evidence/receipt_boundary.py`` at that boundary: the static name + path
matcher must accept the real code-level reference forms (full path literal, path reassembled from
segments, family directory plus file name spanning a quoted include helper) and must reject the
lookalikes that never open the artifact (a sibling file in the same family, a mention in a
comment, a Catch2 tag or scenario key spelling). At the receipt level, an unparsable claim, an
unregistered artifact, a missing test source and a baseline mismatch all fail closed instead of
crashing, and any failing receipt exits non-zero.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "evidence" / "receipt_boundary.py"

SPEC = importlib.util.spec_from_file_location("receipt_boundary", SCRIPT)
boundary = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(boundary)

BASELINE = "test-baseline"
REVISION = "a" * 40
ARTIFACT = "audit-9001/frozen.json"


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


class MatcherTest(unittest.TestCase):
    """The static name + path matcher against the observed C++ reference forms."""

    def setUp(self) -> None:
        self.matcher = boundary.StaticNamePathMatcher()

    def matched(self, code: str, artifact: str = ARTIFACT) -> bool:
        return self.matcher.references(BASELINE, [code], artifact)

    def test_full_fixture_path_literal_matches(self) -> None:
        code = (
            'constexpr std::string_view kArtifact =\n'
            '    "fixtures/pi-v1.3.0/audit-9001/frozen.json";\n'
        )
        self.assertTrue(self.matched(code))

    def test_fixture_root_relative_literal_matches(self) -> None:
        # A file-local loader prepends the fixture root: the literal names the
        # family and file only.
        code = 'const auto scenarios = frozen_scenarios("audit-9001/frozen.json");\n'
        self.assertTrue(self.matched(code))

    def test_path_reassembled_from_segments_matches(self) -> None:
        code = (
            "const auto artifact = std::filesystem::path{CCH_SOURCE_DIR} / \"fixtures\" /\n"
            "                      \"pi-v1.3.0\" / \"audit-9001\" /\n"
            "                      \"frozen.json\";\n"
        )
        self.assertTrue(self.matched(code))

    def test_family_and_file_literals_match_across_includes(self) -> None:
        # The family directory appears only in the quoted include helper; the file
        # name is quoted in the cited source.
        main = 'const auto artifact = tests::audit_9001::read_artifact("frozen.json");\n'
        helper = 'const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-9001/";\n'
        self.assertTrue(self.matcher.references(BASELINE, [main, helper], ARTIFACT))
        self.assertFalse(self.matcher.references(BASELINE, [main], ARTIFACT))

    def test_sibling_file_in_the_same_family_is_not_a_match(self) -> None:
        code = (
            'const auto artifact = frozen_scenarios("audit-9001/other.json");\n'
            'const auto same_family = std::filesystem::path{"fixtures/pi-v1.3.0/audit-9001"};\n'
        )
        self.assertFalse(self.matched(code))

    def test_quoted_family_with_sibling_file_name_is_not_a_match(self) -> None:
        code = 'const auto x = "audit-9001";\nconst auto artifact = read_artifact("sibling.json");\n'
        self.assertFalse(self.matched(code))

    def test_comment_mention_is_not_a_match(self) -> None:
        code = (
            "// Frozen evidence: fixtures/pi-v1.3.0/audit-9001/frozen.json\n"
            'CHECK(value == "7");\n'
        )
        self.assertFalse(self.matched(code))

    def test_block_comment_mention_is_not_a_match(self) -> None:
        code = (
            "/* reads fixtures/pi-v1.3.0/audit-9001/frozen.json */\n"
            'CHECK(value == "7");\n'
        )
        self.assertFalse(self.matched(code))

    def test_catch2_tag_is_not_a_match(self) -> None:
        code = 'TEST_CASE("renders", "[chord][frozen][issue9001][spec]");\n'
        self.assertFalse(self.matched(code, "audit-9001/frozen.json"))

    def test_scenario_key_is_not_a_family_segment(self) -> None:
        code = 'const auto key = create_context_key<std::string>("audit-9001-frozen");\n'
        self.assertFalse(self.matched(code))

    def test_url_string_survives_comment_stripping(self) -> None:
        code = (
            'const auto url = "https://example.com/.well-known/oauth";\n'
            "// audit-9001/frozen.json is recorded upstream\n"
            'const auto artifact = read_artifact("frozen.json");\n'
            'const auto root = "audit-9001";\n'
        )
        self.assertTrue(self.matched(code))

    def test_raw_string_survives_comment_stripping(self) -> None:
        code = (
            'std::string raw = R"({"value":"pong"})";\n'
            'const auto artifact = read_artifact("frozen.json");\n'
            'const auto root = "audit-9001";\n'
        )
        stripped = boundary.strip_comments(code)
        self.assertIn('"frozen.json"', stripped)
        self.assertTrue(self.matched(code))


class ReceiptBoundaryCliTest(unittest.TestCase):
    """The check run against a synthetic repository, deliveries and evidence registry."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        fixture = self.root / "fixtures" / BASELINE
        family = fixture / "audit-9001"
        family.mkdir(parents=True)
        (self.root / "tests" / "owner").mkdir(parents=True)
        (self.root / "docs" / "research" / BASELINE).mkdir(parents=True)
        artifact_bytes = b'{"scenarios": [{"name": "one", "inputs": {}, "expected": {}}]}\n'
        (family / "frozen.json").write_bytes(artifact_bytes)
        manifest = {
            "schema": "pike/pi-v1.3.0-audit-evidence/1",
            "baseline": BASELINE,
            "revision": REVISION,
            "family": "audit-9001",
            "ticket": "9001",
            "capturedAt": "2026-11-05T00:00:00Z",
            "environment": {
                "platform": "linux",
                "arch": "x64",
                "libc": "glibc",
                "glibcVersion": "2.44",
                "node": "v26.11.1",
            },
            "generator": {"path": "capture.mts", "sha256": "0" * 64, "command": "tsx capture.mts", "sources": []},
            "artifacts": [
                {
                    "family": "audit-9001",
                    "path": "frozen.json",
                    "bytes": len(artifact_bytes),
                    "sha256": sha256_bytes(artifact_bytes),
                    "sourceEndpoints": ["packages/owner:src/index.ts"],
                    "scenarios": [{"name": "one", "inputs": {}, "expected": {}}],
                }
            ],
        }
        manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode()
        (family / "manifest.json").write_bytes(manifest_bytes)
        registry = {
            "schema": "pike/pi-v1.3.0-audit-baselines/1",
            "default": BASELINE,
            "baselines": {
                BASELINE: {
                    "revision": REVISION,
                    "bundles": {
                        "audit-9001": {
                            "manifest": "audit-9001/manifest.json",
                            "manifestSha256": sha256_bytes(manifest_bytes),
                        }
                    },
                }
            },
        }
        (fixture / "baselines.json").write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8")
        self.deliveries = self.root / "docs" / "research" / BASELINE / "1058-deliveries.json"

    def write_source(self, relative: str, code: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(code, encoding="utf-8")

    def write_deliveries(self, records: list[dict]) -> None:
        document = {
            "schema": "pike/pi-v1.3.0-capability-deliveries/1",
            "baseline": BASELINE,
            "revision": REVISION,
            "records": records,
        }
        self.deliveries.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--repo-root",
                str(self.root),
                "--baseline",
                BASELINE,
                *arguments,
            ],
            cwd=ROOT,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            text=True,
            capture_output=True,
        )

    @staticmethod
    def claim(claim_id: str, source: str, artifact: str = ARTIFACT, discriminating: bool = True) -> dict:
        row = {
            "id": claim_id,
            "remainingBoundary": [],
            "comparisons": [
                {
                    "artifact": artifact,
                    "scenario": "one",
                    "test": "a test",
                    "testSource": source,
                }
            ],
        }
        if discriminating:
            row["discriminatingCase"] = {
                "test": "a discriminating case",
                "testSource": source,
                "reason": "closes the gap the comparison cannot see",
            }
        return row

    def test_all_anchored_receipts_pass_and_exit_zero(self) -> None:
        self.write_source(
            "tests/owner/AnchoredTest.cpp",
            'const auto artifact = read_artifact("frozen.json");\nconst auto root = "audit-9001";\n',
        )
        self.write_deliveries(
            [
                {
                    "id": "9001-example",
                    "ticket": "9001",
                    "rows": [self.claim("cch_owner:anchored", "tests/owner/AnchoredTest.cpp")],
                }
            ]
        )
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("PASS 9001-example", result.stdout)
        self.assertIn("checked 1 receipt", result.stdout)

    def test_unanchored_comparison_and_discriminating_case_fail(self) -> None:
        self.write_source(
            "tests/owner/LiteralTest.cpp",
            "// Frozen evidence: fixtures/pi-v1.3.0/audit-9001/frozen.json\n"
            'CHECK(value == "7");\n',
        )
        self.write_deliveries(
            [
                {
                    "id": "9001-example",
                    "ticket": "9001",
                    "rows": [self.claim("cch_owner:literal", "tests/owner/LiteralTest.cpp")],
                }
            ]
        )
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL 9001-example (2 unmatched)", result.stdout)
        self.assertIn("audit-9001/frozen.json (comparison, tests/owner/LiteralTest.cpp)", result.stdout)
        self.assertIn("audit-9001/frozen.json (discriminating case, tests/owner/LiteralTest.cpp)", result.stdout)

    def test_unparsable_claim_fails_closed(self) -> None:
        self.write_deliveries(
            [
                {
                    "id": "9001-example",
                    "ticket": "9001",
                    "rows": [{"id": "cch_owner:broken", "comparisons": [{"artifact": ARTIFACT}]}],
                }
            ]
        )
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("unparsable claim", result.stdout)
        self.assertIn("cch_owner:broken", result.stdout)

    def test_unregistered_artifact_fails_closed(self) -> None:
        self.write_source(
            "tests/owner/AnchoredTest.cpp",
            'const auto artifact = read_artifact("unregistered.json");\nconst auto root = "audit-9002";\n',
        )
        self.write_deliveries(
            [
                {
                    "id": "9001-example",
                    "ticket": "9001",
                    "rows": [
                        self.claim(
                            "cch_owner:unregistered", "tests/owner/AnchoredTest.cpp", "audit-9002/missing.json"
                        )
                    ],
                }
            ]
        )
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a registered evidence artifact", result.stdout)

    def test_missing_test_source_fails_the_claim(self) -> None:
        self.write_deliveries(
            [
                {
                    "id": "9001-example",
                    "ticket": "9001",
                    "rows": [self.claim("cch_owner:missing-source", "tests/owner/AbsentTest.cpp")],
                }
            ]
        )
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing test source", result.stdout)

    def test_receipt_filter_selects_by_id_and_ticket(self) -> None:
        self.write_source(
            "tests/owner/AnchoredTest.cpp",
            'const auto artifact = read_artifact("frozen.json");\nconst auto root = "audit-9001";\n',
        )
        self.write_deliveries(
            [
                {"id": "9001-one", "ticket": "9001", "rows": []},
                {
                    "id": "9002-two",
                    "ticket": "9002",
                    "rows": [self.claim("cch_owner:anchored", "tests/owner/AnchoredTest.cpp")],
                },
            ]
        )
        by_id = self.run_cli("--receipt", "9002-two")
        self.assertEqual(by_id.returncode, 0, by_id.stderr + by_id.stdout)
        self.assertIn("checked 1 receipt", by_id.stdout)
        by_ticket = self.run_cli("--receipt", "9002")
        self.assertEqual(by_ticket.returncode, 0, by_ticket.stderr + by_ticket.stdout)
        unknown = self.run_cli("--receipt", "9999")
        self.assertEqual(unknown.returncode, 2)
        self.assertIn("unknown receipt", unknown.stderr)

    def test_baseline_mismatch_fails_closed(self) -> None:
        self.write_deliveries([])
        document = json.loads(self.deliveries.read_text(encoding="utf-8"))
        document["baseline"] = "pi-v1.3.0"
        self.deliveries.write_text(json.dumps(document) + "\n", encoding="utf-8")
        result = self.run_cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("record baseline", result.stderr)


if __name__ == "__main__":
    unittest.main()
