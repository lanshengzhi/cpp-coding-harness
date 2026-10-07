#!/usr/bin/env python3
"""Verify the committed MCP/codemode evidence bundle for a named pi baseline (spec #882).

The baseline-selected equivalent of ``check_pi_ai_provenance.py`` for the MCP/codemode bundle
(ADR 0065): the expected revision is the registry entry for the selected baseline, and each
artifact is checked against its recorded SHA-256/byte count and against the registry's digests.
There is no free-form revision input, so a bundle cannot be verified against another baseline's
checkout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from record_pi_ai_provenance import (
    REGISTRY_PATH,
    bundle_dir,
    resolve_baseline,
)
from record_mcp_codemode_provenance import (
    BUNDLE_SUBDIR,
    GENERATOR,
    PROVENANCE_FILENAME,
    SCHEMA,
)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-root", default=Path("fixtures/pi-ai"), type=Path)
    parser.add_argument(
        "--baseline",
        help="named baseline to verify, as registered in fixtures/pi-ai/baselines.json (default: the registry default)",
    )
    parser.add_argument("--pi-root", type=Path)
    return parser.parse_args()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    args = _parse_args()
    fixture_root = args.fixture_root.resolve()
    # Verification binds to the baseline that owns the bundle under test, not to a module constant.
    baseline = resolve_baseline(args.baseline)
    bundle_root = bundle_dir(fixture_root, baseline)
    provenance_path = bundle_root / BUNDLE_SUBDIR / PROVENANCE_FILENAME
    try:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read provenance {provenance_path}: {error}") from error
    if provenance.get("schema") != SCHEMA:
        raise SystemExit("unexpected provenance schema")
    if provenance.get("baseline") != baseline["name"]:
        raise SystemExit(
            f"bundle records baseline {provenance.get('baseline')!r}, not {baseline['name']!r}"
        )
    source = provenance.get("source", {})
    if source.get("revision") != baseline["revision"]:
        raise SystemExit(
            f"provenance source revision is not the {baseline['name']} revision ({baseline['revision']})"
        )
    if source.get("generator") != GENERATOR:
        raise SystemExit(f"provenance names an unexpected generator: {source.get('generator')!r}")
    if args.pi_root and subprocess.check_output(
        ["git", "-C", str(args.pi_root.resolve()), "rev-parse", "HEAD"], text=True
    ).strip() != baseline["revision"]:
        raise SystemExit(f"pi checkout is not at the {baseline['name']} revision")

    registry = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    recorded = registry.get("baselines", {}).get(baseline["name"], {})
    if recorded.get("captured_at") != source.get("captured_at"):
        raise SystemExit("registry captured_at does not match the recorded provenance")
    registry_digests = recorded.get("digests")
    if not isinstance(registry_digests, dict) or not registry_digests:
        raise SystemExit(f"registry records no digests for {baseline['name']}")

    artifacts = provenance.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        raise SystemExit("provenance records no artifacts")
    computed: dict[str, str] = {}
    for relative, record in sorted(artifacts.items()):
        if not isinstance(record, dict):
            raise SystemExit(f"invalid artifact record: {relative}")
        path = bundle_root / relative
        if not path.is_file():
            raise SystemExit(f"missing artifact: {path}")
        digest = _sha256(path)
        if digest != record.get("sha256"):
            raise SystemExit(f"SHA-256 mismatch: {path}")
        if path.stat().st_size != record.get("bytes"):
            raise SystemExit(f"byte count mismatch: {path}")
        computed[relative] = digest
    if computed != registry_digests:
        raise SystemExit("registry digests do not match the recorded artifacts")

    print(
        f"pi MCP/codemode provenance: PASS ({len(computed)} artifacts verified; "
        f"baseline {baseline['name']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
