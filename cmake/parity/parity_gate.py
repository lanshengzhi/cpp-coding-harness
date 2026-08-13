#!/usr/bin/env python3
"""Fail-closed Parity Architecture Gate validator.

Validates the strict, versioned Parity Architecture Manifest and checks the
resolved ownership index emitted by the central CMake constructors against it.
It uses only the Python standard library and requires Python 3.12 or newer.

The manifest is the single policy authority: it records the frozen baseline
pin, the legal Owner graph, roots, roles, external families, and evidence
identities. Unknown schema versions, unknown fields, missing required fields,
and contradictory declarations fail closed. The index is configured evidence,
never a second policy source; malformed or stale evidence fails closed too.

Diagnostics carry stable rule IDs and are emitted deterministically in both a
human-readable and a machine-readable JSON form.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from typing import Any, Optional, Sequence

MANIFEST_SCHEMA_VERSION = 1

# The closed role vocabulary. The manifest's ``roles`` list must be exactly
# this set; a target may declare only one of these roles.
KNOWN_ROLES = frozenset({"owner", "implementation", "support", "composition", "external"})

# The two package kinds an entry under ``manifest.owners`` may take.
KNOWN_OWNER_ROLES = frozenset({"owner", "support"})

# Stable diagnostic rule IDs. Categories: 1xxx manifest fail-closed schema,
# 2xxx target/dependency policy, 3xxx evidence freshness and integrity.
RULE_UNKNOWN_MANIFEST_VERSION = "PARITY-1001"
RULE_UNKNOWN_MANIFEST_FIELD = "PARITY-1002"
RULE_MISSING_MANIFEST_FIELD = "PARITY-1003"
RULE_INVALID_MANIFEST_VALUE = "PARITY-1004"
RULE_UNKNOWN_INDEX_VERSION = "PARITY-1101"
RULE_UNKNOWN_INDEX_FIELD = "PARITY-1102"
RULE_MISSING_INDEX_FIELD = "PARITY-1103"
RULE_INVALID_INDEX_VALUE = "PARITY-1104"
RULE_ILLEGAL_CROSS_OWNER_EDGE = "PARITY-2001"
RULE_SUPPORT_DEPENDS_ON_OWNER = "PARITY-2002"
RULE_UNKNOWN_TARGET_ROLE = "PARITY-2003"
RULE_UNKNOWN_TARGET_OWNER = "PARITY-2004"
RULE_UNKNOWN_EXTERNAL_FAMILY = "PARITY-2005"
RULE_UNKNOWN_PROJECT_DEPENDENCY = "PARITY-2006"
RULE_NON_AUTHORITATIVE_CROSS_OWNER_TARGET = "PARITY-2007"
RULE_STALE_MANIFEST_DIGEST = "PARITY-3001"
RULE_STALE_PRODUCER_SCHEMA = "PARITY-3002"
RULE_MALFORMED_EVIDENCE = "PARITY-3003"


class SchemaViolation(Exception):
    """A fail-closed manifest/index schema violation carrying one diagnostic."""

    def __init__(self, rule_id: str, message: str) -> None:
        super().__init__(message)
        self.rule_id = rule_id
        self.message = message


@dataclass(frozen=True)
class Diagnostic:
    rule_id: str
    message: str
    target: Optional[str] = None
    dependency: Optional[str] = None
    path: Optional[str] = None

    def as_dict(self) -> dict[str, Any]:
        return {
            "rule_id": self.rule_id,
            "message": self.message,
            "target": self.target,
            "dependency": self.dependency,
            "path": self.path,
        }


@dataclass(frozen=True)
class Owner:
    name: str
    role: str
    root: str
    interface_root: str
    legal_owner_dependencies: tuple[str, ...]


@dataclass(frozen=True)
class Manifest:
    schema_version: int
    baseline_commit: str
    owners: dict[str, Owner]
    roles: frozenset[str]
    external_families: frozenset[str]
    evidence: tuple[dict[str, Any], ...]


@dataclass(frozen=True)
class Target:
    name: str
    role: str
    owner: Optional[str]
    sources: tuple[str, ...]
    dependencies: tuple[dict[str, Any], ...]


@dataclass(frozen=True)
class Index:
    producer: str
    schema_version: int
    manifest_digest: str
    targets: tuple[Target, ...]


def _fail(rule_id: str, message: str) -> None:
    raise SchemaViolation(rule_id, message)


def _require_object(value: Any, context: str, rule_id: str) -> None:
    if not isinstance(value, dict):
        _fail(
            rule_id,
            f"field at {context} has invalid type {type(value).__name__}; expected an object",
        )


def _require_string(value: Any, field: str, context: str, rule_id: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(rule_id, f"field '{field}' at {context} must be a non-empty string")
    return value


def _require_int(value: Any, field: str, context: str, rule_id: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        _fail(rule_id, f"field '{field}' at {context} must be an integer")
    return value


def _check_unknown_keys(
    obj: dict[str, Any], allowed: frozenset[str], context: str, rule_id: str
) -> None:
    for key in obj:
        if key not in allowed:
            _fail(rule_id, f"unknown field '{key}' at {context}")


def _require_member(
    obj: dict[str, Any], key: str, context: str, rule_id: str
) -> Any:
    if key not in obj:
        _fail(rule_id, f"missing required field '{key}' at {context}")
    return obj[key]


def _read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError as exc:
        raise SchemaViolation(
            RULE_MALFORMED_EVIDENCE, f"cannot read evidence file '{path}': {exc}"
        ) from exc


def _load_json(path: str, decode_rule_id: str) -> Any:
    raw = _read_bytes(path)
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SchemaViolation(
            decode_rule_id, f"evidence file '{path}' is not valid UTF-8 JSON: {exc}"
        ) from exc


def parse_manifest(data: Any) -> Manifest:
    _require_object(data, "manifest", RULE_INVALID_MANIFEST_VALUE)
    _check_unknown_keys(
        data,
        frozenset(
            {"schema_version", "baseline_commit", "owners", "roles", "external_families", "evidence"}
        ),
        "manifest",
        RULE_UNKNOWN_MANIFEST_FIELD,
    )

    schema_version = _require_int(
        _require_member(data, "schema_version", "manifest", RULE_MISSING_MANIFEST_FIELD),
        "schema_version",
        "manifest",
        RULE_INVALID_MANIFEST_VALUE,
    )
    if schema_version != MANIFEST_SCHEMA_VERSION:
        _fail(
            RULE_UNKNOWN_MANIFEST_VERSION,
            f"unknown manifest schema_version {schema_version}; supported version is "
            f"{MANIFEST_SCHEMA_VERSION}",
        )

    baseline_commit = _require_string(
        _require_member(data, "baseline_commit", "manifest", RULE_MISSING_MANIFEST_FIELD),
        "baseline_commit",
        "manifest",
        RULE_INVALID_MANIFEST_VALUE,
    )

    owners_raw = _require_member(data, "owners", "manifest", RULE_MISSING_MANIFEST_FIELD)
    _require_object(owners_raw, "manifest.owners", RULE_INVALID_MANIFEST_VALUE)
    if not owners_raw:
        _fail(RULE_INVALID_MANIFEST_VALUE, "field 'owners' at manifest must not be empty")

    owners: dict[str, Owner] = {}
    seen_roots: dict[str, str] = {}
    seen_interface_roots: dict[str, str] = {}
    for name, entry in owners_raw.items():
        owner_context = f"manifest.owners.{name}"
        _require_string(name, "owner name", "manifest.owners", RULE_INVALID_MANIFEST_VALUE)
        _require_object(entry, owner_context, RULE_INVALID_MANIFEST_VALUE)
        _check_unknown_keys(
            entry,
            frozenset({"role", "root", "interface_root", "legal_owner_dependencies"}),
            owner_context,
            RULE_UNKNOWN_MANIFEST_FIELD,
        )

        role = _require_string(
            _require_member(entry, "role", owner_context, RULE_MISSING_MANIFEST_FIELD),
            "role",
            owner_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        if role not in KNOWN_OWNER_ROLES:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"owner '{name}' declares unknown role '{role}'; expected one of "
                f"{sorted(KNOWN_OWNER_ROLES)}",
            )

        root = _require_string(
            _require_member(entry, "root", owner_context, RULE_MISSING_MANIFEST_FIELD),
            "root",
            owner_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        interface_root = _require_string(
            _require_member(entry, "interface_root", owner_context, RULE_MISSING_MANIFEST_FIELD),
            "interface_root",
            owner_context,
            RULE_INVALID_MANIFEST_VALUE,
        )

        legal_raw = _require_member(
            entry, "legal_owner_dependencies", owner_context, RULE_MISSING_MANIFEST_FIELD
        )
        if not isinstance(legal_raw, list) or any(
            not isinstance(item, str) for item in legal_raw
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field 'legal_owner_dependencies' at {owner_context} must be a list of strings",
            )
        legal = tuple(legal_raw)

        if root in seen_roots:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"contradictory declaration: owner '{name}' reuses root '{root}' already owned "
                f"by '{seen_roots[root]}'",
            )
        seen_roots[root] = name
        if interface_root in seen_interface_roots:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"contradictory declaration: owner '{name}' reuses interface_root "
                f"'{interface_root}' already owned by '{seen_interface_roots[interface_root]}'",
            )
        seen_interface_roots[interface_root] = name

        owners[name] = Owner(name, role, root, interface_root, legal)

    # Contradictory legal_owner_dependencies are validated after every owner is
    # present, so forward references are only errors when they name a package
    # that does not exist at all.
    for owner in owners.values():
        seen: set[str] = set()
        for dependency in owner.legal_owner_dependencies:
            if dependency not in owners:
                _fail(
                    RULE_INVALID_MANIFEST_VALUE,
                    f"contradictory declaration: owner '{owner.name}' lists legal owner dependency "
                    f"'{dependency}' which is not a declared owner",
                )
            if dependency == owner.name:
                _fail(
                    RULE_INVALID_MANIFEST_VALUE,
                    f"contradictory declaration: owner '{owner.name}' lists itself as a legal "
                    f"owner dependency",
                )
            if dependency in seen:
                _fail(
                    RULE_INVALID_MANIFEST_VALUE,
                    f"contradictory declaration: owner '{owner.name}' lists legal owner dependency "
                    f"'{dependency}' more than once",
                )
            seen.add(dependency)
            if owners[dependency].role != "owner":
                _fail(
                    RULE_INVALID_MANIFEST_VALUE,
                    f"contradictory declaration: owner '{owner.name}' lists '{dependency}' as a "
                    f"legal owner dependency, but '{dependency}' is a '{owners[dependency].role}' "
                    f"package; support is pi-neutral and never a legal cross-Owner edge",
                )

    roles_raw = _require_member(data, "roles", "manifest", RULE_MISSING_MANIFEST_FIELD)
    if not isinstance(roles_raw, list) or any(
        not isinstance(item, str) for item in roles_raw
    ):
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            "field 'roles' at manifest must be a list of strings",
        )
    roles = frozenset(roles_raw)
    if roles != KNOWN_ROLES:
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            f"contradictory declaration: manifest 'roles' must be exactly "
            f"{sorted(KNOWN_ROLES)}; found {sorted(roles)}",
        )

    families_raw = _require_member(
        data, "external_families", "manifest", RULE_MISSING_MANIFEST_FIELD
    )
    _require_object(families_raw, "manifest.external_families", RULE_INVALID_MANIFEST_VALUE)
    families: set[str] = set()
    for family, entry in families_raw.items():
        family_context = f"manifest.external_families.{family}"
        _require_string(family, "external family name", "manifest.external_families", RULE_INVALID_MANIFEST_VALUE)
        _require_object(entry, family_context, RULE_INVALID_MANIFEST_VALUE)
        _check_unknown_keys(
            entry, frozenset({"description"}), family_context, RULE_UNKNOWN_MANIFEST_FIELD
        )
        if "description" in entry:
            _require_string(entry["description"], "description", family_context, RULE_INVALID_MANIFEST_VALUE)
        families.add(family)

    evidence_raw = _require_member(data, "evidence", "manifest", RULE_MISSING_MANIFEST_FIELD)
    if not isinstance(evidence_raw, list) or not evidence_raw:
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            "field 'evidence' at manifest must be a non-empty list",
        )
    evidence: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_producers: set[str] = set()
    for position, entry in enumerate(evidence_raw):
        evidence_context = f"manifest.evidence[{position}]"
        _require_object(entry, evidence_context, RULE_INVALID_MANIFEST_VALUE)
        _check_unknown_keys(
            entry,
            frozenset({"id", "producer", "producer_schema_version", "input_identities"}),
            evidence_context,
            RULE_UNKNOWN_MANIFEST_FIELD,
        )
        identity = _require_string(
            _require_member(entry, "id", evidence_context, RULE_MISSING_MANIFEST_FIELD),
            "id",
            evidence_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        producer = _require_string(
            _require_member(entry, "producer", evidence_context, RULE_MISSING_MANIFEST_FIELD),
            "producer",
            evidence_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        producer_schema_version = _require_int(
            _require_member(
                entry, "producer_schema_version", evidence_context, RULE_MISSING_MANIFEST_FIELD
            ),
            "producer_schema_version",
            evidence_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        inputs = _require_member(
            entry, "input_identities", evidence_context, RULE_MISSING_MANIFEST_FIELD
        )
        if not isinstance(inputs, list) or not inputs or any(
            not isinstance(item, str) for item in inputs
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field 'input_identities' at {evidence_context} must be a non-empty list "
                f"of strings",
            )
        if identity in seen_ids:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"contradictory declaration: evidence identity '{identity}' is declared more "
                f"than once",
            )
        if producer in seen_producers:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"contradictory declaration: evidence producer '{producer}' is declared more "
                f"than once",
            )
        seen_ids.add(identity)
        seen_producers.add(producer)
        evidence.append(
            {
                "id": identity,
                "producer": producer,
                "producer_schema_version": producer_schema_version,
                "input_identities": tuple(inputs),
            }
        )

    return Manifest(schema_version, baseline_commit, owners, roles, frozenset(families), tuple(evidence))


def parse_index(data: Any) -> Index:
    _require_object(data, "index", RULE_INVALID_INDEX_VALUE)
    _check_unknown_keys(
        data,
        frozenset({"producer", "schema_version", "manifest_digest", "targets"}),
        "index",
        RULE_UNKNOWN_INDEX_FIELD,
    )

    producer = _require_string(
        _require_member(data, "producer", "index", RULE_MISSING_INDEX_FIELD),
        "producer",
        "index",
        RULE_INVALID_INDEX_VALUE,
    )
    schema_version = _require_int(
        _require_member(data, "schema_version", "index", RULE_MISSING_INDEX_FIELD),
        "schema_version",
        "index",
        RULE_INVALID_INDEX_VALUE,
    )
    manifest_digest = _require_string(
        _require_member(data, "manifest_digest", "index", RULE_MISSING_INDEX_FIELD),
        "manifest_digest",
        "index",
        RULE_INVALID_INDEX_VALUE,
    )

    targets_raw = _require_member(data, "targets", "index", RULE_MISSING_INDEX_FIELD)
    if not isinstance(targets_raw, list):
        _fail(RULE_INVALID_INDEX_VALUE, "field 'targets' at index must be a list")

    targets: list[Target] = []
    seen_targets: set[str] = set()
    for position, entry in enumerate(targets_raw):
        target_context = f"index.targets[{position}]"
        _require_object(entry, target_context, RULE_INVALID_INDEX_VALUE)
        _check_unknown_keys(
            entry,
            frozenset({"name", "role", "owner", "sources", "dependencies"}),
            target_context,
            RULE_UNKNOWN_INDEX_FIELD,
        )
        name = _require_string(
            _require_member(entry, "name", target_context, RULE_MISSING_INDEX_FIELD),
            "name",
            target_context,
            RULE_INVALID_INDEX_VALUE,
        )
        role = _require_string(
            _require_member(entry, "role", target_context, RULE_MISSING_INDEX_FIELD),
            "role",
            target_context,
            RULE_INVALID_INDEX_VALUE,
        )
        owner_value = _require_member(entry, "owner", target_context, RULE_MISSING_INDEX_FIELD)
        if owner_value is not None and not isinstance(owner_value, str):
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"field 'owner' at {target_context} must be a string or null",
            )
        sources_value = _require_member(
            entry, "sources", target_context, RULE_MISSING_INDEX_FIELD
        )
        if not isinstance(sources_value, list) or any(
            not isinstance(item, str) for item in sources_value
        ):
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"field 'sources' at {target_context} must be a list of strings",
            )
        dependencies_value = _require_member(
            entry, "dependencies", target_context, RULE_MISSING_INDEX_FIELD
        )
        if not isinstance(dependencies_value, list):
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"field 'dependencies' at {target_context} must be a list",
            )
        dependencies: list[dict[str, Any]] = []
        for dep_position, dep in enumerate(dependencies_value):
            dep_context = f"{target_context}.dependencies[{dep_position}]"
            _require_object(dep, dep_context, RULE_INVALID_INDEX_VALUE)
            _check_unknown_keys(
                dep, frozenset({"name", "family"}), dep_context, RULE_UNKNOWN_INDEX_FIELD
            )
            dep_name = _require_string(
                _require_member(dep, "name", dep_context, RULE_MISSING_INDEX_FIELD),
                "name",
                dep_context,
                RULE_INVALID_INDEX_VALUE,
            )
            dep_family = _require_member(dep, "family", dep_context, RULE_MISSING_INDEX_FIELD)
            if dep_family is not None and not isinstance(dep_family, str):
                _fail(
                    RULE_INVALID_INDEX_VALUE,
                    f"field 'family' at {dep_context} must be a string or null",
                )
            dependencies.append({"name": dep_name, "family": dep_family})

        if name in seen_targets:
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"invalid index value: target '{name}' is declared more than once",
            )
        seen_targets.add(name)
        targets.append(Target(name, role, owner_value, tuple(sources_value), tuple(dependencies)))

    return Index(producer, schema_version, manifest_digest, tuple(targets))


def check(manifest: Manifest, index: Index, manifest_digest: str) -> list[Diagnostic]:
    """Compare the resolved index against the strict manifest and return all findings."""
    diagnostics: list[Diagnostic] = []

    evidence = next(
        (entry for entry in manifest.evidence if entry["producer"] == index.producer), None
    )
    if evidence is None:
        diagnostics.append(
            Diagnostic(
                RULE_INVALID_INDEX_VALUE,
                f"index producer '{index.producer}' is not declared by any manifest evidence "
                f"reference",
            )
        )
    else:
        expected = evidence["producer_schema_version"]
        if index.schema_version < expected:
            diagnostics.append(
                Diagnostic(
                    RULE_STALE_PRODUCER_SCHEMA,
                    f"stale evidence: index producer schema_version {index.schema_version} is "
                    f"older than the declared {expected} for producer '{index.producer}'",
                )
            )
        elif index.schema_version > expected:
            diagnostics.append(
                Diagnostic(
                    RULE_UNKNOWN_INDEX_VERSION,
                    f"unknown index schema_version {index.schema_version} for producer "
                    f"'{index.producer}'; the manifest declares {expected}",
                )
            )

    if index.manifest_digest != manifest_digest:
        diagnostics.append(
            Diagnostic(
                RULE_STALE_MANIFEST_DIGEST,
                "stale evidence: index manifest_digest does not match the current manifest; "
                "reconfigure to regenerate the ownership index",
            )
        )

    by_name = {target.name: target for target in index.targets}
    for target in index.targets:
        if target.role not in manifest.roles:
            diagnostics.append(
                Diagnostic(
                    RULE_UNKNOWN_TARGET_ROLE,
                    f"target '{target.name}' declares unknown role '{target.role}'",
                    target=target.name,
                )
            )

        owner_entry = manifest.owners.get(target.owner) if target.owner else None
        if target.role == "external":
            if target.owner is not None:
                diagnostics.append(
                    Diagnostic(
                        RULE_UNKNOWN_TARGET_OWNER,
                        f"external target '{target.name}' must not declare an owner",
                        target=target.name,
                    )
                )
        elif owner_entry is None:
            diagnostics.append(
                Diagnostic(
                    RULE_UNKNOWN_TARGET_OWNER,
                    f"target '{target.name}' declares unknown owner '{target.owner}'",
                    target=target.name,
                )
            )
        else:
            expected_kind = "support" if target.role == "support" else "owner"
            if owner_entry.role != expected_kind:
                diagnostics.append(
                    Diagnostic(
                        RULE_UNKNOWN_TARGET_OWNER,
                        f"target '{target.name}' has role '{target.role}' but owner "
                        f"'{target.owner}' is a '{owner_entry.role}' package; expected an "
                        f"'{expected_kind}' package",
                        target=target.name,
                    )
                )

        for dependency in target.dependencies:
            dependency_name = dependency["name"]
            family = dependency["family"]
            if family is not None:
                if family not in manifest.external_families:
                    diagnostics.append(
                        Diagnostic(
                            RULE_UNKNOWN_EXTERNAL_FAMILY,
                            f"target '{target.name}' depends on external family '{family}' which "
                            f"is not declared in the manifest",
                            target=target.name,
                            dependency=dependency_name,
                        )
                    )
                continue

            to_target = by_name.get(dependency_name)
            if to_target is None:
                diagnostics.append(
                    Diagnostic(
                        RULE_UNKNOWN_PROJECT_DEPENDENCY,
                        f"target '{target.name}' depends on unknown project target "
                        f"'{dependency_name}'",
                        target=target.name,
                        dependency=dependency_name,
                    )
                )
                continue

            from_owner = target.owner
            to_owner = to_target.owner
            if from_owner is None or to_owner is None or from_owner == to_owner:
                continue

            from_entry = manifest.owners.get(from_owner)
            to_entry = manifest.owners.get(to_owner)
            if from_entry is None or to_entry is None:
                continue  # already reported as an unknown target owner

            if to_entry.role == "support":
                # Every Owner may depend on the pi-neutral support package.
                continue
            if from_entry.role == "support":
                diagnostics.append(
                    Diagnostic(
                        RULE_SUPPORT_DEPENDS_ON_OWNER,
                        f"support target '{target.name}' (owner '{from_owner}') depends on owner "
                        f"target '{dependency_name}' (owner '{to_owner}'); support owns no "
                        f"Supported Capability and depends only on support and external targets",
                        target=target.name,
                        dependency=dependency_name,
                    )
                )
                continue

            legal = set(from_entry.legal_owner_dependencies)
            if to_target.role != "owner":
                diagnostics.append(
                    Diagnostic(
                        RULE_NON_AUTHORITATIVE_CROSS_OWNER_TARGET,
                        f"illegal cross-Owner edge target: target '{target.name}' (owner "
                        f"'{from_owner}') depends on target '{dependency_name}' which has role "
                        f"'{to_target.role}' (owner '{to_owner}'); cross-Owner edges target only "
                        f"the authoritative owner library (role 'owner')",
                        target=target.name,
                        dependency=dependency_name,
                    )
                )
                continue
            if to_owner not in legal:
                diagnostics.append(
                    Diagnostic(
                        RULE_ILLEGAL_CROSS_OWNER_EDGE,
                        f"illegal cross-Owner edge: target '{target.name}' (owner '{from_owner}') "
                        f"depends on target '{dependency_name}' (owner '{to_owner}'); "
                        f"'{to_owner}' is not in '{from_owner}'s legal owner dependencies "
                        f"{sorted(from_entry.legal_owner_dependencies)}",
                        target=target.name,
                        dependency=dependency_name,
                    )
                )

    diagnostics.sort(
        key=lambda diagnostic: (
            diagnostic.rule_id,
            diagnostic.message,
            diagnostic.target or "",
            diagnostic.dependency or "",
        )
    )
    return diagnostics


def _render_human(diagnostics: Sequence[Diagnostic], targets: int, dependencies: int) -> str:
    if not diagnostics:
        return (
            "Parity Architecture Gate: PASS\n"
            f"  targets: {targets}\n"
            f"  dependencies checked: {dependencies}\n"
        ).rstrip()
    lines = ["Parity Architecture Gate: FAIL"]
    for diagnostic in diagnostics:
        lines.append(f"  {diagnostic.rule_id}: {diagnostic.message}")
    return "\n".join(lines)


def _render_json(diagnostics: Sequence[Diagnostic], targets: int, dependencies: int) -> str:
    payload = {
        "ok": not diagnostics,
        "targets": targets,
        "dependencies_checked": dependencies,
        "diagnostics": [diagnostic.as_dict() for diagnostic in diagnostics],
    }
    return json.dumps(payload, indent=2, sort_keys=True)


def _count_dependencies(index: Index) -> int:
    return sum(len(target.dependencies) for target in index.targets)


def build_report(
    diagnostics: Sequence[Diagnostic], index: Optional[Index], output_format: str
) -> str:
    targets = len(index.targets) if index is not None else 0
    dependencies = _count_dependencies(index) if index is not None else 0
    if output_format == "json":
        return _render_json(diagnostics, targets, dependencies)
    return _render_human(diagnostics, targets, dependencies)


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="parity_gate",
        description="Fail-closed Parity Architecture Gate validator.",
    )
    parser.add_argument("--manifest", required=True, help="Path to the strict versioned manifest")
    parser.add_argument("--index", required=True, help="Path to the resolved ownership index")
    parser.add_argument(
        "--format",
        choices=("human", "json"),
        default="human",
        help="Diagnostic output format (default: human)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    diagnostics: list[Diagnostic]
    index: Optional[Index] = None

    try:
        manifest_data = _load_json(args.manifest, RULE_INVALID_MANIFEST_VALUE)
        manifest = parse_manifest(manifest_data)
        manifest_bytes = _read_bytes(args.manifest)
        manifest_digest = hashlib.sha256(manifest_bytes).hexdigest()

        index_data = _load_json(args.index, RULE_MALFORMED_EVIDENCE)
        index = parse_index(index_data)
        diagnostics = check(manifest, index, manifest_digest)
    except SchemaViolation as exc:
        diagnostics = [Diagnostic(exc.rule_id, exc.message)]

    print(build_report(diagnostics, index, args.format))
    return 1 if diagnostics else 0


if __name__ == "__main__":
    sys.exit(main())
