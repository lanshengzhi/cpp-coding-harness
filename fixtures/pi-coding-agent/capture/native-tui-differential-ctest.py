#!/usr/bin/env python3
"""CTest adapter for the deterministic dual-runtime Native TUI harness.

The pi checkout is an optional local comparison prerequisite. A checkout that
is absent is a skip, not a false pass. The checkout's own HEAD is the
comparison target (ADR 0053 treats pi as reference material, not a
specification); `PI_BASELINE_COMMIT` re-asserts one exact revision and turns
a drifted checkout into an error for reproducible runs.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    source_dir = Path(os.environ.get("CCH_SOURCE_DIR", Path(__file__).parents[3]))
    checkout = Path(os.environ.get("PI_CHECKOUT", source_dir.parent / "pi"))
    if not checkout.is_dir():
        print(f"SKIP: pi checkout is unavailable at {checkout}")
        return 77
    # The capture script resolves its own TypeScript runner; it only needs one
    # to exist somewhere (checkout, CCH_DIFFERENTIAL_TSX, or PATH), so this
    # adapter does not require pi to declare tsx.
    runner = os.environ.get("CCH_DIFFERENTIAL_TSX") or str(checkout / "node_modules" / ".bin" / "tsx")
    if not Path(runner).is_file() and shutil.which("tsx") is None:
        print(f"SKIP: no TypeScript runner (tsx) available for the pi capture at {checkout}")
        return 77

    script = source_dir / "fixtures/pi-coding-agent/capture/native-tui-differential.mts"
    binary = os.environ.get(
        "CCH_DIFFERENTIAL_BINARY",
        str(source_dir / "build/cch_tests_coding_agent_interactive"),
    )
    environment = os.environ.copy()
    environment["CCH_SOURCE_DIR"] = str(source_dir)
    environment["PI_CHECKOUT"] = str(checkout)
    environment["CCH_DIFFERENTIAL_BINARY"] = binary
    baseline_commit = os.environ.get("PI_BASELINE_COMMIT")
    if baseline_commit:
        environment["PI_BASELINE_COMMIT"] = baseline_commit
    runner_environment = "CCH_DIFFERENTIAL_TSX"
    if runner_environment not in environment and Path(runner).is_file():
        environment[runner_environment] = str(runner)
    elif shutil.which("tsx") is not None:
        environment.pop(runner_environment, None)
    return subprocess.call(
        [str(runner), str(script), "--verify"],
        cwd=source_dir,
        env=environment,
    )


if __name__ == "__main__":
    sys.exit(main())
