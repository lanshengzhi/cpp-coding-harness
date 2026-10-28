#!/usr/bin/env python3
"""Public boundary regressions for the pi-v1.3.0 named audit evidence registry (#1046).

The registry binds the audit baseline to one full revision, each ticket family to one manifest
digest, each manifest to its executable capture and artifact bytes, and each artifact to its
scenario observations. These tests exercise the public verify/read/register CLI at that boundary:
a bundle that is missing, misaddressed, altered or bound to another source must fail closed, and a
read must emit only bytes that verified through the whole chain. Synthetic fixtures make the
negative cases independent of the committed evidence; the committed example family is checked
against the real frozen revision.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "fixtures" / "pi-v1.3.0"
RUNNER = ROOT / "scripts" / "evidence" / "audit_evidence.py"
BASELINE = "pi-v1.3.0"
REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0"
FAMILY = "audit-1046"
ARTIFACT = "fuzzy-match.json"
CAPTURE = FIXTURES / FAMILY / "capture-extension-example.mts"

AUDIT_SCHEMA = "pike/pi-v1.3.0-audit-baselines/1"
MANIFEST_SCHEMA = "pike/pi-v1.3.0-audit-evidence/1"


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def write_json(path: Path, value: object) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode()
    path.write_bytes(payload)
    return sha256_bytes(payload)


def run(fixture_root: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(RUNNER), *arguments, "--fixture-root", str(fixture_root)],
        cwd=ROOT,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        text=True,
        capture_output=True,
    )


def ignored_paths(root: Path, paths: list[Path]) -> set[Path]:
    """Use Git's ignore rules; tracked files remain evidence even if a pattern matches."""
    if not paths:
        return set()
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "check-ignore", "-z", "--stdin"],
            input=b"".join(os.fsencode(str(path)) + b"\0" for path in paths),
            capture_output=True,
        )
        if result.returncode in (0, 1):
            return {Path(os.fsdecode(path)) for path in result.stdout.split(b"\0") if path}
    except OSError:
        pass
    # Keep the fallback narrow so a Git failure cannot hide the evidence tree.
    return {path for path in paths if "__pycache__" in path.parts or path.suffix == ".pyc"}


def fingerprint(root: Path) -> dict[str, str]:
    digest: dict[str, str] = {}
    if not root.is_dir():
        return digest
    paths = [path.relative_to(root) for path in sorted(root.rglob("*")) if path.is_file()]
    ignored = ignored_paths(root, paths)
    for path in paths:
        if path not in ignored:
            digest[str(path)] = sha256_bytes((root / path).read_bytes())
    return digest


def environment() -> dict[str, str]:
    return {
        "platform": "linux",
        "arch": "x64",
        "libc": "glibc",
        "glibcVersion": "2.44",
        "node": "v26.11.1",
        "tsx": "4.20.3",
    }


def scenarios() -> list[dict[str, object]]:
    return [
        {
            "name": "probe",
            "inputs": {"query": "fb", "text": "foo-bar"},
            "expected": {"matches": True, "score": -10.3},
            "normalizations": [],
        }
    ]


def endpoints() -> list[str]:
    return [
        "packages/tui/src/fuzzy.ts:fuzzyMatch",
        "packages/tui/test/fuzzy.test.ts:word boundary matches score better",
    ]


def build_fixture(root: Path) -> dict[str, object]:
    """Write a complete synthetic bundle in the documented layout; callers mutate from here."""
    audit_root = root / "fixtures" / "pi-v1.3.0"
    family_dir = audit_root / FAMILY
    family_dir.mkdir(parents=True)
    generator = family_dir / "capture-fixture.mts"
    generator.write_text("// synthetic capture for the named-evidence boundary test\n", encoding="utf-8")

    captured_at = "2026-10-21T00:00:00.000Z"
    artifact = {
        "baseline": BASELINE,
        "revision": REVISION,
        "capturedAt": captured_at,
        "environment": environment(),
        "sourceEndpoints": endpoints(),
        "scenarios": scenarios(),
    }
    artifact_path = family_dir / ARTIFACT
    artifact_digest = write_json(artifact_path, artifact)
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "baseline": BASELINE,
        "revision": REVISION,
        "ticket": "1046",
        "family": FAMILY,
        "capturedAt": captured_at,
        "environment": environment(),
        "generator": {
            "path": f"fixtures/pi-v1.3.0/{FAMILY}/{generator.name}",
            "sha256": sha256_bytes(generator.read_bytes()),
            "command": "synthetic capture command",
            "sources": [{"path": f"fixtures/pi-v1.3.0/{FAMILY}/{generator.name}", "sha256": sha256_bytes(generator.read_bytes())}],
        },
        "artifacts": [
            {
                "path": ARTIFACT,
                "family": FAMILY,
                "bytes": artifact_path.stat().st_size,
                "sha256": artifact_digest,
                "sourceEndpoints": endpoints(),
                "scenarios": scenarios(),
            }
        ],
    }
    pin_fixture(audit_root, manifest)
    return manifest


def pin_fixture(audit_root: Path, manifest: dict[str, object]) -> None:
    digest = write_json(audit_root / FAMILY / "manifest.json", manifest)
    write_json(
        audit_root / "baselines.json",
        {
            "schema": AUDIT_SCHEMA,
            "default": BASELINE,
            "baselines": {
                BASELINE: {
                    "revision": REVISION,
                    "bundles": {FAMILY: {"manifest": f"{FAMILY}/manifest.json", "manifestSha256": digest}},
                }
            },
        },
    )


class CommittedEvidenceTest(unittest.TestCase):
    def test_committed_bundle_verifies_against_the_approved_revision(self):
        result = run(FIXTURES, "verify")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(BASELINE, result.stdout)
        registry = json.loads((FIXTURES / "baselines.json").read_text(encoding="utf-8"))
        self.assertEqual(registry["schema"], AUDIT_SCHEMA)
        self.assertEqual(registry["default"], BASELINE)
        self.assertEqual(registry["baselines"][BASELINE]["revision"], REVISION)
        self.assertIn(FAMILY, registry["baselines"][BASELINE]["bundles"])

    def test_captured_artifact_reads_only_through_the_verified_chain(self):
        result = run(FIXTURES, "read", "--family", FAMILY, "--artifact", ARTIFACT)
        self.assertEqual(result.returncode, 0, result.stderr)
        artifact = json.loads(result.stdout)
        self.assertEqual(artifact["baseline"], BASELINE)
        self.assertEqual(artifact["revision"], REVISION)
        self.assertEqual(artifact["environment"]["platform"], "linux")
        records = {record["name"]: record for record in artifact["scenarios"]}
        # Independent known-good literals from the frozen upstream contract: an empty query
        # matches with score zero, and the word-boundary match beats the interior one.
        empty = records["empty query matches everything with score zero"]["expected"]
        self.assertEqual(empty, {"matches": True, "score": 0})
        boundary = records["word boundary scores better than interior match"]["expected"]
        self.assertTrue(boundary["boundary"]["matches"])
        self.assertTrue(boundary["interior"]["matches"])
        self.assertLess(boundary["boundary"]["score"], boundary["interior"]["score"])

    def test_historical_ai_and_tui_baselines_are_preserved(self):
        ai_registry = json.loads((ROOT / "fixtures/pi-ai/baselines.json").read_text(encoding="utf-8"))
        self.assertEqual(ai_registry["default_baseline"], "pi-v0.87.1")
        self.assertNotIn(BASELINE, ai_registry["baselines"])
        self.assertEqual(ai_registry["baselines"]["pi-v0.87.1"]["captured_at"], "2026-09-23T12:05:52Z")
        self.assertEqual(ai_registry["baselines"]["pi-v1.0.4"]["captured_at"], "2026-10-07T00:36:41Z")
        tui_registry = json.loads((ROOT / "fixtures/pi-tui/baselines.json").read_text(encoding="utf-8"))
        self.assertEqual(tui_registry["default"], "pi-v1.0.4")
        self.assertEqual(tui_registry["baselines"]["pi-v1.0.4"]["revision"], "7c10bd4337495ee613f2224843ecdf349b80d1df")


class VerificationBoundaryTest(unittest.TestCase):
    def test_missing_bundle_cannot_fall_back_to_historical_observations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            (audit_root / FAMILY / "manifest.json").unlink()
            # Park an otherwise valid historical observation where a fallback reader might scan.
            (audit_root / "input.json").write_text(
                json.dumps({"baseline": "pi-v1.0.4", "revision": "7c10bd4337495ee613f2224843ecdf349b80d1df"}),
                encoding="utf-8",
            )
            result = run(audit_root, "read", "--family", FAMILY, "--artifact", ARTIFACT)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("missing evidence bundle", result.stderr)

    def test_verifier_rejects_a_checkout_at_another_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            checkout = Path(directory) / "pi"
            checkout.mkdir()
            git = ["git", "-C", str(checkout), "-c", "user.name=evidence", "-c", "user.email=evidence@example.invalid"]
            subprocess.run(["git", "init", "-q", str(checkout)], check=True)
            subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "not the frozen revision"], check=True)
            result = run(audit_root, "verify", "--pi-root", str(checkout))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not the pi-v1.3.0 revision", result.stderr)

    def test_unknown_baseline_and_unrecorded_artifact_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            for arguments in (
                ("verify", "--baseline", "pi-v1.0.4"),
                ("read", "--family", "audit-9999", "--artifact", ARTIFACT),
                ("read", "--family", FAMILY, "--artifact", "missing.json"),
            ):
                with self.subTest(arguments=arguments):
                    result = run(audit_root, *arguments)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, "")

    def test_wrong_revision_and_wrong_name_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            wrong_revision = "7c10bd4337495ee613f2224843ecdf349b80d1df"
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            registry["baselines"][BASELINE]["revision"] = wrong_revision
            write_json(audit_root / "baselines.json", registry)
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source revision mismatch", result.stderr)

            manifest["baseline"] = "pi-v1.0.4"
            pin_fixture(audit_root, manifest)
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("baseline mismatch", result.stderr)

    def test_short_revision_in_the_registry_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            registry["baselines"][BASELINE]["revision"] = REVISION[:8]
            write_json(audit_root / "baselines.json", registry)
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("40-character revision", result.stderr)

    def test_altered_manifest_and_altered_artifact_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            manifest_path = audit_root / FAMILY / "manifest.json"
            manifest_path.write_bytes(manifest_path.read_bytes() + b" ")
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("manifest digest mismatch", result.stderr)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            artifact_path = audit_root / FAMILY / ARTIFACT
            artifact_path.write_bytes(artifact_path.read_bytes() + b"\n")
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("artifact SHA-256 mismatch", result.stderr)

    def test_artifact_from_another_source_is_rejected_despite_valid_digests(self):
        """Re-pinning the manifest over a v1.0.4 artifact still fails the revision binding."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            artifact_path = audit_root / FAMILY / ARTIFACT
            artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
            artifact["revision"] = "7c10bd4337495ee613f2224843ecdf349b80d1df"
            artifact_digest = write_json(artifact_path, artifact)
            manifest["artifacts"][0]["bytes"] = artifact_path.stat().st_size
            manifest["artifacts"][0]["sha256"] = artifact_digest
            pin_fixture(audit_root, manifest)
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("revision/baseline mismatch", result.stderr)

    def test_unsafe_artifact_reference_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            escape = audit_root / "escape.json"
            write_json(escape, json.loads((audit_root / FAMILY / ARTIFACT).read_text(encoding="utf-8")))
            for unsafe in ("../escape.json", str(escape)):
                with self.subTest(unsafe=unsafe):
                    manifest["artifacts"][0]["path"] = unsafe
                    pin_fixture(audit_root, manifest)
                    result = run(audit_root, "verify")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("invalid evidence path", result.stderr)

    def test_duplicate_scenarios_and_missing_source_endpoints_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            artifact_path = audit_root / FAMILY / ARTIFACT
            artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
            artifact["scenarios"] = artifact["scenarios"] + [dict(artifact["scenarios"][0])]
            manifest["artifacts"][0]["scenarios"] = artifact["scenarios"]
            manifest["artifacts"][0]["bytes"] = len((json.dumps(artifact, indent=2, ensure_ascii=False) + "\n").encode())
            write_json(artifact_path, artifact)
            manifest["artifacts"][0]["sha256"] = sha256_bytes(artifact_path.read_bytes())
            pin_fixture(audit_root, manifest)
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("duplicate evidence scenario name", result.stderr)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            artifact_path = audit_root / FAMILY / ARTIFACT
            artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
            artifact["sourceEndpoints"] = []
            manifest["artifacts"][0]["sourceEndpoints"] = []
            write_json(artifact_path, artifact)
            manifest["artifacts"][0]["bytes"] = artifact_path.stat().st_size
            manifest["artifacts"][0]["sha256"] = sha256_bytes(artifact_path.read_bytes())
            pin_fixture(audit_root, manifest)
            result = run(audit_root, "verify")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing source endpoints", result.stderr)


class RegistrationTest(unittest.TestCase):
    def test_registration_validates_then_pins_the_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            registry["baselines"][BASELINE]["bundles"] = {}
            write_json(audit_root / "baselines.json", registry)

            manifest_path = audit_root / FAMILY / "manifest.json"
            candidate = run(audit_root, "check", "--manifest", str(manifest_path))
            self.assertEqual(candidate.returncode, 0, candidate.stderr)
            record = json.loads(candidate.stdout)
            self.assertEqual(record[FAMILY]["manifest"], f"{FAMILY}/manifest.json")

            registered = run(audit_root, "register", "--manifest", str(manifest_path))
            self.assertEqual(registered.returncode, 0, registered.stderr)
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            self.assertEqual(registry["baselines"][BASELINE]["bundles"][FAMILY]["manifestSha256"], record[FAMILY]["manifestSha256"])
            self.assertEqual(run(audit_root, "verify").returncode, 0)

            # A recaptured manifest leaves the registry pin stale; replacing it is deliberate.
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["generator"]["command"] = "recaptured command"
            write_json(manifest_path, manifest)
            refused = run(audit_root, "register", "--manifest", str(manifest_path))
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("--replace", refused.stderr)
            replaced = run(audit_root, "register", "--manifest", str(manifest_path), "--replace")
            self.assertEqual(replaced.returncode, 0, replaced.stderr)
            self.assertEqual(run(audit_root, "verify").returncode, 0)

    def test_variant_family_registers_alongside_its_ticket_family(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            variant = f"{FAMILY}-replay"
            shutil.copytree(audit_root / FAMILY, audit_root / variant)
            manifest_path = audit_root / variant / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["family"] = variant
            manifest["artifacts"][0]["family"] = variant
            generator = audit_root / variant / "capture-fixture.mts"
            generator_path = f"fixtures/pi-v1.3.0/{variant}/{generator.name}"
            manifest["generator"]["path"] = generator_path
            manifest["generator"]["sources"] = [
                {"path": generator_path, "sha256": sha256_bytes(generator.read_bytes())}
            ]
            write_json(manifest_path, manifest)

            registered = run(audit_root, "register", "--manifest", str(manifest_path))
            self.assertEqual(registered.returncode, 0, registered.stderr)
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            bundles = registry["baselines"][BASELINE]["bundles"]
            self.assertIn(FAMILY, bundles)
            self.assertIn(variant, bundles)
            self.assertEqual(run(audit_root, "verify").returncode, 0)
            read = run(audit_root, "read", "--family", variant, "--artifact", ARTIFACT)
            self.assertEqual(read.returncode, 0, read.stderr)

    def test_malformed_family_variant_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            malformed = f"{FAMILY}_replay"
            shutil.copytree(audit_root / FAMILY, audit_root / malformed)
            manifest_path = audit_root / malformed / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["family"] = malformed
            manifest["artifacts"][0]["family"] = malformed
            write_json(manifest_path, manifest)
            result = run(audit_root, "check", "--manifest", str(manifest_path))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("invalid audit family", result.stderr)

    def test_registration_refuses_an_unverified_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            artifact_path = audit_root / FAMILY / ARTIFACT
            artifact_path.write_bytes(artifact_path.read_bytes() + b"\n")
            result = run(audit_root, "register", "--manifest", str(audit_root / FAMILY / "manifest.json"))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("artifact SHA-256 mismatch", result.stderr)
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            self.assertIn(FAMILY, registry["baselines"][BASELINE]["bundles"])

    def test_family_cannot_be_reused_by_another_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build_fixture(root)
            audit_root = root / "fixtures" / "pi-v1.3.0"
            registry = json.loads((audit_root / "baselines.json").read_text(encoding="utf-8"))
            registry["baselines"]["pi-v1.0.4"] = {
                "revision": "7c10bd4337495ee613f2224843ecdf349b80d1df",
                "bundles": {FAMILY: {"manifest": f"{FAMILY}/manifest.json", "manifestSha256": "0" * 64}},
            }
            write_json(audit_root / "baselines.json", registry)
            result = run(audit_root, "register", "--manifest", str(audit_root / FAMILY / "manifest.json"))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("already belongs to baseline", result.stderr)


class RealCaptureTest(unittest.TestCase):
    """The capture-to-verified-read path against the frozen checkout, when one is available."""

    def pi_checkout(self) -> Path:
        checkout = Path(os.environ.get("PI_CHECKOUT", ROOT.parent / "pi"))
        return checkout

    def tsx(self) -> Path | None:
        candidate = self.pi_checkout() / "node_modules" / ".bin" / "tsx"
        return candidate if candidate.is_file() else None

    def stage(self, root: Path, revision: str) -> Path:
        audit_root = root / "fixtures" / "pi-v1.3.0"
        family_dir = audit_root / FAMILY
        family_dir.mkdir(parents=True)
        shutil.copyfile(CAPTURE, family_dir / CAPTURE.name)
        write_json(
            audit_root / "baselines.json",
            {
                "schema": AUDIT_SCHEMA,
                "default": BASELINE,
                "baselines": {BASELINE: {"revision": revision, "bundles": {}}},
            },
        )
        return audit_root

    def run_capture(self, audit_root: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.tsx()), str(audit_root / FAMILY / CAPTURE.name)],
            cwd=ROOT,
            env={
                **os.environ,
                "PI_CHECKOUT": str(self.pi_checkout()),
                "PI_BASELINE": BASELINE,
            },
            text=True,
            capture_output=True,
            timeout=600,
        )

    def test_capture_refuses_a_checkout_at_another_revision_before_writing(self):
        if self.tsx() is None:
            self.skipTest("sibling pi checkout with tsx is unavailable; offline checks still ran")
        with tempfile.TemporaryDirectory() as directory:
            audit_root = self.stage(Path(directory), "7c10bd4337495ee613f2224843ecdf349b80d1df")
            result = self.run_capture(audit_root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must be at", result.stderr)
            self.assertFalse((audit_root / FAMILY / ARTIFACT).exists())
            self.assertFalse((audit_root / FAMILY / "manifest.json").exists())

    def test_capture_register_verify_and_read_end_to_end(self):
        if self.tsx() is None:
            self.skipTest("sibling pi checkout with tsx is unavailable; offline checks still ran")
        with tempfile.TemporaryDirectory() as directory:
            audit_root = self.stage(Path(directory), REVISION)
            captured = self.run_capture(audit_root)
            self.assertEqual(captured.returncode, 0, captured.stderr)
            manifest_dir = audit_root / FAMILY
            self.assertTrue((manifest_dir / ARTIFACT).is_file())
            self.assertTrue((manifest_dir / "manifest.json").is_file())

            registered = run(audit_root, "register", "--manifest", str(manifest_dir / "manifest.json"))
            self.assertEqual(registered.returncode, 0, registered.stderr)
            verified = run(audit_root, "verify")
            self.assertEqual(verified.returncode, 0, verified.stderr)
            read = run(audit_root, "read", "--family", FAMILY, "--artifact", ARTIFACT)
            self.assertEqual(read.returncode, 0, read.stderr)
            self.assertEqual(json.loads(read.stdout)["revision"], REVISION)


class EvidenceTreeTest(unittest.TestCase):
    def test_fingerprint_honors_git_ignores_but_keeps_tracked_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("__pycache__/\n*.pyc\n*.generated\n", encoding="utf-8")
            tracked = root / "tracked.pyc"
            tracked.write_bytes(b"tracked")
            subprocess.run(["git", "-C", str(root), "add", "-f", "tracked.pyc"], check=True)
            (root / "evidence.json").write_bytes(b"evidence")
            before = fingerprint(root)
            cache = root / "__pycache__"
            cache.mkdir()
            (cache / "module.pyc").write_bytes(b"bytecode")
            (root / "other.generated").write_bytes(b"generated")
            self.assertEqual(before, fingerprint(root))
            tracked.write_bytes(b"changed")
            self.assertNotEqual(before, fingerprint(root))

    def test_fingerprint_fallback_keeps_evidence_outside_a_git_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / "evidence.json"
            evidence.write_bytes(b"evidence")
            before = fingerprint(root)
            cache = root / "__pycache__"
            cache.mkdir()
            (cache / "module.pyc").write_bytes(b"bytecode")
            (root / "other.pyc").write_bytes(b"bytecode")
            self.assertEqual(before, fingerprint(root))
            evidence.write_bytes(b"changed")
            self.assertNotEqual(before, fingerprint(root))

    def test_validation_never_mutates_the_committed_evidence(self):
        before = fingerprint(FIXTURES)
        result = run(FIXTURES, "verify")
        self.assertEqual(result.returncode, 0, result.stderr)
        read = run(FIXTURES, "read", "--family", FAMILY, "--artifact", ARTIFACT)
        self.assertEqual(read.returncode, 0, read.stderr)
        after = fingerprint(FIXTURES)
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
