#!/usr/bin/env python3
"""Exercise preflight through its CLI in disposable repositories."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/dev/lane-preflight.py"
DELIVERY_DIR = Path("docs/research/pi-v1.3.0")
SCHEMA = "pike/pi-v1.3.0-capability-deliveries/1"
LANE_ENV_VARS = ("CCH_LANE_NAME", "CCH_LANE_LOCK_SKIP", "CCH_LANE_TASK_ID", "CCH_LANE_AGENT")


class LanePreflightTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "work"
        self.upstream = Path(self.tmp.name) / "upstream"
        for repo in (self.root, self.upstream):
            repo.mkdir()
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "initial", "--allow-empty"], check=True)
        self.revision = subprocess.check_output(["git", "-C", str(self.upstream), "rev-parse", "HEAD"], text=True).strip()

    def run_preflight(self, *args, env=None):
        # The lane environment is set explicitly so an ambient lane cannot leak in, and an
        # omitted CCH_LANE_NAME is a real unset rather than a silent inheritance.
        base = {key: value for key, value in os.environ.items() if key not in LANE_ENV_VARS}
        base.update(env or {})
        return subprocess.run([sys.executable, str(SCRIPT), "--lane", "lane-a", "--revision", self.revision, "--checkout", str(self.upstream), *args], cwd=self.root, capture_output=True, text=True, env=base)

    def commit(self, message="fixture"):
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", message, "--allow-empty"], check=True)

    def stage(self, *paths):
        subprocess.run(["git", "-C", str(self.root), "add", "--", *paths], check=True)

    def pinned(self, path):
        return {"path": path, "sha256": hashlib.sha256((self.root / path).read_bytes()).hexdigest()}

    def write_manifest(self, records, name="1058-deliveries.json", raw=None):
        directory = self.root / DELIVERY_DIR
        directory.mkdir(parents=True, exist_ok=True)
        content = raw if raw is not None else json.dumps({"schema": SCHEMA, "baseline": "pi-v1.3.0", "revision": "0" * 40, "records": records})
        (directory / name).write_text(content)

    def test_clean_snapshot_and_dirty_filename_redaction(self):
        result = self.run_preflight()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (self.root / "private-filename-token").write_text("secret-content-token")
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("attention", result.stdout)
        self.assertNotIn("private-filename-token", result.stdout + result.stderr)
        self.assertNotIn("secret-content-token", result.stdout + result.stderr)

    def test_lock_and_revision_errors_fail_closed_without_metadata(self):
        lock = self.root / ".lane-lock.json"
        lock.write_text('{"lane":"lane-b","agent":"sensitive-token","startedAt":"2026-11-09T00:00:00Z"}')
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("foreign", result.stdout)
        self.assertNotIn("sensitive-token", result.stdout + result.stderr)
        lock.write_text('{"lane":"lane-a", broken')
        self.assertIn("unknown", self.run_preflight().stdout)
        lock.unlink()
        (self.upstream / "untracked-secret").write_text("x")
        self.assertNotEqual(self.run_preflight().returncode, 0)

    def test_matching_lock_and_revision_mismatch(self):
        lock = self.root / ".lane-lock.json"
        lock.write_text(json.dumps({"lane": "lane-a", "agent": "sensitive-token", "startedAt": "2026-11-09T00:00:00Z"}))
        result = self.run_preflight(env={"CCH_LANE_NAME": "lane-a"})
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("lock: matching", result.stdout)
        self.assertNotIn("sensitive-token", result.stdout + result.stderr)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "next", "--allow-empty"], check=True)
        self.assertIn("revision mismatch", self.run_preflight(env={"CCH_LANE_NAME": "lane-a"}).stdout)

    def test_explicit_listing_fails_closed_without_writing(self):
        before = {p.relative_to(self.root) for p in self.root.rglob('*')}
        result = self.run_preflight("--ctest-regex", "private-regex", "--expected-count", "1")
        after = {p.relative_to(self.root) for p in self.root.rglob('*')}
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CTest selection unavailable", result.stdout)
        self.assertNotIn("private-regex", result.stdout + result.stderr)
        self.assertEqual(before, after)

    def test_explicit_selection_never_invokes_ctest_even_if_available(self):
        bin_dir = Path(self.tmp.name) / "bin"
        bin_dir.mkdir()
        marker = Path(self.tmp.name) / "ctest-was-invoked"
        fake = bin_dir / "ctest"
        fake.write_text(f"#!/bin/sh\ntouch '{marker}'\necho 'private-test-name'\n")
        fake.chmod(0o755)
        env = dict(os.environ, PATH=f"{bin_dir}:{os.environ.get('PATH', '')}")
        result = self.run_preflight("--ctest-regex", "private-regex", "--expected-count", "0", env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown: CTest selection unavailable", result.stdout)
        self.assertNotIn("private-regex", result.stdout + result.stderr)
        self.assertNotIn("private-test-name", result.stdout + result.stderr)
        self.assertFalse(marker.exists())

    def test_missing_checkout_and_symlink_lock_are_unknown(self):
        (self.root / ".lane-lock.json").symlink_to(self.root / "absent")
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown: lock", result.stdout)
        (self.root / ".lane-lock.json").unlink()
        result = subprocess.run([sys.executable, str(SCRIPT), "--lane", "lane-a", "--revision", self.revision, "--checkout", str(self.upstream / "missing-secret")], cwd=self.root, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown: upstream", result.stdout)
        self.assertNotIn("missing-secret", result.stdout + result.stderr)

    def test_inherited_git_trace_cannot_modify_existing_files(self):
        for target in (self.root / "trace-secret", Path(self.tmp.name) / "outside-secret"):
            target.write_text("original-sensitive-content")
            env = dict(os.environ, GIT_TRACE=str(target), GIT_TRACE2_EVENT=str(target),
                       GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="core.fsmonitor",
                       GIT_CONFIG_VALUE_0="malicious")
            result = self.run_preflight(env=env)
            self.assertEqual(target.read_text(), "original-sensitive-content")
            self.assertNotIn(str(target), result.stdout + result.stderr)
            self.assertNotIn("original-sensitive-content", result.stdout + result.stderr)

    def test_hidden_index_flags_are_unknown(self):
        tracked = self.upstream / "tracked"
        tracked.write_text("original")
        subprocess.run(["git", "-C", str(self.upstream), "add", "tracked"], check=True)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "tracked"], check=True)
        self.revision = subprocess.check_output(["git", "-C", str(self.upstream), "rev-parse", "HEAD"], text=True).strip()
        for flag in ("assume-unchanged", "skip-worktree"):
            with self.subTest(flag=flag):
                subprocess.run(["git", "-C", str(self.upstream), "update-index", "--" + flag, "tracked"], check=True)
                tracked.write_text("changed")
                result = self.run_preflight()
                self.assertIn("unknown: upstream", result.stdout)
                self.assertNotIn("cleanliness match", result.stdout)
                subprocess.run(["git", "-C", str(self.upstream), "update-index", "--no-" + flag, "tracked"], check=True)
                tracked.write_text("original")

    def test_disabled_filemode_is_unknown_for_both_checkouts(self):
        for repo, expected in ((self.root, "current root, branch, HEAD or status unavailable"),
                               (self.upstream, "upstream checkout unavailable")):
            with self.subTest(repo=repo.name):
                tracked = repo / "private-script"
                tracked.write_text("#!/bin/sh\n")
                tracked.chmod(0o644)
                subprocess.run(["git", "-C", str(repo), "add", tracked.name], check=True)
                subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "script"], check=True)
                if repo == self.upstream:
                    self.revision = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
                subprocess.run(["git", "-C", str(repo), "config", "core.filemode", "false"], check=True)
                tracked.chmod(0o755)
                if not tracked.stat().st_mode & 0o111:
                    self.skipTest("filesystem cannot represent executable permissions")
                self.assertEqual(subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"]), b"")
                result = self.run_preflight()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unknown: " + expected, result.stdout)
                self.assertNotIn("private-script", result.stdout + result.stderr)
                subprocess.run(["git", "-C", str(repo), "config", "core.filemode", "true"], check=True)
                tracked.chmod(0o644)

    def test_unreliable_filemode_config_is_unknown_and_unchanged(self):
        config = self.upstream / ".git" / "config"
        for value in ("false", None):
            with self.subTest(value=value):
                if value is None:
                    subprocess.run(["git", "-C", str(self.upstream), "config", "--unset", "core.filemode"], check=True)
                else:
                    subprocess.run(["git", "-C", str(self.upstream), "config", "core.filemode", value], check=True)
                before = config.read_bytes()
                result = self.run_preflight()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unknown: upstream", result.stdout)
                self.assertEqual(config.read_bytes(), before)

    def test_symlink_replaced_with_regular_file_is_unknown_in_both_checkouts(self):
        for repo, expected in ((self.root, "current root, branch, HEAD or status unavailable"),
                               (self.upstream, "upstream checkout unavailable")):
            with self.subTest(repo=repo.name):
                link = repo / "private-link"
                link.symlink_to("target")
                subprocess.run(["git", "-C", str(repo), "add", link.name], check=True)
                subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "link"], check=True)
                if repo == self.upstream:
                    self.revision = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
                subprocess.run(["git", "-C", str(repo), "config", "core.symlinks", "false"], check=True)
                link.unlink()
                link.write_text("target")
                self.assertEqual(subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"]), b"")
                before = (repo / ".git" / "config").read_bytes(), (repo / ".git" / "index").read_bytes()
                result = self.run_preflight()
                self.assertIn("unknown: " + expected, result.stdout)
                self.assertNotIn("private-link", result.stdout + result.stderr)
                self.assertEqual(before, ((repo / ".git" / "config").read_bytes(), (repo / ".git" / "index").read_bytes()))
                subprocess.run(["git", "-C", str(repo), "config", "core.symlinks", "true"], check=True)
                self.assertIn("attention:", self.run_preflight().stdout)
                link.unlink()
                link.symlink_to("target")
                subprocess.run(["git", "-C", str(repo), "config", "core.symlinks", "true"], check=True)

    def test_symlink_config_without_links_and_missing_with_links(self):
        for value in ("false", None):
            with self.subTest(value=value):
                if value is None:
                    subprocess.run(["git", "-C", str(self.upstream), "config", "--unset", "core.symlinks"], check=True)
                else:
                    subprocess.run(["git", "-C", str(self.upstream), "config", "core.symlinks", value], check=True)
                result = self.run_preflight()
                self.assertEqual(result.returncode, 0, result.stdout)
        link = self.upstream / "link"
        link.symlink_to("target")
        subprocess.run(["git", "-C", str(self.upstream), "add", link.name], check=True)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "link"], check=True)
        self.revision = subprocess.check_output(["git", "-C", str(self.upstream), "rev-parse", "HEAD"], text=True).strip()
        self.assertIn("unknown: upstream", self.run_preflight().stdout)
        subprocess.run(["git", "-C", str(self.upstream), "config", "core.symlinks", "invalid"], check=True)
        self.assertIn("unknown: upstream", self.run_preflight().stdout)
        subprocess.run(["git", "-C", str(self.upstream), "config", "core.symlinks", "true"], check=True)
        self.assertEqual(self.run_preflight().returncode, 0)

    def test_submodule_ignore_is_unknown(self):
        (self.root / "tracked").write_text("original")
        subprocess.run(["git", "-C", str(self.root), "add", "tracked"], check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "tracked"], check=True)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(self.root), "private-submodule"], check=True)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "submodule"], check=True)
        self.revision = subprocess.check_output(["git", "-C", str(self.upstream), "rev-parse", "HEAD"], text=True).strip()
        subprocess.run(["git", "-C", str(self.upstream), "config", "submodule.private-submodule.ignore", "all"], check=True)
        (self.upstream / "private-submodule" / "tracked").write_text("changed")
        self.assertEqual(subprocess.check_output(["git", "-C", str(self.upstream), "status", "--porcelain"]), b"")
        result = self.run_preflight()
        self.assertIn("unknown: upstream", result.stdout)
        self.assertNotIn("private-submodule", result.stdout + result.stderr)

    def test_configured_clean_filter_is_not_executed(self):
        marker = Path(self.tmp.name) / "filter-marker"
        (self.upstream / "tracked").write_text("original")
        (self.upstream / ".gitattributes").write_text("tracked filter=private\n")
        subprocess.run(["git", "-C", str(self.upstream), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "filter fixture"], check=True)
        self.revision = subprocess.check_output(["git", "-C", str(self.upstream), "rev-parse", "HEAD"], text=True).strip()
        (self.upstream / "tracked").write_text("changed")
        subprocess.run(["git", "-C", str(self.upstream), "config", "filter.private.clean", f"touch {marker}; cat"], check=True)
        result = self.run_preflight()
        self.assertIn("unknown: upstream", result.stdout)
        self.assertFalse(marker.exists())

    def test_deep_lock_is_unknown_without_traceback(self):
        (self.root / ".lane-lock.json").write_text('{"private-token":' + '[' * 2000 + '0' + ']' * 2000 + '}')
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown: lock", result.stdout)
        self.assertNotIn("Traceback", result.stdout + result.stderr)
        self.assertNotIn("private-token", result.stdout + result.stderr)

    def test_incomplete_selection_is_rejected(self):
        result = self.run_preflight("--ctest-regex", "private-regex")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("private-regex", result.stdout + result.stderr)

    # Delivery receipt drift (slice 1). The three classes must stay separate: a pin that HEAD
    # already contradicts is committed debt, a pin only the working tree contradicts is
    # temporary state, and a path absent from both is its own case. Each fixture builds its own
    # receipts instead of trusting this repository's current drift.

    def test_delivery_receipts_report_no_drift_when_every_pin_matches(self):
        (self.root / "pinned-one.txt").write_text("one")
        (self.root / "pinned-two.txt").write_text("two")
        self.write_manifest([{"ticket": "1101", "id": "1101-a", "inputs": [self.pinned("pinned-one.txt"), self.pinned("pinned-two.txt")]}])
        self.write_manifest([{"ticket": "1110", "id": "1110-a", "inputs": [self.pinned("pinned-two.txt")]}], name="999-extra-deliveries.json")
        self.stage("pinned-one.txt", "pinned-two.txt", str(DELIVERY_DIR))
        self.commit("receipts")
        result = self.run_preflight()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("pass: delivery receipts: 2 manifests, 2 records, 3 pinned inputs", result.stdout)
        self.assertIn("committed drift 0, uncommitted drift 0, missing 0", result.stdout)
        self.assertNotIn("drift:", result.stdout)

    def test_delivery_receipts_separate_committed_uncommitted_and_missing_drift(self):
        for name in ("committed.txt", "uncommitted.txt", "missing.txt"):
            (self.root / name).write_text(name)
        manifest = [
            {"ticket": "1101", "id": "1101-a", "inputs": [self.pinned("committed.txt")]},
            {"ticket": "1110", "id": "1110-a", "inputs": [self.pinned("uncommitted.txt")]},
            {"ticket": "1120", "id": "1120-a", "inputs": [self.pinned("missing.txt")]},
        ]
        self.write_manifest(manifest)
        self.stage("committed.txt", "uncommitted.txt", "missing.txt", str(DELIVERY_DIR))
        self.commit("receipts")
        (self.root / "committed.txt").write_text("changed")
        self.stage("committed.txt")
        self.commit("committed drift")
        (self.root / "uncommitted.txt").write_text("changed")
        subprocess.run(["git", "-C", str(self.root), "rm", "-q", "missing.txt"], check=True)
        self.commit("remove")
        (self.root / "untracked.txt").write_text("untracked")
        manifest.append({"ticket": "1130", "id": "1130-a", "inputs": [self.pinned("untracked.txt")]})
        self.write_manifest(manifest)
        self.stage(str(DELIVERY_DIR))
        self.commit("untracked pin")
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("committed drift 2, uncommitted drift 1, missing 1", result.stdout)
        self.assertIn("attention: delivery receipts: committed drift: 2 pinned inputs, tickets 1101 1130; paths: committed.txt, untracked.txt", result.stdout)
        self.assertIn("attention: delivery receipts: uncommitted drift: 1 pinned input, tickets 1110; paths: uncommitted.txt", result.stdout)
        self.assertIn("attention: delivery receipts: missing: 1 pinned input, tickets 1120; paths: missing.txt", result.stdout)

    def test_delivery_receipts_are_read_only_and_redact_unpinned_names(self):
        (self.root / "pinned.txt").write_text("pinned")
        self.write_manifest([{"ticket": "1101", "id": "1101-a", "inputs": [self.pinned("pinned.txt")]}])
        self.stage("pinned.txt", str(DELIVERY_DIR))
        self.commit("receipts")
        (self.root / "private-untracked-token").write_text("secret-content-token")
        manifest = self.root / DELIVERY_DIR / "1058-deliveries.json"
        before = ({path.relative_to(self.root) for path in self.root.rglob("*")}, manifest.read_bytes())
        result = self.run_preflight()
        after = ({path.relative_to(self.root) for path in self.root.rglob("*")}, manifest.read_bytes())
        self.assertEqual(before, after)
        self.assertNotIn("private-untracked-token", result.stdout + result.stderr)
        self.assertNotIn("secret-content-token", result.stdout + result.stderr)

    def test_delivery_receipts_do_not_follow_a_directory_symlink_out_of_the_checkout(self):
        outside = Path(self.tmp.name) / "outside"
        outside.mkdir()
        (outside / "pinned.txt").write_text("pinned")
        (self.root / "linked").symlink_to(outside)
        digest = hashlib.sha256((outside / "pinned.txt").read_bytes()).hexdigest()
        self.write_manifest([{"ticket": "1101", "id": "1101-a", "inputs": [{"path": "linked/pinned.txt", "sha256": digest}]}])
        self.stage(str(DELIVERY_DIR))
        self.commit("link pin")
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown: delivery receipts: 1 manifests, 1 records, 1 pinned inputs; committed drift 0, uncommitted drift 0, missing 0, unreadable 1", result.stdout)
        self.assertIn("unknown: delivery receipts: unreadable pinned inputs: 1 pinned input, tickets 1101; paths: linked/pinned.txt", result.stdout)

    def test_delivery_receipts_fail_closed_on_unsafe_manifests(self):
        (self.root / "pinned.txt").write_text("pinned")
        entry = self.pinned("pinned.txt")
        cases = {
            "truncated json": ("{\"schema\":", None),
            "wrong schema": (json.dumps({"schema": "other/1", "records": [{"ticket": "1", "inputs": [entry]}]}), None),
            "duplicate input key": (json.dumps({"schema": SCHEMA, "records": [{"ticket": "1", "inputs": [entry]}]}).replace('"path": "pinned.txt"', '"path": "pinned.txt", "path": "other.txt"'), None),
            "parent traversal": (None, [{"ticket": "1", "id": "1-a", "inputs": [{"path": "../outside-secret", "sha256": entry["sha256"]}]}]),
            "absolute path": (None, [{"ticket": "1", "id": "1-a", "inputs": [{"path": "/outside-secret", "sha256": entry["sha256"]}]}]),
            "missing digest": (None, [{"ticket": "1", "id": "1-a", "inputs": [{"path": "pinned.txt"}]}]),
        }
        for name, (raw, records) in cases.items():
            with self.subTest(name):
                self.write_manifest(records, raw=raw)
                result = self.run_preflight()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unknown: delivery receipts:", result.stdout)
                self.assertNotIn("Traceback", result.stdout + result.stderr)
                self.assertNotIn("outside-secret", result.stdout + result.stderr)

    def test_delivery_receipt_directory_absence_is_reported_not_assumed_clean(self):
        absent = self.run_preflight()
        self.assertEqual(absent.returncode, 0, absent.stdout)
        self.assertIn("pass: delivery receipts: no delivery directory", absent.stdout)
        (self.root / DELIVERY_DIR).mkdir(parents=True)
        (self.root / DELIVERY_DIR / "notes.md").write_text("no manifest here")
        self.stage(str(DELIVERY_DIR))
        self.commit("empty baseline directory")
        empty = self.run_preflight()
        self.assertNotEqual(empty.returncode, 0)
        self.assertIn("unknown: delivery receipts:", empty.stdout)

    def test_held_lock_without_lane_environment_is_reported(self):
        lock = self.root / ".lane-lock.json"
        lock.write_text(json.dumps({"lane": "lane-a", "agent": "test@example.invalid", "startedAt": "2026-11-09T00:00:00Z"}))
        unset = self.run_preflight()
        self.assertNotEqual(unset.returncode, 0)
        self.assertIn("attention: lane environment: CCH_LANE_NAME is unset while this lane holds the lock", unset.stdout)
        self.assertIn("with-lane.sh", unset.stdout)
        other = self.run_preflight(env={"CCH_LANE_NAME": "lane-b"})
        self.assertIn("attention: lane environment: CCH_LANE_NAME names another lane", other.stdout)
        match = self.run_preflight(env={"CCH_LANE_NAME": "lane-a"})
        self.assertEqual(match.returncode, 0, match.stdout)
        self.assertIn("pass: lane environment: CCH_LANE_NAME matches the held lane", match.stdout)


if __name__ == "__main__":
    unittest.main()
