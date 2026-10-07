#!/usr/bin/env python3
"""Attest the committed MCP/codemode evidence bundle for a named pi baseline (spec #882).

The bundle is captured by ``fixtures/pi-ai/capture/capture-mcp-codemode.mts`` into the bundle
path the selected baseline owns. This script records the per-artifact SHA-256/byte provenance and
writes the selected baseline's ``captured_at`` and ``digests`` into the registry (ADR 0065), so a
later reader can bind the bytes to the revision and the checker can verify them. Selection is by
baseline name only; the script refuses a bundle another baseline owns and never rewrites a
preserved baseline's registry entry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from record_pi_ai_provenance import (
    REGISTRY_PATH,
    bundle_dir,
    resolve_baseline,
)


BUNDLE_SUBDIR = "mcp-codemode"
PROVENANCE_FILENAME = "provenance.json"
SCHEMA = "cpp-coding-harness/pi-mcp-codemode-provenance/1"
GENERATOR = "fixtures/pi-ai/capture/capture-mcp-codemode.mts"


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-root", default=Path("fixtures/pi-ai"), type=Path)
    parser.add_argument(
        "--baseline",
        help="named baseline whose bundle to record (default: the registry default)",
    )
    parser.add_argument(
        "--captured-at",
        help="UTC ISO-8601 timestamp recorded for the capture run (default: now)",
    )
    parser.add_argument("--pi-root", type=Path, help="pi checkout to verify against the baseline revision")
    return parser.parse_args()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _artifact_paths(bundle_root: Path) -> list[str]:
    """Artifact paths relative to the bundle root, excluding the provenance record itself."""
    evidence_dir = bundle_root / BUNDLE_SUBDIR
    paths = [
        f"{BUNDLE_SUBDIR}/{path.name}"
        for path in sorted(evidence_dir.rglob("*"))
        if path.is_file() and path.name != PROVENANCE_FILENAME
    ]
    if not paths:
        raise SystemExit(f"no captured artifacts under {evidence_dir}; run the capture script first")
    return paths


def _record_baseline(registry: dict[str, Any], name: str, captured_at: str, digests: dict[str, str]) -> dict[str, Any]:
    entry = registry["baselines"][name]
    entry["captured_at"] = captured_at
    entry["digests"] = digests
    return registry


def main() -> int:
    args = _parse_args()
    fixture_root = args.fixture_root.resolve()
    baseline = resolve_baseline(args.baseline)
    if baseline["bundle_path"] == "":
        raise SystemExit(
            f"baseline {baseline['name']!r} owns the historical fixture root; "
            "the MCP/codemode bundle needs a baseline with its own bundle_path"
        )
    bundle_root = bundle_dir(fixture_root, baseline)
    provenance_path = bundle_root / BUNDLE_SUBDIR / PROVENANCE_FILENAME
    # Never write across baselines: refuse a bundle that already records a different baseline.
    if provenance_path.is_file():
        try:
            existing = json.loads(provenance_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise SystemExit(f"cannot read existing provenance: {error}") from error
        existing_baseline = existing.get("baseline")
        if existing_baseline is not None and existing_baseline != baseline["name"]:
            raise SystemExit(
                f"bundle at {bundle_root} already records baseline {existing_baseline!r}; "
                f"refusing to overwrite it with {baseline['name']!r}"
            )

    if args.pi_root is not None:
        revision = subprocess.check_output(
            ["git", "-C", str(args.pi_root.resolve()), "rev-parse", "HEAD"], text=True
        ).strip()
        if revision != baseline["revision"]:
            raise SystemExit(
                f"pi checkout is {revision}, but baseline {baseline['name']} records {baseline['revision']}"
            )

    captured_at = args.captured_at or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        recorded_at = datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
    except ValueError as error:
        raise SystemExit(f"--captured-at is not ISO-8601: {error}") from error
    if recorded_at.tzinfo is None or recorded_at.utcoffset() != timezone.utc.utcoffset(recorded_at):
        raise SystemExit("--captured-at must include a UTC offset")

    artifacts: dict[str, dict[str, Any]] = {}
    for relative in _artifact_paths(bundle_root):
        path = bundle_root / relative
        artifacts[relative] = {"sha256": _sha256(path), "bytes": path.stat().st_size}

    provenance = {
        "schema": SCHEMA,
        "baseline": baseline["name"],
        "source": {
            "repository": "https://github.com/earendil-works/pi",
            "checkout": "external pi checkout supplied to the capture script",
            "revision": baseline["revision"],
            "generator": GENERATOR,
            "captured_at": captured_at,
        },
        "artifacts": artifacts,
    }
    provenance_path.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.write_text(json.dumps(provenance, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    registry = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    if registry.get("baselines", {}).get(baseline["name"]) is None:
        raise SystemExit(f"baseline {baseline['name']} is not in {REGISTRY_PATH}")
    digests = {relative: record["sha256"] for relative, record in artifacts.items()}
    registry = _record_baseline(registry, baseline["name"], captured_at, digests)
    REGISTRY_PATH.write_text(json.dumps(registry, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(
        f"recorded {len(artifacts)} artifacts for baseline {baseline['name']} "
        f"({baseline['revision']}) at {captured_at}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
