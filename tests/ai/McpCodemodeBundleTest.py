#!/usr/bin/env python3
"""Exercise the pi-v1.0.4 MCP/codemode evidence bundle and its provenance checker.

The bundle captured by ``fixtures/pi-ai/capture/capture-mcp-codemode.mts`` (ADR 0065,
spec #882) is the differential baseline the MCP and codemode parity tickets diff against.
This test is the red/green cover for its existence, shape, and provenance: it runs the
baseline-selected checker green on the committed bundle, asserts the registry records the
capture, and mutates artifacts and provenance to prove the checker refuses a stale bundle.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "fixtures" / "pi-ai"
REGISTRY = FIXTURES / "baselines.json"
CAPTURE = FIXTURES / "capture" / "capture-mcp-codemode.mts"
CHECKER = ROOT / "scripts" / "ai" / "check_mcp_codemode_provenance.py"
BASELINE = "pi-v1.0.4"
BUNDLE_SUBDIR = "mcp-codemode"
BUNDLE = FIXTURES / "v1.0.4" / BUNDLE_SUBDIR

# Every artifact the capture writes into the bundle, relative to ``BUNDLE``.
ARTIFACTS = (
    "mcp-tool-surface.json",
    "mcp-config-surface.json",
    "codemode-tool.json",
    "codemode-source-grammar.lark",
    "mcp-protocol-surface.json",
)


def run_checker(fixture_root: Path, baseline: str = BASELINE) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(CHECKER),
            "--fixture-root",
            str(fixture_root),
            "--baseline",
            baseline,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _fingerprint(root: Path) -> dict[str, str]:
    digest: dict[str, str] = {}
    if not root.is_dir():
        return digest
    for path in sorted(root.rglob("*")):
        if path.is_file():
            digest[str(path.relative_to(root))] = sha256(path)
    return digest


def check_committed_bundle_verifies() -> None:
    result = run_checker(FIXTURES)
    if result.returncode != 0:
        raise AssertionError(f"committed mcp/codemode provenance does not verify:\n{result.stderr}")
    if BASELINE not in result.stdout:
        raise AssertionError(f"checker did not report the selected baseline:\n{result.stdout}")


def check_registry_records_capture() -> None:
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    baselines = registry["baselines"]
    entry = baselines[BASELINE]
    if not entry.get("captured_at"):
        raise AssertionError(f"{BASELINE} still records no capture time")
    digests = entry.get("digests")
    if not isinstance(digests, dict) or not digests:
        raise AssertionError(f"{BASELINE} records no artifact digests")
    if sorted(digests) != sorted(f"{BUNDLE_SUBDIR}/{name}" for name in ARTIFACTS):
        raise AssertionError(f"{BASELINE} digests do not cover exactly the bundle artifacts: {sorted(digests)}")
    for name, digest in digests.items():
        if digest != sha256(FIXTURES / "v1.0.4" / name):
            raise AssertionError(f"{BASELINE} digest for {name} does not match the committed artifact")
    # Recording the new baseline must not rewrite the preserved entries.
    if baselines["pi-v0.87.1"]["captured_at"] != "2026-09-23T12:05:52Z":
        raise AssertionError("the preserved pi-v0.87.1 capture time was rewritten")
    if "digests" in baselines["pi-v0.87.1"] or "digests" in baselines["pi-v1.0.0"]:
        raise AssertionError("a preserved baseline gained digests")


def check_bundle_shape() -> None:
    for name in ARTIFACTS:
        path = BUNDLE / name
        if not path.is_file():
            raise AssertionError(f"bundle artifact is missing: {path}")
        if path.stat().st_size == 0:
            raise AssertionError(f"bundle artifact is empty: {path}")
    grammar = (BUNDLE / "codemode-source-grammar.lark").read_text(encoding="utf-8")
    # The grammar is the verbatim ``String.raw`` value, which begins with a newline.
    if not grammar.startswith("\nstart: options_source | plain_source"):
        raise AssertionError(f"grammar artifact is not the verbatim source grammar:\n{grammar!r}")
    for production in ("OPTIONS_LINE:", "NEWLINE:", "SOURCE:"):
        if production not in grammar:
            raise AssertionError(f"grammar artifact is missing production {production}")


def check_captured_surfaces() -> None:
    tool_surface = json.loads((BUNDLE / "mcp-tool-surface.json").read_text(encoding="utf-8"))
    if tool_surface["toolName"]["pattern"] != "mcp__<server>__<tool>":
        raise AssertionError("MCP tool-name pattern drifted")
    if tool_surface["exposureMapping"] != {
        "codemode": "deferred",
        "deferred": "deferred",
        "direct": "direct",
        "hidden": "hidden",
    }:
        raise AssertionError("MCP exposure mapping drifted")

    codemode_tool = json.loads((BUNDLE / "codemode-tool.json").read_text(encoding="utf-8"))
    if codemode_tool["name"] != "codemode":
        raise AssertionError("codemode tool name drifted")
    if set(codemode_tool["parameters"]["properties"]) != {"code"}:
        raise AssertionError("codemode tool input schema drifted")
    if codemode_tool["constrainedSampling"]["variants"]["openai_lark"] != (
        BUNDLE / "codemode-source-grammar.lark"
    ).read_text(encoding="utf-8"):
        raise AssertionError("the codemode tool no longer attaches the captured grammar variant")

    config_surface = json.loads((BUNDLE / "mcp-config-surface.json").read_text(encoding="utf-8"))
    for exposure in ("codemode", "deferred", "direct", "hidden"):
        if exposure not in json.dumps(config_surface):
            raise AssertionError(f"config surface omits exposure {exposure}")

    protocol_surface = json.loads((BUNDLE / "mcp-protocol-surface.json").read_text(encoding="utf-8"))
    if protocol_surface["notifications"]["cancelled"]["method"] != "notifications/cancelled":
        raise AssertionError("cancelled-notification method drifted")


def check_capture_is_baseline_selected() -> None:
    source = CAPTURE.read_text(encoding="utf-8")
    if "bundle_path" not in source or "refusing" not in source:
        raise AssertionError("capture does not resolve a bundle_path and refuse cross-baseline writes")
    if "PI_BASELINE" not in source:
        raise AssertionError("capture does not select its baseline by name")
    if 'path.join(fixtureDir, "mcp-codemode")' in source:
        raise AssertionError("capture hardcodes its bundle destination instead of deriving it")


def check_selection_and_mutations(tmp: Path) -> None:
    """Selection is real, and the checker refuses a stale bundle."""
    # Selecting the captured baseline passes; a baseline without a bundle fails loudly.
    missing = run_checker(FIXTURES, baseline="pi-v1.0.0")
    if missing.returncode == 0:
        raise AssertionError("checker passed for a baseline whose bundle does not exist")

    hash_root = tmp / "hash-mutation"
    shutil.copytree(FIXTURES, hash_root)
    artifact = hash_root / "v1.0.4" / BUNDLE_SUBDIR / "codemode-tool.json"
    artifact.write_text(artifact.read_text(encoding="utf-8") + "\n", encoding="utf-8")
    result = run_checker(hash_root)
    if result.returncode == 0:
        raise AssertionError("checker passed with a mutated artifact")
    if "SHA-256 mismatch" not in result.stderr:
        raise AssertionError(f"artifact mutation failed for the wrong reason:\n{result.stderr}")

    provenance_root = tmp / "provenance-mutation"
    shutil.copytree(FIXTURES, provenance_root)
    provenance_path = provenance_root / "v1.0.4" / BUNDLE_SUBDIR / "provenance.json"
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance["baseline"] = "pi-v1.0.0"
    write_json(provenance_path, provenance)
    result = run_checker(provenance_root)
    if result.returncode == 0:
        raise AssertionError("checker passed with a provenance record bound to another baseline")

    absent_root = tmp / "absent-bundle"
    shutil.copytree(FIXTURES, absent_root)
    shutil.rmtree(absent_root / "v1.0.4" / BUNDLE_SUBDIR)
    result = run_checker(absent_root)
    if result.returncode == 0:
        raise AssertionError("checker passed with the bundle removed")


def check_real_fixture_tree_never_mutated() -> None:
    before = _fingerprint(FIXTURES)
    checks = (
        check_committed_bundle_verifies,
        check_registry_records_capture,
        check_bundle_shape,
        check_captured_surfaces,
        check_capture_is_baseline_selected,
    )
    for check in checks:
        check()
        print(f"ok: {check.__name__}")
    with tempfile.TemporaryDirectory(prefix="cch-mcp-codemode-bundle-") as directory:
        check_selection_and_mutations(Path(directory))
        print("ok: check_selection_and_mutations")
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
    check_real_fixture_tree_never_mutated()
    print("pi-v1.0.4 mcp/codemode bundle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
