#!/usr/bin/env python3
"""CTest adapter for the deterministic dual-runtime Native TUI harness.

The pi checkout is an optional local comparison prerequisite. A checkout that
is absent, or that cannot actually run pi, is a skip, not a false pass. The
checkout's own HEAD is the comparison target (ADR 0053 treats pi as reference
material, not a specification); `PI_BASELINE_COMMIT` re-asserts one exact
revision and turns a drifted checkout into an error for reproducible runs.

The checked-in report is evidence about one recorded baseline, so a checkout at
any other revision cannot reproduce it. That used to surface deep inside the
capture as a metadata mismatch after minutes of work. It is checked here
instead, where the message can name both revisions and both remedies.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def checkout_revision(checkout: Path) -> str | None:
    try:
        completed = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip() or None


def recorded_baseline(report: Path) -> tuple[str | None, str | None]:
    """The pi revision and artifact the checked-in report was generated against."""
    try:
        document = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return (None, None)
    baseline = document.get("baseline")
    if not isinstance(baseline, dict):
        return (None, None)
    commit = baseline.get("piCommit")
    artifact = baseline.get("artifact")
    return (commit if isinstance(commit, str) else None, artifact if isinstance(artifact, str) else None)


def unrunnable_reason(checkout: Path) -> str | None:
    """Why this checkout cannot run pi's capture, phrased as the remedy.

    pi's provider catalogs under packages/ai/src/providers/data are generated
    and gitignored, so a checkout can be present, current, and still unable to
    import. Import-time failure inside node reports a missing module rather
    than the missing generate step, which is a poor way to learn this.
    """
    package = checkout / "packages" / "coding-agent" / "package.json"
    if not package.is_file():
        return f"{package} is missing, so {checkout} is not a pi coding-agent checkout"
    data_dir = checkout / "packages" / "ai" / "src" / "providers" / "data"
    if not data_dir.is_dir():
        return (
            f"pi's generated provider data is missing at {data_dir}. Run in the pi checkout: "
            "npm --prefix packages/ai run generate-models"
        )
    if not any(data_dir.glob("*.json")):
        return (
            f"pi's generated provider data directory {data_dir} is empty. Run in the pi checkout: "
            "npm --prefix packages/ai run generate-models"
        )
    return None


def main() -> int:
    source_dir = Path(os.environ.get("CCH_SOURCE_DIR", Path(__file__).parents[3]))
    checkout = Path(os.environ.get("PI_CHECKOUT", source_dir.parent / "pi"))
    if not checkout.is_dir():
        print(f"SKIP: pi checkout is unavailable at {checkout}")
        return 77
    unrunnable = unrunnable_reason(checkout)
    if unrunnable is not None:
        print(f"SKIP: {unrunnable}")
        return 77
    # The capture script resolves its own TypeScript runner; it only needs one
    # to exist somewhere (checkout, CCH_DIFFERENTIAL_TSX, or PATH), so this
    # adapter does not require pi to declare tsx.
    runner = os.environ.get("CCH_DIFFERENTIAL_TSX") or str(checkout / "node_modules" / ".bin" / "tsx")
    if not Path(runner).is_file() and shutil.which("tsx") is None:
        print(f"SKIP: no TypeScript runner (tsx) available for the pi capture at {checkout}")
        return 77

    script = source_dir / "fixtures/pi-coding-agent/capture/native-tui-differential.mts"
    report = source_dir / "fixtures" / "pi-coding-agent" / "differential" / "report.json"
    recorded_commit, recorded_artifact = recorded_baseline(report)
    head = checkout_revision(checkout)
    baseline_commit = os.environ.get("PI_BASELINE_COMMIT")
    if baseline_commit and head is not None and head != baseline_commit:
        print(
            f"ERROR: PI_BASELINE_COMMIT={baseline_commit} but {checkout} is at {head}.\n"
            "  Check out that revision, or unset PI_BASELINE_COMMIT to compare against the checkout's own HEAD.",
            file=sys.stderr,
        )
        return 1
    if head is not None and recorded_commit is not None and head != recorded_commit and not baseline_commit:
        print(
            f"ERROR: the checked-in differential report was generated against {recorded_artifact or recorded_commit}\n"
            f"  ({recorded_commit}), but the pi checkout at {checkout} is at {head}.\n"
            "  The report is evidence about one baseline, so this checkout cannot reproduce it. Either:\n"
            f"    - compare against the recorded baseline: PI_BASELINE_COMMIT={recorded_commit}\n"
            "    - or re-record the evidence against this checkout:\n"
            f"        PI_CHECKOUT={checkout} <tsx> fixtures/pi-coding-agent/capture/native-tui-differential.mts --write\n"
            "      and review the classification change before committing it.",
            file=sys.stderr,
        )
        return 1

    binary = os.environ.get(
        "CCH_DIFFERENTIAL_BINARY",
        str(source_dir / "build/cch_tests_coding_agent_interactive"),
    )
    environment = os.environ.copy()
    environment["CCH_SOURCE_DIR"] = str(source_dir)
    environment["PI_CHECKOUT"] = str(checkout)
    environment["CCH_DIFFERENTIAL_BINARY"] = binary
    if baseline_commit:
        environment["PI_BASELINE_COMMIT"] = baseline_commit
    runner_environment = "CCH_DIFFERENTIAL_TSX"
    if runner_environment not in environment and Path(runner).is_file():
        environment[runner_environment] = str(runner)
    elif shutil.which("tsx") is not None:
        environment.pop(runner_environment, None)
    return subprocess.call(
        [str(runner), str(script), "--verify"],
        cwd=source_dir,
        env=environment,
    )


if __name__ == "__main__":
    sys.exit(main())
