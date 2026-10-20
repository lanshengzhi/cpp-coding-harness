#!/usr/bin/env python3
"""Install-time freshness check for the Parity Architecture Gate evidence.

The install path requires fresh successful Gate evidence (ADR 0039; issue
#472): the build-phase Gate report must record success, and the recorded
depfile evidence must still describe a build newer than every compiled source
and header prerequisite. Editing a source or header without rebuilding makes
the recorded depfile older than its prerequisite and fails the install, so
`cmake --install` cannot ship artifacts the current tree has not validated.

The Runtime binary is only compared against its own link closure: the objects
`ninja -t inputs <target>` reports for the target are the ones the binary is
linked from, so an object outside that closure -- an Owner library the Runtime
does not link -- can never make a fresh binary look stale (issue #1044). The
closure is read from the generator's graph at gate time, so it follows the
packages that are added to or removed from the link line.

Python 3.12+ standard library only (same tooling policy as the Gate).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from typing import Optional, Sequence


class LinkClosureError(RuntimeError):
    """The Runtime's link closure could not be read from the build graph."""


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


def runtime_link_closure(*, ninja: str, build_dir: str, target: str) -> list[str]:
    """Return the object files the target is linked from, read from the
    generator's own build graph.

    `ninja -t inputs` reports the transitive inputs of the target's rule: its
    own object plus the objects of every static library on its link line.
    Reading the graph keeps the closure correct when Owner packages are added
    to or removed from that link line, which a recorded list or a package-name
    filter would not. The reported paths are relative to build_dir, like the
    recorded evidence paths. Any failure to read the graph raises
    LinkClosureError, so an unverifiable closure fails the install instead of
    silently narrowing the check.
    """
    command = [ninja, "-C", build_dir, "-t", "inputs", target]
    invocation = " ".join(command)
    try:
        completed = subprocess.run(
            command, capture_output=True, text=True, check=False
        )
    except OSError as error:
        raise LinkClosureError(
            f"cannot determine the Runtime link closure: '{invocation}': {error}"
        ) from error
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise LinkClosureError(
            f"cannot determine the Runtime link closure: '{invocation}' failed: {detail}"
        )
    objects = sorted(
        {
            os.path.normpath(line.strip())
            for line in completed.stdout.splitlines()
            if line.strip().endswith(".o")
        }
    )
    if not objects:
        raise LinkClosureError(
            f"cannot determine the Runtime link closure: '{invocation}' reported no "
            f"compiled input for '{target}'"
        )
    return objects


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


def _check_depfiles(depfiles_path: str, diagnostics: list[str]) -> None:
    """Validate every recorded depfile against its prerequisites."""
    if not os.path.isfile(depfiles_path):
        diagnostics.append(f"missing recorded depfile evidence: {depfiles_path}")
        return
    try:
        with open(depfiles_path, encoding="utf-8") as handle:
            evidence = json.load(handle)
        entries = evidence["entries"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        diagnostics.append(f"invalid recorded depfile evidence: {depfiles_path}: {error}")
        return

    build_dir = os.path.dirname(os.path.abspath(depfiles_path))
    deps_log = evidence.get("deps_log")
    deps_log_mtime = None
    if isinstance(deps_log, str) and os.path.isfile(deps_log):
        deps_log_mtime = _mtime_ns(deps_log)

    for entry in entries:
        source = entry.get("source", "<unknown>")
        depfile = entry.get("depfile")
        if depfile is not None:
            # Schema 1 fallback
            if not isinstance(depfile, str) or not os.path.isfile(depfile):
                diagnostics.append(f"missing depfile evidence for compiled source '{source}'")
                continue
            depfile_mtime = _mtime_ns(depfile)
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
            _check_prerequisites(source, entry_mtime, dependencies, build_dir, diagnostics)


def _check_link_closure(
    link_closure: Sequence[str],
    binary_mtime: int,
    build_dir: str,
    binary: str,
    diagnostics: list[str],
) -> None:
    for object_path in link_closure:
        object_file = (
            object_path
            if os.path.isabs(object_path)
            else os.path.join(build_dir, object_path)
        )
        if not os.path.exists(object_file):
            diagnostics.append(
                f"missing link-closure object '{object_path}' for the Runtime "
                f"binary '{binary}'; rebuild before install"
            )
        elif _mtime_ns(object_file) > binary_mtime:
            diagnostics.append(
                f"stale Runtime binary: '{binary}' is older than link-closure "
                f"object '{object_path}'; rebuild before install"
            )


def check_freshness(
    *,
    report_path: os.PathLike[str] | str,
    depfiles_path: os.PathLike[str] | str,
    binary_path: os.PathLike[str] | str,
    link_closure: Sequence[str],
) -> list[str]:
    """Return the sorted freshness violations for an install attempt.

    link_closure names the object files the Runtime binary is linked from, as
    reported by runtime_link_closure; only those objects date the binary. Both
    the closure and the recorded evidence name paths relative to the build
    directory, which is the directory holding the evidence file.
    """
    report = str(report_path)
    depfiles = str(depfiles_path)
    binary = str(binary_path)
    diagnostics: list[str] = []
    _check_report(report, diagnostics)
    _check_depfiles(depfiles, diagnostics)
    if not os.path.isfile(binary):
        diagnostics.append(f"missing Runtime binary: {binary}")
    else:
        _check_link_closure(
            link_closure,
            _mtime_ns(binary),
            os.path.dirname(os.path.abspath(depfiles)),
            binary,
            diagnostics,
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
    parser.add_argument("--ninja", required=True, help="Ninja executable that generated the build")
    parser.add_argument("--target", required=True, help="Runtime target name in the build graph")
    args = parser.parse_args(argv)

    diagnostics: list[str] = []
    try:
        link_closure = runtime_link_closure(
            ninja=args.ninja,
            build_dir=os.path.dirname(os.path.abspath(args.depfiles)),
            target=args.target,
        )
    except LinkClosureError as error:
        # An unreadable link closure fails closed: without it the Runtime's own
        # objects cannot be dated, so the install must not proceed. The other
        # checks still run and report alongside it.
        diagnostics.append(str(error))
        link_closure = []

    diagnostics.extend(
        check_freshness(
            report_path=args.report,
            depfiles_path=args.depfiles,
            binary_path=args.binary,
            link_closure=link_closure,
        )
    )
    diagnostics = sorted(diagnostics)
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"install freshness: {diagnostic}")
        return 1
    print("install freshness: PASS (fresh successful Gate evidence)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
