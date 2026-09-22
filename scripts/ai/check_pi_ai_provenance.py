#!/usr/bin/env python3
"""Verify the committed six-provider pi-ai provenance bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from record_pi_ai_provenance import BASELINE_REVISION, TARGET_PROVIDERS


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-root", default=Path("fixtures/pi-ai"), type=Path)
    parser.add_argument("--pi-root", type=Path)
    return parser.parse_args()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _revision(pi_root: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(pi_root), "rev-parse", "HEAD"],
        text=True,
    ).strip()


def main() -> int:
    args = _parse_args()
    fixture_root = args.fixture_root.resolve()
    provenance_path = fixture_root / "models" / "provenance.json"
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    if provenance.get("schema") != "cpp-coding-harness/pi-ai-provenance/1":
        raise SystemExit("unexpected provenance schema")
    source = provenance.get("source", {})
    if source.get("revision") != BASELINE_REVISION:
        raise SystemExit("provenance source revision is not the pinned baseline")
    if args.pi_root and _revision(args.pi_root.resolve()) != BASELINE_REVISION:
        raise SystemExit("pi checkout is not at the pinned baseline")

    providers = provenance.get("providers", {})
    if set(providers) != set(TARGET_PROVIDERS):
        raise SystemExit("provenance provider set does not match the six target providers")

    for provider_id in TARGET_PROVIDERS:
        record = providers[provider_id]
        path = fixture_root / record["path"]
        if not path.is_file():
            raise SystemExit(f"missing artifact: {path}")
        if _sha256(path) != record["sha256"]:
            raise SystemExit(f"SHA-256 mismatch: {path}")
        catalog = json.loads(path.read_text(encoding="utf-8"))
        model_ids = sorted(
            model_id
            for models in catalog.values()
            for model_id in models
        )
        if model_ids != record["model_ids"]:
            raise SystemExit(f"model set mismatch: {provider_id}")
        if len(model_ids) != record["model_count"]:
            raise SystemExit(f"model count mismatch: {provider_id}")

    print("pi-ai provenance: PASS (six artifacts, hashes, and model sets verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
