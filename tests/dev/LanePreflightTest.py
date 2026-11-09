#!/usr/bin/env python3
"""Exercise preflight through its CLI in disposable repositories."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/dev/lane-preflight.py"


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
        return subprocess.run([sys.executable, str(SCRIPT), "--lane", "lane-a", "--revision", self.revision, "--checkout", str(self.upstream), *args], cwd=self.root, capture_output=True, text=True, env=env)

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
        result = self.run_preflight()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("lock: matching", result.stdout)
        self.assertNotIn("sensitive-token", result.stdout + result.stderr)
        subprocess.run(["git", "-C", str(self.upstream), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-qm", "next", "--allow-empty"], check=True)
        self.assertIn("revision mismatch", self.run_preflight().stdout)

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


if __name__ == "__main__":
    unittest.main()
