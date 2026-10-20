#!/usr/bin/env python3
"""Kill identity lookup regression for the Pike PTY helper (#1044).

`cmd_spawn` records `broker_starttime` and the broker records
`child_starttime`. `cmd_kill` must pair each recorded start time with its pid.
Looking up `<pidkey>_starttime` instead asks for `broker_pid_starttime`, which no
record carries: `open_verified(pid, None)` answers "unidentified", and every
valid session exits 4 without being cleaned up.

These cases drive the real `cmd_kill` against real state files and real
processes. Only the OS signal boundary and the broker socket are captured, so
the identity arguments and the absence of a signal stay observable while every
process under test is owned by the test itself.
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / ".cursor/skills/verify-pike/helpers/pike_tui.py"


def load_helper():
    spec = importlib.util.spec_from_file_location("pike_tui_under_test", HELPER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pike_tui = load_helper()


class PikeTuiHelperKillTest(unittest.TestCase):
    def setUp(self):
        self._run_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self._run_dir.cleanup)
        previous_run_dir = pike_tui.RUN_DIR
        pike_tui.RUN_DIR = Path(self._run_dir.name)
        self.addCleanup(lambda: setattr(pike_tui, "RUN_DIR", previous_run_dir))
        self._processes = []
        self.addCleanup(self._stop_processes)

    def _stop_processes(self):
        for process in self._processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            else:
                process.wait()

    def _spawn_sleeper(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._processes.append(process)
        return process

    def _starttime(self, pid):
        starttime = pike_tui.process_starttime(pid)
        self.assertIsNotNone(starttime)
        return starttime

    def _spawn_sleeper_with_different_starttime(self, previous):
        # Distinct ticks keep the child assertion from passing if the broker's
        # start time is reused for both pids, which an equal-tick pair would hide.
        for _ in range(50):
            time.sleep(0.02)
            process = self._spawn_sleeper()
            starttime = self._starttime(process.pid)
            if starttime != previous:
                return process, starttime
        self.fail("could not obtain distinct kernel start times")

    def _publish(self, name, state):
        directory = pike_tui.session_dir(name)
        (directory / "state.json").write_text(json.dumps(state))
        Path(pike_tui.sock_path(name, state["token"])).write_text("")
        return directory

    @contextlib.contextmanager
    def _captured(self):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            yield stdout, stderr

    @contextlib.contextmanager
    def _kill_boundary(self, broker_response):
        opened = []
        signalled = []
        events = []
        real_open_verified = pike_tui.open_verified

        def capture_open_verified(pid, starttime):
            fd, reason = real_open_verified(pid, starttime)
            opened.append((pid, starttime, reason, fd))
            events.append(("open", pid))
            return fd, reason

        def record_broker_request(name, payload):
            events.append(("request", payload["token"]))
            return broker_response

        def record_signal(fd, sent):
            signalled.append((fd, sent))
            fd_pid = {handle: pid for pid, _, _, handle in opened
                      if handle is not None}
            events.append(("signal", fd_pid[fd]))

        broker_request = mock.Mock(side_effect=record_broker_request)
        with mock.patch.object(pike_tui, "open_verified", side_effect=capture_open_verified), \
                mock.patch.object(pike_tui, "broker_request", broker_request), \
                mock.patch.object(pike_tui, "pid_alive", return_value=False), \
                mock.patch.object(pike_tui.signal, "pidfd_send_signal",
                                  side_effect=record_signal):
            yield opened, signalled, events, broker_request

    def test_valid_record_checks_broker_and_child_recorded_starttimes(self):
        broker = self._spawn_sleeper()
        broker_starttime = self._starttime(broker.pid)
        child, child_starttime = self._spawn_sleeper_with_different_starttime(
            broker_starttime)
        token = "3155d626d4da438e92127d7c3ca42e2b"
        state = {
            "broker_pid": broker.pid,
            "broker_starttime": broker_starttime,
            "child_pid": child.pid,
            "child_starttime": child_starttime,
            "token": token,
        }
        directory = self._publish("valid", state)

        response = {"ok": False, "error": "not the session the caller resolved"}
        with self._kill_boundary(response) as (opened, signalled, events, request):
            with self._captured() as (stdout, _):
                pike_tui.cmd_kill(argparse.Namespace(name="valid"))

        self.assertEqual(
            events,
            [("open", broker.pid), ("open", child.pid), ("request", token),
             ("signal", broker.pid), ("signal", child.pid)],
        )
        self.assertEqual(
            [(pid, starttime, reason) for pid, starttime, reason, _ in opened],
            [(broker.pid, broker_starttime, None),
             (child.pid, child_starttime, None)],
        )
        request.assert_called_once_with("valid", {"op": "kill", "token": token})
        fd_pid = {fd: pid for pid, _, _, fd in opened}
        self.assertEqual(
            [(fd_pid[fd], sent) for fd, sent in signalled],
            [(broker.pid, signal.SIGKILL), (child.pid, signal.SIGKILL)],
        )
        self.assertIn("killed valid", stdout.getvalue())
        self.assertFalse((directory / "state.json").exists())

    def test_missing_or_unknown_starttime_exits_4_without_signalling(self):
        broker = self._spawn_sleeper()
        child = self._spawn_sleeper()
        token = "b7f8c8b9a65a4f5f9d5c800ef1f7c355"
        valid = {
            "broker_pid": broker.pid,
            "broker_starttime": self._starttime(broker.pid),
            "child_pid": child.pid,
            "child_starttime": self._starttime(child.pid),
            "token": token,
        }
        cases = (
            ("missing-broker-starttime", "broker_starttime", "missing"),
            ("unknown-child-starttime", "child_starttime", "unknown"),
        )
        for name, field, kind in cases:
            with self.subTest(name=name):
                state = dict(valid)
                if kind == "missing":
                    del state[field]
                else:
                    state[field] = None
                directory = self._publish(name, state)

                broker_request = mock.Mock()
                send_signal = mock.Mock()
                with mock.patch.object(pike_tui, "broker_request", broker_request), \
                        mock.patch.object(pike_tui.signal, "pidfd_send_signal", send_signal):
                    with self._captured() as (_, stderr):
                        with self.assertRaises(SystemExit) as raised:
                            pike_tui.cmd_kill(argparse.Namespace(name=name))

                self.assertEqual(raised.exception.code, 4)
                broker_request.assert_not_called()
                send_signal.assert_not_called()
                self.assertIsNone(broker.poll())
                self.assertIsNone(child.poll())
                self.assertTrue((directory / "state.json").exists())
                self.assertTrue(Path(pike_tui.sock_path(name, token)).exists())
                self.assertIn("cannot confirm the process recorded", stderr.getvalue())

    def test_reused_recorded_pid_is_not_signalled(self):
        broker = self._spawn_sleeper()
        token = "c8cc7b21f6d04b3c8f8f0d7e2a1b4c66"
        recorded_starttime = self._starttime(broker.pid) + 1
        directory = self._publish("reused", {
            "broker_pid": broker.pid,
            "broker_starttime": recorded_starttime,
            "token": token,
        })

        response = {"ok": False, "error": "not the session the caller resolved"}
        with self._kill_boundary(response) as (opened, signalled, _, request):
            with self._captured() as (stdout, _):
                pike_tui.cmd_kill(argparse.Namespace(name="reused"))

        self.assertEqual(
            [(pid, starttime, reason) for pid, starttime, reason, _ in opened],
            [(broker.pid, recorded_starttime, "reused")],
        )
        self.assertEqual(signalled, [])
        self.assertIsNone(broker.poll())
        request.assert_called_once_with("reused", {"op": "kill", "token": token})
        self.assertIn("killed reused", stdout.getvalue())
        self.assertFalse((directory / "state.json").exists())


if __name__ == "__main__":
    unittest.main()
