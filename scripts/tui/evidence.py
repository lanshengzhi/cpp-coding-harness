#!/usr/bin/env python3
"""Select and verify frozen TUI evidence; read emits only verified artifact bytes.

This offline reader never imports pi, runs capture, or consults historical fixtures.
The named registry owns the revision and manifest digest. Capture is separate.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sys

DEFAULT_ROOT = Path(__file__).resolve().parents[2] / "fixtures/pi-tui"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def object_value(value, message):
    require(isinstance(value, dict), message)
    return value


def child(root: Path, relative: str) -> Path:
    require(isinstance(relative, str) and bool(relative), "invalid evidence path")
    path = Path(relative)
    require(not path.is_absolute() and ".." not in path.parts, "invalid evidence path")
    target = (root / path).resolve()
    require(target.is_relative_to(root.resolve()), "evidence path escapes bundle")
    return target


def endpoints(value) -> list[str]:
    require(isinstance(value, list) and bool(value) and all(isinstance(item, str) and item.startswith("packages/") and ":" in item for item in value), "missing source endpoints")
    return value


def verify(root: Path, baseline: str | None) -> dict[str, bytes]:
    registry = json.loads((root / "baselines.json").read_bytes())
    if registry.get("schema") != "pike/tui-baselines/1":
        raise ValueError("invalid TUI baseline registry")
    name = baseline if baseline is not None else registry["default"]
    if name not in registry["baselines"]:
        raise ValueError(f"unknown TUI baseline: {name}")
    selected = object_value(registry["baselines"][name], "invalid baseline entry")
    require(isinstance(selected["revision"], str) and re.fullmatch(r"[0-9a-f]{40}", selected["revision"]) is not None, "invalid baseline revision")
    bundle = child(root, selected["bundle"])
    manifest_path = bundle / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError(f"missing evidence bundle for {name}: {manifest_path}")
    manifest_bytes = manifest_path.read_bytes()
    if hashlib.sha256(manifest_bytes).hexdigest() != selected["manifestSha256"]:
        raise ValueError("evidence manifest digest mismatch")
    manifest = object_value(json.loads(manifest_bytes), "invalid TUI evidence manifest")
    if manifest.get("schema") != "pike/tui-evidence/1":
        raise ValueError("invalid TUI evidence manifest")
    if manifest.get("baseline") != name or manifest.get("revision") != selected["revision"]:
        raise ValueError("evidence source revision/baseline mismatch")
    require(all(key in manifest for key in ("capturedAt", "environment", "generator", "artifacts")), "missing capture metadata")
    require(isinstance(manifest["capturedAt"], str) and manifest["capturedAt"].endswith("Z"), "invalid capture timestamp")
    datetime.fromisoformat(manifest["capturedAt"])
    environment = object_value(manifest["environment"], "missing capture metadata: environment")
    require(all(isinstance(environment.get(key), str) and bool(environment[key]) for key in ("platform", "arch", "libc", "glibcVersion", "node", "TERM")), "missing capture metadata: environment")
    require((environment["platform"], environment["arch"], environment["libc"]) == ("linux", "x64", "glibc"), "unsupported capture environment")
    generator = object_value(manifest["generator"], "invalid capture generator")
    require(isinstance(generator.get("path"), str) and bool(generator["path"]) and isinstance(generator.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", generator["sha256"]) is not None, "missing capture metadata: generator")
    require(isinstance(manifest["artifacts"], list) and bool(manifest["artifacts"]), "empty evidence bundle")
    artifacts = {}
    families = set()
    for record in manifest["artifacts"]:
        record = object_value(record, "invalid artifact record")
        require(isinstance(record.get("family"), str) and bool(record["family"]), "invalid artifact family")
        families.add(record["family"])
        source_endpoints = endpoints(record["sourceEndpoints"])
        payload = child(bundle, record["path"]).read_bytes()
        if len(payload) != record["bytes"] or hashlib.sha256(payload).hexdigest() != record["sha256"]:
            raise ValueError(f"artifact digest/size mismatch: {record['path']}")
        require(record["path"] not in artifacts, "duplicate evidence artifact")
        artifact = object_value(json.loads(payload), "invalid evidence artifact")
        require(artifact.get("baseline") == name and artifact.get("revision") == selected["revision"], "artifact source revision/baseline mismatch")
        require(artifact.get("environment") == environment and artifact.get("capturedAt") == manifest["capturedAt"], "artifact capture metadata mismatch")
        require(endpoints(artifact.get("sourceEndpoints")) == source_endpoints, "artifact source endpoints mismatch")
        scenarios = artifact.get("scenarios")
        require(isinstance(scenarios, list) and bool(scenarios), "missing evidence scenarios")
        names = set()
        for scenario in scenarios:
            scenario = object_value(scenario, "invalid evidence scenario")
            require(isinstance(scenario.get("name"), str) and bool(scenario["name"]) and scenario["name"] not in names, "invalid/duplicate scenario name")
            names.add(scenario["name"])
            dimensions = object_value(scenario.get("dimensions"), "missing scenario dimensions")
            require(all(type(dimensions.get(key)) is int and dimensions[key] > 0 for key in ("columns", "rows")), "invalid scenario dimensions")
            require(isinstance(scenario.get("inputs"), (dict, list)) and "expected" in scenario and scenario["expected"] is not None, "missing scenario inputs/observation")
        artifacts[record["path"]] = payload
    require({"input", "component", "screen-state"}.issubset(families), "missing required evidence family")
    return artifacts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["verify", "read"])
    parser.add_argument("--baseline")
    parser.add_argument("--fixture-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--artifact")
    args = parser.parse_args()
    try:
        artifacts = verify(args.fixture_root, args.baseline)
        if args.operation == "read":
            if args.artifact not in artifacts:
                raise ValueError(f"artifact not recorded in selected evidence bundle: {args.artifact}")
            sys.stdout.buffer.write(artifacts[args.artifact])
        else:
            print(f"verified {len(artifacts)} TUI artifacts")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"TUI evidence error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
