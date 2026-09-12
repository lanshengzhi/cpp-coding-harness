#!/usr/bin/env python3
"""Fail-closed Parity Architecture Gate validator.

Validates the strict, versioned Parity Architecture Manifest and checks the
configured evidence against it. The manifest is the single policy authority:
it records the frozen baseline pin, the legal Owner graph, roots, roles,
external families, and evidence identities. Unknown schema versions, unknown
fields, missing required fields, and contradictory declarations fail closed.
Configured evidence is never a second policy source; malformed or stale
evidence fails closed too.

The Gate consumes four evidence classes produced by the build:

* ``ownership-index`` (producer ``cch-parity-constructor``) - the resolved
  target/role/Owner/dependency declarations emitted by the central CMake
  constructors.
* ``direct-includes`` (producer ``cch-parity-lexer``) - a
  preprocessing-directive lexer scan of every declared source, forced include,
  and PCH input that records each ``#include`` directive in every source-text
  branch (including disabled preprocessor branches).
* ``compile-commands`` (producer ``cmake-file-api``) - the generated
  compilation database proving each production source compiles exactly once
  with a supported compiler and only declared forced-include/PCH context.
* ``depfiles`` (producer ``cch-compiler-depfile``, schema version 2) - active
  transitive-closure evidence sourced from ``.ninja_deps`` required at the
  build phase; it never authorizes a direct include.

Project-header includes resolve canonically against the declared interface
roots and reject quote, basename, relative, macro-generated, ambiguous,
escaped, symlinked, and case-conflicting spellings. Diagnostics carry stable
rule IDs and are emitted deterministically in human-readable and JSON forms.

It uses only the Python standard library and requires Python 3.12 or newer.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import shlex
import sys
from dataclasses import dataclass
from typing import Any, Optional, Sequence

MANIFEST_SCHEMA_VERSION = 3

# The closed role vocabulary. The manifest's ``roles`` list must be exactly
# this set; a target may declare only one of these roles.
KNOWN_ROLES = frozenset({"owner", "implementation", "support", "composition", "external"})

# The two package kinds an entry under ``manifest.owners`` may take.
KNOWN_OWNER_ROLES = frozenset({"owner", "support"})

# Evidence producers recorded by the manifest. Each evidence input carries a
# producer identity and a producer schema version that must match the manifest.
PRODUCER_OWNERSHIP_INDEX = "cch-parity-constructor"
PRODUCER_DIRECT_INCLUDES = "cch-parity-lexer"
PRODUCER_COMPILE_COMMANDS = "cmake-file-api"
PRODUCER_DEPFILES = "cch-compiler-depfile"

# Compiled translation-unit extensions: only these sources require a compile
# command and a depfile entry.
COMPILED_SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".c++", ".C", ".m", ".mm")

# Stable diagnostic rule IDs. Categories: 1xxx manifest fail-closed schema,
# 2xxx target/dependency policy, 3xxx evidence freshness and integrity,
# 4xxx direct-include spelling and canonical resolution, 5xxx compile context,
# 6xxx active dependency (depfile) evidence; 7xxx no-exception policy.
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
RULE_INCLUDE_QUOTE_SPELLING = "PARITY-4001"
RULE_INCLUDE_UNCLASSIFIED_ROOT = "PARITY-4002"
RULE_INCLUDE_PATH_ESCAPE = "PARITY-4003"
RULE_INCLUDE_CASE_CONFLICT = "PARITY-4004"
RULE_INCLUDE_AMBIGUOUS = "PARITY-4005"
RULE_INCLUDE_MACRO_GENERATED = "PARITY-4006"
RULE_ILLEGAL_DIRECT_INCLUDE = "PARITY-4007"
RULE_UNRESOLVED_PROJECT_INCLUDE = "PARITY-4008"
RULE_MISSING_INCLUDE_EVIDENCE = "PARITY-4009"
RULE_UNCLASSIFIED_EVIDENCE_SOURCE = "PARITY-4010"
RULE_STALE_INCLUDE_EVIDENCE = "PARITY-4011"
RULE_DUPLICATE_SOURCE_COMPILATION = "PARITY-5001"
RULE_MISSING_COMPILE_COMMAND = "PARITY-5002"
RULE_UNSUPPORTED_COMPILE_FLAG = "PARITY-5003"
RULE_UNDECLARED_FORCED_INCLUDE = "PARITY-5004"
RULE_OPAQUE_PCH_FORBIDDEN = "PARITY-5005"
RULE_UNSCANNED_FORCED_INCLUDE = "PARITY-5006"
RULE_UNKNOWN_COMPILER = "PARITY-5007"
RULE_PROJECT_TARGET_MASQUERADING_AS_EXTERNAL = "PARITY-5008"
RULE_MISSING_DEPFILE_EVIDENCE = "PARITY-6001"
RULE_STALE_DEPFILE_EVIDENCE = "PARITY-6002"
RULE_CONTRADICTORY_DEPFILE_EVIDENCE = "PARITY-6003"
RULE_FORBIDDEN_EXCEPTION_FLAG = "PARITY-7001"
RULE_EXCEPTION_ENABLED_TARGET = "PARITY-7002"
RULE_EXCEPTION_POINTER_NOT_ALLOWLISTED = "PARITY-7003"
RULE_EXCEPTION_RETHROW_FORBIDDEN = "PARITY-7004"
RULE_FORBIDDEN_HEADLESS_FRONTEND_INCLUDE = "PARITY-8001"
RULE_EXPIRED_ARCHITECTURE_EXCEPTION = "PARITY-8002"
RULE_SESSION_AI_PRIVATE_INCLUDE = "PARITY-8003"

# One stable diagnostic id per Product Architecture Contract rule (ADR 0039),
# keyed by the manifest rule id whose clause the violation names. A rule that has
# no entry here reports under the headless-frontend id.
_ARCHITECTURE_RULE_DIAGNOSTIC_IDS = {
    "headless-no-frontend-dependencies": RULE_FORBIDDEN_HEADLESS_FRONTEND_INCLUDE,
    "agent-no-ai-private-includes": RULE_SESSION_AI_PRIVATE_INCLUDE,
}


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
class ExceptionPolicy:
    schema_version: int
    required_compile_flags: tuple[str, ...]
    forbidden_compile_flags: tuple[str, ...]
    allowed_exception_ptr_sources: tuple[str, ...]
    forbidden_exception_calls: tuple[str, ...]


@dataclass(frozen=True)
class ArchitectureRule:
    rule_id: str
    source_prefixes: tuple[str, ...]
    excluded_source_prefixes: tuple[str, ...]
    forbidden_include_prefixes: tuple[str, ...]


@dataclass(frozen=True)
class ArchitectureException:
    exception_id: str
    rule_id: str
    source: str
    owner: str
    removal_ticket: str
    expires: str
    reason: str


@dataclass(frozen=True)
class ArchitectureContract:
    schema_version: int
    name: str
    rules: tuple[ArchitectureRule, ...]
    exceptions: tuple[ArchitectureException, ...]


@dataclass(frozen=True)
class Manifest:
    schema_version: int
    architecture_contract: ArchitectureContract
    owners: dict[str, Owner]
    roles: frozenset[str]
    external_families: frozenset[str]
    evidence: tuple[dict[str, Any], ...]
    exception_policy: ExceptionPolicy


@dataclass(frozen=True)
class Target:
    name: str
    role: str
    owner: Optional[str]
    sources: tuple[str, ...]
    dependencies: tuple[dict[str, Any], ...]
    forced_includes: tuple[str, ...] = ()
    pch_input: Optional[str] = None


@dataclass(frozen=True)
class Index:
    producer: str
    schema_version: int
    manifest_digest: str
    targets: tuple[Target, ...]


@dataclass(frozen=True)
class IncludeDirective:
    path: str
    spelling: str  # "angle" | "quote" | "macro"
    line: int
    macro: bool


@dataclass(frozen=True)
class ScannedSource:
    path: str
    digest: str
    includes: tuple[IncludeDirective, ...]


@dataclass(frozen=True)
class DirectIncludes:
    producer: str
    schema_version: int
    sources: tuple[ScannedSource, ...]


@dataclass(frozen=True)
class CompileCommand:
    file: str
    directory: str
    command: str


@dataclass(frozen=True)
class CompileCommands:
    commands: tuple[CompileCommand, ...]


@dataclass(frozen=True)
class DepfileEntry:
    source: str
    digest: str
    output: Optional[str] = None
    depfile: Optional[str] = None
    dependencies: tuple[str, ...] = ()


@dataclass(frozen=True)
class DepfileEvidence:
    producer: str
    schema_version: int
    config_digest: str
    entries: tuple[DepfileEntry, ...]
    deps_log: Optional[str] = None
    deps_log_digest: Optional[str] = None

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
            {
                "schema_version",
                "architecture_contract",
                "owners",
                "roles",
                "external_families",
                "evidence",
                "exception_policy",
            }
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

    architecture_contract_raw = _require_member(
        data, "architecture_contract", "manifest", RULE_MISSING_MANIFEST_FIELD
    )
    _require_object(
        architecture_contract_raw,
        "manifest.architecture_contract",
        RULE_INVALID_MANIFEST_VALUE,
    )
    architecture_contract_context = "manifest.architecture_contract"
    _check_unknown_keys(
        architecture_contract_raw,
        frozenset({"schema_version", "name", "rules", "exceptions"}),
        architecture_contract_context,
        RULE_UNKNOWN_MANIFEST_FIELD,
    )
    architecture_contract_schema = _require_int(
        _require_member(
            architecture_contract_raw,
            "schema_version",
            architecture_contract_context,
            RULE_MISSING_MANIFEST_FIELD,
        ),
        "schema_version",
        architecture_contract_context,
        RULE_INVALID_MANIFEST_VALUE,
    )
    if architecture_contract_schema != 1:
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            f"unknown architecture contract schema_version {architecture_contract_schema}; "
            "supported version is 1",
        )
    architecture_contract_name = _require_string(
        _require_member(
            architecture_contract_raw,
            "name",
            architecture_contract_context,
            RULE_MISSING_MANIFEST_FIELD,
        ),
        "name",
        architecture_contract_context,
        RULE_INVALID_MANIFEST_VALUE,
    )

    def _contract_paths(entry: dict[str, Any], field: str, context: str) -> tuple[str, ...]:
        values = _require_member(entry, field, context, RULE_MISSING_MANIFEST_FIELD)
        if not isinstance(values, list) or not values or any(
            not isinstance(value, str) or not value for value in values
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field '{field}' at {context} must be a non-empty list of strings",
            )
        result: list[str] = []
        for value in values:
            normalized = value.replace("\\", "/")
            if os.path.isabs(value) or ".." in normalized.split("/"):
                _fail(
                    RULE_INVALID_MANIFEST_VALUE,
                    f"contract path '{value}' at {context} must be repository-relative and "
                    "must not escape the project root",
                )
            result.append(normalized)
        if len(result) != len(set(result)):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field '{field}' at {context} contains duplicate entries",
            )
        return tuple(result)

    rules_raw = _require_member(
        architecture_contract_raw,
        "rules",
        architecture_contract_context,
        RULE_MISSING_MANIFEST_FIELD,
    )
    if not isinstance(rules_raw, list) or not rules_raw:
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            f"field 'rules' at {architecture_contract_context} must be a non-empty list",
        )
    architecture_rules: list[ArchitectureRule] = []
    rule_ids: set[str] = set()
    for position, entry in enumerate(rules_raw):
        rule_context = f"{architecture_contract_context}.rules[{position}]"
        _require_object(entry, rule_context, RULE_INVALID_MANIFEST_VALUE)
        _check_unknown_keys(
            entry,
            frozenset(
                {
                    "id",
                    "source_prefixes",
                    "excluded_source_prefixes",
                    "forbidden_include_prefixes",
                }
            ),
            rule_context,
            RULE_UNKNOWN_MANIFEST_FIELD,
        )
        rule_id = _require_string(
            _require_member(entry, "id", rule_context, RULE_MISSING_MANIFEST_FIELD),
            "id",
            rule_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        if rule_id in rule_ids:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture rule id '{rule_id}' is declared more than once",
            )
        rule_ids.add(rule_id)
        source_prefixes = _contract_paths(entry, "source_prefixes", rule_context)
        excluded_value = _require_member(
            entry, "excluded_source_prefixes", rule_context, RULE_MISSING_MANIFEST_FIELD
        )
        if not isinstance(excluded_value, list) or any(
            not isinstance(value, str) or not value for value in excluded_value
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field 'excluded_source_prefixes' at {rule_context} must be a list of strings",
            )
        excluded_source_prefixes = tuple(
            value.replace("\\", "/") for value in excluded_value
        )
        if len(excluded_source_prefixes) != len(set(excluded_source_prefixes)):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field 'excluded_source_prefixes' at {rule_context} contains duplicate entries",
            )
        for prefix in excluded_source_prefixes:
            if os.path.isabs(prefix) or ".." in prefix.split("/"):
                _fail(
                    RULE_INVALID_MANIFEST_VALUE,
                    f"contract path '{prefix}' at {rule_context} must be repository-relative and "
                    "must not escape the project root",
                )
        forbidden_include_prefixes = _contract_paths(
            entry, "forbidden_include_prefixes", rule_context
        )
        architecture_rules.append(
            ArchitectureRule(
                rule_id,
                source_prefixes,
                excluded_source_prefixes,
                forbidden_include_prefixes,
            )
        )

    exceptions_raw = _require_member(
        architecture_contract_raw,
        "exceptions",
        architecture_contract_context,
        RULE_MISSING_MANIFEST_FIELD,
    )
    if not isinstance(exceptions_raw, list):
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            f"field 'exceptions' at {architecture_contract_context} must be a list",
        )
    architecture_exceptions: list[ArchitectureException] = []
    exception_ids: set[str] = set()
    for position, entry in enumerate(exceptions_raw):
        exception_context = f"{architecture_contract_context}.exceptions[{position}]"
        _require_object(entry, exception_context, RULE_INVALID_MANIFEST_VALUE)
        _check_unknown_keys(
            entry,
            frozenset(
                {
                    "id",
                    "rule_id",
                    "source",
                    "owner",
                    "removal_ticket",
                    "expires",
                    "reason",
                }
            ),
            exception_context,
            RULE_UNKNOWN_MANIFEST_FIELD,
        )
        exception_id = _require_string(
            _require_member(entry, "id", exception_context, RULE_MISSING_MANIFEST_FIELD),
            "id",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        if exception_id in exception_ids:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception id '{exception_id}' is declared more than once",
            )
        exception_ids.add(exception_id)
        exception_rule_id = _require_string(
            _require_member(entry, "rule_id", exception_context, RULE_MISSING_MANIFEST_FIELD),
            "rule_id",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        if exception_rule_id not in rule_ids:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception '{exception_id}' references unknown rule "
                f"'{exception_rule_id}'",
            )
        exception_source = _require_string(
            _require_member(entry, "source", exception_context, RULE_MISSING_MANIFEST_FIELD),
            "source",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        ).replace("\\", "/")
        if os.path.isabs(exception_source) or ".." in exception_source.split("/"):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception source '{exception_source}' must be repository-relative "
                "and must not escape the project root",
            )
        matching_rule = next(rule for rule in architecture_rules if rule.rule_id == exception_rule_id)
        if not any(
            exception_source == prefix.rstrip("/")
            or exception_source.startswith(prefix.rstrip("/") + "/")
            for prefix in matching_rule.source_prefixes
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception '{exception_id}' source '{exception_source}' is outside "
                f"rule '{exception_rule_id}' source_prefixes",
            )
        exception_owner = _require_string(
            _require_member(entry, "owner", exception_context, RULE_MISSING_MANIFEST_FIELD),
            "owner",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        removal_ticket = _require_string(
            _require_member(
                entry, "removal_ticket", exception_context, RULE_MISSING_MANIFEST_FIELD
            ),
            "removal_ticket",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        if not removal_ticket.startswith("#") or not removal_ticket[1:].isdigit():
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception '{exception_id}' removal_ticket must be a GitHub "
                "issue reference such as '#623'",
            )
        expires = _require_string(
            _require_member(entry, "expires", exception_context, RULE_MISSING_MANIFEST_FIELD),
            "expires",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        try:
            parsed_expires = datetime.date.fromisoformat(expires)
        except ValueError:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception '{exception_id}' expires must be an ISO date",
            )
        if parsed_expires.isoformat() != expires:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"architecture exception '{exception_id}' expires must use YYYY-MM-DD",
            )
        reason = _require_string(
            _require_member(entry, "reason", exception_context, RULE_MISSING_MANIFEST_FIELD),
            "reason",
            exception_context,
            RULE_INVALID_MANIFEST_VALUE,
        )
        architecture_exceptions.append(
            ArchitectureException(
                exception_id,
                exception_rule_id,
                exception_source,
                exception_owner,
                removal_ticket,
                expires,
                reason,
            )
        )

    architecture_contract = ArchitectureContract(
        architecture_contract_schema,
        architecture_contract_name,
        tuple(architecture_rules),
        tuple(architecture_exceptions),
    )

    exception_policy_raw = _require_member(
        data, "exception_policy", "manifest", RULE_MISSING_MANIFEST_FIELD
    )
    _require_object(exception_policy_raw, "manifest.exception_policy", RULE_INVALID_MANIFEST_VALUE)
    exception_policy_context = "manifest.exception_policy"
    _check_unknown_keys(
        exception_policy_raw,
        frozenset(
            {
                "schema_version",
                "required_compile_flags",
                "forbidden_compile_flags",
                "allowed_exception_ptr_sources",
                "forbidden_exception_calls",
            }
        ),
        exception_policy_context,
        RULE_UNKNOWN_MANIFEST_FIELD,
    )
    exception_policy_schema = _require_int(
        _require_member(
            exception_policy_raw,
            "schema_version",
            exception_policy_context,
            RULE_MISSING_MANIFEST_FIELD,
        ),
        "schema_version",
        exception_policy_context,
        RULE_INVALID_MANIFEST_VALUE,
    )
    if exception_policy_schema != 1:
        _fail(
            RULE_INVALID_MANIFEST_VALUE,
            f"unknown exception policy schema_version {exception_policy_schema}; supported version is 1",
        )

    def _policy_strings(field: str) -> tuple[str, ...]:
        values = _require_member(
            exception_policy_raw, field, exception_policy_context, RULE_MISSING_MANIFEST_FIELD
        )
        if not isinstance(values, list) or any(
            not isinstance(value, str) or not value for value in values
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field '{field}' at {exception_policy_context} must be a list of "
                "non-empty strings",
            )
        if len(values) != len(set(values)):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"field '{field}' at {exception_policy_context} contains duplicate entries",
            )
        return tuple(values)

    exception_policy = ExceptionPolicy(
        exception_policy_schema,
        _policy_strings("required_compile_flags"),
        _policy_strings("forbidden_compile_flags"),
        _policy_strings("allowed_exception_ptr_sources"),
        _policy_strings("forbidden_exception_calls"),
    )
    for source in exception_policy.allowed_exception_ptr_sources:
        source_parts = source.replace("\\", "/").split("/")
        if os.path.isabs(source) or ".." in source_parts:
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"exception allowlist path '{source}' must be repository-relative and "
                "must not escape the project root",
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
        # An interface root is owner-local: it may live anywhere inside the
        # repository, but it must terminate in the `cch/<subdir>` directory
        # that gives the canonical <cch/...> include spelling. A root without
        # that tail cannot provide the canonical spelling and is rejected.
        interface_parts = interface_root.replace("\\", "/").split("/")
        if (
            os.path.isabs(interface_root)
            or ".." in interface_parts
            or len(interface_parts) < 2
            or interface_parts[-2] != "cch"
            or interface_parts[-1] in ("", "cch")
        ):
            _fail(
                RULE_INVALID_MANIFEST_VALUE,
                f"owner '{name}' declares interface_root '{interface_root}'; an interface root "
                f"must be a repository-relative path ending in 'cch/<subdir>' so the canonical "
                f"<cch/...> spelling resolves through it",
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

    return Manifest(
        schema_version,
        architecture_contract,
        owners,
        roles,
        frozenset(families),
        tuple(evidence),
        exception_policy,
    )


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
            frozenset({"name", "role", "owner", "sources", "dependencies", "forced_includes", "pch_input"}),
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
                dep, frozenset({"name", "family", "visibility"}), dep_context, RULE_UNKNOWN_INDEX_FIELD
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
            # ``visibility`` records whether the dependency is interface-visible
            # (PUBLIC link, ``"public"``) or private (``"private"``). It is
            # optional in the index schema (older producers omit it) and
            # defaults to private; a present value must be one of the two.
            dep_visibility = dep.get("visibility", "private")
            if dep_visibility not in ("public", "private"):
                _fail(
                    RULE_INVALID_INDEX_VALUE,
                    f"field 'visibility' at {dep_context} must be 'public' or 'private'",
                )
            dependencies.append({"name": dep_name, "family": dep_family, "visibility": dep_visibility})

        forced_includes_value = entry.get("forced_includes", [])
        if not isinstance(forced_includes_value, list) or any(
            not isinstance(item, str) for item in forced_includes_value
        ):
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"field 'forced_includes' at {target_context} must be a list of strings",
            )
        pch_input = entry.get("pch_input")
        if pch_input is not None and not isinstance(pch_input, str):
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"field 'pch_input' at {target_context} must be a string or null",
            )

        if name in seen_targets:
            _fail(
                RULE_INVALID_INDEX_VALUE,
                f"invalid index value: target '{name}' is declared more than once",
            )
        seen_targets.add(name)
        targets.append(
            Target(
                name,
                role,
                owner_value,
                tuple(sources_value),
                tuple(dependencies),
                tuple(forced_includes_value),
                pch_input,
            )
        )

    return Index(producer, schema_version, manifest_digest, tuple(targets))


# ---------------------------------------------------------------------------
# Direct-include lexer
#
# The lexer is textual: it scans every source line for a preprocessor
# ``#include`` directive without evaluating ``#if`` conditionals, so an illegal
# include cannot hide in a disabled branch. It records the header name, the
# spelling (angle, quote, or macro-generated), and the 1-based line number.
# ---------------------------------------------------------------------------


def lex_includes(text: str, source: str) -> tuple[IncludeDirective, ...]:
    result: list[IncludeDirective] = []
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line.startswith("#"):
            continue
        rest = line[1:].lstrip()
        if not rest.startswith("include"):
            continue
        tail = rest[len("include"):]
        # `include_next` and other directives beginning with "include" are not
        # the `#include` directive the Gate scans.
        if tail and not tail[0].isspace():
            continue
        rest = tail.lstrip()
        if rest.startswith("<"):
            end = rest.find(">")
            if end == -1:
                continue
            path = rest[1:end]
            spelling = "angle"
            macro = False
        elif rest.startswith('"'):
            end = rest.find('"', 1)
            if end == -1:
                continue
            path = rest[1:end]
            spelling = "quote"
            macro = False
        else:
            # A bare token sequence such as `#include HEADER` or
            # `#include STR(MACRO)` is a macro-generated include.
            first = rest.split()
            path = first[0] if first else ""
            spelling = "macro"
            macro = True
        result.append(IncludeDirective(path, spelling, lineno, macro))
    return tuple(result)


def parse_direct_includes(data: Any) -> DirectIncludes:
    _require_object(data, "direct-includes", RULE_MALFORMED_EVIDENCE)
    _check_unknown_keys(
        data,
        frozenset({"producer", "schema_version", "sources"}),
        "direct-includes",
        RULE_MALFORMED_EVIDENCE,
    )
    producer = _require_string(
        _require_member(data, "producer", "direct-includes", RULE_MALFORMED_EVIDENCE),
        "producer",
        "direct-includes",
        RULE_MALFORMED_EVIDENCE,
    )
    schema_version = _require_int(
        _require_member(data, "schema_version", "direct-includes", RULE_MALFORMED_EVIDENCE),
        "schema_version",
        "direct-includes",
        RULE_MALFORMED_EVIDENCE,
    )
    sources_raw = _require_member(data, "sources", "direct-includes", RULE_MALFORMED_EVIDENCE)
    if not isinstance(sources_raw, list):
        _fail(RULE_MALFORMED_EVIDENCE, "field 'sources' at direct-includes must be a list")
    sources: list[ScannedSource] = []
    seen: set[str] = set()
    for position, entry in enumerate(sources_raw):
        context = f"direct-includes.sources[{position}]"
        _require_object(entry, context, RULE_MALFORMED_EVIDENCE)
        _check_unknown_keys(
            entry, frozenset({"path", "digest", "includes"}), context, RULE_MALFORMED_EVIDENCE
        )
        path = _require_string(
            _require_member(entry, "path", context, RULE_MALFORMED_EVIDENCE),
            "path",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        digest = _require_string(
            _require_member(entry, "digest", context, RULE_MALFORMED_EVIDENCE),
            "digest",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        includes_raw = _require_member(entry, "includes", context, RULE_MALFORMED_EVIDENCE)
        if not isinstance(includes_raw, list):
            _fail(RULE_MALFORMED_EVIDENCE, f"field 'includes' at {context} must be a list")
        includes: list[IncludeDirective] = []
        for inc_position, inc in enumerate(includes_raw):
            inc_context = f"{context}.includes[{inc_position}]"
            _require_object(inc, inc_context, RULE_MALFORMED_EVIDENCE)
            _check_unknown_keys(
                inc, frozenset({"path", "spelling", "line", "macro"}), inc_context, RULE_MALFORMED_EVIDENCE
            )
            inc_path = _require_string(
                _require_member(inc, "path", inc_context, RULE_MALFORMED_EVIDENCE),
                "path",
                inc_context,
                RULE_MALFORMED_EVIDENCE,
            )
            spelling = _require_string(
                _require_member(inc, "spelling", inc_context, RULE_MALFORMED_EVIDENCE),
                "spelling",
                inc_context,
                RULE_MALFORMED_EVIDENCE,
            )
            if spelling not in ("angle", "quote", "macro"):
                _fail(RULE_MALFORMED_EVIDENCE, f"field 'spelling' at {inc_context} must be angle, quote, or macro")
            line = _require_int(
                _require_member(inc, "line", inc_context, RULE_MALFORMED_EVIDENCE),
                "line",
                inc_context,
                RULE_MALFORMED_EVIDENCE,
            )
            macro = _require_member(inc, "macro", inc_context, RULE_MALFORMED_EVIDENCE)
            if not isinstance(macro, bool):
                _fail(RULE_MALFORMED_EVIDENCE, f"field 'macro' at {inc_context} must be a boolean")
            includes.append(IncludeDirective(inc_path, spelling, line, macro))
        if path in seen:
            _fail(RULE_MALFORMED_EVIDENCE, f"scanned source '{path}' is declared more than once")
        seen.add(path)
        sources.append(ScannedSource(path, digest, tuple(includes)))
    return DirectIncludes(producer, schema_version, tuple(sources))


def parse_compile_commands(data: Any) -> CompileCommands:
    # The CMake File API / compile_commands.json format is a plain JSON list;
    # it carries no producer or schema version of its own.
    if not isinstance(data, list):
        _fail(RULE_MALFORMED_EVIDENCE, "compile-commands evidence must be a JSON list")
    commands: list[CompileCommand] = []
    for position, entry in enumerate(data):
        context = f"compile-commands[{position}]"
        _require_object(entry, context, RULE_MALFORMED_EVIDENCE)
        file = _require_string(
            _require_member(entry, "file", context, RULE_MALFORMED_EVIDENCE),
            "file",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        directory = _require_string(
            _require_member(entry, "directory", context, RULE_MALFORMED_EVIDENCE),
            "directory",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        command = _require_string(
            _require_member(entry, "command", context, RULE_MALFORMED_EVIDENCE),
            "command",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        commands.append(CompileCommand(file, directory, command))
    return CompileCommands(tuple(commands))


def parse_depfile_evidence(data: Any) -> DepfileEvidence:
    _require_object(data, "depfiles", RULE_MALFORMED_EVIDENCE)
    _check_unknown_keys(
        data,
        frozenset({"producer", "schema_version", "config_digest", "entries", "deps_log", "deps_log_digest"}),
        "depfiles",
        RULE_MALFORMED_EVIDENCE,
    )
    producer = _require_string(
        _require_member(data, "producer", "depfiles", RULE_MALFORMED_EVIDENCE),
        "producer",
        "depfiles",
        RULE_MALFORMED_EVIDENCE,
    )
    schema_version = _require_int(
        _require_member(data, "schema_version", "depfiles", RULE_MALFORMED_EVIDENCE),
        "schema_version",
        "depfiles",
        RULE_MALFORMED_EVIDENCE,
    )
    config_digest = _require_string(
        _require_member(data, "config_digest", "depfiles", RULE_MALFORMED_EVIDENCE),
        "config_digest",
        "depfiles",
        RULE_MALFORMED_EVIDENCE,
    )
    deps_log = None
    if "deps_log" in data and data["deps_log"] is not None:
        deps_log = _require_string(data["deps_log"], "deps_log", "depfiles", RULE_MALFORMED_EVIDENCE)
    deps_log_digest = None
    if "deps_log_digest" in data and data["deps_log_digest"] is not None:
        deps_log_digest = _require_string(
            data["deps_log_digest"], "deps_log_digest", "depfiles", RULE_MALFORMED_EVIDENCE
        )

    entries_raw = _require_member(data, "entries", "depfiles", RULE_MALFORMED_EVIDENCE)
    if not isinstance(entries_raw, list):
        _fail(RULE_MALFORMED_EVIDENCE, "field 'entries' at depfiles must be a list")
    entries: list[DepfileEntry] = []
    seen: set[str] = set()
    for position, entry in enumerate(entries_raw):
        context = f"depfiles.entries[{position}]"
        _require_object(entry, context, RULE_MALFORMED_EVIDENCE)
        _check_unknown_keys(
            entry,
            frozenset({"source", "output", "digest", "dependencies", "depfile"}),
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        source = _require_string(
            _require_member(entry, "source", context, RULE_MALFORMED_EVIDENCE),
            "source",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        digest = _require_string(
            _require_member(entry, "digest", context, RULE_MALFORMED_EVIDENCE),
            "digest",
            context,
            RULE_MALFORMED_EVIDENCE,
        )
        output: Optional[str] = None
        if "output" in entry and entry["output"] is not None:
            output = _require_string(entry["output"], "output", context, RULE_MALFORMED_EVIDENCE)
        depfile: Optional[str] = None
        if "depfile" in entry and entry["depfile"] is not None:
            depfile = _require_string(entry["depfile"], "depfile", context, RULE_MALFORMED_EVIDENCE)
        if output is None and depfile is None:
            _fail(RULE_MALFORMED_EVIDENCE, f"field 'output' is required at {context}")
        dependencies: tuple[str, ...] = ()
        if "dependencies" in entry and entry["dependencies"] is not None:
            deps_raw = entry["dependencies"]
            if not isinstance(deps_raw, list):
                _fail(RULE_MALFORMED_EVIDENCE, f"field 'dependencies' at {context} must be a list")
            dependencies = tuple(
                _require_string(d, f"dependencies[{i}]", context, RULE_MALFORMED_EVIDENCE)
                for i, d in enumerate(deps_raw)
            )
        if source in seen:
            _fail(RULE_MALFORMED_EVIDENCE, f"depfile entry for '{source}' is declared more than once")
        seen.add(source)
        entries.append(
            DepfileEntry(
                source=source,
                digest=digest,
                output=output,
                depfile=depfile,
                dependencies=dependencies,
            )
        )
    return DepfileEvidence(
        producer=producer,
        schema_version=schema_version,
        config_digest=config_digest,
        entries=tuple(entries),
        deps_log=deps_log,
        deps_log_digest=deps_log_digest,
    )


# ---------------------------------------------------------------------------
# Canonical project-header resolution
# ---------------------------------------------------------------------------


def _norm_path(path: str) -> str:
    return os.path.normpath(os.path.abspath(path))


def _interface_prefixes(manifest: Manifest) -> dict[str, str]:
    """Map owner name to its canonical include prefix (e.g. ``cch_ai`` -> ``cch/ai``).

    The prefix is the trailing ``cch/<subdir>`` of the declared interface
    root; manifest validation guarantees that shape.
    """
    result: dict[str, str] = {}
    for name, owner in manifest.owners.items():
        parts = owner.interface_root.replace("\\", "/").split("/")
        result[name] = "/".join(parts[-2:])
    return result


def _walk_interface_headers(project_root: str, interface_root: str) -> set[str]:
    base = os.path.join(project_root, interface_root)
    result: set[str] = set()
    if not os.path.isdir(base):
        return result
    for dirpath, _dirnames, filenames in os.walk(base):
        for filename in filenames:
            full = os.path.join(dirpath, filename)
            rel = os.path.relpath(full, base)
            result.add(rel.replace(os.sep, "/"))
    return result


def _basename_map(headers_by_owner: dict[str, set[str]]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for owner, rels in headers_by_owner.items():
        for rel in rels:
            base = rel.split("/")[-1].lower()
            result.setdefault(base, set()).add(owner)
    return result


def _find_case_insensitive(path: str) -> Optional[str]:
    """Return the real path matching ``path`` case-insensitively, if any."""
    current = os.path.abspath(os.sep)
    for part in os.path.normpath(path).split(os.sep):
        if part in ("", "."):
            continue
        if part == "..":
            current = os.path.dirname(current)
            continue
        try:
            entries = os.listdir(current)
        except OSError:
            return None
        match = None
        for entry in entries:
            if entry == part:
                match = entry
                break
        if match is None:
            for entry in entries:
                if entry.lower() == part.lower():
                    match = entry
                    break
        if match is None:
            return None
        current = os.path.join(current, match)
    return os.path.normpath(current)


def _resolve_include(
    include: IncludeDirective,
    manifest: Manifest,
    project_root: Optional[str],
    headers_by_owner: dict[str, set[str]],
    basename_map: dict[str, set[str]],
) -> tuple[Optional[str], Optional[Diagnostic]]:
    """Resolve one include to its owning package, or return a spelling diagnostic.

    Returns ``(owner_name, None)`` for a canonical, resolvable project header,
    ``(None, None)`` for a non-project (system or third-party) header, and
    ``(None, diagnostic)`` for a rejected project-header spelling.
    """
    path = include.path
    if include.macro or include.spelling == "macro":
        return None, Diagnostic(
            RULE_INCLUDE_MACRO_GENERATED,
            f"macro-generated include '{path}' is not a canonical project-header spelling",
        )
    norm = path.replace("\\", "/")
    if os.path.isabs(path):
        return None, Diagnostic(
            RULE_INCLUDE_PATH_ESCAPE,
            f"absolute include path '{path}' bypasses declared include roots",
        )
    if any(part == ".." for part in norm.split("/")):
        return None, Diagnostic(
            RULE_INCLUDE_PATH_ESCAPE,
            f"include path '{path}' escapes declared include roots via '..'",
        )

    if norm.startswith("cch/"):
        if include.spelling == "quote":
            return None, Diagnostic(
                RULE_INCLUDE_QUOTE_SPELLING,
                f"project header '{path}' must use the canonical <cch/...> angle spelling",
            )
        rest = norm[len("cch/"):]
        parts = rest.split("/")
        if len(parts) < 2:
            return None, Diagnostic(
                RULE_INCLUDE_UNCLASSIFIED_ROOT,
                f"include '{path}' does not name a header under a declared interface root",
            )
        subdir = parts[0]
        prefix_to_owner = {p: n for n, p in _interface_prefixes(manifest).items()}
        owner_name = prefix_to_owner.get("cch/" + subdir)
        if owner_name is None:
            lower_map = {p.lower(): n for p, n in prefix_to_owner.items()}
            candidate = lower_map.get(("cch/" + subdir).lower())
            if candidate is not None:
                canonical_subdir = _interface_prefixes(manifest)[candidate].split("/")[1]
                return None, Diagnostic(
                    RULE_INCLUDE_CASE_CONFLICT,
                    f"include '{path}' uses case-conflicting root '{subdir}'; the canonical "
                    f"spelling is '{canonical_subdir}'",
                )
            return None, Diagnostic(
                RULE_INCLUDE_UNCLASSIFIED_ROOT,
                f"include '{path}' names an unclassified interface root '{subdir}'",
            )
        owner = manifest.owners[owner_name]
        rel = "/".join(parts[1:])
        if project_root is None:
            return owner_name, None
        header_abs = os.path.join(project_root, owner.interface_root, rel.replace("/", os.sep))
        root_abs = os.path.join(project_root, owner.interface_root)
        root_real = os.path.realpath(root_abs)
        if not os.path.isfile(header_abs):
            actual = _find_case_insensitive(header_abs)
            if actual is not None and actual != header_abs:
                return None, Diagnostic(
                    RULE_INCLUDE_CASE_CONFLICT,
                    f"include '{path}' does not match the on-disk case; found "
                    f"'{os.path.relpath(actual, project_root)}'",
                )
            return None, Diagnostic(
                RULE_UNRESOLVED_PROJECT_INCLUDE,
                f"project header '{path}' does not resolve to a file under '{owner.interface_root}'",
            )
        real = os.path.realpath(header_abs)
        if real != root_real and not real.startswith(root_real + os.sep):
            return None, Diagnostic(
                RULE_INCLUDE_PATH_ESCAPE,
                f"include '{path}' resolves through a symlink outside '{owner.interface_root}'",
            )
        conflicts = []
        for other_name in manifest.owners:
            if other_name == owner_name:
                continue
            other_abs = os.path.join(
                project_root, manifest.owners[other_name].interface_root, rel.replace("/", os.sep)
            )
            if os.path.isfile(other_abs):
                conflicts.append(other_name)
        if conflicts:
            return None, Diagnostic(
                RULE_INCLUDE_AMBIGUOUS,
                f"include '{path}' is ambiguous: it also resolves under owners {sorted(conflicts)}",
            )
        return owner_name, None

    # Non-``cch/`` path: detect basename and relative alternate spellings of a
    # declared interface header.
    if "/" not in norm:
        if basename_map.get(norm.lower()):
            return None, Diagnostic(
                RULE_INCLUDE_UNCLASSIFIED_ROOT,
                f"include '{path}' is a basename alternate spelling; use the canonical "
                f"<cch/...> form",
            )
        return None, None
    for owner_name, prefix in _interface_prefixes(manifest).items():
        subdir = prefix.split("/")[1] if "/" in prefix else prefix
        if norm.startswith(subdir + "/"):
            rel = norm[len(subdir) + 1:]
            if rel in headers_by_owner.get(owner_name, set()):
                return None, Diagnostic(
                    RULE_INCLUDE_UNCLASSIFIED_ROOT,
                    f"include '{path}' is an alternate relative spelling; use the canonical "
                    f"<{prefix}/{rel}> form",
                )
    return None, None


def _include_edge_diagnostic(
    from_owner: str,
    to_owner: str,
    manifest: Manifest,
    source: str,
    include_path: str,
    line: int,
) -> Optional[Diagnostic]:
    if from_owner == to_owner:
        return None
    from_entry = manifest.owners.get(from_owner)
    to_entry = manifest.owners.get(to_owner)
    if from_entry is None or to_entry is None:
        return None
    if to_entry.role == "support":
        return None
    if from_entry.role == "support":
        return Diagnostic(
            RULE_ILLEGAL_DIRECT_INCLUDE,
            f"support source '{source}' (owner '{from_owner}') includes owner header "
            f"'{include_path}' (owner '{to_owner}'); support owns no Supported Capability",
            target=from_owner,
            dependency=to_owner,
            path=f"{source}:{line}",
        )
    if to_owner not in from_entry.legal_owner_dependencies:
        return Diagnostic(
            RULE_ILLEGAL_DIRECT_INCLUDE,
            f"illegal direct include: '{source}' (owner '{from_owner}') includes "
            f"'{include_path}' (owner '{to_owner}'); '{to_owner}' is not in "
            f"'{from_owner}'s legal owner dependencies "
            f"{sorted(from_entry.legal_owner_dependencies)}",
            target=from_owner,
            dependency=to_owner,
            path=f"{source}:{line}",
        )
    return None


# ---------------------------------------------------------------------------
# Compile context and depfile validation helpers
# ---------------------------------------------------------------------------


def _is_compiled_source(path: str) -> bool:
    return path.endswith(COMPILED_SOURCE_EXTS)


def _is_supported_compiler(token: str) -> bool:
    base = os.path.basename(token).lower()
    for name in ("gcc", "g++", "clang", "clang++", "cc", "c++"):
        if base == name:
            return True
        if base.startswith(name + "-"):
            return True
        if base.startswith(name) and base[len(name):].isdigit():
            return True
    return False


def _split_command(command: str) -> list[str]:
    try:
        return shlex.split(command)
    except ValueError:
        return command.split()


def _is_project_path(
    arg: str,
    directory: str,
    project_root: Optional[str],
    external_include_roots: Sequence[str] = (),
) -> bool:
    """True when `arg` names a path under the project source tree.

    Paths under a declared external include root (for example the vcpkg
    installed-dependency include directory, which lives under the build tree
    inside the project root) are external dependency locations, not project
    ownership, so they never count as project paths.
    """
    if project_root is None:
        return False
    candidate = arg if os.path.isabs(arg) else os.path.join(directory, arg)
    real_root = os.path.realpath(project_root)
    real = os.path.realpath(candidate)
    if real == real_root or real.startswith(real_root + os.sep):
        for external_root in external_include_roots:
            real_external = os.path.realpath(external_root)
            if real == real_external or real.startswith(real_external + os.sep):
                return False
        return True
    return False


def _scan_compile_tokens(
    tokens: list[str],
    target: Optional[Target],
    source: str,
    directory: str,
    project_root: Optional[str],
    diagnostics: list[Diagnostic],
    external_include_roots: Sequence[str] = (),
) -> None:
    forced_declared: set[str] = set()
    pch_declared: Optional[str] = None
    if target is not None:
        forced_declared = {_norm_path(f) for f in target.forced_includes}
        pch_declared = _norm_path(target.pch_input) if target.pch_input else None

    forced_two_arg = {"-include", "-imacros", "-include-pch"}
    unsupported_two_arg = {
        "-isystem", "-idirafter", "-iquote", "-iframework",
        "-iprefix", "-iwithprefix", "-ivfsoverlay", "-isysroot",
    }
    i = 0
    n = len(tokens)
    while i < n:
        tok = tokens[i]
        if tok in forced_two_arg:
            arg = tokens[i + 1] if i + 1 < n else ""
            if tok == "-include-pch":
                diagnostics.append(
                    Diagnostic(
                        RULE_OPAQUE_PCH_FORBIDDEN,
                        f"opaque precompiled header '{arg}' is forbidden; declare the PCH input "
                        f"source and let the compiler build it",
                        path=source,
                    )
                )
            elif arg.endswith(".gch") or arg.endswith(".pch"):
                diagnostics.append(
                    Diagnostic(
                        RULE_OPAQUE_PCH_FORBIDDEN,
                        f"opaque precompiled header artifact '{arg}' is forbidden; declare and "
                        f"scan the PCH input source",
                        path=source,
                    )
                )
            elif target is not None and _norm_path(arg) not in forced_declared and _norm_path(arg) != pch_declared:
                diagnostics.append(
                    Diagnostic(
                        RULE_UNDECLARED_FORCED_INCLUDE,
                        f"forced include '{arg}' is not declared by target '{target.name}'",
                        path=source,
                    )
                )
            i += 2
            continue
        if tok in unsupported_two_arg:
            arg = tokens[i + 1] if i + 1 < n else ""
            if _is_project_path(arg, directory, project_root, external_include_roots):
                diagnostics.append(
                    Diagnostic(
                        RULE_UNSUPPORTED_COMPILE_FLAG,
                        f"unsupported include-affecting flag '{tok} {arg}' references a project path",
                        path=source,
                    )
                )
            i += 2
            continue
        matched_unsupported = False
        for flag in unsupported_two_arg:
            if tok.startswith(flag + "="):
                arg = tok[len(flag) + 1:]
                if _is_project_path(arg, directory, project_root, external_include_roots):
                    diagnostics.append(
                        Diagnostic(
                            RULE_UNSUPPORTED_COMPILE_FLAG,
                            f"unsupported include-affecting flag '{flag}={arg}' references a "
                            f"project path",
                            path=source,
                        )
                    )
                matched_unsupported = True
                break
        if matched_unsupported:
            i += 1
            continue
        if tok.endswith(".gch") or tok.endswith(".pch"):
            diagnostics.append(
                Diagnostic(
                    RULE_OPAQUE_PCH_FORBIDDEN,
                    f"opaque precompiled header artifact '{tok}' is forbidden; declare and scan "
                    f"the PCH input source",
                    path=source,
                )
            )
        i += 1


def _check_evidence_reference(
    manifest: Manifest, producer: str, schema_version: int, diagnostics: list[Diagnostic]
) -> None:
    evidence = next((entry for entry in manifest.evidence if entry["producer"] == producer), None)
    if evidence is None:
        diagnostics.append(
            Diagnostic(
                RULE_INVALID_INDEX_VALUE,
                f"evidence producer '{producer}' is not declared by any manifest evidence "
                f"reference",
            )
        )
        return
    expected = evidence["producer_schema_version"]
    if schema_version < expected:
        diagnostics.append(
            Diagnostic(
                RULE_STALE_PRODUCER_SCHEMA,
                f"stale evidence: producer schema_version {schema_version} is older than the "
                f"declared {expected} for producer '{producer}'",
            )
        )
    elif schema_version > expected:
        diagnostics.append(
            Diagnostic(
                RULE_UNKNOWN_INDEX_VERSION,
                f"unknown schema_version {schema_version} for producer '{producer}'; the manifest "
                f"declares {expected}",
            )
        )


def _path_matches_prefix(path: str, prefix: str) -> bool:
    normalized_path = path.replace("\\", "/")
    normalized_prefix = prefix.replace("\\", "/").rstrip("/")
    return normalized_path == normalized_prefix or normalized_path.startswith(normalized_prefix + "/")


def _relative_project_path(path: str, project_root: Optional[str]) -> str:
    if project_root is None:
        return path.replace("\\", "/")
    candidate = path if os.path.isabs(path) else os.path.join(project_root, path)
    return os.path.relpath(candidate, project_root).replace(os.sep, "/")


def _architecture_exception_matches(
    contract: ArchitectureContract,
    rule_id: str,
    source: str,
) -> bool:
    return any(
        exception.rule_id == rule_id and exception.source == source
        for exception in contract.exceptions
    )


def _architecture_include_diagnostic(
    manifest: Manifest,
    source: str,
    include_path: str,
    include_line: int,
    target_name: str,
    project_root: Optional[str],
) -> Optional[Diagnostic]:
    relative_source = _relative_project_path(source, project_root)
    normalized_include = include_path.replace("\\", "/")
    for rule in manifest.architecture_contract.rules:
        if not any(
            _path_matches_prefix(relative_source, prefix)
            for prefix in rule.source_prefixes
        ):
            continue
        if any(
            _path_matches_prefix(relative_source, prefix)
            for prefix in rule.excluded_source_prefixes
        ):
            continue
        if not any(
            _path_matches_prefix(normalized_include, prefix)
            for prefix in rule.forbidden_include_prefixes
        ):
            continue
        if _architecture_exception_matches(
            manifest.architecture_contract, rule.rule_id, relative_source
        ):
            continue
        return Diagnostic(
            _ARCHITECTURE_RULE_DIAGNOSTIC_IDS.get(
                rule.rule_id, RULE_FORBIDDEN_HEADLESS_FRONTEND_INCLUDE
            ),
            f"Product Architecture Contract rule '{rule.rule_id}' forbids source "
            f"'{relative_source}' from including header '{include_path}'",
            target=target_name,
            dependency=include_path,
            path=f"{source}:{include_line}",
        )
    return None


def _check_architecture_exception_dates(
    manifest: Manifest,
    diagnostics: list[Diagnostic],
) -> None:
    today = datetime.date.today()
    for exception in manifest.architecture_contract.exceptions:
        if datetime.date.fromisoformat(exception.expires) < today:
            diagnostics.append(
                Diagnostic(
                    RULE_EXPIRED_ARCHITECTURE_EXCEPTION,
                    f"architecture exception '{exception.exception_id}' expired on "
                    f"{exception.expires}; remove it or renew it with an owner and removal ticket",
                    path=exception.source,
                )
            )


def _check_includes(
    manifest: Manifest,
    index: Index,
    direct_includes: Optional[DirectIncludes],
    project_root: Optional[str],
    diagnostics: list[Diagnostic],
) -> None:
    if direct_includes is None:
        return

    source_owner: dict[str, str] = {}
    source_target: dict[str, str] = {}
    forced_owner: dict[str, str] = {}
    pch_owner: dict[str, str] = {}
    compiled_sources: set[str] = set()
    for target in index.targets:
        if target.role == "external":
            continue
        owner = target.owner or ""
        for source in target.sources:
            normalized_source = _norm_path(source)
            source_owner[normalized_source] = owner
            source_target[normalized_source] = target.name
            if _is_compiled_source(source):
                compiled_sources.add(_norm_path(source))
        for forced in target.forced_includes:
            forced_owner[_norm_path(forced)] = owner
        if target.pch_input:
            pch_owner[_norm_path(target.pch_input)] = owner

    scanned_by_path = {_norm_path(s.path): s for s in direct_includes.sources}

    for source in sorted(compiled_sources):
        if source not in scanned_by_path:
            diagnostics.append(
                Diagnostic(
                    RULE_MISSING_INCLUDE_EVIDENCE,
                    f"compiled source '{source}' has no direct-include scan entry",
                    path=source,
                )
            )
    for forced in sorted(forced_owner):
        if forced not in scanned_by_path:
            diagnostics.append(
                Diagnostic(
                    RULE_UNSCANNED_FORCED_INCLUDE,
                    f"declared forced include '{forced}' was not scanned by the lexer",
                    path=forced,
                )
            )
    for pch in sorted(pch_owner):
        if pch not in scanned_by_path:
            diagnostics.append(
                Diagnostic(
                    RULE_UNSCANNED_FORCED_INCLUDE,
                    f"declared PCH input '{pch}' was not scanned by the lexer",
                    path=pch,
                )
            )

    headers_by_owner: dict[str, set[str]] = {}
    basename_map: dict[str, set[str]] = {}
    if project_root is not None:
        for name, owner in manifest.owners.items():
            headers_by_owner[name] = _walk_interface_headers(project_root, owner.interface_root)
        basename_map = _basename_map(headers_by_owner)

    for scanned in direct_includes.sources:
        spath = _norm_path(scanned.path)
        if spath in source_owner:
            from_owner = source_owner[spath]
            from_target = source_target[spath]
        elif spath in forced_owner:
            from_owner = forced_owner[spath]
            from_target = from_owner
        elif spath in pch_owner:
            from_owner = pch_owner[spath]
            from_target = from_owner
        else:
            diagnostics.append(
                Diagnostic(
                    RULE_UNCLASSIFIED_EVIDENCE_SOURCE,
                    f"scanned file '{scanned.path}' is not declared by any target",
                    path=scanned.path,
                )
            )
            continue

        if os.path.exists(scanned.path) and scanned.digest:
            current = hashlib.sha256(_read_bytes(scanned.path)).hexdigest()
            if current != scanned.digest:
                diagnostics.append(
                    Diagnostic(
                        RULE_STALE_INCLUDE_EVIDENCE,
                        f"source '{scanned.path}' changed after the include scan; rescan the "
                        f"direct-include evidence",
                        path=scanned.path,
                    )
                )

        for include in scanned.includes:
            to_owner, diag = _resolve_include(
                include, manifest, project_root, headers_by_owner, basename_map
            )
            if diag is not None:
                diagnostics.append(
                    Diagnostic(
                        diag.rule_id,
                        diag.message,
                        target=from_owner,
                        path=f"{scanned.path}:{include.line}",
                    )
                )
            else:
                architecture_diag = _architecture_include_diagnostic(
                    manifest,
                    scanned.path,
                    include.path,
                    include.line,
                    from_target,
                    project_root,
                )
                if architecture_diag is not None:
                    diagnostics.append(architecture_diag)
                if to_owner is not None:
                    edge = _include_edge_diagnostic(
                        from_owner, to_owner, manifest, scanned.path, include.path, include.line
                    )
                    if edge is not None:
                        diagnostics.append(edge)


def _check_exception_sources(
    manifest: Manifest,
    project_root: Optional[str],
    diagnostics: list[Diagnostic],
) -> None:
    if project_root is None:
        return

    source_root = os.path.join(project_root, "src")
    if not os.path.isdir(source_root):
        return
    allowed = {
        os.path.normpath(os.path.join(project_root, source))
        for source in manifest.exception_policy.allowed_exception_ptr_sources
    }
    for root, directories, files in os.walk(source_root):
        directories.sort()
        for filename in sorted(files):
            if not filename.endswith((".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")):
                continue
            path = os.path.join(root, filename)
            text = _read_bytes(path).decode("utf-8", errors="replace")
            relative = os.path.relpath(path, project_root)
            if "std::exception_ptr" in text and os.path.normpath(path) not in allowed:
                diagnostics.append(
                    Diagnostic(
                        RULE_EXCEPTION_POINTER_NOT_ALLOWLISTED,
                        f"std::exception_ptr is allowed only in a private completion bridge; "
                        f"'{relative}' is not in the exception allowlist",
                        path=relative,
                    )
                )
            for forbidden_call in manifest.exception_policy.forbidden_exception_calls:
                if forbidden_call in text:
                    diagnostics.append(
                        Diagnostic(
                            RULE_EXCEPTION_RETHROW_FORBIDDEN,
                            f"forbidden exception bridge call '{forbidden_call}' appears in '{relative}'",
                            path=relative,
                        )
                    )


def _check_exception_compile_flags(
    manifest: Manifest,
    command: CompileCommand,
    target: Optional[Target],
    tokens: Sequence[str],
    strict_no_exceptions: bool,
    diagnostics: list[Diagnostic],
) -> None:
    flags = set(tokens)
    for forbidden in manifest.exception_policy.forbidden_compile_flags:
        if forbidden in flags:
            diagnostics.append(
                Diagnostic(
                    RULE_FORBIDDEN_EXCEPTION_FLAG,
                    f"compile command for '{command.file}' contains forbidden exception flag "
                    f"'{forbidden}'",
                    target=target.name if target is not None else None,
                    path=command.file,
                )
            )
    if strict_no_exceptions:
        for required in manifest.exception_policy.required_compile_flags:
            if required not in flags:
                diagnostics.append(
                    Diagnostic(
                        RULE_EXCEPTION_ENABLED_TARGET,
                        f"project compile command for '{command.file}' does not contain required "
                        f"no-exception flag '{required}'",
                        target=target.name if target is not None else None,
                        path=command.file,
                    )
                )


def _check_compile_commands(
    manifest: Manifest,
    index: Index,
    compile_commands: Optional[CompileCommands],
    project_root: Optional[str],
    diagnostics: list[Diagnostic],
    external_include_roots: Sequence[str] = (),
    strict_no_exceptions: bool = False,
) -> None:
    if compile_commands is None:
        return

    declared: dict[str, Target] = {}
    for target in index.targets:
        if target.role == "external":
            continue
        for source in target.sources:
            if _is_compiled_source(source):
                declared[_norm_path(source)] = target

    by_file: dict[str, list[CompileCommand]] = {}
    for command in compile_commands.commands:
        by_file.setdefault(_norm_path(command.file), []).append(command)

    for source in sorted(declared):
        count = len(by_file.get(source, []))
        if count == 0:
            diagnostics.append(
                Diagnostic(
                    RULE_MISSING_COMPILE_COMMAND,
                    f"compiled source '{source}' has no compile command",
                    path=source,
                )
            )
        elif count > 1:
            diagnostics.append(
                Diagnostic(
                    RULE_DUPLICATE_SOURCE_COMPILATION,
                    f"compiled source '{source}' is compiled {count} times; every production "
                    f"source compiles exactly once",
                    path=source,
                )
            )

    for command in compile_commands.commands:
        source = _norm_path(command.file)
        target = declared.get(source)
        tokens = _split_command(command.command)
        if not tokens:
            continue
        compiler = os.path.basename(tokens[0])
        if not _is_supported_compiler(compiler):
            diagnostics.append(
                Diagnostic(
                    RULE_UNKNOWN_COMPILER,
                    f"compile command for '{source}' uses unsupported compiler '{compiler}'; "
                    f"expected GCC or Clang",
                    path=source,
                )
            )
        _scan_compile_tokens(
            tokens, target, source, command.directory, project_root, diagnostics,
            external_include_roots,
        )
        _check_exception_compile_flags(
            manifest, command, target, tokens, strict_no_exceptions, diagnostics
        )


def _check_depfiles(
    index: Index,
    depfiles: Optional[DepfileEvidence],
    config_digest: Optional[str],
    phase: str,
    diagnostics: list[Diagnostic],
) -> None:
    if depfiles is None:
        if phase == "build":
            diagnostics.append(
                Diagnostic(
                    RULE_MISSING_DEPFILE_EVIDENCE,
                    "active dependency evidence (compiler depfiles) is required at the build "
                    "phase but was not provided",
                )
            )
        return

    if config_digest is not None and depfiles.config_digest != config_digest:
        diagnostics.append(
            Diagnostic(
                RULE_STALE_DEPFILE_EVIDENCE,
                "depfile evidence is stale: its configuration digest does not match the current "
                "index and compile commands",
            )
        )

    declared = {
        _norm_path(source)
        for target in index.targets
        if target.role != "external"
        for source in target.sources
        if _is_compiled_source(source)
    }
    entry_by_source: dict[str, DepfileEntry] = {}
    for entry in depfiles.entries:
        entry_by_source.setdefault(_norm_path(entry.source), entry)

    for source in sorted(declared):
        if source not in entry_by_source:
            diagnostics.append(
                Diagnostic(
                    RULE_CONTRADICTORY_DEPFILE_EVIDENCE,
                    f"missing depfile entry for compiled source '{source}'",
                    path=source,
                )
            )

    # Validate deps_log integrity in schema 2
    if depfiles.schema_version >= 2:
        if not depfiles.deps_log or not os.path.exists(depfiles.deps_log):
            diagnostics.append(
                Diagnostic(
                    RULE_STALE_DEPFILE_EVIDENCE,
                    f"deps log '{depfiles.deps_log or '.ninja_deps'}' is missing",
                )
            )
        elif depfiles.deps_log_digest is not None:
            actual = hashlib.sha256(_read_bytes(depfiles.deps_log)).hexdigest()
            if actual != depfiles.deps_log_digest:
                diagnostics.append(
                    Diagnostic(
                        RULE_STALE_DEPFILE_EVIDENCE,
                        f"deps log '{depfiles.deps_log}' changed since it was recorded",
                    )
                )

    # Validate individual entries
    for entry in depfiles.entries:
        if entry.depfile is not None:
            if not os.path.exists(entry.depfile):
                diagnostics.append(
                    Diagnostic(
                        RULE_STALE_DEPFILE_EVIDENCE,
                        f"depfile '{entry.depfile}' is missing",
                        path=entry.source,
                    )
                )
            else:
                actual = hashlib.sha256(_read_bytes(entry.depfile)).hexdigest()
                if actual != entry.digest:
                    diagnostics.append(
                        Diagnostic(
                            RULE_STALE_DEPFILE_EVIDENCE,
                            f"depfile '{entry.depfile}' changed since it was recorded",
                            path=entry.source,
                        )
                    )
        elif entry.dependencies:
            expected_digest = hashlib.sha256("\n".join(sorted(entry.dependencies)).encode("utf-8")).hexdigest()
            if entry.digest != expected_digest:
                diagnostics.append(
                    Diagnostic(
                        RULE_STALE_DEPFILE_EVIDENCE,
                        f"depfile entry digest mismatch for source '{entry.source}'",
                        path=entry.source,
                    )
                )

def check(
    manifest: Manifest,
    index: Index,
    manifest_digest: str,
    *,
    direct_includes: Optional[DirectIncludes] = None,
    compile_commands: Optional[CompileCommands] = None,
    depfiles: Optional[DepfileEvidence] = None,
    project_root: Optional[str] = None,
    phase: str = "configure",
    config_digest: Optional[str] = None,
    external_include_roots: Sequence[str] = (),
    strict_no_exceptions: bool = False,
) -> list[Diagnostic]:
    """Compare the resolved evidence against the strict manifest and return all findings."""
    diagnostics: list[Diagnostic] = []

    _check_architecture_exception_dates(manifest, diagnostics)
    _check_evidence_reference(manifest, index.producer, index.schema_version, diagnostics)

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
                # A dependency that declares an external family but names a
                # project target is a project wrapper masquerading as an
                # external package (ADR 0039).
                if dependency_name in by_name:
                    diagnostics.append(
                        Diagnostic(
                            RULE_PROJECT_TARGET_MASQUERADING_AS_EXTERNAL,
                            f"target '{target.name}' classifies dependency '{dependency_name}' "
                            f"with external family '{family}', but '{dependency_name}' is a "
                            f"declared project target; a project wrapper is not an external package",
                            target=target.name,
                            dependency=dependency_name,
                        )
                    )
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

    if direct_includes is not None:
        _check_evidence_reference(
            manifest, direct_includes.producer, direct_includes.schema_version, diagnostics
        )
    if depfiles is not None:
        _check_evidence_reference(manifest, depfiles.producer, depfiles.schema_version, diagnostics)

    _check_includes(manifest, index, direct_includes, project_root, diagnostics)
    _check_compile_commands(
        manifest,
        index,
        compile_commands,
        project_root,
        diagnostics,
        external_include_roots,
        strict_no_exceptions,
    )
    if strict_no_exceptions:
        _check_exception_sources(manifest, project_root, diagnostics)
    _check_depfiles(index, depfiles, config_digest, phase, diagnostics)

    diagnostics.sort(
        key=lambda diagnostic: (
            diagnostic.rule_id,
            diagnostic.message,
            diagnostic.target or "",
            diagnostic.dependency or "",
            diagnostic.path or "",
        )
    )
    return diagnostics


def _render_human(
    diagnostics: Sequence[Diagnostic],
    targets: int,
    dependencies: int,
    sources_scanned: int,
    includes_checked: int,
) -> str:
    if not diagnostics:
        return (
            "Parity Architecture Gate: PASS\n"
            f"  targets: {targets}\n"
            f"  dependencies checked: {dependencies}\n"
            f"  sources scanned: {sources_scanned}\n"
            f"  includes checked: {includes_checked}\n"
        ).rstrip()
    lines = ["Parity Architecture Gate: FAIL"]
    for diagnostic in diagnostics:
        lines.append(f"  {diagnostic.rule_id}: {diagnostic.message}")
    return "\n".join(lines)


def _render_json(
    diagnostics: Sequence[Diagnostic],
    targets: int,
    dependencies: int,
    sources_scanned: int,
    includes_checked: int,
) -> str:
    payload = {
        "ok": not diagnostics,
        "targets": targets,
        "dependencies_checked": dependencies,
        "sources_scanned": sources_scanned,
        "includes_checked": includes_checked,
        "diagnostics": [diagnostic.as_dict() for diagnostic in diagnostics],
    }
    return json.dumps(payload, indent=2, sort_keys=True)


def _count_dependencies(index: Index) -> int:
    return sum(len(target.dependencies) for target in index.targets)


def _count_includes(direct_includes: Optional[DirectIncludes]) -> int:
    if direct_includes is None:
        return 0
    return sum(len(source.includes) for source in direct_includes.sources)


def build_report(
    diagnostics: Sequence[Diagnostic],
    index: Optional[Index],
    output_format: str,
    direct_includes: Optional[DirectIncludes] = None,
) -> str:
    targets = len(index.targets) if index is not None else 0
    dependencies = _count_dependencies(index) if index is not None else 0
    sources_scanned = len(direct_includes.sources) if direct_includes is not None else 0
    includes_checked = _count_includes(direct_includes)
    if output_format == "json":
        return _render_json(
            diagnostics, targets, dependencies, sources_scanned, includes_checked
        )
    return _render_human(
        diagnostics, targets, dependencies, sources_scanned, includes_checked
    )


def _emit_scan(sources: Sequence[str], output_path: str) -> None:
    payload: list[dict[str, Any]] = []
    for source in sorted(set(sources)):
        raw = _read_bytes(source)
        digest = hashlib.sha256(raw).hexdigest()
        text = raw.decode("utf-8", errors="replace")
        includes = lex_includes(text, source)
        payload.append(
            {
                "path": source,
                "digest": digest,
                "includes": [
                    {"path": inc.path, "spelling": inc.spelling, "line": inc.line, "macro": inc.macro}
                    for inc in includes
                ],
            }
        )
    document = {
        "producer": PRODUCER_DIRECT_INCLUDES,
        "schema_version": 1,
        "sources": payload,
    }
    with open(output_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")


def _compute_config_digest(
    manifest_digest: str,
    index_path: Optional[str],
    compile_commands_path: Optional[str],
) -> str:
    parts = [manifest_digest]
    for path in (index_path, compile_commands_path):
        if path is not None and os.path.exists(path):
            parts.append(hashlib.sha256(_read_bytes(path)).hexdigest())
        else:
            parts.append("none")
    return hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()


# ---------------------------------------------------------------------------
# Active dependency (depfile) evidence producer
# ---------------------------------------------------------------------------


def _split_command_tokens(command: str) -> list[str]:
    """Split a compile command into tokens, honoring shell quoting.

    Reuses the canonical command splitter: compile commands recorded by the
    CMake File API are already shell-escaped, and `_split_command` (shlex)
    handles the double-quoted tokens CMake emits for paths that contain
    spaces.
    """
    return _split_command(command)


def _output_for_command(command: str, directory: str) -> Optional[str]:
    """Derive the compiled object output path from a compile command."""
    tokens = _split_command_tokens(command)
    output: Optional[str] = None
    i = 0
    n = len(tokens)
    while i < n:
        token = tokens[i]
        if token == "-o" and i + 1 < n:
            output = tokens[i + 1]
            break
        elif token.startswith("-o") and len(token) > 2:
            output = token[2:]
            break
        i += 1
    return output


def _parse_ninja_deps_binary(ninja_deps_path: str) -> dict[str, tuple[int, list[str]]]:
    """Parse Ninja's binary .ninja_deps log format (versions 3 and 4)."""
    with open(ninja_deps_path, "rb") as handle:
        magic = handle.read(12)
        if magic != b"# ninjadeps\n":
            raise ValueError(f"invalid .ninja_deps signature: {magic!r}")
        version_bytes = handle.read(4)
        if len(version_bytes) < 4:
            raise ValueError("truncated .ninja_deps header")
        version = struct.unpack("<I", version_bytes)[0]
        if version not in (3, 4):
            raise ValueError(f"unsupported .ninja_deps version: {version}")

        nodes: list[str] = []
        deps_by_out: dict[int, tuple[int, list[int]]] = {}

        while True:
            size_raw = handle.read(4)
            if not size_raw or len(size_raw) < 4:
                break
            size_word = struct.unpack("<I", size_raw)[0]
            is_deps = bool(size_word & 0x80000000)
            size = size_word & 0x7FFFFFFF
            data = handle.read(size)
            if len(data) < size:
                break
            if is_deps:
                if version == 4:
                    if size < 12 or (size % 4) != 0:
                        break
                    out_id, mtime_low, mtime_high = struct.unpack("<III", data[:12])
                    mtime = (mtime_high << 32) | mtime_low
                    dep_count = (size - 12) // 4
                    node_ids = list(struct.unpack(f"<{dep_count}I", data[12:]))
                else:  # version 3 uses 32-bit mtime
                    if size < 8 or (size % 4) != 0:
                        break
                    out_id, mtime = struct.unpack("<II", data[:8])
                    dep_count = (size - 8) // 4
                    node_ids = list(struct.unpack(f"<{dep_count}I", data[8:]))
                deps_by_out[out_id] = (mtime, node_ids)
            else:
                if size < 4:
                    break
                path_size = size - 4
                raw_path = data[:path_size].rstrip(b"\0")
                checksum = struct.unpack("<I", data[-4:])[0]
                expected_id = (~checksum) & 0xFFFFFFFF
                if expected_id != len(nodes):
                    break
                nodes.append(raw_path.decode("utf-8", errors="replace"))

        result: dict[str, tuple[int, list[str]]] = {}
        for out_id, (mtime, dep_ids) in deps_by_out.items():
            if out_id < len(nodes):
                out_node = nodes[out_id]
                dep_nodes = [nodes[did] for did in dep_ids if did < len(nodes)]
                result[out_node] = (mtime, dep_nodes)
        return result


def _load_ninja_deps(
    ninja_deps_path: Optional[str], build_dir: Optional[str] = None
) -> tuple[Optional[str], Optional[str], dict[str, tuple[int, list[str]]]]:
    """Load active dependency entries from .ninja_deps.

    Returns (resolved_path, file_digest, entries_by_output).
    """
    candidates: list[str] = []
    if ninja_deps_path is not None:
        candidates.append(ninja_deps_path)
    if build_dir is not None:
        candidates.append(os.path.join(build_dir, ".ninja_deps"))

    for candidate in candidates:
        if os.path.isfile(candidate):
            resolved_path = os.path.normpath(os.path.abspath(candidate))
            digest = hashlib.sha256(_read_bytes(resolved_path)).hexdigest()
            try:
                entries = _parse_ninja_deps_binary(resolved_path)
                return resolved_path, digest, entries
            except Exception:
                return resolved_path, digest, {}

    return None, None, {}

def _record_depfiles(
    manifest: Manifest,
    index: Index,
    manifest_digest: str,
    compile_commands: CompileCommands,
    index_path: str,
    compile_commands_path: str,
    output_path: str,
    ninja_deps_path: Optional[str] = None,
) -> None:
    """Produce the active dependency (depfile) evidence JSON from .ninja_deps.

    The evidence carries the configuration identity (config_digest) that ties
    the depfile evidence to the current manifest, ownership index, and compile
    commands, and one entry per compiled project source whose compilation has
    an active dependency record in .ninja_deps. A compiled source with no entry
    in the deps log is omitted; the validator fails closed on the missing entry
    with PARITY-6003.
    """
    compiled_sources: set[str] = set()
    for target in index.targets:
        if target.role == "external":
            continue
        for source in target.sources:
            if _is_compiled_source(source):
                compiled_sources.add(_norm_path(source))

    build_dir: Optional[str] = None
    if compile_commands_path and os.path.exists(compile_commands_path):
        build_dir = os.path.dirname(os.path.abspath(compile_commands_path))
    elif compile_commands.commands:
        build_dir = compile_commands.commands[0].directory

    resolved_deps_path, deps_log_digest, ninja_deps = _load_ninja_deps(
        ninja_deps_path, build_dir
    )

    normalized_deps: dict[str, tuple[int, list[str]]] = {}
    for out_key, val in ninja_deps.items():
        normalized_deps[out_key] = val
        norm_k = os.path.normpath(out_key)
        normalized_deps[norm_k] = val
        if build_dir is not None:
            if not os.path.isabs(out_key):
                normalized_deps[os.path.normpath(os.path.join(build_dir, out_key))] = val
            else:
                try:
                    normalized_deps[os.path.normpath(os.path.relpath(out_key, build_dir))] = val
                except ValueError:
                    pass

    entry_by_source: dict[str, dict[str, Any]] = {}
    for command in compile_commands.commands:
        source = _norm_path(command.file)
        if source not in compiled_sources:
            continue
        output = _output_for_command(command.command, command.directory)
        if output is None:
            continue

        cmd_dir = command.directory
        candidates = [output, os.path.normpath(output)]
        if not os.path.isabs(output):
            candidates.append(os.path.normpath(os.path.join(cmd_dir, output)))
        else:
            try:
                candidates.append(os.path.normpath(os.path.relpath(output, cmd_dir)))
            except ValueError:
                pass

        matched_val: Optional[tuple[int, list[str]]] = None
        for candidate in candidates:
            if candidate in normalized_deps:
                matched_val = normalized_deps[candidate]
                break

        if matched_val is None:
            continue

        mtime, raw_deps = matched_val
        deps_list = [d for d in raw_deps]
        deps_digest = hashlib.sha256("\n".join(sorted(deps_list)).encode("utf-8")).hexdigest()
        entry_data = {
            "source": source,
            "output": output,
            "digest": deps_digest,
            "dependencies": deps_list,
        }
        entry_by_source.setdefault(source, entry_data)

    config_digest = _compute_config_digest(
        manifest_digest, index_path, compile_commands_path
    )
    entries = [entry_by_source[source] for source in sorted(entry_by_source)]
    document = {
        "producer": PRODUCER_DEPFILES,
        "schema_version": 2,
        "config_digest": config_digest,
        "deps_log": resolved_deps_path,
        "deps_log_digest": deps_log_digest,
        "entries": entries,
    }
    with open(output_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")

def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="parity_gate",
        description="Fail-closed Parity Architecture Gate validator.",
    )
    parser.add_argument("--manifest", help="Path to the strict versioned manifest")
    parser.add_argument("--index", help="Path to the resolved ownership index")
    parser.add_argument("--direct-includes", help="Path to the direct-include lexer output")
    parser.add_argument("--compile-commands", help="Path to the generated compile_commands.json")
    parser.add_argument("--depfiles", help="Path to the recorded depfile evidence")
    parser.add_argument(
        "--phase",
        choices=("configure", "build"),
        default="configure",
        help="Gate phase: build additionally requires depfile evidence (default: configure)",
    )
    parser.add_argument(
        "--project-root",
        help="Directory against which manifest interface roots resolve (default: none)",
    )
    parser.add_argument(
        "--format",
        choices=("human", "json"),
        default="human",
        help="Diagnostic output format (default: human)",
    )
    parser.add_argument(
        "--strict-no-exceptions",
        action="store_true",
        help="Require the manifest no-exception compile policy and audit exception bridges",
    )
    parser.add_argument("--sources", nargs="*", help="Source files to scan (with --out)")
    parser.add_argument("--out", help="Write direct-include evidence JSON and exit (scan mode)")
    parser.add_argument(
        "--record-depfiles",
        help="Write active dependency (depfile) evidence JSON and exit (requires "
        "--manifest --index --compile-commands)",
    )
    parser.add_argument(
        "--ninja-deps",
        help="Path to the .ninja_deps dependency log (default: .ninja_deps in the build directory)",
    )
    parser.add_argument(
        "--external-include-root",
        action="append",
        default=[],
        metavar="PATH",
        help="External dependency include root; paths under it are never project paths "
        "(repeatable)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)

    if args.out is not None:
        if not args.sources:
            print("parity_gate: --out requires at least one --sources file", file=sys.stderr)
            return 2
        try:
            _emit_scan(args.sources, args.out)
        except SchemaViolation as exc:
            print(f"{exc.rule_id}: {exc.message}", file=sys.stderr)
            return 1
        return 0

    if args.record_depfiles is not None:
        if not args.manifest or not args.index or not args.compile_commands:
            print(
                "parity_gate: --record-depfiles requires --manifest --index --compile-commands",
                file=sys.stderr,
            )
            return 2
        try:
            manifest_data = _load_json(args.manifest, RULE_INVALID_MANIFEST_VALUE)
            manifest = parse_manifest(manifest_data)
            manifest_digest = hashlib.sha256(_read_bytes(args.manifest)).hexdigest()
            index = parse_index(_load_json(args.index, RULE_MALFORMED_EVIDENCE))
            compile_commands = parse_compile_commands(
                _load_json(args.compile_commands, RULE_MALFORMED_EVIDENCE)
            )
            _record_depfiles(
                manifest,
                index,
                manifest_digest,
                compile_commands,
                args.index,
                args.compile_commands,
                args.record_depfiles,
                ninja_deps_path=args.ninja_deps,
            )
        except SchemaViolation as exc:
            print(f"{exc.rule_id}: {exc.message}", file=sys.stderr)
            return 1
        return 0

    diagnostics: list[Diagnostic]
    index: Optional[Index] = None
    direct_includes: Optional[DirectIncludes] = None
    compile_commands: Optional[CompileCommands] = None
    depfiles: Optional[DepfileEvidence] = None

    try:
        if not args.manifest or not args.index:
            print("parity_gate: --manifest and --index are required (or use --out to scan)",
                  file=sys.stderr)
            return 2

        manifest_data = _load_json(args.manifest, RULE_INVALID_MANIFEST_VALUE)
        manifest = parse_manifest(manifest_data)
        manifest_bytes = _read_bytes(args.manifest)
        manifest_digest = hashlib.sha256(manifest_bytes).hexdigest()

        index_data = _load_json(args.index, RULE_MALFORMED_EVIDENCE)
        index = parse_index(index_data)

        if args.direct_includes:
            direct_includes = parse_direct_includes(
                _load_json(args.direct_includes, RULE_MALFORMED_EVIDENCE)
            )
        if args.compile_commands:
            compile_commands = parse_compile_commands(
                _load_json(args.compile_commands, RULE_MALFORMED_EVIDENCE)
            )
        if args.depfiles:
            depfiles = parse_depfile_evidence(
                _load_json(args.depfiles, RULE_MALFORMED_EVIDENCE)
            )

        config_digest = None
        if depfiles is not None:
            config_digest = _compute_config_digest(
                manifest_digest, args.index, args.compile_commands
            )

        diagnostics = check(
            manifest,
            index,
            manifest_digest,
            direct_includes=direct_includes,
            compile_commands=compile_commands,
            depfiles=depfiles,
            project_root=args.project_root,
            phase=args.phase,
            config_digest=config_digest,
            external_include_roots=tuple(args.external_include_root),
            strict_no_exceptions=args.strict_no_exceptions,
        )
    except SchemaViolation as exc:
        diagnostics = [Diagnostic(exc.rule_id, exc.message)]

    print(build_report(diagnostics, index, args.format, direct_includes))
    return 1 if diagnostics else 0


if __name__ == "__main__":
    sys.exit(main())
