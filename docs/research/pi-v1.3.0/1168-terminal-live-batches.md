# #1168: Reconcile terminal-to-live writes in one durable Storage batch

Frozen baseline pi-v1.3.0 at `71023aec1546d6d677a65669c5665c0927ce0dd0`; parent #1085,
grandparent #1044. Follows the divergence recorded in
[1085-filesystem-boundary.md](1085-filesystem-boundary.md).

## Frozen contract

The frozen public durable Storage accepts one `commit` batch carrying
terminal-then-live (and live-then-terminal) writes for a single task, with the
last write winning:

- a batch `[terminal, pending]` for an absent task ID commits seq 1 on a new
  store and reads back `pending` both immediately and after close/reopen;
- a batch `[pending, terminal]` settles the task terminal and reclaims the
  live sidecar;
- one live write revives an already-terminal task;
- a mixed invalid batch rejects atomically and consumes no sequence;
- JSONL keeps live task records in `task-{id}.jsonl` sidecars, retained or
  reclaimed according to the batch's final write per ID.

None of the frozen `MemoryStorage`, `JsonlStorage` or `SqliteStorage` commit
paths carries a terminal-replace check; the check lives only in the frozen
Session transaction (`session/transaction.ts` `#assemble`,
`Task ${id} is already terminal`). Session-level unreachability of a batch is
therefore not proof of Storage equivalence.

## Correction

C++ previously enforced the terminal-replace invariant in the shared
`StorageState`, rejecting the frozen batch on all three backends from the
wrong layer. The correction moves the invariant to Session transaction
assembly, mirroring the frozen layering:

- `src/durable/StorageState.cpp`: a task write replaces the record wholesale,
  terminal status included (last write wins per batch).
- `src/durable/Session.cpp`: a transaction may not replace a committed
  terminal task, with the frozen message `Task N is already terminal`; a task
  created and settled inside one transaction is a create, not a replace.

The adjacent frozen Session rules (`Task N does not exist`,
`Task N cannot change conversations`) and the Storage-level
conversation-identity check are unchanged; they are separate reconciliations.

## Evidence

Frozen captures with provenance live in `fixtures/pi-v1.3.0/audit-1168/`
(see its `manifest.json`; registered in `fixtures/pi-v1.3.0/baselines.json`):

- `terminal-live.json`: 15 scenarios (5 cases across memory, JSONL and SQLite
  backends) covering nonexistent and existing tasks, sequence allocation,
  atomic rejection, and revival.
- `interchange.json`: 3 scenarios embedding the exact frozen JSONL store
  bytes after terminal↔live batches, including a reclaimed sidecar and a
  revived terminal task, for offline C++ recovery comparison.

The earlier #1085 probe artifacts
([frozen-terminal-live.mts](1085-captures/frozen-terminal-live.mts),
[output](1085-captures/frozen-terminal-live.txt),
[C++ RED](1085-captures/terminal-live-red.txt)) are preserved untouched.

## Tests

- `tests/durable/TerminalLiveBatchTest.cpp`: the public Storage contract per
  backend — atomic failure consumes no sequence and publishes nothing
  (no marker, no sidecar), immediate reads, close/reopen recovery, sidecar
  retention and reclamation, and reopen of the frozen stores byte-for-byte,
  including a corrupted-sidecar fail-closed case.
- `tests/durable/SessionTransactionTest.cpp`: the assembly invariant rejects
  replacing a terminal task with the frozen message and publishes nothing;
  the layering pin revives the same task through direct public Storage after
  the Session rejected it; a task created and settled in one transaction stays
  accepted.
- `tests/durable/JsonlStorageTest.cpp`: the previously RED C++ check now pins
  the accepted contract (last write wins, sidecar retained, reopen pending).

## Validation

- Focused: `cch_tests_durable` shard passes (292 cases), including the 20
  `[issue1168]` cases.
- Full: complete `ctest --preset dev-fast` run; the six failures observed
  under parallel load (two codemode abort-timing cases, one PTY stop case,
  one warning-gate timeout, and the two consolidation gates) all pass in
  isolation; the consolidation gates are re-passed after the delivery-evidence
  rebinding recorded in `1058-deliveries.json` (`revalidation1168`).
- The `issue1090` delivery selection (10 cases) was rebuilt and re-passed at
  the #1168 revision because `cmake/tests/DurableTests.cmake`,
  `fixtures/pi-v1.3.0/baselines.json` and
  `docs/research/pi-v1.3.0/1058-implementation-frontier.json` are its pinned
  inputs.
