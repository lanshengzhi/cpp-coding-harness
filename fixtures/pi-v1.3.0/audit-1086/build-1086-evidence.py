#!/usr/bin/env python3
"""Assemble the #1086 SQLite cross-runtime evidence bundle from frozen-pi observations."""

from __future__ import annotations

import argparse
import base64
import gzip
import os
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import sqlite3
import subprocess


BASELINE = "pi-v1.3.0"
REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0"
TICKET = "1086"
FAMILY = "audit-1086"
MANIFEST_SCHEMA = "pike/pi-v1.3.0-audit-evidence/1"
TASK_KIND = 'audit1086.work"\\é\x1f'
DOC_KIND = 'audit1086.history"\\é'
REQUEST_ID = 'audit1086/request"\\é'
SOURCE_ENDPOINTS = [
    "packages/durable/src/harness/harness.ts:Harness.open,root,resume,close",
    "packages/durable/src/storage/sqlite/node.ts:openNodeSqliteStorage",
    "packages/durable/src/storage/sqlite/storage.ts:SqliteStorage.scanTasks,submissionByRequest,findDocument",
    "packages/durable/src/tasks.ts:defineTask",
    "packages/durable/src/documents.ts:defineDoc",
]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def raw_columns(path: Path, ids: dict) -> dict:
    connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        task_id = ids.get("task", ids.get("id"))
        task_kind = connection.execute("SELECT kind FROM tasks WHERE id = ?", (task_id,)).fetchone()[0]
        request_id = connection.execute(
            "SELECT request_id FROM submissions WHERE id = ?", (ids["submission"],)
        ).fetchone()[0]
        documents = connection.execute(
            "SELECT kind, key_value FROM documents WHERE kind LIKE ? ORDER BY id",
            ('"audit1086%',),
        ).fetchall()
        version = connection.execute("SELECT version FROM durable_schema").fetchone()[0]
        next_id, next_seq = connection.execute("SELECT next_id, next_seq FROM durable_metadata").fetchone()
        return {
            "schemaVersion": version,
            "nextId": next_id,
            "nextSeq": next_seq,
            "taskKind": task_kind,
            "requestId": request_id,
            "documents": [{"kind": kind, "keyValue": key_value} for kind, key_value in documents],
        }
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture_dir", type=Path, help="directory containing pi/ and cpp/ observations")
    parser.add_argument(
        "--bundle",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="audit family bundle directory",
    )
    args = parser.parse_args()

    # Refuse any target that is a registered manifest, artifact, or family
    # directory, even when --bundle uses an alias or a different family name.
    fixture_root = Path(__file__).resolve().parents[1]
    registry = json.loads((fixture_root / "baselines.json").read_text())
    target = args.bundle.resolve()
    for baseline in registry["baselines"].values():
        for entry in baseline["bundles"].values():
            manifest_path = (fixture_root / entry["manifest"]).resolve()
            if target == manifest_path.parent:
                raise SystemExit(f"refusing to overwrite registered family: {target}")
            manifest = json.loads(manifest_path.read_text())
            protected = [manifest_path] + [(manifest_path.parent / item["path"]).resolve() for item in manifest["artifacts"]]
            if any((target / name).resolve() in protected for name in ("manifest.json", "interchange.json")):
                raise SystemExit(f"refusing to overwrite registered evidence in {target}")

    pi_observations = json.loads((args.capture_dir / "pi" / "observations.json").read_text())
    cpp_observations = json.loads((args.capture_dir / "cpp" / "observations.json").read_text())
    pi_store = args.capture_dir / "pi" / "storage.sqlite"
    cpp_store = args.capture_dir / "cpp" / "storage.sqlite"
    if gzip.decompress(base64.b64decode(pi_observations["storeGzipBase64"], validate=True)) != pi_store.read_bytes():
        raise SystemExit("pi fixture bytes do not match the captured store")

    captured_at = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    libc, glibc_version = platform.libc_ver()
    node_version = subprocess.check_output(["node", "--version"], text=True).strip()
    tsx_version = json.loads(
        (Path(os.environ["PI_CHECKOUT"]) / "node_modules/tsx/package.json").read_text()
    )["version"]
    environment = {
        "platform": "linux",
        "arch": "x64",
        "libc": libc or "glibc",
        "glibcVersion": glibc_version,
        "node": node_version,
        "tsx": tsx_version,
    }

    scenarios = [
        {
            "name": "pi creates a SQLite store with an unfinished task, document and checkpoint",
            "inputs": {
                "backend": "sqlite",
                "creator": "pi",
                "taskKind": TASK_KIND,
                "documentKind": DOC_KIND,
                "requestId": REQUEST_ID,
                "action": "create and close",
            },
            "expected": {
                "storeGzipBase64": pi_observations["storeGzipBase64"],
                "ids": pi_observations["ids"],
                "task": pi_observations["task"],
                "document": pi_observations["document"],
                "submission": pi_observations["submission"],
                "raw": raw_columns(pi_store, pi_observations["ids"]),
            },
        },
        {
            "name": "C++ creates a SQLite store that pi reopens, indexes and advances",
            "inputs": {
                "backend": "sqlite",
                "creator": "c++",
                "taskKind": TASK_KIND,
                "documentKind": DOC_KIND,
                "requestId": REQUEST_ID,
                "action": "scanTasks, submissionByRequest, findDocument, resume",
                "fixture": {"encoding": "gzip+base64", "sqlite": cpp_observations["storeGzipBase64"]},
                "ids": cpp_observations["ids"],
            },
            "expected": {
                "initial": cpp_observations["initial"],
                "advanced": cpp_observations["advanced"],
                "rawAfterPiAdvance": raw_columns(cpp_store, cpp_observations["ids"]),
            },
        },
    ]

    artifact = {
        "schema": MANIFEST_SCHEMA,
        "baseline": BASELINE,
        "revision": REVISION,
        "ticket": TICKET,
        "family": FAMILY,
        "capturedAt": captured_at,
        "environment": environment,
        "sourceEndpoints": SOURCE_ENDPOINTS,
        "scenarios": scenarios,
    }
    artifact_path = args.bundle / "interchange.json"
    artifact_path.write_text(json.dumps(artifact, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    payload = artifact_path.read_bytes()

    capture_script = args.bundle / "continue-1086-sqlite.mts"
    assembler = Path(__file__).resolve()
    test_source = Path(__file__).resolve().parents[3] / "tests" / "durable" / "SqliteStorageTest.cpp"
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "baseline": BASELINE,
        "revision": REVISION,
        "ticket": TICKET,
        "family": FAMILY,
        "capturedAt": captured_at,
        "environment": environment,
        "generator": {
            "path": "fixtures/pi-v1.3.0/audit-1086/continue-1086-sqlite.mts",
            "sha256": digest(capture_script),
            "command": (
                "PI_CHECKOUT=/tmp/cch-1080-pi-frozen python3 fixtures/pi-v1.3.0/audit-1086/capture-1086.py build/audit-1086"
            ),
            "sources": [
                {
                    "path": "fixtures/pi-v1.3.0/audit-1086/capture-1086.py",
                    "sha256": digest(assembler.parent / "capture-1086.py"),
                },
                {
                    "path": "fixtures/pi-v1.3.0/audit-1086/build-1086-evidence.py",
                    "sha256": digest(assembler),
                },
                {
                    "path": "tests/durable/SqliteStorageTest.cpp",
                    "sha256": digest(test_source),
                },
            ],
        },
        "artifacts": [
            {
                "path": "interchange.json",
                "family": FAMILY,
                "bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
                "sourceEndpoints": SOURCE_ENDPOINTS,
                "scenarios": scenarios,
            }
        ],
    }
    (args.bundle / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"wrote {artifact_path} ({len(payload)} bytes)")
    print(f"wrote {args.bundle / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
