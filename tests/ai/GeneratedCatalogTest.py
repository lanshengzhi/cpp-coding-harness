#!/usr/bin/env python3
"""Exercise the committed catalog generator without mutating the worktree."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts" / "ai" / "generate_default_models.py"
COMMITTED_HEADER = ROOT / "src" / "ai" / "DefaultModelsJson.hpp"
COMMITTED_SOURCE = ROOT / "src" / "ai" / "DefaultModelsJson.cpp"


def run_generator(header: Path, source: Path) -> None:
    subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--header",
            str(header),
            "--output",
            str(source),
        ],
        cwd=ROOT,
        check=True,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cch-generated-catalog-") as directory:
        temporary_root = Path(directory)
        first_header = temporary_root / "first.hpp"
        first_source = temporary_root / "first.cpp"
        second_header = temporary_root / "second.hpp"
        second_source = temporary_root / "second.cpp"
        run_generator(first_header, first_source)
        run_generator(second_header, second_source)
        if first_header.read_bytes() != second_header.read_bytes():
            raise AssertionError("catalog header regeneration is not byte-identical")
        if first_source.read_bytes() != second_source.read_bytes():
            raise AssertionError("catalog source regeneration is not byte-identical")
        if first_header.read_bytes() != COMMITTED_HEADER.read_bytes():
            raise AssertionError("committed catalog header is stale")
        if first_source.read_bytes() != COMMITTED_SOURCE.read_bytes():
            raise AssertionError("committed catalog source is stale")

    subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--check",
        ],
        cwd=ROOT,
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
