#!/usr/bin/env python3
"""Owner Interface standalone-compile evidence (ADR 0039; issue #469).

Every Owner Interface header must compile standalone using only its declared
package interface dependencies (CODING_STANDARDS.md section 8). For each
header under each manifest-declared owner-local interface root, this script
compiles one minimal translation unit that includes only that header, with the
include path restricted to:

  * the owning package's own interface root,
  * the pi-neutral support package's interface root, and
  * the interface roots of the owner's legal direct Owner dependencies.

No third-party include path and no private src root is provided, so a header
that leaks Boost/Glaze/etc., a private path, or an undeclared Owner edge fails
to compile. The manifest (cmake/parity/manifest.json) is the single authority
for roots and legal edges; this script duplicates none of that policy.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor


def load_manifest(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def interface_headers(root: str) -> list[str]:
    headers = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for filename in sorted(filenames):
            if filename.endswith(".hpp"):
                full = os.path.join(dirpath, filename)
                headers.append(os.path.relpath(full, root))
    return sorted(headers)


def include_dir_for(root: str) -> str:
    """The `-I` directory for an interface root: the root ends in
    ``cch/<subdir>``; the include path must name the directory that contains
    ``cch/`` so the canonical <cch/...> spelling resolves."""
    parts = root.replace(os.sep, "/").split("/")
    return "/".join(parts[:-2])


def compile_header(
    compiler: str,
    project_root: str,
    spelling: str,
    include_roots: list[str],
    workdir: str,
) -> tuple[str, str]:
    """Compile one TU that includes only `spelling`; return (header, error)."""
    tu_path = os.path.join(workdir, "tu.cpp")
    with open(tu_path, "w", encoding="utf-8") as handle:
        handle.write(f"#include <{spelling}>\n")
    command = [
        compiler,
        "-std=c++23",
        "-fsyntax-only",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    ]
    for root in include_roots:
        command.extend(["-I", os.path.join(project_root, root)])
    command.append(tu_path)
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode == 0:
        return spelling, ""
    return spelling, result.stderr.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--jobs", type=int, default=max(4, (os.cpu_count() or 4) // 2))
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    owners = manifest["owners"]

    jobs = []
    for name, entry in owners.items():
        root = entry["interface_root"]
        # Declared package interface dependencies: the owner itself, the
        # pi-neutral support package, and its legal direct Owner dependencies.
        allowed = [include_dir_for(root)]
        for other, other_entry in owners.items():
            if other_entry["role"] == "support" or other in entry["legal_owner_dependencies"]:
                allowed.append(include_dir_for(owners[other]["interface_root"]))
        prefix = "/".join(root.replace(os.sep, "/").split("/")[-2:])
        for rel in interface_headers(os.path.join(args.project_root, root)):
            spelling = f"{prefix}/{rel.replace(os.sep, '/')}"
            jobs.append((spelling, allowed))

    if not jobs:
        print("no Owner Interface headers found", file=sys.stderr)
        return 1

    failures: list[tuple[str, str]] = []
    with tempfile.TemporaryDirectory(prefix="cch-interface-standalone-") as workdir:
        # One subdirectory per job keeps the translation-unit paths distinct
        # under parallel compilation.
        def run(indexed):
            index, (spelling, allowed) = indexed
            job_dir = os.path.join(workdir, str(index))
            os.makedirs(job_dir, exist_ok=True)
            return compile_header(args.compiler, args.project_root, spelling, allowed, job_dir)

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for spelling, error in pool.map(run, enumerate(jobs)):
                if error:
                    failures.append((spelling, error))

    if failures:
        print(
            f"{len(failures)} Owner Interface header(s) do not compile standalone:",
            file=sys.stderr,
        )
        for spelling, error in sorted(failures):
            print(f"\n=== <{spelling}> ===\n{error}", file=sys.stderr)
        return 1
    print(f"{len(jobs)} Owner Interface headers compile standalone")
    return 0


if __name__ == "__main__":
    sys.exit(main())
