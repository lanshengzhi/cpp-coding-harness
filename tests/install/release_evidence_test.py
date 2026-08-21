#!/usr/bin/env python3
"""Unit tests for the fail-closed release-qualification evidence verifier
(issue #474).

Covers the evidence-directory contract: presence, closed JSON vocabularies,
freshness against the qualification run window, toolchain contradiction rules,
required PASS markers, and the artifact digest binding.

Run directly: `python3 tests/install/release_evidence_test.py`, or through the
CTest case `cch_release_evidence_unit` registered in `cmake/tests/InstallTests.cmake`.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "ci"))

import verify_release_evidence as verifier  # noqa: E402

VCPKG_BASELINE = "2f1d605400c8727cc00c15797aba796c88ccd523"
ARTIFACT_BYTES = b"fake cpp_harness runtime bytes"


class EvidenceVerifierTest(unittest.TestCase):
    """Builds a complete passing evidence directory; each test mutates one
    aspect and asserts the corresponding stable rule identifier fires."""

    def setUp(self):
        self.now = time.time()
        self.run_started_at = self.now - 600
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.evidence_dir = Path(self.scratch.name) / "evidence"
        self.evidence_dir.mkdir()
        self.artifact = Path(self.scratch.name) / "cpp_harness"
        self.artifact.write_bytes(ARTIFACT_BYTES)
        self._write_complete_evidence()

    def _write(self, name: str, content: str) -> Path:
        path = self.evidence_dir / name
        path.write_text(content, encoding="utf-8")
        return path

    def _write_complete_evidence(self) -> None:
        self._write(
            "toolchain.json",
            json.dumps(
                {
                    "schema_version": 1,
                    "recorded_at": self.now - 60,
                    "gcc_version": "16.1.1",
                    "cmake_version": "4.4.2",
                    "ninja_version": "1.13.2",
                    "build_type": "Release",
                    "ipo_lto": True,
                    "vcpkg_revision": VCPKG_BASELINE,
                }
            ),
        )
        self._write("parity-build-gate.json", json.dumps({"ok": True}))
        self._write(
            "install-gate.log",
            "configure output\nRuntime install gate: PASS (Parity Architecture Gate)\n",
        )
        self._write(
            "dependency-closure.log",
            "dependency audit: PASS (6 declared runtime dependencies)\n",
        )
        self._write(
            "relocation-smoke.log",
            "".join(
                f"smoke PASS: {marker}\n"
                for marker in ("version", "help", "offline-determinism", "relocation")
            ),
        )
        digest = hashlib.sha256(ARTIFACT_BYTES).hexdigest()
        self._write("artifact.sha256", f"{digest}  bin/cpp_harness\n")

    def verify(self) -> list[str]:
        return verifier.verify_evidence(
            evidence_dir=self.evidence_dir,
            run_started_at=self.run_started_at,
            vcpkg_baseline=VCPKG_BASELINE,
            artifact_path=self.artifact,
            now=self.now,
        )

    def _assert_rule(self, rule: str) -> None:
        diagnostics = self.verify()
        matching = [d for d in diagnostics if d.startswith(rule)]
        self.assertTrue(
            matching,
            f"expected rule {rule} in diagnostics, got: {diagnostics}",
        )

    def test_complete_evidence_passes(self):
        self.assertEqual(self.verify(), [])

    # ── Missing evidence (REL-1001) ──

    def test_missing_evidence_file_fails(self):
        for name in verifier.REQUIRED_EVIDENCE:
            with self.subTest(name=name):
                (self.evidence_dir / name).unlink()
                self._assert_rule("REL-1001")
                self._write_complete_evidence()

    # ── Invalid evidence (REL-1002) ──

    def test_unparseable_toolchain_json_fails(self):
        self._write("toolchain.json", "{not json")
        self._assert_rule("REL-1002")

    def test_unparseable_gate_report_fails(self):
        self._write("parity-build-gate.json", "[1, 2")
        self._assert_rule("REL-1002")

    def test_malformed_digest_file_fails(self):
        self._write("artifact.sha256", "not-a-hex-digest\n")
        self._assert_rule("REL-1002")

    # ── Closed vocabulary (REL-1003) ──

    def _mutate_toolchain(self, **changes) -> None:
        document = json.loads((self.evidence_dir / "toolchain.json").read_text())
        document.update(changes)
        self._write("toolchain.json", json.dumps(document))

    def test_unknown_toolchain_field_fails(self):
        self._mutate_toolchain(unexpected="field")
        self._assert_rule("REL-1003")

    def test_unknown_toolchain_schema_version_fails(self):
        self._mutate_toolchain(schema_version=99)
        self._assert_rule("REL-1003")

    # ── Incomplete evidence (REL-1004) ──

    def test_missing_toolchain_field_fails(self):
        document = json.loads((self.evidence_dir / "toolchain.json").read_text())
        del document["gcc_version"]
        self._write("toolchain.json", json.dumps(document))
        self._assert_rule("REL-1004")

    def test_missing_smoke_marker_fails(self):
        self._write(
            "relocation-smoke.log",
            "smoke PASS: version\nsmoke PASS: help\nsmoke PASS: relocation\n",
        )
        self._assert_rule("REL-1004")

    def test_missing_install_gate_marker_fails(self):
        self._write("install-gate.log", "installed files\n")
        self._assert_rule("REL-1004")

    def test_missing_dependency_closure_marker_fails(self):
        self._write("dependency-closure.log", "ldd output\n")
        self._assert_rule("REL-1004")

    # ── Stale evidence (REL-2001) ──

    def test_toolchain_recorded_before_run_start_fails(self):
        self._mutate_toolchain(recorded_at=self.run_started_at - 1)
        self._assert_rule("REL-2001")

    def test_toolchain_recorded_in_the_future_fails(self):
        self._mutate_toolchain(recorded_at=self.now + 3600)
        self._assert_rule("REL-2001")

    def test_evidence_file_older_than_run_start_fails(self):
        stale = self.run_started_at - 5
        target = self.evidence_dir / "install-gate.log"
        os.utime(target, (stale, stale))
        self._assert_rule("REL-2001")

    # ── Contradictory toolchain facts (REL-3001) ──

    def test_wrong_gcc_major_fails(self):
        self._mutate_toolchain(gcc_version="15.2.0")
        self._assert_rule("REL-3001")

    def test_unparseable_gcc_version_fails(self):
        self._mutate_toolchain(gcc_version="gcc")
        self._assert_rule("REL-3001")

    def test_cmake_below_floor_fails(self):
        self._mutate_toolchain(cmake_version="4.3.0")
        self._assert_rule("REL-3001")

    def test_ninja_below_floor_fails(self):
        self._mutate_toolchain(ninja_version="1.10.2")
        self._assert_rule("REL-3001")

    def test_non_release_build_type_fails(self):
        self._mutate_toolchain(build_type="Debug")
        self._assert_rule("REL-3001")

    def test_disabled_ipo_fails(self):
        self._mutate_toolchain(ipo_lto=False)
        self._assert_rule("REL-3001")

    def test_vcpkg_baseline_mismatch_fails(self):
        self._mutate_toolchain(vcpkg_revision="0" * 40)
        self._assert_rule("REL-3001")

    # ── Evidence recording failure (REL-3002) ──

    def test_gate_report_recording_failure_fails(self):
        self._write("parity-build-gate.json", json.dumps({"ok": False}))
        self._assert_rule("REL-3002")

    # ── Artifact binding (REL-3003) ──

    def test_artifact_digest_mismatch_fails(self):
        self._write("artifact.sha256", hashlib.sha256(b"other").hexdigest() + "\n")
        self._assert_rule("REL-3003")

    def test_missing_artifact_fails(self):
        self.artifact.unlink()
        self._assert_rule("REL-3003")


class MainTest(unittest.TestCase):
    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as scratch:
            evidence_dir = Path(scratch) / "evidence"
            artifact = Path(scratch) / "cpp_harness"
            artifact.write_bytes(ARTIFACT_BYTES)
            now = time.time()

            # Incomplete directory: missing everything.
            evidence_dir.mkdir()
            exit_code = verifier.main(
                [
                    "--evidence-dir",
                    str(evidence_dir),
                    "--run-started-at",
                    str(now - 600),
                    "--vcpkg-baseline",
                    VCPKG_BASELINE,
                    "--artifact",
                    str(artifact),
                ]
            )
            self.assertEqual(exit_code, 1)


if __name__ == "__main__":
    unittest.main()
