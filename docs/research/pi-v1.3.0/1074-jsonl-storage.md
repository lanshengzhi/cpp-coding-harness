# #1074 transactional JSONL storage: delivery record

Delivered by commits `77b6b0415` and `fc868c939` on
`task/1044-1074-jsonl`, against the `pi-v1.3.0 /
71023aec1546d6d677a65669c5665c0927ce0dd0` named baseline.

## Scope

`cch::durable::JsonlStorage` is the JSONL backend of the frozen public
`Storage` contract. `JsonlStorage::open` creates or recovers a directory;
`Storage::commit`, reads, and `Session` are unchanged public callers.

The implementation is in [JsonlStorage.cpp](../../../src/durable/JsonlStorage.cpp)
with the delivered-fact test suite in
[JsonlStorageTest.cpp](../../../tests/durable/JsonlStorageTest.cpp). The
shared validated state extracted from `MemoryStorage` is in
[StorageState.cpp](../../../src/durable/StorageState.cpp); both backends validate
and advance IDs and sequences through that one seam, and JSONL replays the
published prefix through it.

## Delivered behavior

- A commit appends every sidecar record before the single `main.jsonl`
  marker. The marker confirms sidecar records by `(file, seq, ordinal)`.
- Opening truncates a torn final line and an unconfirmed sidecar tail. It
  compares an unconfirmed tail sequence with the last confirmed main
  sequence and rejects a tail at a closed sequence.
- A confirmed sidecar record that is missing rejects the open with the frozen
  `Missing confirmed sidecar record doc-3.jsonl at sequence 2` outcome.
- Non-increasing main sequences, out-of-order sidecar records, duplicate
  confirmations, malformed complete lines and invalid UTF-8 reject the open.
- A sidecar or marker append failure poisons the instance. Later commits and
  reads reject the frozen poison outcome; only `JsonlStorage::open` recovers
  the published prefix.
- A `document.change` without a committed value is rejected before writing,
  so a malformed batch cannot leave memory and JSONL replay in different
  states.
- A checkpoint (`document.change` with empty ops) reclaims a current-only
  sidecar to the new base; retirement removes the sidecar. Reclamation is
  retryable maintenance after the marker has published the commit.
- Recovery removes `.reclaim` scratch files and empty unconfirmed sidecars,
  and replays document scope, creation sequences and address indexes.

Frozen observations reused from
[jsonl-store.json](../../../fixtures/pi-v1.3.0/audit-1054/jsonl-store.json):

- `JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail`
- `JSONL refuses a main marker whose confirmed sidecar record is missing`
- `a JSONL checkpoint replaces a current-only sidecar and retirement removes it`
- `a JSONL main-marker I/O failure poisons the instance while reopen recovers the published prefix`
- `a JSONL commit appends every sidecar before the main marker`

## Validation

| Check | Result |
| --- | --- |
| `cmake --build build --target cch_tests_durable -j8` | passed |
| `./build/cch_tests_durable "[issue1074]"` | 280 assertions in 6 test cases |
| `ctest --preset vcpkg -L durable` | 22/22 tests passed |
| `ctest --preset vcpkg -L architecture` | 43/43 tests passed; Parity Architecture Gate PASS |
| `cmake --build --preset vcpkg` | passed; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -j1` | 3673/3673 tests passed; one test skipped (`Native TUI differential capture exposes deterministic Pike cells and ANSI`) |
| `scripts/format-check.sh` | clean |

A first four-job `ctest --preset vcpkg` run hit three load-sensitive cases
(the timed-out query, projection cost bound, and named-evidence gate). Each
passed when run alone, and the complete serial rerun above passed all tests.

## Capability status

No consolidated-ledger row is promoted to verified. The tests compare the
selected delivered scenarios with the frozen artifacts, but the ledger's
verification route remains upstream-captured until a consolidation
regeneration records the comparison; implemented is not verified at the
ledger level. The F2 frontier note in
[1058-implementation-frontier.json](1058-implementation-frontier.json) records
the delivery, but the ledger counts still need a consolidation regeneration.

## Remaining boundary

- No fsync option, fsync-failure observation, power-loss guarantee or
  multi-process coordination. The injected append failure is not SIGKILL or
  power loss. **Resolved by [#1174](https://github.com/lanshengzhi/cpp-coding-harness/issues/1174)**
  ([1174-jsonl-fsync.md](1174-jsonl-fsync.md)): the public JSONL backend now
  exposes the frozen `JsonlStorageOptions.fsync` option and the sidecar/main
  flush contract; the row's verification stays with #1180.
- No terminal task sidecar reclamation (owned by the #1055 task slice).
- No SQLite backend, JSONL/SQLite interchange, historical import, migration,
  dual reads or fallback.
- `Session` does not expose `checkpointWhen` or `retireDoc` in this slice, so
  the checkpoint and retirement cases use `Storage::commit`, the highest
  public caller available; the Session document slice owns those operations.
