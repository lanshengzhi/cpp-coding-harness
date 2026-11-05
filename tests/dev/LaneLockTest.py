#!/usr/bin/env python3
"""Behavioral regressions for the lane-lock concurrency mitigation (owner rule, 2026-11-05).

One code lane at a time may hold the main checkout (docs/wiki/pi-alignment/ops-lanes.md):
`scripts/dev/lane-lock.sh` gives a lane an atomic, self-identifying hold, and the
consolidation build/check entry points refuse to run under a foreign hold. The CLI cases
drive the real tool inside a throwaway git repository, so they cannot leave a lock in the
project checkout; the consolidation-gate cases hold the project lock only for the body of
the test and always remove it afterwards.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "dev" / "lane-lock.sh"
CONSOLIDATE = ROOT / "fixtures" / "pi-v1.3.0" / "consolidation-1058" / "consolidate_audits.py"
LOCK = ROOT / ".lane-lock.json"
STALE_SECONDS = 14400
LANE_ENV_VARS = ("CCH_LANE_TASK_ID", "CCH_LANE_AGENT", "CCH_LANE_NAME", "CCH_LANE_LOCK_SKIP")


def clean_env(overrides: dict[str, str] | None = None) -> dict[str, str]:
    """The lane environment is set explicitly per call so an ambient lane cannot leak in."""
    env = {key: value for key, value in os.environ.items() if key not in LANE_ENV_VARS}
    return {**env, **(overrides or {})}


def run_lock(repo: Path, *arguments: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["sh", str(SCRIPT), *arguments],
        cwd=repo,
        env=clean_env(env),
        text=True,
        capture_output=True,
    )


def run_consolidate(env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CONSOLIDATE), "check"],
        cwd=ROOT,
        env=clean_env(env),
        text=True,
        capture_output=True,
    )


def init_repo(directory: Path) -> Path:
    subprocess.run(["git", "init", "-q", str(directory)], check=True)
    return directory


def backdate(lock_file: Path, seconds: int) -> None:
    """Rewrite startedAt to `seconds` ago, keeping the machine-written single-line shape."""
    lock = json.loads(lock_file.read_text(encoding="utf-8"))
    started = datetime.now(timezone.utc) - timedelta(seconds=seconds)
    lock["startedAt"] = started.strftime("%Y-%m-%dT%H:%M:%SZ")
    lock_file.write_text(json.dumps(lock, separators=(",", ":")) + "\n", encoding="utf-8")


class LaneLockCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = init_repo(Path(self._tmp.name))

    def lock_file(self) -> Path:
        return self.repo / ".lane-lock.json"

    def read_lock(self) -> dict:
        return json.loads(self.lock_file().read_text(encoding="utf-8"))

    def test_acquire_records_the_holding_lane_and_release_frees_it(self) -> None:
        result = run_lock(self.repo, "acquire", "lane-a", env={"CCH_LANE_TASK_ID": "#1192"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("acquired: lane 'lane-a'", result.stdout)
        lock = self.read_lock()
        self.assertEqual(lock["lane"], "lane-a")
        self.assertEqual(lock["taskId"], "#1192")
        self.assertTrue(lock["agent"])
        datetime.strptime(lock["startedAt"], "%Y-%m-%dT%H:%M:%SZ")

        status = run_lock(self.repo, "status")
        self.assertEqual(status.returncode, 1, status.stdout)
        self.assertIn("held by lane 'lane-a'", status.stdout)
        self.assertIn("stale=no", status.stdout)

        release = run_lock(self.repo, "release", "lane-a")
        self.assertEqual(release.returncode, 0, release.stderr)
        self.assertFalse(self.lock_file().exists())
        status = run_lock(self.repo, "status")
        self.assertEqual(status.returncode, 0, status.stdout)
        self.assertIn("free", status.stdout)

    def test_acquire_omits_task_id_when_the_environment_sets_none(self) -> None:
        result = run_lock(self.repo, "acquire", "lane-a")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("taskId", self.read_lock())

    def test_reacquire_by_the_holding_lane_is_a_successful_no_op(self) -> None:
        first = run_lock(self.repo, "acquire", "lane-a")
        self.assertEqual(first.returncode, 0, first.stderr)
        started_at = self.read_lock()["startedAt"]
        again = run_lock(self.repo, "acquire", "lane-a")
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn("already held by this lane", again.stdout)
        self.assertEqual(self.read_lock()["startedAt"], started_at)

    def test_a_second_lane_is_refused_with_the_holder_named(self) -> None:
        self.assertEqual(run_lock(self.repo, "acquire", "lane-a").returncode, 0)
        refused = run_lock(self.repo, "acquire", "lane-b")
        self.assertEqual(refused.returncode, 1)
        self.assertIn("lock held by lane 'lane-a'", refused.stderr)
        self.assertEqual(self.read_lock()["lane"], "lane-a")

    def test_acquire_waits_up_to_the_timeout_for_a_foreign_lock(self) -> None:
        self.assertEqual(run_lock(self.repo, "acquire", "lane-a").returncode, 0)
        started = time.monotonic()
        refused = run_lock(self.repo, "acquire", "lane-b", "1")
        waited = time.monotonic() - started
        self.assertEqual(refused.returncode, 1)
        self.assertIn("timed out after 1s", refused.stderr)
        self.assertGreaterEqual(waited, 1.0)
        self.assertLess(waited, 10.0)
        self.assertEqual(self.read_lock()["lane"], "lane-a")

    def test_release_by_a_non_holder_is_refused_fail_closed(self) -> None:
        self.assertEqual(run_lock(self.repo, "acquire", "lane-a").returncode, 0)
        refused = run_lock(self.repo, "release", "lane-b")
        self.assertEqual(refused.returncode, 1)
        self.assertIn("fail-closed", refused.stderr)
        self.assertIn("lane 'lane-a'", refused.stderr)
        self.assertTrue(self.lock_file().exists())
        self.assertEqual(self.read_lock()["lane"], "lane-a")

    def test_release_of_a_free_lock_is_a_successful_no_op(self) -> None:
        result = run_lock(self.repo, "release", "lane-a")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nothing to release", result.stdout)

    def test_a_stale_lock_is_taken_over_and_status_reports_staleness(self) -> None:
        self.assertEqual(run_lock(self.repo, "acquire", "lane-a").returncode, 0)
        backdate(self.lock_file(), STALE_SECONDS + 600)

        status = run_lock(self.repo, "status")
        self.assertEqual(status.returncode, 1, status.stdout)
        self.assertIn("stale=yes", status.stdout)

        takeover = run_lock(self.repo, "acquire", "lane-b")
        self.assertEqual(takeover.returncode, 0, takeover.stderr)
        self.assertIn("taking over stale lock", takeover.stderr)
        self.assertEqual(self.read_lock()["lane"], "lane-b")

    def test_an_unparsable_lock_reads_as_fresh_never_stale(self) -> None:
        self.lock_file().write_text(
            '{"lane":"lane-a","agent":"ghost","startedAt":"garbage"}\n', encoding="utf-8"
        )
        status = run_lock(self.repo, "status")
        self.assertEqual(status.returncode, 1, status.stdout)
        self.assertIn("stale=unknown", status.stdout)
        refused = run_lock(self.repo, "acquire", "lane-b")
        self.assertEqual(refused.returncode, 1)
        self.assertIn("lock held by lane 'lane-a'", refused.stderr)

    def test_help_lists_the_commands_and_no_arguments_is_an_error(self) -> None:
        help_result = run_lock(self.repo, "--help")
        self.assertEqual(help_result.returncode, 0)
        for command in ("acquire", "release", "status"):
            self.assertIn(command, help_result.stdout)
        self.assertIn("CCH_LANE_LOCK_SKIP", help_result.stdout)

        empty = run_lock(self.repo)
        self.assertEqual(empty.returncode, 2)
        self.assertIn("Usage", empty.stderr)

    def test_lane_names_are_restricted_to_identifier_characters(self) -> None:
        for bad in ('bad"lane', "bad\\lane", "bad/lane", "bad lane", ""):
            result = run_lock(self.repo, "acquire", bad)
            self.assertEqual(result.returncode, 1, bad)
            self.assertIn("invalid lane name", result.stderr)
        self.assertFalse(self.lock_file().exists())


class ConsolidationLaneGateTest(unittest.TestCase):
    """The build/check CLI refuses a foreign lane lock in the project checkout.

    These tests create the project lock themselves and always remove it; when a real lane
    already holds the checkout they skip rather than touch a lock they do not own.
    """

    def setUp(self) -> None:
        if LOCK.exists():
            self.skipTest(f"{LOCK.name} already exists; a real lane may be in flight")
        self._created = False

    def tearDown(self) -> None:
        if self._created:
            LOCK.unlink(missing_ok=True)

    def acquire_project_lock(self, lane: str) -> None:
        result = run_lock(ROOT, "acquire", lane, env={"CCH_LANE_TASK_ID": "#1192"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self._created = True

    def test_the_holding_lane_runs_check_while_foreign_lanes_are_refused(self) -> None:
        self.acquire_project_lock("lane-a")
        holder = run_consolidate({"CCH_LANE_NAME": "lane-a"})
        self.assertEqual(holder.returncode, 0, holder.stderr)
        self.assertIn("consolidation check: PASS", holder.stdout)

        foreign = run_consolidate({"CCH_LANE_NAME": "lane-b"})
        self.assertEqual(foreign.returncode, 1)
        self.assertIn("consolidation check: FAIL", foreign.stderr)
        self.assertIn("lane lock held by 'lane-a'", foreign.stderr)
        self.assertIn("CCH_LANE_LOCK_SKIP", foreign.stderr)

        unnamed = run_consolidate({})
        self.assertEqual(unnamed.returncode, 1)
        self.assertIn("'unset'", unnamed.stderr)

    def test_cch_lane_lock_skip_lets_the_owning_lane_bypass_the_gate(self) -> None:
        self.acquire_project_lock("lane-a")
        skipped = run_consolidate({"CCH_LANE_NAME": "lane-b", "CCH_LANE_LOCK_SKIP": "1"})
        self.assertEqual(skipped.returncode, 0, skipped.stderr)
        self.assertIn("consolidation check: PASS", skipped.stdout)


if __name__ == "__main__":
    unittest.main()
