#!/usr/bin/env python3
"""Verify the committed six-provider pi-ai provenance bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from record_pi_ai_provenance import (
    TARGET_PROVIDERS,
    bundle_dir,
    resolve_baseline,
)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-root", default=Path("fixtures/pi-ai"), type=Path)
    parser.add_argument("--pi-root", type=Path)
    parser.add_argument(
        "--baseline",
        help=(
            "named baseline to verify, as registered in "
            "fixtures/pi-ai/baselines.json (default: the registry default)"
        ),
    )
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


def _load_artifact(path: Path, provider_id: str) -> dict[str, dict[str, dict]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read artifact {path}: {error}") from error
    if not isinstance(value, dict) or not value:
        raise SystemExit(f"artifact is not a non-empty object: {path}")

    seen_ids: set[str] = set()
    for api, models in value.items():
        if not isinstance(api, str) or not api:
            raise SystemExit(f"artifact has an invalid API group: {path}")
        if not isinstance(models, dict) or not models:
            raise SystemExit(f"artifact has an empty API group: {path} ({api})")
        for model_id, model in models.items():
            if not isinstance(model_id, str) or not model_id:
                raise SystemExit(f"artifact has an invalid model id: {path} ({api})")
            if not isinstance(model, dict):
                raise SystemExit(f"artifact model is not an object: {path} ({model_id})")
            if model_id in seen_ids:
                raise SystemExit(f"artifact repeats model id: {path} ({model_id})")
            if model.get("id") != model_id:
                raise SystemExit(f"artifact model key disagrees with id: {path} ({model_id})")
            if model.get("provider") != provider_id:
                raise SystemExit(f"artifact model has the wrong provider: {path} ({model_id})")
            if model.get("api") != api:
                raise SystemExit(f"artifact model is in the wrong API group: {path} ({model_id})")
            seen_ids.add(model_id)
    return value


def main() -> int:
    args = _parse_args()
    fixture_root = args.fixture_root.resolve()
    # Verification binds to the baseline that owns the bundle under test, not to a module
    # constant: the expected revision is the registry entry for that baseline (ADR 0065).
    baseline = resolve_baseline(args.baseline)
    bundle_root = bundle_dir(fixture_root, baseline)
    provenance_path = bundle_root / "models" / "provenance.json"
    try:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read provenance: {error}") from error
    if provenance.get("schema") != "cpp-coding-harness/pi-ai-provenance/1":
        raise SystemExit("unexpected provenance schema")
    recorded_baseline = provenance.get("baseline")
    if recorded_baseline is not None and recorded_baseline != baseline["name"]:
        raise SystemExit(
            f"bundle records baseline {recorded_baseline!r}, not {baseline['name']!r}"
        )
    source = provenance.get("source", {})
    if source.get("revision") != baseline["revision"]:
        raise SystemExit(
            f"provenance source revision is not the {baseline['name']} revision "
            f"({baseline['revision']})"
        )
    if args.pi_root and _revision(args.pi_root.resolve()) != baseline["revision"]:
        raise SystemExit(f"pi checkout is not at the {baseline['name']} revision")

    providers = provenance.get("providers", {})
    if set(providers) != set(TARGET_PROVIDERS):
        raise SystemExit("provenance provider set does not match the six target providers")

    for provider_id in TARGET_PROVIDERS:
        record = providers[provider_id]
        if not isinstance(record, dict):
            raise SystemExit(f"invalid provenance record: {provider_id}")
        expected_path = f"models/providers/{provider_id}.json"
        if record.get("path") != expected_path:
            raise SystemExit(f"invalid provenance path: {provider_id}")
        path = bundle_root / record["path"]
        if not path.is_file():
            raise SystemExit(f"missing artifact: {path}")
        if _sha256(path) != record["sha256"]:
            raise SystemExit(f"SHA-256 mismatch: {path}")
        if path.stat().st_size != record["bytes"]:
            raise SystemExit(f"byte count mismatch: {path}")
        catalog = _load_artifact(path, provider_id)
        model_ids = sorted(
            model_id
            for models in catalog.values()
            for model_id in models
        )
        if model_ids != record["model_ids"]:
            raise SystemExit(f"model set mismatch: {provider_id}")
        if len(model_ids) != record["model_count"]:
            raise SystemExit(f"model count mismatch: {provider_id}")
        apis = {
            api: sorted(models)
            for api, models in sorted(catalog.items())
        }
        if apis != record["apis"]:
            raise SystemExit(f"API grouping mismatch: {provider_id}")

    print(
        f"pi-ai provenance: PASS (six artifacts, hashes, and model sets verified; "
        f"baseline {baseline['name']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
