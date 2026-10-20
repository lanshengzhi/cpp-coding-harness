#!/usr/bin/env python3
"""Compiler-gate concurrency registration regression (issue #1044).

Both the Owner Interface standalone compile and the zero-warning gate spawn
compiler workers of their own. Their CTest registrations must therefore carry
the same bounded worker count through `--jobs` and reserve that width through
`PROCESSORS`, so the default `ctest -j4` cannot stack their workers above the
four-job host ceiling measured in `docs/agents/validation.md`.

The check configures a throwaway CMake project that includes the production
`cmake/tests/ArchitectureTests.cmake` with only its build-policy test
constructor stubbed, then reads the registered command lines and properties
from CTest's own JSON. It never runs either gate (the warning gate recompiles
the whole project), so this is registration evidence without compiler work.

Uses only the Python standard library and requires Python 3.12 or newer.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

HOST_CEILING = 4
GATE_TESTS = (
    "cch_owner_interface_standalone",
    "cch_warning_gate",
)

FIXTURE_CMAKE = r"""cmake_minimum_required(VERSION 4.4)
project(cch_compiler_gate_registration_fixture NONE)

enable_testing()
set(CCH_PARITY_EXTERNAL_INCLUDE_ROOTS "")
set(CMAKE_CXX_COMPILER "/bin/true")
function(cch_add_supported_build_policy_test case_name should_pass diagnostic_pattern)
endfunction()

cmake_host_system_information(RESULT fixture_host_cores QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT fixture_host_cores)
    set(fixture_host_cores 4)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/host-cores.txt" "${fixture_host_cores}")

include("${CCH_ARCHITECTURE_TESTS}")
"""


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cmake", required=True, help="cmake executable")
    parser.add_argument("--ctest", required=True, help="ctest executable")
    parser.add_argument("--source-dir", required=True, help="repository root")
    return parser.parse_args(argv)


def bounded_jobs(host_cores: int) -> int:
    """The shared compiler-gate budget: host cores capped at four, at least one."""
    return max(1, min(host_cores, HOST_CEILING))


def registered_tests(ctest: str, build_dir: Path) -> dict[str, dict]:
    completed = subprocess.run(
        [ctest, "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        output = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(
            f"ctest --show-only=json-v1 failed ({completed.returncode}):\n{output}"
        )
    document = json.loads(completed.stdout)
    return {test["name"]: test for test in document["tests"]}


def check_gate(test: dict, expected_jobs: int) -> list[str]:
    failures: list[str] = []
    name = test["name"]
    command = test.get("command", [])
    jobs_positions = [
        index for index, argument in enumerate(command) if argument == "--jobs"
    ]
    if len(jobs_positions) != 1 or jobs_positions[0] + 1 >= len(command):
        failures.append(f"{name}: expected exactly one --jobs argument, got {command}")
    else:
        jobs = command[jobs_positions[0] + 1]
        if jobs != str(expected_jobs):
            failures.append(
                f"{name}: --jobs is {jobs}, expected {expected_jobs}"
            )
    properties = {
        prop["name"]: prop["value"] for prop in test.get("properties", [])
    }
    processors = properties.get("PROCESSORS")
    if str(processors) != str(expected_jobs):
        failures.append(
            f"{name}: PROCESSORS is {processors!r}, expected {expected_jobs}"
        )
    return failures


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    source_dir = Path(args.source_dir).resolve()
    architecture_tests = source_dir / "cmake" / "tests" / "ArchitectureTests.cmake"
    if not architecture_tests.is_file():
        print(
            f"compiler budget registration: missing '{architecture_tests}'",
            file=sys.stderr,
        )
        return 1

    with tempfile.TemporaryDirectory(
        prefix="cch-compiler-gate-registration-"
    ) as temporary_directory:
        root = Path(temporary_directory)
        fixture_source = root / "source"
        fixture_build = root / "build"
        fixture_source.mkdir()
        (fixture_source / "CMakeLists.txt").write_text(
            FIXTURE_CMAKE, encoding="utf-8"
        )
        configure = subprocess.run(
            [
                args.cmake,
                "-S",
                str(fixture_source),
                "-B",
                str(fixture_build),
                f"-DCCH_ARCHITECTURE_TESTS={architecture_tests}",
            ],
            capture_output=True,
            text=True,
        )
        if configure.returncode != 0:
            output = configure.stderr.strip() or configure.stdout.strip()
            print(
                f"compiler budget registration: fixture configure failed "
                f"({configure.returncode}):\n{output}",
                file=sys.stderr,
            )
            return 1

        host_cores = int(
            (fixture_build / "host-cores.txt").read_text(encoding="utf-8").strip()
        )
        expected_jobs = bounded_jobs(host_cores)
        tests = registered_tests(args.ctest, fixture_build)
        failures: list[str] = []
        for name in GATE_TESTS:
            test = tests.get(name)
            if test is None:
                failures.append(f"{name}: not registered")
                continue
            failures.extend(check_gate(test, expected_jobs))

    if failures:
        for failure in failures:
            print(f"compiler budget registration: {failure}", file=sys.stderr)
        return 1
    print(
        f"compiler budget registration: PASS (host cores {host_cores}; both gates "
        f"--jobs {expected_jobs}, PROCESSORS {expected_jobs})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
