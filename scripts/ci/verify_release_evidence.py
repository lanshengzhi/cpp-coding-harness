#!/usr/bin/env python3
"""Fail-closed release-qualification evidence verifier (issue #474).

The GCC 16.x Release IPO/LTO artifact qualifies only when one evidence
directory records a complete, fresh, and mutually consistent qualification
run: the toolchain facts, the successful build-phase Parity Architecture Gate
report, the install-gate PASS, the dependency-closure audit PASS, the
relocation/offline smoke PASS markers, and the artifact digest binding the
evidence to the exact staged Runtime binary.

Missing (REL-1001), unreadable (REL-1002), out-of-vocabulary (REL-1003),
incomplete (REL-1004), stale (REL-2001), contradictory (REL-3001/3002), or
artifact-mismatched (REL-3003) evidence fails qualification; nothing degrades
to a warning. Rule identifiers are stable so CI and maintainers can track one
failure across runs.

Python 3.12+ standard library only (same tooling policy as the Gate).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path
from typing import Optional, Sequence, Union

PathLike = Union[str, os.PathLike[str]]

REQUIRED_EVIDENCE = (
    "toolchain.json",
    "parity-build-gate.json",
    "install-gate.log",
    "dependency-closure.log",
    "relocation-smoke.log",
    "artifact.sha256",
)

TOOLCHAIN_FIELDS = frozenset(
    {
        "schema_version",
        "recorded_at",
        "gcc_version",
        "cmake_version",
        "ninja_version",
        "build_type",
        "ipo_lto",
        "vcpkg_revision",
    }
)

INSTALL_GATE_MARKER = "Runtime install gate: PASS"
DEPENDENCY_CLOSURE_MARKER = "dependency audit: PASS"
SMOKE_MARKERS = ("version", "help", "offline-determinism", "relocation")

GCC_MAJOR = 16
CMAKE_FLOOR = (4, 4)
NINJA_FLOOR = (1, 11)

# Tolerance for clock skew between the qualification run's hosts.
CLOCK_SKEW_SECONDS = 300


def _parse_version(text: object, field: str) -> tuple[int, ...]:
    if not isinstance(text, str):
        raise ValueError(f"field '{field}' must be a version string")
    parts = text.split(".")
    if not parts or not all(part.isdigit() for part in parts):
        raise ValueError(f"field '{field}' is not a dotted numeric version: {text!r}")
    return tuple(int(part) for part in parts)


def _load_json(path: Path, name: str, diagnostics: list[str]) -> Optional[dict]:
    try:
        with open(path, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        diagnostics.append(f"REL-1002 invalid release evidence {name}: {error}")
        return None
    if not isinstance(document, dict):
        diagnostics.append(f"REL-1002 invalid release evidence {name}: not a JSON object")
        return None
    return document


def _check_toolchain(
    document: dict,
    *,
    run_started_at: float,
    vcpkg_baseline: str,
    now: float,
    diagnostics: list[str],
) -> None:
    unknown = sorted(set(document) - TOOLCHAIN_FIELDS)
    if unknown:
        diagnostics.append(
            f"REL-1003 toolchain evidence has unknown fields: {unknown}"
        )
    if document.get("schema_version") != 1:
        diagnostics.append(
            f"REL-1003 toolchain schema_version "
            f"{document.get('schema_version')!r} is not supported"
        )

    missing = sorted(TOOLCHAIN_FIELDS - set(document))
    if missing:
        diagnostics.append(
            f"REL-1004 toolchain evidence is incomplete, missing fields: {missing}"
        )
        return

    recorded_at = document["recorded_at"]
    if not isinstance(recorded_at, (int, float)) or isinstance(recorded_at, bool):
        diagnostics.append("REL-1002 toolchain field 'recorded_at' must be a number")
    elif recorded_at < run_started_at:
        diagnostics.append(
            "REL-2001 stale toolchain evidence: recorded_at predates the "
            "qualification run start"
        )
    elif recorded_at > now + CLOCK_SKEW_SECONDS:
        diagnostics.append(
            "REL-2001 stale toolchain evidence: recorded_at is in the future"
        )

    try:
        gcc_version = _parse_version(document["gcc_version"], "gcc_version")
        if gcc_version[0] != GCC_MAJOR:
            diagnostics.append(
                f"REL-3001 contradictory toolchain evidence: release artifacts "
                f"require GCC {GCC_MAJOR}.x, recorded {document['gcc_version']!r}"
            )
    except ValueError as error:
        diagnostics.append(f"REL-3001 contradictory toolchain evidence: {error}")

    for field, floor in (("cmake_version", CMAKE_FLOOR), ("ninja_version", NINJA_FLOOR)):
        try:
            version = _parse_version(document[field], field)
            if version < floor:
                diagnostics.append(
                    f"REL-3001 contradictory toolchain evidence: {field} "
                    f"{document[field]!r} is below the supported floor "
                    f"{'.'.join(str(part) for part in floor)}"
                )
        except ValueError as error:
            diagnostics.append(f"REL-3001 contradictory toolchain evidence: {error}")

    if document["build_type"] != "Release":
        diagnostics.append(
            f"REL-3001 contradictory toolchain evidence: build_type "
            f"{document['build_type']!r} is not 'Release'"
        )
    if document["ipo_lto"] is not True:
        diagnostics.append(
            "REL-3001 contradictory toolchain evidence: the release artifact "
            "must be built with IPO/LTO (ipo_lto is not true)"
        )
    if document["vcpkg_revision"] != vcpkg_baseline:
        diagnostics.append(
            f"REL-3001 contradictory toolchain evidence: vcpkg revision "
            f"{document['vcpkg_revision']!r} does not match the pinned "
            f"vcpkg.json baseline {vcpkg_baseline!r}"
        )


def _check_marker(path: Path, name: str, marker: str, diagnostics: list[str]) -> None:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        diagnostics.append(f"REL-1002 unreadable release evidence {name}: {error}")
        return
    if marker not in text:
        diagnostics.append(
            f"REL-1004 incomplete release evidence {name}: missing marker {marker!r}"
        )


def verify_evidence(
    *,
    evidence_dir: PathLike,
    run_started_at: float,
    vcpkg_baseline: str,
    artifact_path: PathLike,
    now: Optional[float] = None,
) -> list[str]:
    """Return the sorted rule-tagged qualification violations."""
    if now is None:
        now = time.time()
    directory = Path(evidence_dir)
    artifact = Path(artifact_path)
    diagnostics: list[str] = []

    present: dict[str, Path] = {}
    for name in REQUIRED_EVIDENCE:
        path = directory / name
        if not path.is_file():
            diagnostics.append(f"REL-1001 missing release evidence: {name}")
            continue
        mtime = path.stat().st_mtime
        if mtime < run_started_at:
            diagnostics.append(
                f"REL-2001 stale release evidence {name}: predates the "
                f"qualification run start"
            )
        elif mtime > now + CLOCK_SKEW_SECONDS:
            diagnostics.append(
                f"REL-2001 stale release evidence {name}: modification time is "
                f"in the future"
            )
        present[name] = path

    if "toolchain.json" in present:
        toolchain = _load_json(present["toolchain.json"], "toolchain.json", diagnostics)
        if toolchain is not None:
            _check_toolchain(
                toolchain,
                run_started_at=run_started_at,
                vcpkg_baseline=vcpkg_baseline,
                now=now,
                diagnostics=diagnostics,
            )

    if "parity-build-gate.json" in present:
        report = _load_json(
            present["parity-build-gate.json"], "parity-build-gate.json", diagnostics
        )
        if report is not None and report.get("ok") is not True:
            diagnostics.append(
                "REL-3002 contradictory release evidence: the Parity Architecture "
                "Gate report does not record success"
            )

    if "install-gate.log" in present:
        _check_marker(
            present["install-gate.log"], "install-gate.log", INSTALL_GATE_MARKER, diagnostics
        )
    if "dependency-closure.log" in present:
        _check_marker(
            present["dependency-closure.log"],
            "dependency-closure.log",
            DEPENDENCY_CLOSURE_MARKER,
            diagnostics,
        )
    if "relocation-smoke.log" in present:
        for marker in SMOKE_MARKERS:
            _check_marker(
                present["relocation-smoke.log"],
                "relocation-smoke.log",
                f"smoke PASS: {marker}",
                diagnostics,
            )

    if not artifact.is_file():
        diagnostics.append(f"REL-3003 release artifact not found: {artifact}")
    elif "artifact.sha256" in present:
        try:
            digest_text = present["artifact.sha256"].read_text(encoding="utf-8")
        except OSError as error:
            diagnostics.append(f"REL-1002 unreadable release evidence artifact.sha256: {error}")
        else:
            tokens = digest_text.split()
            recorded = tokens[0] if tokens else ""
            if len(recorded) != 64 or any(c not in "0123456789abcdef" for c in recorded):
                diagnostics.append(
                    "REL-1002 invalid release evidence artifact.sha256: not a "
                    "lowercase hex SHA-256 digest"
                )
            else:
                actual = hashlib.sha256(artifact.read_bytes()).hexdigest()
                if recorded != actual:
                    diagnostics.append(
                        "REL-3003 contradictory release evidence: artifact digest "
                        "does not match the staged Runtime binary"
                    )

    return sorted(diagnostics)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--evidence-dir", required=True, help="release evidence directory")
    parser.add_argument(
        "--run-started-at",
        required=True,
        type=float,
        help="epoch seconds when the qualification run started; older evidence is stale",
    )
    parser.add_argument(
        "--vcpkg-baseline",
        required=True,
        help="expected pinned vcpkg baseline revision (vcpkg.json builtin-baseline)",
    )
    parser.add_argument("--artifact", required=True, help="staged Runtime binary path")
    args = parser.parse_args(argv)

    diagnostics = verify_evidence(
        evidence_dir=args.evidence_dir,
        run_started_at=args.run_started_at,
        vcpkg_baseline=args.vcpkg_baseline,
        artifact_path=args.artifact,
    )
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"release evidence: {diagnostic}")
        return 1
    print("release evidence: PASS (complete, fresh, consistent qualification evidence)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
