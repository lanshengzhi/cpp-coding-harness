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

A private project root and every undeclared Owner package are off that path,
so a header that reaches for one fails to compile. A third-party dependency
is a different question: its headers may be installed anywhere the compiler
searches, so restricting `-I` cannot answer it. The resolved include closure of
every Owner Interface header is therefore scanned against the external
families the manifest declares, and any header from one of them is reported.

The same restriction is asserted from the other side: a fixture header that
leaks a third-party header, an undeclared Owner edge in either direction, a
private root, or an exception type must be *rejected* under the owning
package's declared interface dependencies (issue #834). A positive-only pass
proves the ordinary case; the poison cases prove the restriction is real.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# The package the poison fixtures are compiled against. `cch_mcp` is the
# Capability Owner Package whose Owner Interface must stay a passive value
# surface with no third-party, no undeclared Owner edge, and no exception type
# (ADR 0065); it declares no legal Owner dependency, so an include of any other
# Owner package is an illegal edge out of it.
POISON_OWNER = "cch_mcp"
POISON_DIR = "tests/fixtures/owner-interface"

# Fixture file name -> the rejection it must produce. A third-party family
# leaks whether the compiler found the header or not (the family is in the
# resolved closure either way); an undeclared Owner edge, a private root, and
# an exception type all surface as a failed compile.
POISON_CASES = (
    ("BoostAsioLeakFixture.hpp", "third-party"),
    ("BoostBeastLeakFixture.hpp", "third-party"),
    ("OwnerEdgeInLeakFixture.hpp", "compile-failure"),
    ("OwnerEdgeOutLeakFixture.hpp", "compile-failure"),
    ("PrivateRootLeakFixture.hpp", "compile-failure"),
    ("ExceptionTypeLeakFixture.hpp", "compile-failure"),
)

# Directory and file names that identify each external family the manifest
# declares. The manifest is the authority for the family set; this table only
# says where that family's headers land, and a family with no entry fails the
# run closed rather than going unchecked.
EXTERNAL_FAMILY_MARKERS = {
    "boost": ("boost",),
    "openssl": ("openssl",),
    "glaze": ("glaze",),
    "md4c": ("md4c",),
    "webp": ("webp",),
    "utf8proc": ("utf8proc",),
    "catch2": ("catch2",),
    # POSIX threads is a link-time system library with no header directory of
    # its own; it cannot appear in an include closure.
    "threads": (),
}


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


def external_family_markers(manifest: dict) -> dict:
    markers = {}
    for family in manifest["external_families"]:
        if family not in EXTERNAL_FAMILY_MARKERS:
            raise KeyError(
                f"external family '{family}' has no header marker in "
                f"owner_interface_standalone.py; an unchecked family is not a check"
            )
        markers[family] = EXTERNAL_FAMILY_MARKERS[family]
    return markers


def external_family_of(path: str, markers: dict) -> str:
    """The external family a resolved header belongs to, or the empty string."""
    parts = path.replace(os.sep, "/").split("/")
    names = {part[:-2] for part in parts if part.endswith(".h")}
    names.update(parts)
    for family, family_markers in markers.items():
        for marker in family_markers:
            if marker in names:
                return family
    return ""


def declared_interface_roots(owners: dict, owner_name: str) -> list[str]:
    """The include directories one owner may compile its Owner Interface with.

    Declared package interface dependencies only: the owner itself, the
    pi-neutral support package, and its legal direct Owner dependencies.
    """
    allowed = [include_dir_for(owners[owner_name]["interface_root"])]
    for other, other_entry in owners.items():
        if other_entry["role"] == "support" or other in owners[owner_name]["legal_owner_dependencies"]:
            allowed.append(include_dir_for(owners[other]["interface_root"]))
    return allowed


def probe_translation_unit(
    compiler: str,
    project_root: str,
    directive: str,
    include_roots: list[str],
    markers: dict,
    workdir: str,
) -> tuple[str, list[str], list[str]]:
    """Compile and resolve one TU carrying exactly `directive`.

    Returns ``(compile_error, leaked_headers, external_families)``. The compile
    runs with the supported no-exception policy (ADR 0042), so exception
    handling in a header fails exactly as it does in the real build; the
    resolved closure names every header the directive actually pulled in,
    wherever the toolchain found it.
    """
    tu_path = os.path.join(workdir, "tu.cpp")
    with open(tu_path, "w", encoding="utf-8") as handle:
        handle.write(directive)
    include_flags = []
    for root in include_roots:
        include_flags.extend(["-I", os.path.join(project_root, root)])

    compiled = subprocess.run(
        [compiler, "-std=c++23", "-fsyntax-only", "-fno-exceptions",
         "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
        + include_flags
        + [tu_path],
        capture_output=True,
        text=True,
    )
    compile_error = "" if compiled.returncode == 0 else compiled.stderr.strip()

    resolved = subprocess.run(
        [compiler, "-std=c++23", "-M"] + include_flags + [tu_path],
        capture_output=True,
        text=True,
    )
    if resolved.returncode != 0:
        return compile_error, [], []
    # `-M` prints `target: dep` with backslash continuations; the first token is
    # the make target, not a header.
    tokens = resolved.stdout.replace("\\\n", " ").split()
    leaked: list[str] = []
    families: set[str] = set()
    for token in tokens[1:]:
        if not token.startswith("/"):
            continue
        family = external_family_of(token, markers)
        if family:
            families.add(family)
            leaked.append(token)
    return compile_error, sorted(leaked), sorted(families)


def rejections(compile_error: str, leaked: list[str]) -> list[str]:
    """The rejection kinds one probe produced; empty means the TU was accepted."""
    found = []
    if compile_error:
        found.append("compile-failure")
    if leaked:
        found.append("third-party")
    return found


def probe_batch(prefix, entries, compiler, project_root, markers, jobs):
    """Probe `(label, directive, include_roots)` entries under parallel compiles."""
    results: list[tuple[str, list[str], list[str], list[str]]] = []
    with tempfile.TemporaryDirectory(prefix=prefix) as workdir:

        def run(indexed):
            index, (label, directive, include_roots) = indexed
            # One subdirectory per job keeps the translation-unit paths
            # distinct under parallel compilation.
            job_dir = os.path.join(workdir, str(index))
            os.makedirs(job_dir, exist_ok=True)
            compile_error, leaked, families = probe_translation_unit(
                compiler, project_root, directive, include_roots, markers, job_dir
            )
            return label, rejections(compile_error, leaked), families, leaked

        with ThreadPoolExecutor(max_workers=jobs) as pool:
            results.extend(pool.map(run, enumerate(entries)))
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--jobs", type=int, default=max(4, (os.cpu_count() or 4) // 2))
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    owners = manifest["owners"]
    # The translation units are compiled in a temporary directory, so every
    # project path reaching a command line is absolute.
    project_root = os.path.abspath(args.project_root)
    try:
        markers = external_family_markers(manifest)
    except KeyError as error:
        print(str(error).strip("'"), file=sys.stderr)
        return 1

    positive = []
    for name, entry in owners.items():
        root = entry["interface_root"]
        allowed = declared_interface_roots(owners, name)
        prefix = "/".join(root.replace(os.sep, "/").split("/")[-2:])
        for rel in interface_headers(os.path.join(project_root, root)):
            spelling = f"{prefix}/{rel.replace(os.sep, '/')}"
            positive.append((spelling, f"#include <{spelling}>\n", allowed))

    if not positive:
        print("no Owner Interface headers found", file=sys.stderr)
        return 1

    findings = probe_batch(
        "cch-interface-standalone", positive, args.compiler, project_root, markers, args.jobs
    )
    failures = [(label, families, leaked) for label, found, families, leaked in findings if found]
    if failures:
        print(
            f"{len(failures)} Owner Interface header(s) do not compile standalone:",
            file=sys.stderr,
        )
        for label, families, leaked in sorted(failures):
            detail = f"external families {families}: {leaked[:5]}" if leaked else ""
            print(f"  {label}: {detail}", file=sys.stderr)
        return 1
    print(f"{len(positive)} Owner Interface headers compile standalone")

    # Poison phase: each fixture header must be rejected under exactly the
    # include path its owning package declares, and for the reason it stands
    # for. The property under test is the restriction itself, so a fixture
    # that is accepted is the failure.
    if POISON_OWNER not in owners:
        print(
            f"manifest declares no '{POISON_OWNER}' owner package to compile the poison "
            f"fixtures against",
            file=sys.stderr,
        )
        return 1
    poison_roots = declared_interface_roots(owners, POISON_OWNER)
    poison = []
    missing = []
    for case, _expected in POISON_CASES:
        case_path = os.path.join(project_root, POISON_DIR, case)
        if not os.path.isfile(case_path):
            missing.append(case)
            continue
        poison.append((f"{POISON_DIR}/{case}", f'#include "{case_path}"\n', poison_roots))
    if missing:
        print(
            f"missing Owner Interface leak fixture(s) under {POISON_DIR}: {missing}",
            file=sys.stderr,
        )
        return 1

    poison_results = probe_batch(
        "cch-interface-poison", poison, args.compiler, project_root, markers, args.jobs
    )
    accepted = []
    for (case, expected), (_label, found, _families, _leaked) in zip(POISON_CASES, poison_results):
        if expected not in found:
            accepted.append((f"{POISON_DIR}/{case}", expected, found))
    if accepted:
        print(
            f"{len(accepted)} Owner Interface leak fixture(s) were not rejected under "
            f"'{POISON_OWNER}' interface dependencies {poison_roots}:",
            file=sys.stderr,
        )
        for label, expected, found in sorted(accepted):
            print(f"  {label}: expected {expected}, observed {found or ['accepted']}",
                  file=sys.stderr)
        return 1
    print(f"{len(poison)} Owner Interface leak fixtures are rejected as declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
