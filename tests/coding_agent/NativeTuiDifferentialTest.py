#!/usr/bin/env python3
"""CTest adapter for the deterministic dual-runtime Native TUI harness.

The pi checkout is an optional local comparison prerequisite. A checkout that
is absent is a skip, not a false pass; a checkout at the wrong commit is an
error because comparing a moving pi baseline would invalidate the evidence.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    source_dir = Path(os.environ.get("CCH_SOURCE_DIR", Path(__file__).parents[2]))
    checkout = Path(os.environ.get("PI_CHECKOUT", source_dir.parent / "pi"))
    tsx = checkout / "node_modules" / ".bin" / "tsx"
    if not checkout.is_dir() or not tsx.is_file():
        print(f"SKIP: frozen pi checkout/tsx is unavailable at {checkout}")
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
    return subprocess.call(
        [str(tsx), str(script), "--verify"],
        cwd=source_dir,
        env=environment,
    )


if __name__ == "__main__":
    sys.exit(main())
