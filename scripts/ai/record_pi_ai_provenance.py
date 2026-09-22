#!/usr/bin/env python3
"""Copy and attest the six pi-ai provider artifacts for issue #758."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from shutil import copyfile
from typing import Any


BASELINE_PREFIX = "1a584a7a5"
BASELINE_REVISION = "1a584a7a56eb5e7b4ff8ccbd46430f1533282eed"
TARGET_PROVIDERS = (
    "deepseek",
    "kimi-coding",
    "openai",
    "openai-codex",
    "openrouter",
    "opencode-go",
)
PROVENANCE_FILENAME = "provenance.json"


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pi-root", required=True, type=Path)
    parser.add_argument("--generated-catalog", required=True, type=Path)
    parser.add_argument(
        "--fixture-root",
        default=Path("fixtures/pi-ai"),
        type=Path,
        help="fixture root where models/providers and provenance.json are written",
    )
    parser.add_argument(
        "--generated-at",
        required=True,
        help="UTC ISO-8601 timestamp recorded for the generator run",
    )
    parser.add_argument(
        "--generation-command",
        required=True,
        help="exact generator command, including its output directory",
    )
    return parser.parse_args()


def _git_revision(pi_root: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(pi_root), "rev-parse", "HEAD"],
        text=True,
    ).strip()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_provider(path: Path, provider_id: str) -> dict[str, dict[str, Any]]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict) or not value:
        raise ValueError(f"{path} must be a non-empty object")

    model_ids: set[str] = set()
    for api, models in value.items():
        if not isinstance(api, str) or not isinstance(models, dict):
            raise ValueError(f"{path} has an invalid API group")
        for model_id, model in models.items():
            if not isinstance(model_id, str) or not isinstance(model, dict):
                raise ValueError(f"{path} has an invalid model entry")
            if model.get("id") != model_id:
                raise ValueError(f"{path} model key {model_id!r} disagrees with its id")
            if model.get("provider") != provider_id:
                raise ValueError(f"{path} model {model_id!r} has the wrong provider")
            if model.get("api") != api:
                raise ValueError(f"{path} model {model_id!r} is in the wrong API group")
            model_ids.add(model_id)
    if not model_ids:
        raise ValueError(f"{path} has no models")
    return value


def _artifact_record(path: Path, provider_id: str) -> dict[str, Any]:
    catalog = _load_provider(path, provider_id)
    model_ids = sorted(
        model_id
        for models in catalog.values()
        for model_id in models
    )
    return {
        "path": f"models/providers/{path.name}",
        "sha256": _sha256(path),
        "bytes": path.stat().st_size,
        "model_count": len(model_ids),
        "apis": {
            api: sorted(models)
            for api, models in sorted(catalog.items())
        },
        "model_ids": model_ids,
    }


def main() -> int:
    args = _parse_args()
    pi_root = args.pi_root.resolve()
    generated_catalog = args.generated_catalog.resolve()
    fixture_root = args.fixture_root.resolve()
    revision = _git_revision(pi_root)
    if revision != BASELINE_REVISION or not revision.startswith(BASELINE_PREFIX):
        raise SystemExit(
            f"pi checkout is {revision}, expected {BASELINE_REVISION} ({BASELINE_PREFIX})"
        )

    source_dir = generated_catalog / "providers"
    if not source_dir.is_dir():
        source_dir = generated_catalog
    fixture_dir = fixture_root / "models" / "providers"
    fixture_dir.mkdir(parents=True, exist_ok=True)

    artifacts: dict[str, dict[str, Any]] = {}
    for provider_id in TARGET_PROVIDERS:
        source = source_dir / f"{provider_id}.json"
        if not source.is_file():
            raise SystemExit(f"generated provider artifact is missing: {source}")
        destination = fixture_dir / source.name
        copyfile(source, destination)
        artifacts[provider_id] = _artifact_record(destination, provider_id)

    try:
        generated_at = datetime.fromisoformat(args.generated_at.replace("Z", "+00:00"))
    except ValueError as error:
        raise SystemExit(f"--generated-at is not ISO-8601: {error}") from error
    if generated_at.tzinfo is None or generated_at.utcoffset() != timezone.utc.utcoffset(generated_at):
        raise SystemExit("--generated-at must include a UTC offset")

    provenance = {
        "schema": "cpp-coding-harness/pi-ai-provenance/1",
        "source": {
            "repository": "https://github.com/earendil-works/pi",
            "checkout": "external pi checkout supplied via --pi-root",
            "revision": revision,
            "generator": "packages/ai/scripts/generate-models.ts",
            "generated_at": args.generated_at,
            "command": args.generation_command,
        },
        "providers": artifacts,
        "codex_discrepancy": {
            "old_snapshot_count": 7,
            "verified_generator_count": artifacts["openai-codex"]["model_count"],
            "resolution": "The pinned generator output is authoritative; the final set is recorded below.",
        },
    }
    provenance_path = fixture_root / "models" / PROVENANCE_FILENAME
    provenance_path.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.write_text(
        json.dumps(provenance, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    print(json.dumps(provenance, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
