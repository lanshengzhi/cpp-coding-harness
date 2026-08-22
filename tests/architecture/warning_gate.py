#!/usr/bin/env python3
"""Fail-closed zero-compiler-warning build gate (issue #492).

The project compiles every owned target with `-Wall -Wextra -Wpedantic`
without warnings-as-errors (CODING_STANDARDS.md §14: compiler diagnostics
remain review findings). This gate makes that requirement enforceable: it
recompiles every project-owned compile command from the generated
compile_commands.json with the project's own flags plus `-Werror` (and
`-fsyntax-only`, so nothing is written), failing closed on any diagnostic in
the production or test source graph.

A warning anywhere in the project graph — production `src/` or test `tests/`
— fails the gate with the offending file, line, and message. Non-project
sources (for example a vcpkg installed include directory living under the
build tree) are excluded by `--exclude-root`, matching how the Parity
Architecture Gate treats external include roots.

Uses only the Python standard library and requires Python 3.12 or newer.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor


def load_compile_commands(path: str) -> list[dict]:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, list):
        raise SystemExit("compile-commands evidence must be a JSON list")
    return data


def source_path(entry: dict) -> str:
    """Return the compilation database source path resolved from its directory."""
    file = entry.get("file", "")
    directory = entry.get("directory", "")
    if not isinstance(file, str) or not file:
        return ""
    if not isinstance(directory, str):
        return ""
    if os.path.isabs(file):
        return file
    return os.path.join(directory, file)


def compile_arguments(entry: dict) -> list[str]:
    """Extract compiler arguments from either spelling in the Clang format."""
    if "arguments" in entry:
        arguments = entry["arguments"]
        if not isinstance(arguments, list) or not all(
            isinstance(argument, str) for argument in arguments
        ):
            raise ValueError("arguments must be a list of strings")
        if not arguments:
            raise ValueError("arguments must not be empty")
        return list(arguments)

    command = entry.get("command")
    if not isinstance(command, str) or not command:
        raise ValueError("entry must contain command or arguments")
    try:
        arguments = shlex.split(command)
    except ValueError as exc:
        raise ValueError(f"command is not valid shell-escaped text: {exc}") from exc
    if not arguments:
        raise ValueError("command must not be empty")
    return arguments


def is_project_source(entry: dict, project_root: str, exclude_roots: list[str]) -> bool:
    """True when the entry's source file lives under the project root and not
    under an excluded root (for example the vcpkg installed include dir)."""
    source = source_path(entry)
    if not source:
        return False
    real = os.path.realpath(source)
    root = os.path.realpath(project_root)
    if real != root and not real.startswith(root + os.sep):
        return False
    for excluded in exclude_roots:
        real_excluded = os.path.realpath(excluded)
        if real == real_excluded or real.startswith(real_excluded + os.sep):
            return False
    return True


def check_entry(entry: dict, ccache: bool) -> tuple[str, str]:
    """Recompile one entry with -Werror; return (source, diagnostic-or-empty)."""
    source = source_path(entry)
    directory = entry.get("directory", "")
    try:
        invocation = compile_arguments(entry)
    except ValueError as exc:
        return source, f"invalid compile command: {exc}"
    # -Werror turns the project's -Wall -Wextra -Wpedantic profile into
    # errors; -fsyntax-only prevents any output from being written. ccache
    # (when available) keeps repeat runs fast. Passing argv directly avoids
    # reparsing arguments-list entries through a shell.
    if ccache:
        invocation.insert(0, "ccache")
    invocation.extend(("-Werror", "-fsyntax-only"))
    try:
        result = subprocess.run(
            invocation, cwd=directory, capture_output=True, text=True
        )
    except (OSError, TypeError) as exc:
        return source, f"could not run compile command: {exc}"
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        return source, diagnostic or "compile command failed"
    return source, ""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fail-closed zero-compiler-warning gate (issue #492)"
    )
    parser.add_argument(
        "--compile-commands",
        required=True,
        help="Path to the generated compile_commands.json",
    )
    parser.add_argument(
        "--project-root",
        required=True,
        help="Project root against which sources resolve",
    )
    parser.add_argument(
        "--exclude-root",
        action="append",
        default=[],
        help="Root to exclude from the project graph (repeatable)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=os.cpu_count() or 4,
        help="Parallel compile workers (default: CPU count)",
    )
    parser.add_argument(
        "--no-ccache",
        action="store_true",
        help="Disable the ccache compiler wrapper even when ccache is on PATH",
    )
    args = parser.parse_args(argv)

    ccache = False
    if not args.no_ccache:
        # ccache is an optional speedup: the gate must degrade to direct
        # compiler invocations when the launcher is not installed (issue
        # #520) instead of crashing on the probe.
        try:
            probe = subprocess.run(
                ["ccache", "--version"], capture_output=True, text=True
            )
            ccache = probe.returncode == 0
        except OSError:
            ccache = False

    commands = load_compile_commands(args.compile_commands)
    entries = [
        entry
        for entry in commands
        if is_project_source(entry, args.project_root, args.exclude_root)
    ]

    failures: list[tuple[str, str]] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for source, diagnostic in pool.map(
            lambda entry: check_entry(entry, ccache), entries
        ):
            if diagnostic:
                failures.append((source, diagnostic))

    print(
        f"warning gate: {len(entries)} project compile commands recompiled with "
        f"-Werror; {len(failures)} failed"
    )
    if failures:
        print("warning gate FAILED (project sources compile with warnings):")
        for source, diagnostic in failures:
            print(f"  {source}:")
            print(diagnostic)
        return 1
    print("warning gate: PASS (zero compiler warnings in the project graph)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
