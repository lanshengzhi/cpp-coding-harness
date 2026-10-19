#!/usr/bin/env python3
"""Validate the #1050 v1.3.0 TUI toolkit capability ledger.

Checks the ledger against the same frozen v1.3.0 checkout used for capture, verifies that every
historical v1.0.4 identity is preserved, and checks that every new/changed capability has an
existing v1.3.0 scenario or an explicit comparison route.

Usage:
    python3 fixtures/pi-v1.3.0/audit-1050/validate-ledger.py \
        --pi-checkout /tmp/cch-1050-pi-frozen \
        --ledger docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0"
HISTORICAL_REVISION = "7c10bd4337495ee613f2224843ecdf349b80d1df"
HISTORICAL_LEDGER = Path("docs/research/tui-v1.0.4-capability-ledger.json")
AUDIT_DIR = Path("fixtures/pi-v1.3.0/audit-1050")


def index_exports(index_text: str) -> set[str]:
    names: set[str] = set()
    for match in re.finditer(r"export\s+(?:type\s+)?\{([^}]+)\}\s+from", index_text, re.S):
        for raw in match.group(1).split(","):
            raw = re.sub(r"^type\s+", "", raw.strip())
            if raw:
                names.add(raw.split(" as ")[-1].strip())
    return names


def scenario_names() -> dict[str, set[str]]:
    names: dict[str, set[str]] = {}
    for path in sorted(AUDIT_DIR.glob("*.json")):
        if path.name == "manifest.json":
            continue
        payload = json.loads(path.read_text())
        names[path.name] = {entry["name"] for entry in payload["scenarios"]}
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pi-checkout", type=Path, required=True)
    parser.add_argument("--ledger", type=Path, required=True)
    args = parser.parse_args()
    failures: list[str] = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    head = subprocess.run(
        ["git", "-C", str(args.pi_checkout), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
    ).stdout.strip()
    check(head == REVISION, f"frozen checkout is {head}, expected {REVISION}")

    index_text = subprocess.run(
        ["git", "-C", str(args.pi_checkout), "show", f"{REVISION}:packages/tui/src/index.ts"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    frozen_exports = index_exports(index_text)

    ledger = json.loads(args.ledger.read_text())
    check(ledger.get("schema") == "pike/pi-v1.3.0-capability-ledger/1", "unexpected ledger schema")
    check(ledger.get("baseline") == "pi-v1.3.0", "unexpected baseline")
    check(ledger.get("revision") == REVISION, "unexpected ledger revision")

    rows = ledger["rows"]
    by_id = {}
    for row in rows:
        check(row["id"] not in by_id, f"duplicate row id {row['id']}")
        by_id[row["id"]] = row
        check(bool(row["source"]), f"{row['id']}: missing source endpoints")
        check(bool(row["cpp"].get("interface")), f"{row['id']}: missing C++ interface")
        check(bool(row.get("comparisonCase")), f"{row['id']}: missing comparison case")
        check(bool(row["consumer"]), f"{row['id']}: missing consumer")
        check(row["comparisonRoute"] in {
            "v1.3.0-observation",
            "member-absent-from-diff",
            "unchanged-frozen-source",
            "not-audited",
            "not-applicable",
        }, f"{row['id']}: unknown comparison route {row['comparisonRoute']}")

    export_rows = {
        row["export"]: row for row in rows if row.get("member") is None and row.get("export") and row["kind"] in {
            "function", "interface", "type", "class", "const", "excluded-reexport"
        }
    }
    check(set(export_rows) == frozen_exports, f"index export drift: missing={sorted(frozen_exports - set(export_rows))} extra={sorted(set(export_rows) - frozen_exports)}")

    historical = json.loads(HISTORICAL_LEDGER.read_text())
    historical_ids = {row["id"] for row in historical["rows"]}
    check(historical_ids <= set(by_id), f"historical ids lost: {sorted(historical_ids - set(by_id))[:5]}")
    excluded = [row for row in rows if row["membership"] == "excluded"]
    check(len(excluded) == 5, f"expected 5 platform exclusions, found {len(excluded)}")
    for row in excluded:
        check(row["verification"] == "not-applicable", f"{row['id']}: exclusion must be not-applicable")
        check(not row["gaps"], f"{row['id']}: exclusion must not report implementation gaps")
        check(not row.get("evidence"), f"{row['id']}: exclusion must not carry evidence")

    artifacts = scenario_names()
    for row in rows:
        evidence = row.get("evidence")
        if evidence:
            artifact = evidence["artifact"]
            if "audit-1050/" in artifact:
                name = artifact.rsplit("/", 1)[-1]
                check(name in artifacts, f"{row['id']}: unknown evidence artifact {name}")
                if name in artifacts:
                    check(evidence["scenario"] in artifacts[name], f"{row['id']}: scenario {evidence['scenario']} absent from {name}")
                check(evidence.get("baseline") == "pi-v1.3.0", f"{row['id']}: v1.3.0 evidence mislabelled")
                check(evidence.get("revision") == REVISION, f"{row['id']}: v1.3.0 evidence revision mismatch")
            elif "pi-tui/bundles/pi-v1.0.4" in artifact:
                check(evidence.get("historicalBaseline") == "pi-v1.0.4", f"{row['id']}: historical evidence mislabelled")
                check(evidence.get("historicalRevision") == HISTORICAL_REVISION, f"{row['id']}: historical revision mismatch")
            else:
                failures.append(f"{row['id']}: unrecognized evidence artifact {artifact}")
        if row["verification"] == "upstream-captured":
            check(bool(evidence), f"{row['id']}: upstream-captured without evidence")
        if row["verification"] == "unverified" and row["comparisonRoute"] != "not-audited":
            check(bool(evidence), f"{row['id']}: unverified non-audited route without comparison evidence")
        if row["implementation"] in ("partial", "missing") and row["membership"] == "supported":
            check(bool(row["gaps"]), f"{row['id']}: implementation gap without a gap note")

    required = {
        "renderFakeCursor": ("missing", "upstream-captured"),
        "formatProgramStatus": ("missing", "upstream-captured"),
        "ProgramStatus": ("missing", "upstream-captured"),
        "Box.setPaddingX": ("missing", "upstream-captured"),
        "Text.setPaddingX": ("implemented", "upstream-captured"),
        "TuiAltScreen.resetTextSelection": ("missing", "upstream-captured"),
        "Terminal.setProgramStatus": ("missing", "upstream-captured"),
        "ProcessTerminal.setProgramStatus": ("missing", "upstream-captured"),
        "detectCapabilities": ("partial", "upstream-captured"),
        "parseKey": ("partial", "upstream-captured"),
        "matchesKey": ("partial", "upstream-captured"),
        "StdinBuffer.process": ("partial", "upstream-captured"),
        "TuiAltScreen.handleViewportInput": ("partial", "upstream-captured"),
        "TUI.showOverlay": ("partial", "upstream-captured"),
        "Editor.render": ("partial", "upstream-captured"),
        "Input.render": ("partial", "upstream-captured"),
        "CURSOR_MARKER": ("partial", "upstream-captured"),
        "cch_tui:program-status:detection-override": ("missing", "upstream-captured"),
    }
    for row_id, (implementation, verification) in required.items():
        row = by_id.get(row_id)
        check(row is not None, f"required change row missing: {row_id}")
        if row:
            check(row["implementation"] == implementation, f"{row_id}: expected {implementation}, found {row['implementation']}")
            check(row["verification"] == verification, f"{row_id}: expected {verification}, found {row['verification']}")

    new_rows = [row for row in rows if row["id"] not in historical_ids]
    check(len(new_rows) == 18, f"expected 18 new v1.3.0 rows, found {len(new_rows)}")

    if failures:
        print("LEDGER VALIDATION FAILED")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print(
        "LEDGER VALIDATION PASSED: "
        f"{len(rows)} rows, {len(export_rows)} index exports, {len(excluded)} exclusions, "
        f"{len(new_rows)} v1.3.0 additions, {len(artifacts)} evidence artifacts"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
