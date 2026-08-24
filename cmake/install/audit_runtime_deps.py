#!/usr/bin/env python3
"""Runtime dependency-closure audit for the staged Pike Runtime.

Audits the dynamic dependency closure of the installed Runtime against the
declared closure in cmake/install/runtime-deps.json: every resolved library
must be a declared virtual/system/loader entry under an allowed system root;
undeclared, unresolved, build-tree, or otherwise unsupported resolutions fail
the audit. The third-party dependencies (Boost, OpenSSL, Glaze, md4c, libwebp,
utf8proc) link statically from the pinned vcpkg manifest, so the
supported dynamic closure is the measured glibc/libstdc++ baseline.

Python 3.12+ standard library only (same tooling policy as the Gate).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence

ALLOWLIST_FIELDS = {
    "schema_version",
    "allowed_virtual",
    "allowed_system",
    "allowed_loader",
    "system_roots",
}


@dataclass(frozen=True)
class ResolvedDependency:
    """One ldd-resolved runtime dependency: a name, and the resolved path for
    file-backed libraries (None for virtual entries such as linux-vdso and for
    unresolved libraries)."""

    name: str
    path: Optional[str]


@dataclass(frozen=True)
class Allowlist:
    virtual: frozenset[str]
    system: frozenset[str]
    loader: frozenset[str]
    system_roots: tuple[str, ...]

    def declares(self, name: str) -> bool:
        return name in self.virtual or name in self.system or name in self.loader


def parse_ldd_output(text: str) -> list[ResolvedDependency]:
    """Parse `ldd` output into resolved dependencies.

    Recognized line forms (leading whitespace ignored):
      linux-vdso.so.1 (0x...)                     virtual, no path
      libstdc++.so.6 => /usr/lib/libstdc++.so.6 (0x...)   named resolution
      libfoo.so.1 => not found                    unresolved
      /lib64/ld-linux-x86-64.so.2 (0x...)         loader, path form
    Anything else is a ValueError: unrecognized output fails closed.
    """
    dependencies: list[ResolvedDependency] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if "=>" in line:
            name, _, rest = line.partition("=>")
            name = name.strip()
            # A symlinked entry (typically the loader) resolves to
            # `<link-path> => <target>`; the link's basename is the soname.
            if name.startswith("/"):
                name = os.path.basename(name)
            rest = rest.strip()
            if rest == "not found":
                dependencies.append(ResolvedDependency(name, None))
                continue
            path, _, _address = rest.rpartition(" ")
            path = path.strip()
            if not name or not path.startswith("/"):
                raise ValueError(f"unparseable ldd output line: {line!r}")
            dependencies.append(ResolvedDependency(name, path))
            continue
        token, _, _address = line.partition(" ")
        if not token:
            raise ValueError(f"unparseable ldd output line: {line!r}")
        if token.startswith("/"):
            dependencies.append(ResolvedDependency(os.path.basename(token), token))
        elif token.endswith(".so") or ".so." in token:
            dependencies.append(ResolvedDependency(token, None))
        else:
            raise ValueError(f"unparseable ldd output line: {line!r}")
    return dependencies


def _string_list(document: dict, field: str) -> list[str]:
    value = document.get(field)
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"allowlist field '{field}' must be a list of strings")
    return value


def load_allowlist(path: os.PathLike[str] | str) -> Allowlist:
    """Load the strict declared-closure allowlist; unknown schema versions and
    unknown fields fail closed (same closed-vocabulary policy as the manifest)."""
    with open(path, encoding="utf-8") as handle:
        document = json.load(handle)
    if not isinstance(document, dict):
        raise ValueError("allowlist must be a JSON object")
    unknown = set(document) - ALLOWLIST_FIELDS
    if unknown:
        raise ValueError(f"allowlist has unknown fields: {sorted(unknown)}")
    if document.get("schema_version") != 1:
        raise ValueError(
            f"allowlist schema_version {document.get('schema_version')!r} is not supported"
        )
    roots = tuple(
        os.path.realpath(root) for root in _string_list(document, "system_roots")
    )
    if not roots:
        raise ValueError("allowlist must declare at least one system root")
    return Allowlist(
        virtual=frozenset(_string_list(document, "allowed_virtual")),
        system=frozenset(_string_list(document, "allowed_system")),
        loader=frozenset(_string_list(document, "allowed_loader")),
        system_roots=roots,
    )


def _is_under(path: str, root: str) -> bool:
    return path == root or path.startswith(root + os.sep)


def audit_dependencies(
    dependencies: Sequence[ResolvedDependency],
    allowlist: Allowlist,
    forbid_roots: Sequence[str],
) -> list[str]:
    """Return the sorted closure violations for the resolved dependencies."""
    real_forbid_roots = [os.path.realpath(root) for root in forbid_roots]
    diagnostics: list[str] = []
    for dependency in dependencies:
        if dependency.path is None:
            if dependency.name in allowlist.virtual:
                continue
            if dependency.name in allowlist.system or dependency.name in allowlist.loader:
                diagnostics.append(
                    f"unresolved runtime dependency: {dependency.name} (not found)"
                )
            else:
                diagnostics.append(
                    f"undeclared runtime dependency: {dependency.name}"
                )
            continue
        if not allowlist.declares(dependency.name):
            diagnostics.append(f"undeclared runtime dependency: {dependency.name}")
            continue
        resolved = os.path.realpath(dependency.path)
        forbidden = next(
            (root for root in real_forbid_roots if _is_under(resolved, root)), None
        )
        if forbidden is not None:
            diagnostics.append(
                f"forbidden runtime dependency path: {resolved} "
                f"(under forbidden root {forbidden})"
            )
            continue
        if not any(_is_under(resolved, root) for root in allowlist.system_roots):
            diagnostics.append(
                f"unsupported runtime dependency path: {resolved} "
                f"(outside the allowed system roots)"
            )
    return sorted(diagnostics)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", required=True, help="path to the staged Runtime binary")
    parser.add_argument("--allowlist", required=True, help="declared-closure JSON allowlist")
    parser.add_argument(
        "--forbid-root",
        action="append",
        default=[],
        help="root (source tree, build tree, vcpkg tree) a resolution must never enter; repeatable",
    )
    args = parser.parse_args(argv)

    if not os.path.isfile(args.binary):
        print(f"dependency audit: Runtime binary not found: {args.binary}")
        return 1
    try:
        allowlist = load_allowlist(args.allowlist)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"dependency audit: invalid allowlist {args.allowlist}: {error}")
        return 1

    completed = subprocess.run(
        ["ldd", args.binary],
        capture_output=True,
        text=True,
        env={"LC_ALL": "C", "PATH": os.environ.get("PATH", "/usr/bin:/bin")},
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0 or "not a dynamic executable" in output:
        print(f"dependency audit: ldd rejected {args.binary}: {output.strip()}")
        return 1
    try:
        dependencies = parse_ldd_output(completed.stdout)
    except ValueError as error:
        print(f"dependency audit: {error}")
        return 1

    diagnostics = audit_dependencies(dependencies, allowlist, args.forbid_root)
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"dependency audit: {diagnostic}")
        return 1
    print(
        f"dependency audit: PASS ({len(dependencies)} declared runtime dependencies "
        f"for {args.binary})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
