#!/usr/bin/env python3
"""Verify, read and register pi-v1.3.0 audit evidence bundles by named baseline.

The audit registry (``fixtures/pi-v1.3.0/baselines.json``) is the one named authority for the
revision behind every ticket family bundle: a baseline name resolves to exactly one full
40-character revision, and each registered family pins the SHA-256 of its manifest. The manifest
pins the executable capture (plus its helper sources) and each artifact's exact bytes; every
artifact carries its own envelope and scenario observations. A ``read`` emits only bytes that
verified through that whole chain, so a missing or wrong-revision bundle cannot fall back to a
historical ``pi-v1.0.4`` observation. Selection is by baseline name only: there is no free-form
revision input.

This generalizes the named-bundle discipline of ``fixtures/pi-tui/baselines.json`` and
``scripts/tui/evidence.py`` (ADR 0065) to the multi-module pi-v1.3.0 audit. Historical registries,
bundles and tooling are untouched.

A ticket lane captures its family with the bundle-local capture script and validates it with
``check``; the printed registry record is the exact patch the serial merger applies with
``register``. ``verify`` and ``read`` operate on registered bundles only.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


REGISTRY_SCHEMA = "pike/pi-v1.3.0-audit-baselines/1"
MANIFEST_SCHEMA = "pike/pi-v1.3.0-audit-evidence/1"
DEFAULT_FIXTURE_ROOT = Path(__file__).resolve().parents[2] / "fixtures" / "pi-v1.3.0"
REVISION_PATTERN = re.compile(r"[0-9a-f]{40}")
DIGEST_PATTERN = re.compile(r"[0-9a-f]{64}")
FAMILY_PATTERN = re.compile(r"audit-(?P<ticket>[0-9]+)(?:-(?P<variant>[a-z0-9]+(?:-[a-z0-9]+)*))?")
SOURCE_ENDPOINT_PATTERN = re.compile(r"packages/[^:]+:.+")


class EvidenceError(Exception):
    """A registry, manifest, artifact or selection failed verification."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def object_value(value, message: str) -> dict:
    require(isinstance(value, dict), message)
    return value


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_child(root: Path, relative, context: str) -> Path:
    """Resolve a recorded relative path, refusing absolute and escaping references."""
    require(isinstance(relative, str) and bool(relative), f"invalid evidence path ({context})")
    path = Path(relative)
    require(not path.is_absolute() and ".." not in path.parts, f"invalid evidence path ({context})")
    resolved_root = root.resolve()
    target = (root / path).resolve()
    require(target.is_relative_to(resolved_root), f"invalid evidence path ({context})")
    return target


def read_json(path: Path, context: str) -> dict:
    try:
        return object_value(json.loads(path.read_text(encoding="utf-8")), f"invalid evidence document ({context})")
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read {context} {path}: {error}") from error


def validate_timestamp(value, context: str) -> None:
    require(isinstance(value, str) and value.endswith("Z"), f"invalid capture timestamp ({context})")
    try:
        datetime.fromisoformat(value)
    except ValueError as error:
        raise EvidenceError(f"invalid capture timestamp ({context}): {error}") from error


def validate_environment(value, context: str) -> dict:
    environment = object_value(value, f"missing capture metadata: environment ({context})")
    for key in ("platform", "arch", "libc", "glibcVersion", "node"):
        require(
            isinstance(environment.get(key), str) and bool(environment[key]),
            f"missing capture metadata: environment.{key} ({context})",
        )
    require(
        (environment["platform"], environment["arch"], environment["libc"]) == ("linux", "x64", "glibc"),
        f"unsupported capture environment ({context})",
    )
    return environment


def validate_source_endpoints(value, context: str) -> list[str]:
    require(isinstance(value, list) and bool(value), f"missing source endpoints ({context})")
    for endpoint in value:
        require(
            isinstance(endpoint, str) and SOURCE_ENDPOINT_PATTERN.fullmatch(endpoint) is not None,
            f"invalid source endpoint ({context}): {endpoint!r}",
        )
    return value


def validate_scenarios(value, context: str) -> list[dict]:
    require(isinstance(value, list) and bool(value), f"missing evidence scenarios ({context})")
    names: set[str] = set()
    for scenario in value:
        scenario = object_value(scenario, f"invalid evidence scenario ({context})")
        name = scenario.get("name")
        require(isinstance(name, str) and bool(name), f"invalid evidence scenario name ({context})")
        require(name not in names, f"duplicate evidence scenario name ({context}): {name}")
        names.add(name)
        require(
            isinstance(scenario.get("inputs"), (dict, list)) and bool(scenario["inputs"]),
            f"missing scenario inputs ({context}: {name})",
        )
        require("expected" in scenario and scenario["expected"] is not None, f"missing scenario observation ({context}: {name})")
        normalizations = scenario.get("normalizations", [])
        require(
            isinstance(normalizations, list) and all(isinstance(item, str) and bool(item) for item in normalizations),
            f"invalid scenario normalizations ({context}: {name})",
        )
    return value


def resolve_repository_file(repository_root: Path, relative, context: str) -> Path:
    require(isinstance(relative, str) and bool(relative), f"missing repository path ({context})")
    path = Path(relative)
    require(not path.is_absolute() and ".." not in path.parts, f"invalid repository path ({context})")
    target = (repository_root / path).resolve()
    require(target.is_relative_to(repository_root.resolve()), f"repository path escapes the repository ({context})")
    require(target.is_file(), f"missing repository file ({context}): {relative}")
    return target


def validate_generator(value, repository_root: Path) -> None:
    generator = object_value(value, "invalid capture generator")
    target = resolve_repository_file(repository_root, generator.get("path"), "generator.path")
    require(
        isinstance(generator.get("sha256"), str) and DIGEST_PATTERN.fullmatch(generator["sha256"]) is not None,
        "invalid generator digest",
    )
    require(sha256_file(target) == generator["sha256"], f"generator digest mismatch: {generator['path']}")
    require(isinstance(generator.get("command"), str) and bool(generator["command"]), "missing generator command")
    sources = generator.get("sources")
    require(isinstance(sources, list), "missing generator sources")
    for source in sources:
        source = object_value(source, "invalid generator source")
        source_path = resolve_repository_file(repository_root, source.get("path"), "generator.sources")
        require(
            isinstance(source.get("sha256"), str) and DIGEST_PATTERN.fullmatch(source["sha256"]) is not None,
            "invalid generator source digest",
        )
        require(sha256_file(source_path) == source["sha256"], f"generator source digest mismatch: {source['path']}")


def validate_artifact(record, bundle_root: Path, baseline: dict, manifest: dict, family: str, seen: set[str]) -> bytes:
    record = object_value(record, "invalid artifact record")
    relative = record.get("path")
    context = f"{family}: {relative}"
    require(record.get("family") == family, f"artifact family mismatch ({context})")
    require(relative not in seen, f"duplicate evidence artifact ({context})")
    seen.add(relative)
    target = safe_child(bundle_root, relative, context)
    require(target.is_file(), f"missing artifact ({context})")
    size = record.get("bytes")
    require(type(size) is int and size > 0, f"invalid artifact byte count ({context})")
    digest = record.get("sha256")
    require(isinstance(digest, str) and DIGEST_PATTERN.fullmatch(digest) is not None, f"invalid artifact digest ({context})")
    payload = target.read_bytes()
    require(sha256_bytes(payload) == digest, f"artifact SHA-256 mismatch ({context})")
    require(len(payload) == size, f"artifact byte count mismatch ({context})")
    try:
        artifact = object_value(json.loads(payload), f"invalid evidence artifact ({context})")
    except (UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"invalid evidence artifact ({context}): {error}") from error
    require(
        artifact.get("baseline") == baseline["name"] and artifact.get("revision") == baseline["revision"],
        f"artifact source revision/baseline mismatch ({context})",
    )
    require(artifact.get("capturedAt") == manifest["capturedAt"], f"artifact capture metadata mismatch ({context})")
    require(artifact.get("environment") == manifest["environment"], f"artifact capture metadata mismatch ({context})")
    endpoints = validate_source_endpoints(record.get("sourceEndpoints"), context)
    require(artifact.get("sourceEndpoints") == endpoints, f"artifact source endpoints disagree with the manifest ({context})")
    scenarios = validate_scenarios(record.get("scenarios"), context)
    require(artifact.get("scenarios") == scenarios, f"artifact scenarios disagree with the manifest ({context})")
    return payload


def validate_manifest(
    fixture_root: Path, repository_root: Path, baseline: dict, family: str, manifest_relative: str, manifest_digest: str
) -> tuple[dict, dict[str, bytes]]:
    """Validate one family manifest and every artifact it pins; return the payloads by path."""
    require(FAMILY_PATTERN.fullmatch(family) is not None, f"invalid audit family: {family!r}")
    require(
        isinstance(manifest_digest, str) and DIGEST_PATTERN.fullmatch(manifest_digest) is not None,
        f"invalid manifest digest ({family})",
    )
    manifest_path = safe_child(fixture_root, manifest_relative, f"manifest for {family}")
    require(manifest_path.is_file(), f"missing evidence bundle for {family}: {manifest_path}")
    manifest_bytes = manifest_path.read_bytes()
    require(sha256_bytes(manifest_bytes) == manifest_digest, f"evidence manifest digest mismatch ({family})")
    try:
        manifest = object_value(json.loads(manifest_bytes), f"invalid evidence manifest ({family})")
    except (UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"invalid evidence manifest ({family}): {error}") from error
    require(manifest.get("schema") == MANIFEST_SCHEMA, f"invalid evidence manifest schema ({family})")
    require(manifest.get("baseline") == baseline["name"], f"evidence baseline mismatch ({family})")
    require(manifest.get("revision") == baseline["revision"], f"evidence source revision mismatch ({family})")
    require(manifest.get("family") == family, f"evidence family mismatch ({family})")
    ticket = manifest.get("ticket")
    expected_ticket = FAMILY_PATTERN.fullmatch(family)["ticket"]
    require(ticket == expected_ticket, f"evidence ticket does not match family ({family})")
    validate_timestamp(manifest.get("capturedAt"), family)
    manifest["environment"] = validate_environment(manifest.get("environment"), family)
    validate_generator(manifest.get("generator"), repository_root)
    artifacts = manifest.get("artifacts")
    require(isinstance(artifacts, list) and bool(artifacts), f"empty evidence bundle ({family})")
    seen: set[str] = set()
    payloads: dict[str, bytes] = {}
    for record in artifacts:
        record = object_value(record, f"invalid artifact record ({family})")
        payloads[record.get("path")] = validate_artifact(record, manifest_path.parent, baseline, manifest, family, seen)
    return manifest, payloads


def load_registry(fixture_root: Path) -> dict:
    path = fixture_root / "baselines.json"
    try:
        registry = object_value(json.loads(path.read_text(encoding="utf-8")), f"invalid baseline registry {path}")
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read baseline registry {path}: {error}") from error
    require(registry.get("schema") == REGISTRY_SCHEMA, f"unexpected baseline registry schema: {path}")
    baselines = object_value(registry.get("baselines"), "baseline registry has no baselines")
    require(bool(baselines), "baseline registry has no baselines")
    for name, entry in baselines.items():
        require(isinstance(name, str) and bool(name), "invalid baseline name")
        entry = object_value(entry, f"baseline entry is not an object: {name}")
        revision = entry.get("revision")
        require(
            isinstance(revision, str) and REVISION_PATTERN.fullmatch(revision) is not None,
            f"baseline {name} must record a full 40-character revision, got {revision!r}",
        )
        require(isinstance(entry.get("bundles"), dict), f"baseline {name} must record a bundles object")
    require(registry.get("default") in baselines, "default baseline is not registered")
    return registry


def resolve_baseline(registry: dict, name: str | None) -> dict:
    selected = name if name is not None else registry["default"]
    require(isinstance(selected, str) and bool(selected), "invalid baseline name")
    entry = registry["baselines"].get(selected)
    require(entry is not None, f"unknown baseline {selected!r}; registered baselines: {', '.join(sorted(registry['baselines']))}")
    return {"name": selected, **entry}


def repository_root_for(fixture_root: Path) -> Path:
    root = fixture_root.resolve()
    require(
        root.name == "pi-v1.3.0" and root.parent.name == "fixtures",
        f"fixture root is not <repository>/fixtures/pi-v1.3.0: {root}",
    )
    return root.parents[1]


def verify(fixture_root: Path, baseline_name: str | None, pi_root: Path | None = None) -> tuple[dict, dict[str, bytes], str]:
    """Validate every registered family of the selected baseline; return payloads by family/path."""
    registry = load_registry(fixture_root)
    baseline = resolve_baseline(registry, baseline_name)
    if pi_root is not None:
        revision = subprocess.check_output(["git", "-C", str(pi_root.resolve()), "rev-parse", "HEAD"], text=True).strip()
        require(revision == baseline["revision"], f"pi checkout is at {revision}, not the {baseline['name']} revision")
    bundles = baseline["bundles"]
    require(bool(bundles), f"baseline {baseline['name']} has no registered evidence bundle")
    repository_root = repository_root_for(fixture_root)
    artifacts: dict[str, bytes] = {}
    scenarios = 0
    for family, record in sorted(bundles.items()):
        record = object_value(record, f"invalid registry record for {family}")
        manifest, payloads = validate_manifest(
            fixture_root.resolve(), repository_root, baseline, family, record.get("manifest"), record.get("manifestSha256")
        )
        scenarios += sum(len(artifact["scenarios"]) for artifact in manifest["artifacts"])
        for relative, payload in payloads.items():
            artifacts[f"{family}/{relative}"] = payload
    summary = (
        f"verified {len(bundles)} bundle(s), {len(artifacts)} artifact(s), {scenarios} scenario(s); "
        f"baseline {baseline['name']} at {baseline['revision']}"
    )
    return baseline, artifacts, summary


def describe_candidate(fixture_root: Path, manifest_path: Path) -> tuple[dict, dict, dict]:
    """Validate an unregistered candidate manifest and return (registry, manifest, registry record)."""
    fixture_root = fixture_root.resolve()
    manifest_path = manifest_path.resolve()
    require(manifest_path.is_file(), f"missing candidate manifest: {manifest_path}")
    require(manifest_path.name == "manifest.json", f"a family manifest must be named manifest.json: {manifest_path}")
    require(
        manifest_path.is_relative_to(fixture_root),
        f"candidate manifest is outside the audit fixture root: {manifest_path}",
    )
    family = manifest_path.parent.name
    registry = load_registry(fixture_root)
    manifest = read_json(manifest_path, f"candidate manifest {family}")
    baseline = resolve_baseline(registry, manifest.get("baseline"))
    repository_root = repository_root_for(fixture_root)
    manifest_relative = manifest_path.relative_to(fixture_root).as_posix()
    digest = sha256_bytes(manifest_path.read_bytes())
    validate_manifest(fixture_root, repository_root, baseline, family, manifest_relative, digest)
    record = {family: {"manifest": manifest_relative, "manifestSha256": digest}}
    return registry, baseline, record


def register(fixture_root: Path, manifest_path: Path, replace: bool) -> dict:
    registry, baseline, record = describe_candidate(fixture_root, manifest_path)
    family = next(iter(record))
    for name, entry in registry["baselines"].items():
        if name != baseline["name"] and family in entry["bundles"]:
            raise EvidenceError(f"family {family} already belongs to baseline {name!r}; families are not shared")
    bundles = registry["baselines"][baseline["name"]]["bundles"]
    if bundles.get(family) is not None and bundles[family] != record[family] and not replace:
        raise EvidenceError(
            f"family {family} is already registered to different bytes; "
            "pass --replace to record a deliberate recapture"
        )
    bundles[family] = record[family]
    registry["baselines"][baseline["name"]]["bundles"] = {name: bundles[name] for name in sorted(bundles)}
    (fixture_root.resolve() / "baselines.json").write_text(
        json.dumps(registry, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"registered {family} for {baseline['name']} (manifest sha256 {record[family]['manifestSha256']})")
    return record


def emit_artifact(artifacts: dict[str, bytes], family: str, relative: str) -> None:
    key = f"{family}/{relative}"
    require(key in artifacts, f"artifact not recorded in the selected evidence bundle: {key}")
    sys.stdout.buffer.write(artifacts[key])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)

    def add_fixture_argument(subparser: argparse.ArgumentParser) -> None:
        subparser.add_argument("--fixture-root", type=Path, default=DEFAULT_FIXTURE_ROOT)

    verify_parser = subparsers.add_parser("verify", help="verify every registered family of a named baseline")
    add_fixture_argument(verify_parser)
    verify_parser.add_argument("--baseline", help="named baseline to verify (default: the registry default)")
    verify_parser.add_argument("--pi-root", type=Path, help="pi checkout that must sit at the selected revision")

    check_parser = subparsers.add_parser("check", help="validate an unregistered candidate manifest")
    add_fixture_argument(check_parser)
    check_parser.add_argument("--manifest", required=True, type=Path)

    read_parser = subparsers.add_parser("read", help="emit only verified bytes of a registered artifact")
    add_fixture_argument(read_parser)
    read_parser.add_argument("--baseline", help="named baseline to read (default: the registry default)")
    read_parser.add_argument("--family", required=True)
    read_parser.add_argument("--artifact", required=True)

    register_parser = subparsers.add_parser("register", help="pin a verified candidate manifest in the registry")
    add_fixture_argument(register_parser)
    register_parser.add_argument("--manifest", required=True, type=Path)
    register_parser.add_argument("--replace", action="store_true", help="record a deliberate recapture of a registered family")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.operation == "verify":
            _, _, summary = verify(args.fixture_root, args.baseline, args.pi_root)
            print(summary)
        elif args.operation == "check":
            _, _, record = describe_candidate(args.fixture_root, args.manifest)
            print(json.dumps(record, indent=2, ensure_ascii=False))
        elif args.operation == "read":
            _, artifacts, _ = verify(args.fixture_root, args.baseline)
            emit_artifact(artifacts, args.family, args.artifact)
        elif args.operation == "register":
            register(args.fixture_root, args.manifest, args.replace)
        return 0
    except (EvidenceError, OSError, KeyError, TypeError, ValueError, subprocess.SubprocessError) as error:
        print(f"audit evidence error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
