# #1075 durable SQLite backend: delivery record

Delivered on `task/1044-1075-sqlite` against the `pi-v1.3.0 /
71023aec1546d6d677a65669c5665c0927ce0dd0` named baseline.

## Scope

`cch::durable::SqliteStorage` is the SQLite backend of the frozen public
`Storage` contract. `SqliteStorage::open` opens or creates a SQLite database,
applies schema migrations, configures WAL and busy timeout pragmas, and
recovers published records and allocator high-water marks. `Storage::commit`,
reads, and `Session` interact with it through the existing public `Storage`
interface.

The implementation is in [SqliteStorage.cpp](../../../src/durable/SqliteStorage.cpp)
and [SqliteStorage.hpp](../../../src/durable/include/cch/durable/SqliteStorage.hpp)
with test cases in [SqliteStorageTest.cpp](../../../tests/durable/SqliteStorageTest.cpp).
Shared validated state and allocator high-water marks recovery are in
[StorageState.cpp](../../../src/durable/StorageState.cpp).

## Delivered behavior

- `SqliteStorage::open` applies schema migration version 1, creating
  STRICT tables for `record_ids`, `conversations`, `entries`, `tasks`,
  `submissions`, `documents`, `document_revisions`, `durable_metadata`, and
  `durable_schema`, matching `audit-1054`.
- Shared Chord delta op serialization and deserialization helpers are factored
  into `StorageEncoding.hpp` and `StorageEncoding.cpp` (§16.1), shared between
  `JsonlStorage` and `SqliteStorage`.
- Configured options support `prefix`, `busy_timeout_ms`, and
  `wal_auto_checkpoint_pages`.
- Reserved prefix `sqlite_` is rejected. Prefixes isolate schemas within the
  same database. Stores written by a newer schema version (> 1) are rejected.
- Each atomic batch runs inside `BEGIN IMMEDIATE` / `COMMIT`. Any SQL failure
  rolls back all staged writes and leaves the sequence unchanged.
- Document creations and revisions without ops are stored as `base` revisions.
  Revisions with delta ops are stored as `delta` revisions.
- Document retirement updates persisted record JSON and records `retired_seq`.
- Recovery restores conversations, entries, and document revisions, verifies ID
  claims in the `record_ids` ledger, detects ledger corruption or schema mismatch,
  and repairs lagging allocator metadata rows up to observed record high-water marks.
- Empty commit batches advance and persist sequence continuity across reopens.
- Higher-level callers (`Session`, `Transaction`) execute normal transactions,
  scans, and document revisions over SQLite, as well as rollback mid-batch
  injected trigger failures.

Frozen observations captured in
[audit-1075/sqlite-transaction-options.json](../../../fixtures/pi-v1.3.0/audit-1075/sqlite-transaction-options.json)
and reused from [audit-1054/sqlite-store.json](../../../fixtures/pi-v1.3.0/audit-1054/sqlite-store.json):

- `the SQLite schema records the durable tables, metadata and document revisions` (tested via `FrozenScenario`)
- `SQLite applies entry ID bounds and stores document revisions with configured busy and WAL options`
- `the SQLite schema prefix renames every durable table and isolates an unprefixed schema`
- `a failing SQLite batch rolls back every write and leaves the sequence unchanged`
- `SQLite refuses a store written by a newer durable schema version`

## Validation

| Check | Result |
| --- | --- |
| `cmake --build --preset vcpkg --target cch_tests_durable` | passed |
| `./build/cch_tests_durable "[issue1075]"` | 158 assertions in 10 test cases |
| `ctest --preset vcpkg -L durable` | 32/32 tests passed |
| `ctest --preset vcpkg -L architecture` | 43/43 tests passed; Parity Architecture Gate PASS |
| `cmake --build --preset vcpkg` | passed; build-phase Parity Architecture Gate PASS |
| `scripts/format-check.sh 6efbc6ed286da76a2d0a7a1cd5983a2212ff1503` | clean |
| `python3 scripts/evidence/audit_evidence.py verify` | 24 bundles, 78 artifacts, 469 scenarios passed |

## Capability status

`audit-1075` is registered in `fixtures/pi-v1.3.0/baselines.json`.
The F7 entry in `1058-implementation-frontier.json` is updated with the delivered
behavior, test comparisons, and related issue `#1075`.
No consolidated-ledger rows are promoted to verified without a complete
consolidation regeneration.

## Remaining boundary

- Frozen `Tx.task`, `Tx.scanTasks`, `Tx.latestHeadMarker`, and `Tx.submissionByRequest`
  have no C++ counterparts in the current public `Transaction` contract and
  remain parity gaps outside the scope of #1075.
- Bidirectional JSONL/SQLite continued operation is owned by #1085/#1086;
  #1076 delivered the separate task/checkpoint execution slice.
- Cloudflare SQLite hosting stays excluded.
