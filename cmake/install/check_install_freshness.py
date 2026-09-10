#!/usr/bin/env python3
"""Install-time freshness check for the Parity Architecture Gate evidence.

The install path requires fresh successful Gate evidence (ADR 0039; issue
#472): the build-phase Gate report must record success, and the recorded
depfile evidence must still describe a build newer than every compiled source
and header prerequisite. Editing a source or header without rebuilding makes
the recorded depfile older than its prerequisite and fails the install, so
`cmake --install` cannot ship artifacts the current tree has not validated.

Python 3.12+ standard library only (same tooling policy as the Gate).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Optional, Sequence


def parse_makefile_depfile(text: str) -> list[str]:
    """Return the prerequisites of a compiler depfile's first (object) rule.

    Handles backslash line continuations and the Makefile escapes `\\ `,
    `\\#`, and `\\\\`; the phony per-header rules GCC appends carry no
    prerequisites and are ignored.
    """
    logical_lines: list[str] = []
    pending = ""
    for raw_line in text.splitlines():
        line = raw_line.rstrip("\n")
        if line.endswith("\\"):
            pending += line[:-1]
            continue
        logical_lines.append(pending + line)
        pending = ""
    if pending:
        logical_lines.append(pending)
    if not logical_lines:
        raise ValueError("depfile has no rules")

    rule = logical_lines[0]
    _target, separator, prerequisites_text = rule.partition(":")
    if not separator:
        raise ValueError("depfile's first rule has no ':' separator")

    prerequisites: list[str] = []
    token = ""
    index = 0
    while index < len(prerequisites_text):
        character = prerequisites_text[index]
        if character == "\\" and index + 1 < len(prerequisites_text):
            token += prerequisites_text[index + 1]
            index += 2
            continue
        if character in " \t":
            if token:
                prerequisites.append(token)
                token = ""
            index += 1
            continue
        token += character
        index += 1
    if token:
        prerequisites.append(token)
    return prerequisites


def _mtime_ns(path: str) -> int:
    return os.stat(path).st_mtime_ns


def _check_report(report_path: str, diagnostics: list[str]) -> None:
    if not os.path.isfile(report_path):
        diagnostics.append(f"missing successful Gate evidence: {report_path}")
        return
    try:
        with open(report_path, encoding="utf-8") as handle:
            report = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        diagnostics.append(f"invalid Gate evidence: {report_path}: {error}")
        return
    if not isinstance(report, dict) or report.get("ok") is not True:
        diagnostics.append(f"Gate evidence does not record success: {report_path}")


def _check_prerequisites(
    source: str,
    evidence_mtime: int,
    prerequisites: Sequence[str],
    build_dir: str,
    diagnostics: list[str],
) -> None:
    for prerequisite in prerequisites:
        prereq_path = (
            prerequisite
            if os.path.isabs(prerequisite)
            else os.path.join(build_dir, prerequisite)
        )
        if not os.path.exists(prereq_path):
            diagnostics.append(
                f"stale build products: prerequisite '{prerequisite}' for "
                f"'{source}' is missing; rebuild before install"
            )
        elif _mtime_ns(prereq_path) > evidence_mtime:
            diagnostics.append(
                f"stale build products: '{prerequisite}' is newer than the "
                f"recorded depfile for '{source}'; rebuild before install"
            )


def _check_depfiles(depfiles_path: str, diagnostics: list[str]) -> list[tuple[str, int]]:
    """Validate every recorded depfile against its prerequisites and return
    the (label, mtime_ns) pairs for the binary freshness comparison."""
    if not os.path.isfile(depfiles_path):
        diagnostics.append(f"missing recorded depfile evidence: {depfiles_path}")
        return []
    try:
        with open(depfiles_path, encoding="utf-8") as handle:
            evidence = json.load(handle)
        entries = evidence["entries"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        diagnostics.append(f"invalid recorded depfile evidence: {depfiles_path}: {error}")
        return []

    build_dir = os.path.dirname(os.path.abspath(depfiles_path))
    deps_log = evidence.get("deps_log")
    deps_log_mtime = None
    if isinstance(deps_log, str) and os.path.isfile(deps_log):
        deps_log_mtime = _mtime_ns(deps_log)

    depfile_pairs: list[tuple[str, int]] = []
    for entry in entries:
        source = entry.get("source", "<unknown>")
        depfile = entry.get("depfile")
        if depfile is not None:
            # Schema 1 fallback
            if not isinstance(depfile, str) or not os.path.isfile(depfile):
                diagnostics.append(f"missing depfile evidence for compiled source '{source}'")
                continue
            depfile_mtime = _mtime_ns(depfile)
            depfile_pairs.append((depfile, depfile_mtime))
            try:
                with open(depfile, encoding="utf-8") as handle:
                    prerequisites = parse_makefile_depfile(handle.read())
            except (OSError, ValueError) as error:
                diagnostics.append(f"unreadable depfile evidence for '{source}': {error}")
                continue
            _check_prerequisites(source, depfile_mtime, prerequisites, build_dir, diagnostics)
        else:
            # Schema 2 (.ninja_deps active dependency evidence)
            output = entry.get("output")
            dependencies = entry.get("dependencies")
            if dependencies is None or not isinstance(dependencies, list):
                diagnostics.append(f"missing depfile evidence for compiled source '{source}'")
                continue

            if not output:
                diagnostics.append(f"missing compiled object path for source '{source}'")
                continue
            out_path = os.path.join(build_dir, output) if not os.path.isabs(output) else output
            if not os.path.isfile(out_path):
                diagnostics.append(f"missing compiled object '{out_path}' for source '{source}'; rebuild before install")
                continue
            entry_mtime = _mtime_ns(out_path)
            depfile_pairs.append((output, entry_mtime))
            _check_prerequisites(source, entry_mtime, dependencies, build_dir, diagnostics)
    return depfile_pairs


def check_freshness(
    *,
    report_path: os.PathLike[str] | str,
    depfiles_path: os.PathLike[str] | str,
    binary_path: os.PathLike[str] | str,
) -> list[str]:
    """Return the sorted freshness violations for an install attempt."""
    report = str(report_path)
    depfiles = str(depfiles_path)
    binary = str(binary_path)
    diagnostics: list[str] = []
    _check_report(report, diagnostics)
    depfile_pairs = _check_depfiles(depfiles, diagnostics)
    if not os.path.isfile(binary):
        diagnostics.append(f"missing Runtime binary: {binary}")
    else:
        binary_mtime = _mtime_ns(binary)
        for label, depfile_mtime in depfile_pairs:
            if depfile_mtime > binary_mtime:
                diagnostics.append(
                    f"stale Runtime binary: '{binary}' is older than depfile "
                    f"'{label}'; rebuild before install"
                )
    return sorted(diagnostics)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--report", required=True, help="build-phase Gate report (parity-build-gate.json)")
    parser.add_argument(
        "--depfiles",
        required=True,
        help="recorded depfile evidence (parity-build-gate-depfiles.json)",
    )
    parser.add_argument("--binary", required=True, help="built Runtime binary")
    args = parser.parse_args(argv)

    diagnostics = check_freshness(
        report_path=args.report,
        depfiles_path=args.depfiles,
        binary_path=args.binary,
    )
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"install freshness: {diagnostic}")
        return 1
    print("install freshness: PASS (fresh successful Gate evidence)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
