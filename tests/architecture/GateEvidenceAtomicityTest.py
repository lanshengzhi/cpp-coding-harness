#!/usr/bin/env python3
"""Atomic publication of the build-phase Gate evidence (issue #734).

The build-phase Gate publishes two artifacts into the build directory, the
active-dependency evidence (`parity-build-gate-depfiles.json`) and the
machine-readable report (`parity-build-gate.json`). Those paths are shared:
the production build-gate CTest case and the `cmake --install` gate run the
same record+validate cycle against one build directory, concurrently under
`ctest -j`, and every cycle both rewrites the evidence and reads it back
(the Gate validator reads the depfile evidence; the install freshness check
reads the report and the depfile evidence).

An in-place truncate-then-write lets a concurrent reader observe a partially
written document; it then rejects its own freshly recorded evidence as
malformed (`PARITY-3003`) or stale, `cmake --install` aborts, and the staged
install test fails intermittently. Publication must therefore be atomic: a
reader observes either the previous complete document or the new one. This
test drives the production Gate entry point (`run-build-gate.cmake`, the same
command the production build-gate case and the install gate run) from several
processes at once and asserts that no reader ever observes a torn document.

Uses only the Python standard library and requires Python 3.12 or newer.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys
import threading
import time
from typing import Optional


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cmake", required=True, help="cmake executable")
    parser.add_argument("--source-dir", required=True, help="repository root")
    parser.add_argument("--build-dir", required=True, help="configured build directory")
    parser.add_argument(
        "--rounds", type=int, default=3, help="concurrent Gate rounds (default: 3)"
    )
    parser.add_argument(
        "--writers", type=int, default=4, help="Gate processes per round (default: 4)"
    )
    return parser.parse_args(argv)


class Reader:
    """Reads the published artifacts and records every partially written state.

    The read is exactly what the Gate's readers do with the artifact bytes
    (`json.load` over the file), so a torn document is observed here the same
    way the validator observes it as `PARITY-3003` and the freshness check as
    `invalid recorded depfile evidence`.
    """

    def __init__(self, paths: list[str]) -> None:
        self.paths = paths
        self.complete = 0
        self.torn: list[tuple[str, str]] = []
        self.missing = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "Reader":
        self._thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self._stop.set()
        self._thread.join()

    def _run(self) -> None:
        while not self._stop.is_set():
            for path in self.paths:
                try:
                    with open(path, encoding="utf-8") as handle:
                        json.load(handle)
                    self.complete += 1
                except FileNotFoundError:
                    # Only possible before a round publishes for the first
                    # time; absence is not a partially written document.
                    self.missing += 1
                    time.sleep(0.001)
                except (json.JSONDecodeError, UnicodeDecodeError, OSError) as error:
                    self.torn.append((path, str(error)))


def external_include_root(build_dir: str) -> str:
    """Return the vcpkg installed include root recorded by the build."""
    matches = sorted(glob.glob(os.path.join(build_dir, "vcpkg_installed", "*", "include")))
    if not matches:
        raise SystemExit(
            f"cannot find the vcpkg installed include root under '{build_dir}/vcpkg_installed'"
        )
    return matches[0]


def gate_command(args: argparse.Namespace, depfiles: str, report: str) -> list[str]:
    source_dir = os.path.abspath(args.source_dir)
    build_dir = os.path.abspath(args.build_dir)
    return [
        args.cmake,
        f"-DCCH_PARITY_GATE_SCRIPT={source_dir}/cmake/parity/parity_gate.py",
        f"-DCCH_PARITY_MANIFEST={source_dir}/cmake/parity/manifest.json",
        f"-DCCH_PARITY_INDEX={build_dir}/parity-ownership-index.json",
        f"-DCCH_PARITY_DIRECT_INCLUDES={build_dir}/parity-direct-includes.json",
        f"-DCCH_PARITY_COMPILE_COMMANDS={build_dir}/compile_commands.json",
        f"-DCCH_PARITY_PROJECT_ROOT={source_dir}",
        f"-DCCH_PARITY_DEPFILES={depfiles}",
        f"-DCCH_PARITY_REPORT={report}",
        f"-DCCH_PARITY_NINJA_DEPS={build_dir}/.ninja_deps",
        f"-DCCH_PARITY_EXTERNAL_INCLUDE_ROOTS={external_include_root(build_dir)}",
        "-P",
        f"{source_dir}/cmake/parity/run-build-gate.cmake",
    ]


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    source_dir = os.path.abspath(args.source_dir)
    build_dir = os.path.abspath(args.build_dir)
    depfiles = os.path.join(build_dir, "parity-gate-atomicity-depfiles.json")
    report = os.path.join(build_dir, "parity-gate-atomicity-report.json")

    required = [
        f"{source_dir}/cmake/parity/run-build-gate.cmake",
        f"{source_dir}/cmake/parity/parity_gate.py",
        f"{build_dir}/parity-ownership-index.json",
        f"{build_dir}/parity-direct-includes.json",
        f"{build_dir}/compile_commands.json",
        f"{build_dir}/.ninja_deps",
    ]
    for path in required:
        if not os.path.isfile(path):
            print(f"gate evidence atomicity: missing input '{path}'", file=sys.stderr)
            return 1

    command = gate_command(args, depfiles, report)
    failures: list[str] = []
    try:
        for round_index in range(args.rounds):
            with Reader([depfiles, report]) as reader:
                running = [
                    subprocess.Popen(
                        command,
                        cwd=build_dir,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                    )
                    for _ in range(args.writers)
                ]
                outputs = [process.communicate()[0] for process in running]
            for process, output in zip(running, outputs):
                if process.returncode != 0:
                    failures.append(
                        f"round {round_index}: a concurrent Gate process rejected its own "
                        f"evidence (exit {process.returncode}):\n{output.strip()}"
                    )
            for path, error in reader.torn:
                failures.append(
                    f"round {round_index}: observed a partially written "
                    f"'{os.path.basename(path)}': {error}"
                )
            if reader.complete < 2 * args.writers:
                failures.append(
                    f"round {round_index}: only {reader.complete} complete reads observed while "
                    f"{args.writers} Gate processes published the evidence"
                )
            print(
                f"gate evidence atomicity: round {round_index}: "
                f"{args.writers} writers, {reader.complete} complete reads, "
                f"{len(reader.torn)} torn reads, {reader.missing} absent"
            )
            if failures:
                break
    finally:
        for path in (depfiles, report):
            try:
                os.unlink(path)
            except OSError:
                pass

    if failures:
        for failure in failures:
            print(f"gate evidence atomicity: {failure}", file=sys.stderr)
        return 1
    print("gate evidence atomicity: PASS (no reader observed a partially written document)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
