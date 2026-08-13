#!/usr/bin/env python3
"""Unit tests for the fail-closed Parity Architecture Gate validator.

Covers the strict manifest schema (unknown versions/fields, missing fields,
contradictory declarations), the configured-evidence index (malformed, stale,
unknown producer), the Gate's cross-Owner edge policy, and deterministic
human/JSON diagnostics with stable rule IDs.

Run directly: `python3 tests/architecture/parity_gate_test.py`, or through the
CTest case `cch_parity_gate_unit` registered in CMakeLists.txt.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "cmake" / "parity"))

import parity_gate as pg  # noqa: E402

VALID_MANIFEST = {
    "schema_version": 1,
    "baseline_commit": "83114817c68f5413e4d7ba6d7003ddc511cd31d2",
    "owners": {
        "cch_ai": {
            "role": "owner",
            "root": "src/ai",
            "interface_root": "include/cch/ai",
            "legal_owner_dependencies": [],
        },
        "cch_agent_core": {
            "role": "owner",
            "root": "src/agent",
            "interface_root": "include/cch/agent",
            "legal_owner_dependencies": ["cch_ai"],
        },
        "cch_tui": {
            "role": "owner",
            "root": "src/tui",
            "interface_root": "include/cch/tui",
            "legal_owner_dependencies": [],
        },
        "cch_coding_agent": {
            "role": "owner",
            "root": "src/coding_agent",
            "interface_root": "include/cch/coding_agent",
            "legal_owner_dependencies": ["cch_agent_core", "cch_ai", "cch_tui"],
        },
        "cch_support": {
            "role": "support",
            "root": "src/support",
            "interface_root": "include/cch/support",
            "legal_owner_dependencies": [],
        },
    },
    "roles": ["owner", "implementation", "support", "composition", "external"],
    "external_families": {
        "boost": {"description": "Boost libraries"},
        "openssl": {"description": "OpenSSL crypto and TLS"},
    },
    "evidence": [
        {
            "id": "ownership-index",
            "producer": "cch-parity-constructor",
            "producer_schema_version": 1,
            "input_identities": ["manifest"],
        }
    ],
}

VALID_INDEX = {
    "producer": "cch-parity-constructor",
    "schema_version": 1,
    "manifest_digest": "d" * 64,
    "targets": [
        {
            "name": "cch_support",
            "role": "support",
            "owner": "cch_support",
            "sources": ["src/support/value.cpp"],
            "dependencies": [],
        },
        {
            "name": "cch_ai",
            "role": "owner",
            "owner": "cch_ai",
            "sources": ["src/ai/model.cpp"],
            "dependencies": [{"name": "cch_support", "family": None}],
        },
        {
            "name": "cch_tui",
            "role": "owner",
            "owner": "cch_tui",
            "sources": ["src/tui/render.cpp"],
            "dependencies": [{"name": "cch_support", "family": None}],
        },
        {
            "name": "cch_agent_core",
            "role": "owner",
            "owner": "cch_agent_core",
            "sources": ["src/agent/agent.cpp"],
            "dependencies": [
                {"name": "cch_ai", "family": None},
                {"name": "cch_support", "family": None},
            ],
        },
        {
            "name": "cch_coding_agent",
            "role": "owner",
            "owner": "cch_coding_agent",
            "sources": ["src/coding_agent/compose.cpp"],
            "dependencies": [
                {"name": "cch_agent_core", "family": None},
                {"name": "cch_ai", "family": None},
                {"name": "cch_tui", "family": None},
                {"name": "cch_support", "family": None},
            ],
        },
    ],
}


def deep_copy(value):
    return json.loads(json.dumps(value))


def valid_manifest():
    return pg.parse_manifest(deep_copy(VALID_MANIFEST))


def valid_index():
    return pg.parse_index(deep_copy(VALID_INDEX))


def rule_ids(diagnostics):
    return [diagnostic.rule_id for diagnostic in diagnostics]


class ManifestSchemaTest(unittest.TestCase):
    def test_checked_in_manifest_parses(self):
        manifest_path = REPO_ROOT / "cmake" / "parity" / "manifest.json"
        manifest = pg.parse_manifest(json.loads(manifest_path.read_text()))
        self.assertEqual(
            set(manifest.owners),
            {"cch_ai", "cch_agent_core", "cch_tui", "cch_coding_agent", "cch_support"},
        )

    def test_valid_manifest_parses(self):
        manifest = valid_manifest()
        self.assertEqual(manifest.schema_version, 1)
        self.assertEqual(manifest.owners["cch_agent_core"].legal_owner_dependencies, ("cch_ai",))

    def test_unknown_schema_version_fails_closed(self):
        data = deep_copy(VALID_MANIFEST)
        data["schema_version"] = 99
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_UNKNOWN_MANIFEST_VERSION)

    def test_unknown_top_level_field_fails_closed(self):
        data = deep_copy(VALID_MANIFEST)
        data["legal_edges"] = []
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_UNKNOWN_MANIFEST_FIELD)

    def test_unknown_owner_field_fails_closed(self):
        data = deep_copy(VALID_MANIFEST)
        data["owners"]["cch_ai"]["color"] = "red"
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_UNKNOWN_MANIFEST_FIELD)

    def test_missing_required_field_fails_closed(self):
        data = deep_copy(VALID_MANIFEST)
        del data["owners"]["cch_ai"]["root"]
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_MISSING_MANIFEST_FIELD)

    def test_unknown_owner_dependency_is_contradictory(self):
        data = deep_copy(VALID_MANIFEST)
        data["owners"]["cch_agent_core"]["legal_owner_dependencies"] = ["cch_does_not_exist"]
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_MANIFEST_VALUE)

    def test_self_dependency_is_contradictory(self):
        data = deep_copy(VALID_MANIFEST)
        data["owners"]["cch_ai"]["legal_owner_dependencies"] = ["cch_ai"]
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_MANIFEST_VALUE)

    def test_support_in_legal_owner_dependencies_is_contradictory(self):
        data = deep_copy(VALID_MANIFEST)
        data["owners"]["cch_ai"]["legal_owner_dependencies"] = ["cch_support"]
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_MANIFEST_VALUE)

    def test_duplicate_root_is_contradictory(self):
        data = deep_copy(VALID_MANIFEST)
        data["owners"]["cch_tui"]["root"] = "src/ai"
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_MANIFEST_VALUE)

    def test_duplicate_evidence_producer_is_contradictory(self):
        data = deep_copy(VALID_MANIFEST)
        data["evidence"].append(deep_copy(data["evidence"][0]))
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_MANIFEST_VALUE)

    def test_unknown_role_is_contradictory(self):
        data = deep_copy(VALID_MANIFEST)
        data["roles"] = ["owner", "implementation", "support", "composition", "external", "plugin"]
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_manifest(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_MANIFEST_VALUE)


class IndexSchemaTest(unittest.TestCase):
    def test_valid_index_parses(self):
        index = valid_index()
        self.assertEqual(len(index.targets), 5)

    def test_malformed_json_fails_closed(self):
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_index("not json at all")
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_INDEX_VALUE)

    def test_missing_required_field_fails_closed(self):
        data = deep_copy(VALID_INDEX)
        del data["targets"][0]["role"]
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_index(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_MISSING_INDEX_FIELD)

    def test_unknown_field_fails_closed(self):
        data = deep_copy(VALID_INDEX)
        data["targets"][0]["surprise"] = True
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_index(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_UNKNOWN_INDEX_FIELD)

    def test_duplicate_target_fails_closed(self):
        data = deep_copy(VALID_INDEX)
        data["targets"].append(deep_copy(data["targets"][0]))
        with self.assertRaises(pg.SchemaViolation) as raised:
            pg.parse_index(data)
        self.assertEqual(raised.exception.rule_id, pg.RULE_INVALID_INDEX_VALUE)


class GatePolicyTest(unittest.TestCase):
    def test_legal_graph_passes(self):
        manifest = valid_manifest()
        index = valid_index()
        self.assertEqual(pg.check(manifest, index, VALID_INDEX["manifest_digest"]), [])

    def test_illegal_cross_owner_edge_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][3]["dependencies"].append({"name": "cch_tui", "family": None})
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_ILLEGAL_CROSS_OWNER_EDGE])
        self.assertEqual(diagnostics[0].target, "cch_agent_core")
        self.assertEqual(diagnostics[0].dependency, "cch_tui")

    def test_support_depending_on_owner_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][0]["dependencies"].append({"name": "cch_ai", "family": None})
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_SUPPORT_DEPENDS_ON_OWNER])

    def test_cross_owner_edge_to_non_authoritative_target_is_rejected(self):
        manifest = valid_manifest()
        data = {
            "producer": "cch-parity-constructor",
            "schema_version": 1,
            "manifest_digest": "d" * 64,
            "targets": [
                {
                    "name": "cch_ai_impl",
                    "role": "implementation",
                    "owner": "cch_ai",
                    "sources": [],
                    "dependencies": [],
                },
                {
                    "name": "cch_agent_core",
                    "role": "owner",
                    "owner": "cch_agent_core",
                    "sources": [],
                    "dependencies": [{"name": "cch_ai_impl", "family": None}],
                },
            ],
        }
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, "d" * 64)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_NON_AUTHORITATIVE_CROSS_OWNER_TARGET])

    def test_unknown_external_family_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][0]["dependencies"].append({"name": "zlib::zlib", "family": "zlib"})
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNKNOWN_EXTERNAL_FAMILY])

    def test_known_external_family_is_accepted(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][0]["dependencies"].append({"name": "Boost::filesystem", "family": "boost"})
        index = pg.parse_index(data)
        self.assertEqual(pg.check(manifest, index, VALID_INDEX["manifest_digest"]), [])

    def test_unknown_project_dependency_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][1]["dependencies"].append({"name": "cch_ghost", "family": None})
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNKNOWN_PROJECT_DEPENDENCY])

    def test_unknown_target_role_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][4]["role"] = "plugin"
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNKNOWN_TARGET_ROLE])

    def test_unknown_target_owner_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][1]["owner"] = "cch_mystery"
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNKNOWN_TARGET_OWNER])

    def test_unknown_index_producer_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["producer"] = "someone-else"
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertIn(pg.RULE_INVALID_INDEX_VALUE, rule_ids(diagnostics))

    def test_stale_producer_schema_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["schema_version"] = 0
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_STALE_PRODUCER_SCHEMA])

    def test_newer_producer_schema_is_rejected(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["schema_version"] = 2
        index = pg.parse_index(data)
        diagnostics = pg.check(manifest, index, VALID_INDEX["manifest_digest"])
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNKNOWN_INDEX_VERSION])

    def test_stale_manifest_digest_is_rejected(self):
        manifest = valid_manifest()
        index = valid_index()
        diagnostics = pg.check(manifest, index, "e" * 64)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_STALE_MANIFEST_DIGEST])


class DiagnosticDeterminismTest(unittest.TestCase):
    def _illegal_diagnostics(self):
        manifest = valid_manifest()
        data = deep_copy(VALID_INDEX)
        data["targets"][3]["dependencies"].append({"name": "cch_tui", "family": None})
        data["targets"][1]["dependencies"].append({"name": "cch_ghost", "family": None})
        return manifest, pg.parse_index(data), pg.check(
            manifest, pg.parse_index(data), VALID_INDEX["manifest_digest"]
        )

    def test_human_output_is_stable_and_carries_rule_ids(self):
        manifest, index, diagnostics = self._illegal_diagnostics()
        report = pg.build_report(diagnostics, index, "human")
        self.assertIn("PARITY-2001", report)
        self.assertIn("PARITY-2006", report)
        self.assertEqual(report, pg.build_report(diagnostics, index, "human"))

    def test_json_output_is_deterministic_and_parseable(self):
        manifest, index, diagnostics = self._illegal_diagnostics()
        first = pg.build_report(diagnostics, index, "json")
        second = pg.build_report(diagnostics, index, "json")
        self.assertEqual(first, second)
        payload = json.loads(first)
        self.assertFalse(payload["ok"])
        self.assertEqual(
            {item["rule_id"] for item in payload["diagnostics"]},
            {pg.RULE_ILLEGAL_CROSS_OWNER_EDGE, pg.RULE_UNKNOWN_PROJECT_DEPENDENCY},
        )
        # Diagnostics are sorted for deterministic ordering.
        ids = [item["rule_id"] for item in payload["diagnostics"]]
        self.assertEqual(ids, sorted(ids))


class CliTest(unittest.TestCase):
    def test_cli_end_to_end_exit_codes_and_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            manifest_path = tmp_path / "manifest.json"
            manifest_path.write_text(json.dumps(VALID_MANIFEST))
            index_path = tmp_path / "index.json"
            index_data = deep_copy(VALID_INDEX)
            digest = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            index_data["manifest_digest"] = digest
            index_path.write_text(json.dumps(index_data))

            script = str(REPO_ROOT / "cmake" / "parity" / "parity_gate.py")
            passed = subprocess.run(
                [sys.executable, script, "--manifest", str(manifest_path),
                 "--index", str(index_path), "--format", "json"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(passed.returncode, 0, passed.stderr)
            self.assertTrue(json.loads(passed.stdout)["ok"])

            # Mutating the manifest after the index was produced must make the
            # evidence stale and fail closed.
            manifest_path.write_text(json.dumps({**VALID_MANIFEST, "baseline_commit": "0" * 40}))
            failed = subprocess.run(
                [sys.executable, script, "--manifest", str(manifest_path),
                 "--index", str(index_path), "--format", "json"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(failed.returncode, 1)
            payload = json.loads(failed.stdout)
            self.assertIn(pg.RULE_STALE_MANIFEST_DIGEST,
                          [item["rule_id"] for item in payload["diagnostics"]])


if __name__ == "__main__":
    unittest.main()
