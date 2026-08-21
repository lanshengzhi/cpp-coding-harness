#!/usr/bin/env python3
"""Unit tests for the fail-closed Parity Architecture Gate validator.

Covers the strict manifest schema (unknown versions/fields, missing fields,
contradictory declarations), the configured-evidence index (malformed, stale,
unknown producer), the Gate's cross-Owner edge policy, and deterministic
human/JSON diagnostics with stable rule IDs.

Run directly: `python3 tests/architecture/parity_gate_test.py`, or through the
CTest case `cch_parity_gate_unit` registered in `cmake/tests/ArchitectureTests.cmake`.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "cmake" / "parity"))

import parity_gate as pg  # noqa: E402

VALID_MANIFEST = {
    "schema_version": 2,
    "baseline_commit": "83114817c68f5413e4d7ba6d7003ddc511cd31d2",
    "exception_policy": {
        "schema_version": 1,
        "required_compile_flags": ["-fno-exceptions"],
        "forbidden_compile_flags": ["-fexceptions"],
        "allowed_exception_ptr_sources": [
            "src/ai/AsyncResultBridge.hpp",
            "src/ai/ModelStreamBridge.hpp",
        ],
        "forbidden_exception_calls": ["std::rethrow_exception"],
    },
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
        },
        {
            "id": "direct-includes",
            "producer": "cch-parity-lexer",
            "producer_schema_version": 1,
            "input_identities": ["manifest", "ownership-index"],
        },
        {
            "id": "compile-commands",
            "producer": "cmake-file-api",
            "producer_schema_version": 1,
            "input_identities": ["manifest", "ownership-index"],
        },
        {
            "id": "depfiles",
            "producer": "cch-compiler-depfile",
            "producer_schema_version": 1,
            "input_identities": ["manifest", "ownership-index", "compile-commands"],
        },
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
        self.assertEqual(manifest.schema_version, 2)
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

    def test_dependency_visibility_defaults_to_private(self):
        index = valid_index()
        for dependency in index.targets[1].dependencies:
            self.assertEqual(dependency["visibility"], "private")

    def test_dependency_visibility_is_recorded(self):
        data = deep_copy(VALID_INDEX)
        data["targets"][1]["dependencies"][0]["visibility"] = "public"
        index = pg.parse_index(data)
        self.assertEqual(index.targets[1].dependencies[0]["visibility"], "public")

    def test_unknown_dependency_visibility_fails_closed(self):
        data = deep_copy(VALID_INDEX)
        data["targets"][1]["dependencies"][0]["visibility"] = "exported"
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


# ---------------------------------------------------------------------------
# Direct-include lexer, canonical resolution, compile context, and depfiles
# ---------------------------------------------------------------------------

INTERFACE_HEADERS = {
    "cch_ai": "include/cch/ai/Model.hpp",
    "cch_agent_core": "include/cch/agent/Agent.hpp",
    "cch_tui": "include/cch/tui/Render.hpp",
    "cch_support": "include/cch/support/Value.hpp",
    "cch_coding_agent": "include/cch/coding_agent/Compose.hpp",
}


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def include_doc(path, spelling="angle", line=1, macro=False):
    return {"path": path, "spelling": spelling, "line": line, "macro": macro}


def make_index(targets):
    return pg.parse_index(
        {
            "producer": "cch-parity-constructor",
            "schema_version": 1,
            "manifest_digest": "d" * 64,
            "targets": targets,
        }
    )


def make_direct_includes(sources):
    return pg.parse_direct_includes(
        {"producer": "cch-parity-lexer", "schema_version": 1, "sources": sources}
    )


def make_compile_commands(entries):
    return pg.parse_compile_commands(entries)


def make_depfiles(config_digest, entries):
    return pg.parse_depfile_evidence(
        {
            "producer": "cch-compiler-depfile",
            "schema_version": 1,
            "config_digest": config_digest,
            "entries": entries,
        }
    )


def make_project_tree(root):
    root = Path(root)
    for rel in INTERFACE_HEADERS.values():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#pragma once\n")
    return root


def run_include_case(root, from_owner, source_name, include_path, spelling="angle", macro=False):
    project_root = make_project_tree(root)
    source = project_root / "src" / source_name
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text("// fixture source\n")
    role = "support" if from_owner == "cch_support" else "owner"
    index = make_index(
        [
            {
                "name": from_owner,
                "role": role,
                "owner": from_owner,
                "sources": [str(source)],
                "dependencies": [],
            }
        ]
    )
    direct = make_direct_includes(
        [
            {
                "path": str(source),
                "digest": sha256_file(source),
                "includes": [include_doc(include_path, spelling=spelling, macro=macro)],
            }
        ]
    )
    manifest = valid_manifest()
    return pg.check(
        manifest,
        index,
        "d" * 64,
        direct_includes=direct,
        project_root=str(project_root),
    )


class LexerTest(unittest.TestCase):
    def test_scans_disabled_preprocessor_branches(self):
        text = "#if 0\n#include <cch/ai/Model.hpp>\n#endif\n"
        includes = pg.lex_includes(text, "x.cpp")
        self.assertEqual(len(includes), 1)
        self.assertEqual(includes[0].path, "cch/ai/Model.hpp")
        self.assertEqual(includes[0].spelling, "angle")

    def test_records_angle_quote_and_macro_spellings(self):
        text = '#include <cch/ai/Model.hpp>\n#include "cch/ai/Model.hpp"\n#include HDR\n'
        includes = pg.lex_includes(text, "x.cpp")
        self.assertEqual([inc.spelling for inc in includes], ["angle", "quote", "macro"])
        self.assertTrue(includes[2].macro)

    def test_ignores_non_include_directives(self):
        text = "#define X 1\n#ifdef Y\n#endif\n#include_next <foo>\n#pragma once\n"
        self.assertEqual(pg.lex_includes(text, "x.cpp"), ())


class IncludeResolutionTest(unittest.TestCase):
    def test_quote_spelling_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(
                tmp, "cch_ai", "model.cpp", "cch/ai/Model.hpp", spelling="quote"
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_QUOTE_SPELLING])

    def test_unclassified_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(tmp, "cch_ai", "model.cpp", "cch/util/Error.hpp")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_UNCLASSIFIED_ROOT])

    def test_path_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(tmp, "cch_ai", "model.cpp", "../cch/ai/Model.hpp")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_PATH_ESCAPE])

    def test_absolute_path_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(
                tmp, "cch_ai", "model.cpp", os.path.join(tmp, "include/cch/ai/Model.hpp")
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_PATH_ESCAPE])

    def test_case_conflicting_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(tmp, "cch_ai", "model.cpp", "cch/AI/Model.hpp")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_CASE_CONFLICT])

    def test_macro_generated_include_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(
                tmp, "cch_ai", "model.cpp", "HDR", spelling="macro", macro=True
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_MACRO_GENERATED])

    def test_illegal_direct_include_edge_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(tmp, "cch_ai", "model.cpp", "cch/tui/Render.hpp")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_ILLEGAL_DIRECT_INCLUDE])
        self.assertEqual(diagnostics[0].target, "cch_ai")
        self.assertEqual(diagnostics[0].dependency, "cch_tui")

    def test_support_include_owner_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(tmp, "cch_support", "value.cpp", "cch/ai/Model.hpp")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_ILLEGAL_DIRECT_INCLUDE])

    def test_unresolved_project_include_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = run_include_case(tmp, "cch_ai", "model.cpp", "cch/ai/Missing.hpp")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNRESOLVED_PROJECT_INCLUDE])

    def test_symlink_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = make_project_tree(tmp)
            outside = project_root / "outside.hpp"
            outside.write_text("#pragma once\n")
            header = project_root / "include/cch/ai/Model.hpp"
            header.unlink()
            try:
                os.symlink(outside, header)
            except OSError:
                self.skipTest("symlinks are not supported on this filesystem")
            source = project_root / "src/model.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// fixture\n")
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(source)],
                        "dependencies": [],
                    }
                ]
            )
            direct = make_direct_includes(
                [
                    {
                        "path": str(source),
                        "digest": sha256_file(source),
                        "includes": [include_doc("cch/ai/Model.hpp")],
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, direct_includes=direct, project_root=str(project_root)
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_PATH_ESCAPE])

    def test_ambiguous_include_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = make_project_tree(tmp)
            # The same relative header exists under a second interface root.
            duplicate = project_root / "include/cch/tui/Model.hpp"
            duplicate.write_text("#pragma once\n")
            source = project_root / "src/model.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// fixture\n")
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(source)],
                        "dependencies": [],
                    }
                ]
            )
            direct = make_direct_includes(
                [
                    {
                        "path": str(source),
                        "digest": sha256_file(source),
                        "includes": [include_doc("cch/ai/Model.hpp")],
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(),
                index,
                "d" * 64,
                direct_includes=direct,
                project_root=str(project_root),
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_INCLUDE_AMBIGUOUS])

    def test_missing_include_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = make_project_tree(tmp)
            source = project_root / "src/model.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// fixture\n")
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(source)],
                        "dependencies": [],
                    }
                ]
            )
            direct = make_direct_includes([])
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, direct_includes=direct, project_root=str(project_root)
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_MISSING_INCLUDE_EVIDENCE])

    def test_unclassified_evidence_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = make_project_tree(tmp)
            ghost = project_root / "src/ghost.cpp"
            ghost.parent.mkdir(parents=True, exist_ok=True)
            ghost.write_text("// not declared by any target\n")
            index = make_index([])
            direct = make_direct_includes(
                [{"path": str(ghost), "digest": sha256_file(ghost), "includes": []}]
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, direct_includes=direct, project_root=str(project_root)
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNCLASSIFIED_EVIDENCE_SOURCE])

    def test_stale_include_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = make_project_tree(tmp)
            source = project_root / "src/model.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// fixture\n")
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(source)],
                        "dependencies": [],
                    }
                ]
            )
            direct = make_direct_includes(
                [
                    {
                        "path": str(source),
                        "digest": "e" * 64,  # does not match the on-disk bytes
                        "includes": [],
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, direct_includes=direct, project_root=str(project_root)
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_STALE_INCLUDE_EVIDENCE])


class CompileContextTest(unittest.TestCase):
    def _single_source_index(self, source_path, **target_extra):
        target = {
            "name": "cch_ai",
            "role": "owner",
            "owner": "cch_ai",
            "sources": [source_path],
            "dependencies": [],
        }
        target.update(target_extra)
        return make_index([target])

    def test_duplicate_source_compilation_is_rejected(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        commands = make_compile_commands(
            [
                {"file": "/tmp/fake/model.cpp", "directory": "/tmp/fake", "command": "g++ -c /tmp/fake/model.cpp"},
                {"file": "/tmp/fake/model.cpp", "directory": "/tmp/fake", "command": "g++ -c /tmp/fake/model.cpp"},
            ]
        )
        diagnostics = pg.check(valid_manifest(), index, "d" * 64, compile_commands=commands)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_DUPLICATE_SOURCE_COMPILATION])

    def test_missing_compile_command_is_rejected(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        diagnostics = pg.check(
            valid_manifest(), index, "d" * 64, compile_commands=make_compile_commands([])
        )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_MISSING_COMPILE_COMMAND])

    def test_explicit_exception_enable_flag_is_rejected(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        commands = make_compile_commands(
            [
                {
                    "file": "/tmp/fake/model.cpp",
                    "directory": "/tmp/fake",
                    "command": "g++ -fexceptions -c /tmp/fake/model.cpp",
                }
            ]
        )
        diagnostics = pg.check(valid_manifest(), index, "d" * 64, compile_commands=commands)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_FORBIDDEN_EXCEPTION_FLAG])

    def test_strict_no_exception_policy_rejects_missing_flag(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        commands = make_compile_commands(
            [
                {
                    "file": "/tmp/fake/model.cpp",
                    "directory": "/tmp/fake",
                    "command": "g++ -c /tmp/fake/model.cpp",
                }
            ]
        )
        diagnostics = pg.check(
            valid_manifest(),
            index,
            "d" * 64,
            compile_commands=commands,
            strict_no_exceptions=True,
        )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_EXCEPTION_ENABLED_TARGET])

    def test_strict_no_exception_policy_rejects_exception_pointer_outside_bridge(self):
        with tempfile.TemporaryDirectory() as tmp:
            source_root = Path(tmp) / "src"
            source_root.mkdir()
            (source_root / "not_a_bridge.cpp").write_text(
                "#include <exception>\nstd::exception_ptr value;\n"
            )
            diagnostics = pg.check(
                valid_manifest(),
                make_index([]),
                "d" * 64,
                project_root=tmp,
                strict_no_exceptions=True,
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_EXCEPTION_POINTER_NOT_ALLOWLISTED])

    def test_strict_no_exception_policy_rejects_exception_rethrow(self):
        with tempfile.TemporaryDirectory() as tmp:
            source_root = Path(tmp) / "src"
            source_root.mkdir()
            (source_root / "bridge.cpp").write_text("std::rethrow_exception(exception);\n")
            diagnostics = pg.check(
                valid_manifest(),
                make_index([]),
                "d" * 64,
                project_root=tmp,
                strict_no_exceptions=True,
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_EXCEPTION_RETHROW_FORBIDDEN])

    def test_unsupported_compile_flag_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = self._single_source_index(os.path.join(tmp, "model.cpp"))
            commands = make_compile_commands(
                [
                    {
                        "file": os.path.join(tmp, "model.cpp"),
                        "directory": tmp,
                        "command": f"g++ -isystem {os.path.join(tmp, 'include')} -c {os.path.join(tmp, 'model.cpp')}",
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, compile_commands=commands, project_root=tmp
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNSUPPORTED_COMPILE_FLAG])

    def test_unsupported_flag_on_declared_external_include_root_is_allowed(self):
        # An unsupported include-affecting flag that points at a declared
        # external dependency root (for example the vcpkg installed-dependency
        # include directory, which lives under the build tree inside the
        # project root) is external, not a project path, so it does not fail.
        with tempfile.TemporaryDirectory() as tmp:
            external = os.path.join(tmp, "build", "vcpkg_installed", "x64-linux", "include")
            os.makedirs(external, exist_ok=True)
            index = self._single_source_index(os.path.join(tmp, "model.cpp"))
            commands = make_compile_commands(
                [
                    {
                        "file": os.path.join(tmp, "model.cpp"),
                        "directory": tmp,
                        "command": f"g++ -isystem {external} -c {os.path.join(tmp, 'model.cpp')}",
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(),
                index,
                "d" * 64,
                compile_commands=commands,
                project_root=tmp,
                external_include_roots=(external,),
            )
        self.assertEqual(diagnostics, [])

    def test_unsupported_flag_on_project_path_still_rejected_with_external_root(self):
        # Declaring one external root never makes an undeclared project path
        # external: the same flag pointing at the project source tree still
        # fails closed.
        with tempfile.TemporaryDirectory() as tmp:
            external = os.path.join(tmp, "build", "vcpkg_installed", "x64-linux", "include")
            os.makedirs(external, exist_ok=True)
            project_include = os.path.join(tmp, "include")
            os.makedirs(project_include, exist_ok=True)
            index = self._single_source_index(os.path.join(tmp, "model.cpp"))
            commands = make_compile_commands(
                [
                    {
                        "file": os.path.join(tmp, "model.cpp"),
                        "directory": tmp,
                        "command": f"g++ -isystem {project_include} -c {os.path.join(tmp, 'model.cpp')}",
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(),
                index,
                "d" * 64,
                compile_commands=commands,
                project_root=tmp,
                external_include_roots=(external,),
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNSUPPORTED_COMPILE_FLAG])

    def test_undeclared_forced_include_is_rejected(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        commands = make_compile_commands(
            [
                {
                    "file": "/tmp/fake/model.cpp",
                    "directory": "/tmp/fake",
                    "command": "g++ -include /tmp/fake/undeclared.hpp -c /tmp/fake/model.cpp",
                }
            ]
        )
        diagnostics = pg.check(valid_manifest(), index, "d" * 64, compile_commands=commands)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNDECLARED_FORCED_INCLUDE])

    def test_opaque_pch_artifact_is_rejected(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        commands = make_compile_commands(
            [
                {
                    "file": "/tmp/fake/model.cpp",
                    "directory": "/tmp/fake",
                    "command": "g++ -include /tmp/fake/pch.hpp.gch -c /tmp/fake/model.cpp",
                }
            ]
        )
        diagnostics = pg.check(valid_manifest(), index, "d" * 64, compile_commands=commands)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_OPAQUE_PCH_FORBIDDEN])

    def test_unscanned_forced_include_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = make_project_tree(tmp)
            source = project_root / "src/model.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// fixture\n")
            forced = project_root / "include/cch/ai/force.hpp"
            forced.write_text("#pragma once\n")
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(source)],
                        "dependencies": [],
                        "forced_includes": [str(forced)],
                    }
                ]
            )
            direct = make_direct_includes(
                [{"path": str(source), "digest": sha256_file(source), "includes": []}]
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, direct_includes=direct, project_root=str(project_root)
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNSCANNED_FORCED_INCLUDE])

    def test_unknown_compiler_is_rejected(self):
        index = self._single_source_index("/tmp/fake/model.cpp")
        commands = make_compile_commands(
            [
                {
                    "file": "/tmp/fake/model.cpp",
                    "directory": "/tmp/fake",
                    "command": "/usr/bin/python3 -c /tmp/fake/model.cpp",
                }
            ]
        )
        diagnostics = pg.check(valid_manifest(), index, "d" * 64, compile_commands=commands)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_UNKNOWN_COMPILER])

    def test_project_target_masquerading_as_external_is_rejected(self):
        index = make_index(
            [
                {
                    "name": "cch_ai",
                    "role": "owner",
                    "owner": "cch_ai",
                    "sources": [],
                    "dependencies": [{"name": "cch_tui", "family": "boost"}],
                },
                {
                    "name": "cch_tui",
                    "role": "owner",
                    "owner": "cch_tui",
                    "sources": [],
                    "dependencies": [],
                },
            ]
        )
        diagnostics = pg.check(valid_manifest(), index, "d" * 64)
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_PROJECT_TARGET_MASQUERADING_AS_EXTERNAL])


class DepfileEvidenceTest(unittest.TestCase):
    def _compiled_index(self, source_path):
        return make_index(
            [
                {
                    "name": "cch_ai",
                    "role": "owner",
                    "owner": "cch_ai",
                    "sources": [source_path],
                    "dependencies": [],
                }
            ]
        )

    def test_missing_depfile_evidence_fails_closed_at_build_phase(self):
        index = self._compiled_index("/tmp/fake/model.cpp")
        diagnostics = pg.check(valid_manifest(), index, "d" * 64, phase="build")
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_MISSING_DEPFILE_EVIDENCE])

    def test_depfile_evidence_not_required_at_configure_phase(self):
        index = self._compiled_index("/tmp/fake/model.cpp")
        self.assertEqual(pg.check(valid_manifest(), index, "d" * 64, phase="configure"), [])

    def test_stale_config_digest_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "model.cpp"
            depfile = Path(tmp) / "model.d"
            depfile.write_text("model.o: model.cpp\n")
            index = self._compiled_index(str(source))
            depfiles = make_depfiles(
                "a" * 64,
                [{"source": str(source), "depfile": str(depfile), "digest": sha256_file(depfile)}],
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, depfiles=depfiles, config_digest="b" * 64
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_STALE_DEPFILE_EVIDENCE])

    def test_missing_depfile_file_is_rejected(self):
        index = self._compiled_index("/tmp/fake/model.cpp")
        depfiles = make_depfiles(
            "a" * 64,
            [{"source": "/tmp/fake/model.cpp", "depfile": "/tmp/fake/missing.d", "digest": "c" * 64}],
        )
        diagnostics = pg.check(
            valid_manifest(), index, "d" * 64, depfiles=depfiles, config_digest="a" * 64
        )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_STALE_DEPFILE_EVIDENCE])

    def test_changed_depfile_digest_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            depfile = Path(tmp) / "model.d"
            depfile.write_text("model.o: model.cpp\n")
            index = self._compiled_index(str(Path(tmp) / "model.cpp"))
            depfiles = make_depfiles(
                "a" * 64,
                [{"source": str(Path(tmp) / "model.cpp"), "depfile": str(depfile), "digest": "c" * 64}],
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, depfiles=depfiles, config_digest="a" * 64
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_STALE_DEPFILE_EVIDENCE])

    def test_missing_depfile_entry_is_contradictory(self):
        with tempfile.TemporaryDirectory() as tmp:
            depfile = Path(tmp) / "model.d"
            depfile.write_text("model.o: model.cpp\n")
            index = self._compiled_index(str(Path(tmp) / "model.cpp"))
            depfiles = make_depfiles(
                "a" * 64,
                [{"source": str(Path(tmp) / "model.cpp"), "depfile": str(depfile), "digest": sha256_file(depfile)}],
            )
            # A second compiled source has no depfile entry.
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(Path(tmp) / "model.cpp"), str(Path(tmp) / "other.cpp")],
                        "dependencies": [],
                    }
                ]
            )
            diagnostics = pg.check(
                valid_manifest(), index, "d" * 64, depfiles=depfiles, config_digest="a" * 64
            )
        self.assertEqual(rule_ids(diagnostics), [pg.RULE_CONTRADICTORY_DEPFILE_EVIDENCE])

    def test_checked_in_manifest_declares_all_evidence_producers(self):
        manifest = pg.parse_manifest(
            json.loads((REPO_ROOT / "cmake" / "parity" / "manifest.json").read_text())
        )
        self.assertEqual(
            {entry["producer"] for entry in manifest.evidence},
            {
                pg.PRODUCER_OWNERSHIP_INDEX,
                pg.PRODUCER_DIRECT_INCLUDES,
                pg.PRODUCER_COMPILE_COMMANDS,
                pg.PRODUCER_DEPFILES,
            },
        )


class DepfileRecorderTest(unittest.TestCase):
    """The active dependency evidence producer (`--record-depfiles`)."""

    def _record(self, tmp, commands, manifest_digest="d" * 64):
        source = Path(tmp) / "model.cpp"
        index = make_index(
            [
                {
                    "name": "cch_ai",
                    "role": "owner",
                    "owner": "cch_ai",
                    "sources": [str(source)],
                    "dependencies": [],
                }
            ]
        )
        manifest = valid_manifest()
        index_path = Path(tmp) / "index.json"
        index_path.write_text("index\n")
        commands_path = Path(tmp) / "compile_commands.json"
        commands_path.write_text("commands\n")
        output_path = Path(tmp) / "depfiles.json"
        pg._record_depfiles(
            manifest,
            index,
            manifest_digest,
            make_compile_commands(commands),
            str(index_path),
            str(commands_path),
            str(output_path),
        )
        return json.loads(output_path.read_text())

    def test_records_module_mapper_depfile_for_each_compiled_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "model.cpp"
            source.write_text("int x;\n")
            depfile = Path(tmp) / "model.cpp.o.ddi.d"
            depfile.write_text("model.cpp.o.ddi: model.cpp\n")
            command = (
                f"g++ -MD -fmodule-mapper=model.cpp.o.modmap "
                f"-o model.cpp.o -c {source}"
            )
            document = self._record(tmp, [{"file": str(source), "directory": tmp, "command": command}])
            self.assertEqual(document["producer"], pg.PRODUCER_DEPFILES)
            self.assertEqual(document["schema_version"], 1)
            self.assertEqual(
                document["entries"],
                [
                    {
                        "source": str(source),
                        "depfile": str(depfile),
                        "digest": sha256_file(depfile),
                    }
                ],
            )

    def test_records_clang_modmap_response_depfile(self):
        # Clang/CMake passes the module mapper through a response file
        # (`@<base>.modmap`); the depfile is still `<base>.ddi.d`.
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "model.cpp"
            source.write_text("int x;\n")
            depfile = Path(tmp) / "model.cpp.o.ddi.d"
            depfile.write_text("model.cpp.o.ddi: model.cpp\n")
            command = (
                f"clang++ -MD @model.cpp.o.modmap "
                f"-o model.cpp.o -c {source}"
            )
            document = self._record(tmp, [{"file": str(source), "directory": tmp, "command": command}])
            self.assertEqual(
                document["entries"],
                [
                    {
                        "source": str(source),
                        "depfile": str(depfile),
                        "digest": sha256_file(depfile),
                    }
                ],
            )

    def test_records_traditional_output_depfile_without_module_mapper(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "model.cpp"
            source.write_text("int x;\n")
            depfile = Path(tmp) / "model.o.d"
            depfile.write_text("model.o: model.cpp\n")
            command = f"g++ -MD -o model.o -c {source}"
            document = self._record(tmp, [{"file": str(source), "directory": tmp, "command": command}])
            self.assertEqual(
                document["entries"][0]["depfile"],
                str(depfile),
            )

    def test_missing_depfile_is_omitted(self):
        # A compiled source whose depfile does not exist yet is omitted; the
        # validator then fails closed with PARITY-6003 for the missing entry.
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "model.cpp"
            source.write_text("int x;\n")
            command = f"g++ -MD -o model.o -c {source}"
            document = self._record(tmp, [{"file": str(source), "directory": tmp, "command": command}])
            self.assertEqual(document["entries"], [])

    def test_config_digest_ties_evidence_to_index_and_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "model.cpp"
            source.write_text("int x;\n")
            depfile = Path(tmp) / "model.o.d"
            depfile.write_text("model.o: model.cpp\n")
            command = f"g++ -MD -o model.o -c {source}"
            document = self._record(tmp, [{"file": str(source), "directory": tmp, "command": command}])
            # Same inputs -> same digest.
            second = self._record(tmp, [{"file": str(source), "directory": tmp, "command": command}])
            self.assertEqual(document["config_digest"], second["config_digest"])
            # A different compile-commands input -> a different digest.
            index_path = Path(tmp) / "index.json"
            commands_path = Path(tmp) / "compile_commands.json"
            commands_path.write_text("changed\n")
            index = make_index(
                [
                    {
                        "name": "cch_ai",
                        "role": "owner",
                        "owner": "cch_ai",
                        "sources": [str(source)],
                        "dependencies": [],
                    }
                ]
            )
            output_path = Path(tmp) / "depfiles2.json"
            pg._record_depfiles(
                valid_manifest(),
                index,
                "d" * 64,
                make_compile_commands([{"file": str(source), "directory": tmp, "command": command}]),
                str(index_path),
                str(commands_path),
                str(output_path),
            )
            second_document = json.loads(output_path.read_text())
            self.assertNotEqual(document["config_digest"], second_document["config_digest"])


if __name__ == "__main__":
    unittest.main()
