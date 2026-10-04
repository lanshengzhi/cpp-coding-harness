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


TARGET_PROVIDERS = (
    "deepseek",
    "kimi-coding",
    "openai",
    "openai-codex",
    "openrouter",
    "opencode-go",
)
PROVENANCE_FILENAME = "provenance.json"

# The named-baseline registry is the single authority for pi revisions and bundle paths
# (ADR 0065). It is resolved relative to this file, never relative to --fixture-root, so a
# copied fixture tree still verifies against the repository's policy.
REGISTRY_PATH = Path(__file__).resolve().parents[2] / "fixtures" / "pi-ai" / "baselines.json"
REGISTRY_SCHEMA = "cpp-coding-harness/pi-ai-baselines/1"


def load_baselines(registry_path: Path = REGISTRY_PATH) -> dict[str, Any]:
    """Return the named-baseline registry, refusing any schema or shape we do not understand."""
    try:
        registry = json.loads(registry_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read baseline registry {registry_path}: {error}") from error
    if not isinstance(registry, dict) or registry.get("schema") != REGISTRY_SCHEMA:
        raise SystemExit(f"unexpected baseline registry schema: {registry_path}")
    baselines = registry.get("baselines")
    if not isinstance(baselines, dict) or not baselines:
        raise SystemExit(f"baseline registry has no baselines: {registry_path}")
    for name, entry in baselines.items():
        if not isinstance(name, str) or not name:
            raise SystemExit(f"baseline registry has an invalid baseline name: {name!r}")
        if not isinstance(entry, dict):
            raise SystemExit(f"baseline registry entry is not an object: {name}")
        revision = entry.get("revision")
        if not isinstance(revision, str) or len(revision) != 40:
            raise SystemExit(
                f"baseline {name} must record a full 40-character revision, got {revision!r}"
            )
        if not isinstance(entry.get("bundle_path"), str):
            raise SystemExit(f"baseline {name} must record a string bundle_path")
    if registry.get("default_baseline") not in baselines:
        raise SystemExit(f"default_baseline is not registered: {registry_path}")
    return registry


def resolve_baseline(name: str | None = None, registry_path: Path = REGISTRY_PATH) -> dict[str, Any]:
    """Resolve a baseline by name to its registry entry. Selection is by name only."""
    registry = load_baselines(registry_path)
    selected = name if name is not None else registry["default_baseline"]
    entry = registry["baselines"].get(selected)
    if entry is None:
        known = ", ".join(sorted(registry["baselines"]))
        raise SystemExit(f"unknown baseline {selected!r}; registered baselines: {known}")
    return {"name": selected, **entry}


def bundle_dir(fixture_root: Path, baseline: dict[str, Any]) -> Path:
    """The bundle directory a baseline owns, relative to the fixture root."""
    bundle_path = baseline["bundle_path"]
    return fixture_root if bundle_path == "" else fixture_root / bundle_path


# Kept for importers that still expect module-level symbols. These are derived from the
# registry's default baseline rather than restated, so the registry stays the only place a
# revision is written down.
_DEFAULT_BASELINE = resolve_baseline()
BASELINE_PREFIX = _DEFAULT_BASELINE["revision"][:8]
BASELINE_REVISION = _DEFAULT_BASELINE["revision"]


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
    parser.add_argument(
        "--baseline",
        help=(
            "named baseline to record, as registered in "
            "fixtures/pi-ai/baselines.json (default: the registry default)"
        ),
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
    baseline = resolve_baseline(args.baseline)
    revision = _git_revision(pi_root)
    if revision != baseline["revision"]:
        raise SystemExit(
            f"pi checkout is {revision}, but baseline {baseline['name']} records "
            f"{baseline['revision']}"
        )

    source_dir = generated_catalog / "providers"
    if not source_dir.is_dir():
        source_dir = generated_catalog
    bundle_root = bundle_dir(fixture_root, baseline)
    provenance_path = bundle_root / "models" / PROVENANCE_FILENAME
    # Never write across baselines: if the destination bundle already records a different
    # baseline, refuse rather than overwrite the only surviving record of that snapshot.
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
    fixture_dir = bundle_root / "models" / "providers"
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
        "baseline": baseline["name"],
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
    provenance_path.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.write_text(
        json.dumps(provenance, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    print(json.dumps(provenance, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
