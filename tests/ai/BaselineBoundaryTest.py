#!/usr/bin/env python3
"""Assert the pi-ai evidence tools resolve paths through the Named Baseline registry.

Regression cover for two defects that shipped with ADR 0065's migration while the whole
suite stayed green:

1. ``capture-completions-ts-events.mts`` guarded only the paths passed on ``process.argv``,
   while writing to a hardcoded ``fixtureDir/wire``. Under the README invocation no
   destination argument is passed, so selecting ``pi-v1.0.0`` wrote three evidence files
   into the ``pi-v0.87.1`` bundle.
2. ``t0_cost_probe.mjs`` read ``args.fixtureRoot/models/providers`` regardless of the
   selected baseline, so a ``pi-v1.0.0`` report would carry ``pi-v0.87.1`` model data
   under a ``pi-v1.0.0`` label.

A guard that inspects arguments instead of the destination it actually writes is not a
boundary, so this test pins the resolved destinations rather than the guard's existence.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "fixtures" / "pi-ai"
REGISTRY = FIXTURES / "baselines.json"
CAPTURE = FIXTURES / "capture" / "capture-completions-ts-events.mts"
PROBE = ROOT / "scripts" / "ai" / "t0_cost_probe.mjs"

SNAPSHOT_RETURN_CODE = 77




def _tsx() -> Path | None:
    """The sibling pi checkout's tsx, when this environment has one."""
    candidate = ROOT.parent / "pi" / "node_modules" / ".bin" / "tsx"
    return candidate if candidate.is_file() else None


def check_registry_has_both_baselines() -> None:
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    baselines = registry["baselines"]
    for name in ("pi-v0.87.1", "pi-v1.0.0"):
        if name not in baselines:
            raise AssertionError(f"registry is missing baseline {name}")
        if len(baselines[name]["revision"]) != 40:
            raise AssertionError(f"baseline {name} does not record a full 40-character revision")
    if baselines["pi-v0.87.1"]["bundle_path"] != "":
        raise AssertionError("the historical bundle must keep owning the fixture root")


def check_no_hardcoded_bundle_destinations() -> None:
    """Neither guard may hardcode a destination; both must resolve via the registry."""
    capture = CAPTURE.read_text(encoding="utf-8")
    probe = PROBE.read_text(encoding="utf-8")

    if 'path.join(fixtureDir, "wire")' in capture:
        raise AssertionError(
            "capture writes to a hardcoded fixture-root wire directory; the destination must "
            "be derived from the selected baseline's bundle_path"
        )
    if 'join(fixtureRoot, "models", "providers"' in probe:
        raise AssertionError(
            "probe reads models from a fixed models/providers path; the catalog must come from "
            "the selected baseline's bundle_path"
        )
    for source, label in ((capture, "capture"), (probe, "probe")):
        if "bundle_path" not in source:
            raise AssertionError(f"{label} does not resolve a bundle_path at all")
        if "refusing" not in source:
            raise AssertionError(f"{label} has no cross-baseline refusal message")


def _stage_capture_sandbox(tmp: Path) -> Path:
    """Mirror the capture script's fixture layout inside a temporary directory.

    The capture script derives its fixture root and pi checkout from its own location, so
    running it against the repository would let a regression write into real evidence —
    and would make any cleanup step a chance to delete it. Staging the script plus the
    registry into a temporary tree and pointing PI_CHECKOUT at the real sibling checkout
    keeps every write inside the sandbox, which is what makes this test safe to re-run.
    """
    fixture_root = tmp / "fixtures" / "pi-ai"
    (fixture_root / "capture").mkdir(parents=True)
    shutil.copyfile(CAPTURE, fixture_root / "capture" / CAPTURE.name)
    shutil.copyfile(REGISTRY, fixture_root / REGISTRY.name)
    # Stands in for the historical bundle, which owns the fixture root in the real tree.
    (fixture_root / "wire").mkdir()
    return fixture_root


def check_capture_does_not_write_into_the_historical_bundle() -> None:
    """Selecting an uncaptured baseline must not deposit files in the v0.87.1 bundle."""
    tsx = _tsx()
    pi_checkout = ROOT.parent / "pi"
    if tsx is None or not pi_checkout.is_dir():
        print("SKIP: sibling pi checkout or its tsx is unavailable; static checks still ran")
        return

    with tempfile.TemporaryDirectory() as directory:
        sandbox = _stage_capture_sandbox(Path(directory))
        historical_wire = sandbox / "wire"
        environment = {
            **os.environ,
            "PI_BASELINE": "pi-v1.0.0",
            "PI_CHECKOUT": str(pi_checkout),
        }
        result = subprocess.run(
            [str(tsx), str(sandbox / "capture" / CAPTURE.name)],
            cwd=sandbox.parent,
            env=environment,
            text=True,
            capture_output=True,
            timeout=600,
        )
        deposited = sorted(path.name for path in historical_wire.iterdir())
        if deposited:
            raise AssertionError(
                "capturing pi-v1.0.0 wrote into the pi-v0.87.1 bundle's wire directory: "
                f"{deposited} (exit {result.returncode})"
            )
        # Whatever the outcome, the refusal must name the baseline rather than silently
        # succeeding against another bundle's evidence.
        combined = result.stdout + result.stderr
        if result.returncode == 0 and "pi-v1.0.0" not in combined:
            raise AssertionError("capture neither failed nor reported its baseline")

    # No cleanup of the repository fixture tree is needed or performed: every write this
    # test makes lands inside its own temporary sandbox. A real pi-v1.0.0 bundle may already
    # exist, and evidence the test does not own is evidence it must never remove.


def _fingerprint(root: Path) -> dict[str, str]:
    """Path -> sha256 for every file under root, so a mutation is detectable byte-wise."""
    digest: dict[str, str] = {}
    if not root.is_dir():
        return digest
    for path in sorted(root.rglob("*")):
        if path.is_file():
            digest[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return digest


def check_real_fixture_tree_is_never_mutated() -> None:
    """The test may not add, alter, or remove anything in the real evidence tree.

    @Cindy asked for the narrower form of this — "if fixtures/pi-ai/v1.0.0 exists and is
    non-empty, it must still exist with identical contents" — which is right and would by
    itself have rejected the whole-directory ``rmtree`` this test previously carried. The
    form implemented here is strictly stronger: it fingerprints the entire real fixture tree
    before and after every check, so no bundle, present or future, can be disturbed. It also
    keeps the "does not overstep under the real layout" coverage that staging into a sandbox
    would otherwise drop.
    """
    before = _fingerprint(FIXTURES)
    checks = (
        check_registry_has_both_baselines,
        check_no_hardcoded_bundle_destinations,
        check_capture_does_not_write_into_the_historical_bundle,
    )
    for check in checks:
        check()
        print(f"ok: {check.__name__}")

    after = _fingerprint(FIXTURES)
    added = sorted(set(after) - set(before))
    removed = sorted(set(before) - set(after))
    changed = sorted(path for path in set(before) & set(after) if before[path] != after[path])
    if added or removed or changed:
        raise AssertionError(
            "the test mutated the real fixture tree — "
            f"added={added} removed={removed} changed={changed}"
        )
    print(f"ok: real fixture tree unchanged ({len(before)} files fingerprinted)")


def main() -> int:
    check_real_fixture_tree_is_never_mutated()
    print("pi-ai baseline boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
