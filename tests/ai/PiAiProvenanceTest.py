#!/usr/bin/env python3
"""Exercise the pi-ai provenance checker with stale-snapshot mutations."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts" / "ai" / "check_pi_ai_provenance.py"
FIXTURES = ROOT / "fixtures" / "pi-ai"


def run_checker(fixture_root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(CHECKER),
            "--fixture-root",
            str(fixture_root),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_failure(result: subprocess.CompletedProcess[str], detail: str) -> None:
    if result.returncode == 0:
        raise AssertionError(f"{detail}: checker unexpectedly passed")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cch-pi-ai-provenance-") as directory:
        root = Path(directory) / "pi-ai"
        shutil.copytree(FIXTURES, root)
        baseline = run_checker(root)
        if baseline.returncode != 0:
            raise AssertionError(f"committed provenance does not verify:\n{baseline.stderr}")

        hash_root = Path(directory) / "hash-mutation"
        shutil.copytree(FIXTURES, hash_root)
        provenance_path = hash_root / "models" / "provenance.json"
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        provenance["providers"]["deepseek"]["sha256"] = "0" * 64
        write_json(provenance_path, provenance)
        expect_failure(run_checker(hash_root), "stale artifact hash")

        model_set_root = Path(directory) / "model-set-mutation"
        shutil.copytree(FIXTURES, model_set_root)
        artifact_path = model_set_root / "models" / "providers" / "deepseek.json"
        artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
        del artifact["openai-completions"]["deepseek-v4-pro"]
        write_json(artifact_path, artifact)
        provenance_path = model_set_root / "models" / "provenance.json"
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        record = provenance["providers"]["deepseek"]
        record["sha256"] = sha256(artifact_path)
        record["bytes"] = artifact_path.stat().st_size
        write_json(provenance_path, provenance)
        result = run_checker(model_set_root)
        expect_failure(result, "stale model set")
        if "model set mismatch" not in result.stderr:
            raise AssertionError(f"model-set mutation failed for the wrong reason:\n{result.stderr}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
