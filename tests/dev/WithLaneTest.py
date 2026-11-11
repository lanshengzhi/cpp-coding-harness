#!/usr/bin/env python3
"""Exercise the with-lane wrapper against the real lane-lock tool.

`scripts/dev/with-lane.sh` exists because two hand-rolled steps produced incidents on
2026-11-11: a lane acquired the checkout lock without exporting CCH_LANE_NAME, so the
consolidation cases it spawned were refused by the lane gate, and a lane finished without
releasing the lock, blocking the next lane for about 1.5 hours. The wrapper is driven here
through `sh` inside a throwaway Git repository, so a case can never leave a lock in the
project checkout. Each case asserts both halves of the contract: the command's own exit
status is propagated, and the lock is gone afterwards — on success, on failure, when the
command is killed by a signal, and when the wrapper itself is interrupted.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "scripts" / "dev" / "with-lane.sh"
LOCK_TOOL = ROOT / "scripts" / "dev" / "lane-lock.sh"
LANE_ENV_VARS = ("CCH_LANE_NAME", "CCH_LANE_LOCK_SKIP", "CCH_LANE_TASK_ID", "CCH_LANE_AGENT")


def clean_env(overrides: dict[str, str] | None = None) -> dict[str, str]:
    """The lane environment is set explicitly per call so an ambient lane cannot leak in."""
    env = {key: value for key, value in os.environ.items() if key not in LANE_ENV_VARS}
    return {**env, **(overrides or {})}


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


class WithLaneTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / "work"
        self.repo.mkdir()
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "-c", "user.name=test", "-c", "user.email=test@example.invalid",
             "commit", "-qm", "initial", "--allow-empty"],
            check=True,
        )
        self.lock = self.repo / ".lane-lock.json"

    def run_wrapper(self, *arguments: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["sh", str(WRAPPER), *arguments],
            cwd=self.repo,
            env=clean_env(env),
            text=True,
            capture_output=True,
            timeout=60,
        )

    def holder(self) -> str:
        return json.loads(self.lock.read_text(encoding="utf-8"))["lane"]

    def test_success_exports_the_lane_and_releases_the_lock(self) -> None:
        result = self.run_wrapper("lane-a", "--", "sh", "-c", 'printf "%s" "$CCH_LANE_NAME"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "lane-a")
        self.assertFalse(self.lock.exists())
        # The acquire happens before the command: the lock already names this lane while it runs.
        during = self.run_wrapper("lane-a", "--", "cat", str(self.lock))
        self.assertEqual(during.returncode, 0, during.stderr)
        self.assertEqual(json.loads(during.stdout)["lane"], "lane-a")
        self.assertFalse(self.lock.exists())

    def test_failure_propagates_the_exit_status_and_releases_the_lock(self) -> None:
        for command, status in (
            (["sh", "-c", 'test -f "$PWD/.lane-lock.json" && exit 7'], 7),
            (["with-lane-missing-command"], 127),
        ):
            with self.subTest(command=command):
                result = self.run_wrapper("lane-a", "--", *command)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertFalse(self.lock.exists())

    def test_signalled_command_propagates_the_signal_status_and_releases_the_lock(self) -> None:
        for sig, status in (("TERM", 143), ("INT", 130)):
            with self.subTest(signal=sig):
                result = self.run_wrapper("lane-a", "--", "sh", "-c", f"kill -s {sig} $$")
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertFalse(self.lock.exists())

    def test_interrupting_the_wrapper_releases_the_lock_and_stops_the_command(self) -> None:
        marker = Path(self.tmp.name) / "child.pid"
        for sig, status in ((signal.SIGHUP, 129), (signal.SIGTERM, 143), (signal.SIGINT, 130)):
            with self.subTest(signal=sig.name):
                process = subprocess.Popen(
                    ["sh", str(WRAPPER), "lane-a", "--", "sh", "-c", f'printf "%s" "$$" > "{marker}"; exec sleep 60'],
                    cwd=self.repo,
                    env=clean_env(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    start_new_session=True,
                )
                self.addCleanup(lambda fixture=process: fixture.poll() is None and fixture.kill())
                deadline = time.monotonic() + 30
                while not (self.lock.exists() and marker.exists()) and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertTrue(self.lock.exists() and marker.exists(), "wrapper never acquired the lock")
                child = int(marker.read_text())
                # A terminal sends the interrupt to the whole foreground process group.
                os.killpg(process.pid, sig)
                process.communicate(timeout=30)
                self.assertEqual(process.returncode, status)
                self.assertFalse(self.lock.exists())
                self.assertFalse(_alive(child), "the command outlived the interrupted wrapper")
                marker.unlink()

    def test_command_stdin_and_stdout_are_preserved(self) -> None:
        result = subprocess.run(
            ["sh", str(WRAPPER), "lane-a", "--", "cat"],
            cwd=self.repo,
            env=clean_env(),
            input="lane-stdin-token\n",
            text=True,
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "lane-stdin-token\n")
        self.assertFalse(self.lock.exists())

    def test_a_different_ambient_lane_is_refused_without_taking_the_lock(self) -> None:
        result = self.run_wrapper("lane-a", "--", "true", env={"CCH_LANE_NAME": "lane-b"})
        self.assertEqual(result.returncode, 2)
        self.assertIn("CCH_LANE_NAME already names a different lane", result.stderr)
        self.assertFalse(self.lock.exists())

    def test_a_foreign_lock_is_left_untouched(self) -> None:
        subprocess.run(["sh", str(LOCK_TOOL), "acquire", "lane-b"], cwd=self.repo, env=clean_env(), check=True, capture_output=True)
        result = self.run_wrapper("lane-a", "--", "true")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("lane-b", result.stderr)
        self.assertTrue(self.lock.exists())
        self.assertEqual(self.holder(), "lane-b")
        waited = self.run_wrapper("lane-a", "--timeout", "1", "--", "true")
        self.assertEqual(waited.returncode, 1, waited.stderr)
        self.assertIn("timed out after 1s", waited.stderr)
        self.assertEqual(self.holder(), "lane-b")

    def test_usage_errors_exit_two_without_taking_the_lock(self) -> None:
        for arguments in ((), ("lane-a",), ("lane-a", "true"), ("lane-a", "--timeout", "soon", "--", "true")):
            with self.subTest(arguments=arguments):
                result = self.run_wrapper(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(self.lock.exists())


if __name__ == "__main__":
    unittest.main()
