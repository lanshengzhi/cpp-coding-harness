# Durable abandon-on-restart marks and restart reasons (#1175)

## Scope

This slice follows ADR 0075 and #1175 under #1180 / #1183 / #1044 at Named
Baseline `pi-v1.3.0`, revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`, on Linux x86-64 glibc. It consumes
the already-public `TaskOptions::abandon_on_restart` field in the durable
task/reservation lifecycle: the first reservation pass of a later Harness
abort-marks a flagged task with reason `restart`, that reason propagates to its
ordinary owned work, background and unflagged work are not reached, a
restart-marked task with a missing definition waits instead of orphaning, and
an explicit abort request replaces the reason.

#1175 changes no capability membership. Whole-row verification for
`cch_durable:abandon-on-restart` remains owned by #1180. The
`src/durable/Codemode.cpp`, `src/durable/Environment.cpp` and codemode/test
domain of the parallel #1172 lane is untouched.

## Frozen evidence

The registered `audit-1055` family already records one flagged-task reopen
("a task abandoned on restart is aborted with reason restart after reopen"). It
does not observe the open-only state, reason propagation to owned work,
background/unflagged boundaries, the missing-definition wait, or request
replacement. Those observations are captured independently from the exact
frozen checkout in the new candidate family
`fixtures/pi-v1.3.0/audit-1175/`:

| Artifact | Scenarios | Bytes | SHA-256 |
|---|---:|---:|---|
| `abandon-on-restart.json` | 5 | 12337 | `987af05e42c66ed057efad9333a32015112c6183abc553b6e11f7ce45ec8fe7e` |

`manifest.json` SHA-256:
`7f61bd3633756f264aa6c2ee59ffd7cda336f3637d588019a6799724602f96ab`.
Capture generator SHA-256:
`78b3cdc23da706fba8198df7f59e8ea6a8f37cd039cf39502a202335476c3d5b`.
Captured 2026-11-01T17:13:54.979Z on Linux x86-64, glibc 2.44, Node v26.13.0,
tsx 4.22.1. The generator refuses a checkout whose `HEAD` differs from the
registered revision, a modified tree, a non-Linux/glibc host, or a family owned
by another baseline.

The five scenarios drive public `Harness` calls:

1. **Paused open vs first reservation pass**, over in-memory storage (a later
   Harness over the same live records) and over a closed/reopened SQLite store.
   After open the task is `pending`, `abandonOnRestart:true`,
   `abortRequested:false`, with no `abortReason`, and it stays unmarked while
   scheduling is paused. After resume the run phase never runs; the abort
   handler observes `restart`, and the terminal record is `aborted` with reason
   `restart`. Both storage paths produce the same observation.
2. **Cascade and boundaries.** A flagged parent with an ordinary owned child,
   an unflagged conversation-owned sibling, and a background task in a
   conversation the parent owns. The parent and child abort bottom-up with
   `abortReason:"restart"`, and only their abort handlers run. The sibling and
   background task run and complete with `abortRequested:false`.
3. **Missing definition, request replacement, restoration.** Two flagged tasks
   reopen without their definitions. Both are `pending`, blocked
   `missing_task`, `abortRequested:true` with reason `restart` and are not
   orphaned. An explicit abort of one orphans it (`orphaned/missing_task`) while
   retaining the recorded `restart` reason; installing the other's definition
   runs its abort handler with reason `restart`.
4. **Request replaces restart down the tree.** An unflagged owner, a flagged
   middle task and an unflagged missing-definition leaf. The restart cascade
   marks the leaf `restart`, so it waits. An explicit abort of the owner
   replaces the reasons two levels down: the leaf is orphaned, and the middle
   and owner abort with no reason.
5. **Created after open.** A flagged task created after the Harness opened runs
   and completes; the abandonment set is populated only from records read at
   open.

`abandon_on_restart` is persisted as the optional frozen
`TaskRecord.abandonOnRestart` field (`StorageEncoding.cpp`), set from
`TaskOptions` at creation (`Session::create_task`) and immutable afterwards.
`python3 scripts/evidence/audit_evidence.py check --manifest
fixtures/pi-v1.3.0/audit-1175/manifest.json` accepted the bundle and returned
the registry patch:

```json
{
  "audit-1175": {
    "manifest": "audit-1175/manifest.json",
    "manifestSha256": "7f61bd3633756f264aa6c2ee59ffd7cda336f3637d588019a6799724602f96ab"
  }
}
```

Registration in `fixtures/pi-v1.3.0/baselines.json` is left to the serial
merger, matching the lane precedent of #1113/#1168; the bundle stays out of the
consolidation index until it is registered.

## Delivered behavior

The reservation and reconciliation pass lives in `src/durable/Harness.cpp` in
this checkout; `src/durable/WorkflowRuntime.cpp` carries the generation and
compaction workflows and has no reservation pass. `Task.cpp`/`Task.hpp` expose
the definition/handler surface consumed by the pass, and `Records.hpp` carries
the persisted field.

- `TaskRecord` carries `abandon_on_restart`, encoded only when true and decoded
  as false for older records. `Transaction::create_task` copies it from
  `TaskOptions`; no later path mutates it.
- `Harness::open` collects live flagged tasks into a private abandonment set.
  It writes no mark for them; `running -> pending` remains the only open-time
  write. An explicit `Harness::open_tasks()` re-reconciliation is not a Harness
  open and collects nothing.
- The first enabled scheduling pass writes one `restart` abort mark for every
  still-live, unmarked flagged task and reserves nothing else. The next pass
  reconciles the cascade and reserves the abort invocations.
- `reconcile()` derives reason-aware cascade marks from the nearest cancellation
  intent owner: an abandoned, non-failed owner passes `restart`; every other
  intent passes a request. A request staged in the same pass wins over a
  restart, an existing request is never downgraded, and a request replaces an
  existing restart mark.
- Background tasks are reached but neither marked nor entered by an ordinary
  walk, so the cascade stops at background boundaries. Unflagged tasks are
  reached only as ordinary owned work of an abandoned task, never because of
  their own record.
- An abort-marked `waiting` task is an abort candidate rather than resuming its
  wait, matching frozen `#waitingOn`.
- Explicit `abort_task` and conversation abort marks are requests: they clear a
  `restart` reason. A request-marked task whose definition is blocked still
  orphans with the blocked reason; a restart-marked one keeps waiting. A run
  invocation whose definition disappears while restart-marked ends without
  orphaning, and the scheduler reconsiders it when the definition returns.
- No nested-tool default is changed: this ticket consumes the public flag when
  a caller sets it. The frozen `tool.ts` default that sets the flag on
  replay-unsafe nested tool tasks belongs to the tool-task caller, not to this
  reservation slice.

## Validation

- Focused Validation:
  `cmake --build --preset vcpkg --target cch_tests_durable -j 2`, then
  `ctest --preset vcpkg -L issue1175 -j1`: **10 selected, 10 passed, 0 failed**
  (five Catch2 cases discovered by both the general and the issue1175 specs),
  **251 assertions in 5 test cases**. The selection includes the open-only
  discriminator (a mark at open would leave `abortRequested:true` while paused)
  and the created-after-open discriminator (a scanner that abandons every
  flagged record would abort it instead of running it).
- Owning module: `ctest --preset vcpkg -L durable -j1`: **324 selected, 315
  passed, 9 skipped, 0 failed**. The skips are the frozen-pi durable
  continuation prerequisites.
- Architecture-sensitive selection: `ctest --preset vcpkg -L architecture -j1`:
  **43 selected, 43 passed**, including the build-phase Parity Architecture Gate
  on the changed Header/CMake surface.
- Full Validation (serial, as requested by the lane):
  `TERM_PROGRAM=herdr ctest --preset vcpkg -j1 --output-on-failure`:
  **4169 selected, 4158 passed, 11 skipped, 0 failed** in 653.66 seconds. The
  skips are nine frozen-pi durable continuation cases, the realtime-clock
  adjustment case and the native terminal differential capture; none can run in
  this environment.
- A default four-job `-L durable` run transiently failed the timing-sensitive
  #1170 nested-call ordering case and the #1163 generation-publication case on
  this loaded host. Both passed alone, the durable module passed completely
  serially, and the serial full run above is green. No bound, timeout or
  quarantine policy changed.
- The default four-job full preset was also run as a cross-check:
  **4169 selected, 4157 passed, 11 skipped, 1 failed** (99%). The single failure
  was the unrelated #1019 TUI atomic-drain assertion on this loaded host; it
  passed in isolation and in the serial full run above. No TUI source or policy
  was changed by this ticket.
- Delivery reconciliation: adding the new C++ test to
  `cmake/tests/DurableTests.cmake` changes a pinned input of the #1090
  delivery. The exact `^(issue1090|issue1101|issue1104)$` selection was rerun
  (**36 selected, 36 passed**) into
  `docs/research/pi-v1.3.0/deliveries/1175-1090-revalidation.xml` (SHA-256
  `b1302a2bc0891c817843185dcee2abc37cfb1ca00ceb06a529f22451bb14c65a`), the
  pinned digest was refreshed, and a `revalidation1175` entry records the
  change. `consolidate_audits.py build` and `check` pass (1464 source rows,
  41 receipt-backed rows, 568 scenarios, 150 explicit orphans, 0 uncovered).
- `python3 tests/evidence/NamedEvidenceTest.py`: 24 tests, OK (2 skipped),
  including the offline bundle and negative cases.
- `scripts/format-check.sh origin/main`: clean.

## Capability status

`cch_durable:abandon-on-restart` moves from an exposed-but-unconsumed field to
implemented runtime behavior for the five frozen observations above. Membership
and the source audit ledger are unchanged, and the whole-row receipt is not
claimed here: `docs/research/pi-v1.3.0/1058-deliveries.json` remains owned by
#1180. In particular, this record does not promote a partial or adjacent row.

## Remaining boundary

- #1180 owns the whole-row comparison receipt, including the exact
  scenario-to-case mapping and the delivery-receipt contract for
  `cch_durable:abandon-on-restart`.
- The nested tool caller's frozen default (`tool.ts` sets
  `abandonOnRestart` on replay-unsafe child tasks) is outside this reservation
  slice and remains with the tool-task/Agent-runtime work.
- The frozen terminal `TaskState` drops the checkpoint, while the C++ durable
  record retains the last checkpoint on terminal. That pre-existing shape
  difference is orthogonal to the restart mark; the comparison test projects
  the frozen live-state checkpoint and does not claim a change to terminal
  checkpoint retention.
- SIGKILL does not prove power-loss or multi-process guarantees; this slice
  makes no such claim.
