#!/usr/bin/env python3
"""Public CLI regressions for the named TUI evidence boundary (#947)."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts/tui/evidence.py"
REVISION = "7c10bd4337495ee613f2224843ecdf349b80d1df"


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(value) + "\n").encode()
    path.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def fixture(root):
    """Synthetic valid bundle; assertions below exercise selection/provenance, not pi behavior."""
    environment = {"platform": "linux", "arch": "x64", "libc": "glibc", "glibcVersion": "2.42", "node": "v24", "TERM": "xterm-256color"}
    captured = "2026-10-10T00:00:00.000Z"
    manifest = {
        "schema": "pike/tui-evidence/1", "baseline": "pi-v1.0.4", "revision": REVISION,
        "capturedAt": captured, "environment": environment,
        "generator": {"path": "capture/capture-named-tui.mts", "sha256": "0" * 64},
        "artifacts": [],
    }
    for family in ("input", "component", "screen-state", "capability-ledger", "utils-width", "utils-ansi", "fuzzy", "keys-kitty-text", "latex"):
        artifact = {
            "baseline": "pi-v1.0.4", "revision": REVISION, "capturedAt": captured,
            "environment": environment, "sourceEndpoints": ["packages/tui/src/keys.ts:parseKey"],
            "scenarios": [{"name": "probe", "dimensions": {"columns": 20, "rows": 8}, "inputs": {"sequence": "a"}, "expected": {"value": "a"}}],
        }
        relative = f"{family}.json"
        target = root / "bundles/pi-v1.0.4" / relative
        digest = write_json(target, artifact)
        manifest["artifacts"].append({"path": relative, "family": family, "sha256": digest, "bytes": target.stat().st_size, "sourceEndpoints": artifact["sourceEndpoints"]})
    pin_manifest(root, manifest)
    return manifest


def pin_manifest(root, manifest):
    digest = write_json(root / "bundles/pi-v1.0.4/manifest.json", manifest)
    write_json(root / "baselines.json", {
        "schema": "pike/tui-baselines/1", "default": "pi-v1.0.4",
        "baselines": {"pi-v1.0.4": {"revision": REVISION, "bundle": "bundles/pi-v1.0.4", "manifestSha256": digest}},
    })


def run(root, operation="verify", *extra):
    return subprocess.run([sys.executable, str(RUNNER), operation, "--fixture-root", str(root), "--baseline", "pi-v1.0.4", *extra], capture_output=True)


class EvidenceBoundaryTest(unittest.TestCase):
    def test_missing_named_bundle_cannot_fall_back_to_historical_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            (root / "bundles/pi-v1.0.4/manifest.json").unlink()
            (root / "input-decode.json").write_text('{"historical": true}')
            result = run(root, "read", "--artifact", "input.json")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"missing evidence bundle", result.stderr)

    def test_wrong_revision_with_valid_digests_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root)
            manifest["revision"] = "83114817c68f5413e4d7ba6d7003ddc511cd31d2"
            pin_manifest(root, manifest)
            result = run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"source revision/baseline mismatch", result.stderr)

    def test_same_length_artifact_tampering_cannot_reach_reader(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            target = root / "bundles/pi-v1.0.4/input.json"
            target.write_bytes(target.read_bytes().replace(b'"value": "a"', b'"value": "b"'))
            result = run(root, "read", "--artifact", "input.json")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"digest/size mismatch", result.stderr)

    def test_hashes_alone_cannot_accept_missing_capture_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root)
            del manifest["environment"]
            pin_manifest(root, manifest)
            result = run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"capture metadata", result.stderr)

    def test_artifact_wrong_revision_is_rejected_even_with_recomputed_digests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root)
            target = root / "bundles/pi-v1.0.4/input.json"
            artifact = json.loads(target.read_bytes())
            artifact["revision"] = "83114817c68f5413e4d7ba6d7003ddc511cd31d2"
            manifest["artifacts"][0]["sha256"] = write_json(target, artifact)
            manifest["artifacts"][0]["bytes"] = target.stat().st_size
            pin_manifest(root, manifest)
            result = run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"artifact source revision/baseline mismatch", result.stderr)

    def test_incomplete_family_set_is_not_a_verified_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root)
            manifest["artifacts"].pop()
            pin_manifest(root, manifest)
            result = run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"missing required evidence family", result.stderr)

    def test_verified_reader_emits_original_bytes_and_no_status_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            result = run(root, "read", "--artifact", "input.json")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, (root / "bundles/pi-v1.0.4/input.json").read_bytes())

    def test_unknown_baseline_and_unrecorded_artifact_never_use_default(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            for arguments in (("--baseline", "missing"), ("--artifact", "input-decode.json")):
                with self.subTest(arguments=arguments):
                    result = run(root, "read", "--artifact", "input.json", *arguments)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b"")

    def test_manifest_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            target = root / "bundles/pi-v1.0.4/manifest.json"
            target.write_bytes(target.read_bytes().replace(b"probe", b"other") + b" ")
            result = run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"manifest digest mismatch", result.stderr)

    def test_recorded_paths_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = fixture(root)
            manifest["artifacts"][0]["path"] = "../../input-decode.json"
            pin_manifest(root, manifest)
            result = run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"invalid evidence path", result.stderr)

    def test_committed_bundle_passes_verification(self):
        result = run(ROOT / "fixtures/pi-tui")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
