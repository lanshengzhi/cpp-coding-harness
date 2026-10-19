# #1055 Durable tasks, submissions, inbox and crash-recovery audit

**Status:** audit complete. The frozen behavior is captured as `upstream-captured`; no row is
`verified`, because this ticket did not run a C++ parity suite and did not change production code.
The ticket did not implement scheduler fixes, change task policy, relocate owners or mutate the
frozen upstream checkout.

- **Baseline:** `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
- **Capture host:** Linux x86-64, glibc 2.44, Node v26.12.0, tsx 4.22.1
- **Private frozen checkout:** `/tmp/cch-1055-pi-frozen`; `git rev-parse HEAD` was the full baseline
  SHA and `git status --porcelain` was empty before every capture
- **Integration base:** `e8cf93dd1e062f522c55613b1c634653fb371edd`
  (`integration/1044-pi-v1.3.0`, including the landed #1046–#1052 bundles)
- **Ticket branch:** `ticket/1055-audit`
- **Machine ledger:** [1055-durable-tasks-submissions-inbox-recovery.json](1055-durable-tasks-submissions-inbox-recovery.json)
  — 40 capability rows, the implementation/verification split, the historical-spec mapping and the
  exact C++ seams
- **Evidence family:** `fixtures/pi-v1.3.0/audit-1055/` — five artifacts, 34 scenarios
- **Capture source:** `fixtures/pi-v1.3.0/audit-1055/capture-1055.mts`,
  `scenarios.mts`, `audit-tasks.mts`, `pi.mts`, `crash-worker.mts`

The report answers the A11 acceptance criteria. The JSON ledger is the authoritative inventory;
this report explains what was observed, what it proves, what it does not prove, and which narrow
slices follow. It deliberately distinguishes “the frozen Harness does this” from “the current C++
TaskStore/TaskScheduler does this”. A passing C++ task-store test is regression evidence, not proof
of the full durable target.

## Evidence bundle

| Artifact | Scenarios | SHA-256 |
|---|---:|---|
| `task-lifecycle.json` | 5 | `413511ba940dc2c0a4a3a85c45ac50f56f6b9af84ab88be8de01e38af0fe1e8e` |
| `task-versioning.json` | 7 | `7fae5080c87f11397279dc8fc158c31e4a841163e6c16c8af67a2a09c30fba1f` |
| `ownership-abort.json` | 5 | `e913d57038013a5773977e1db5eeb40089474e4e34f9bc8f504a7fad7f7fdc06` |
| `submissions-inbox.json` | 12 | `4e1e2e365a3bb772c2e992a2f396eb4cb0596e62e6c364e91de657dcfa4bb2d0` |
| `crash-recovery.json` | 5 | `19b205fdbc79f4d7c0a96d95caa0c85e8fdf26eef4dc79549af7b54180a1eec3` |

`manifest.json` SHA-256: `0468e8d06857687ed21198a3fc89f74fc76cb3035213601085b566519de7333e`.

The exact capture command (run from the Pike repository; the capture re-executes itself under the
frozen checkout’s `tsx` so the recorded runtime belongs to that install):

```bash
NODE_OPTIONS=--conditions=source \
PI_CHECKOUT=/tmp/cch-1055-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1055-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1055/capture-1055.mts
```

`python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1055/manifest.json`
accepted the bundle and printed this registry record:

```json
{
  "audit-1055": {
    "manifest": "audit-1055/manifest.json",
    "manifestSha256": "0468e8d06857687ed21198a3fc89f74fc76cb3035213601085b566519de7333e"
  }
}
```

Serial registration in `fixtures/pi-v1.3.0/baselines.json` is left to the parent/serial merger.
Until that registration lands, every row is `upstream-captured`, never `verified`.

### Frozen-checkout and process boundaries

The private checkout is a copy at the exact revision, not the shared `../pi` working tree. Its
`node_modules` is a local directory of dependency links into the shared install, while workspace
links resolve the **local** frozen `packages/*`. `NODE_OPTIONS=--conditions=source` selects the
frozen package sources; `TSX_TSCONFIG_PATH` points at the frozen root `tsconfig.json`. No shared
package source, shared build output or upstream file was edited.

Crash scenarios spawn `crash-worker.mts` with `node --import tsx`, so the measured process is the
direct child. A previous probe using the `.bin/tsx` wrapper was invalid for crash evidence: killing
the wrapper left the real node worker alive. The final worker records `READY` only after its
documented boundary, the parent sends `SIGKILL`, and the worker is confirmed gone before reopen.
The observations are process-crash observations, not power-loss, fsync or multi-process-concurrency
observations.

## 1. Inventory and ownership boundary

The selected durable surface is small enough to separate into five layers:

| Layer | Frozen pi seam | Pike seam today | Consumer / boundary |
|---|---|---|---|
| Task definition | `defineTask`, `TaskDefinition` (`initial`, `phases`, `abort`, `migrate`, `hooks`), registry extensions | `TaskScheduler::register_handler` (one handler, integer version), `DurableTask.kind/definition_version` | Scheduler and extension authors; a record stores kind/version, never code |
| Scheduling | `TaskScheduler` reserve/step/abort/reconcile, `Harness.openTasks/resume` | `TaskScheduler` queue, `TaskStore::transition/recover_tasks`, `resume()`/`close()` | Host task submission and recovery |
| Records | `TaskRecord` state machine, checkpoint, memos, ownership, lifecycle times, terminal outcome | `tasks` table: state, checkpoint text, owner session, parent task, version, abort flag | Transaction/storage; task inspection |
| Submissions | `SubmissionRecord`, request ID, `submitted`/`placed`/`done`/`unanswered`, handles and waits | `submissions` table attached to a task; request-ID primary key | Host input admission and retry |
| Inbox | `pi.inbox` document, steer/followUp/write items, boundary placement, queue modes | `inbox` table plus `claim_pending_inbox`; no admission path | Session commits and generation/tool boundaries |

The C++ `TaskStore` and `TaskScheduler` are the current durable vertical slice from #925; the
`parent_task_id`, version-migration and hook work are the #933/#934/#936 slices; the low-level
`inbox` table is the #935 slice. Those slices prove a useful foundation, but they stop short of the
frozen record model and high-level admission surface.

### Historical-spec map

| Historical slice | C++ artifacts / tests | Frozen contract observed here |
|---|---|---|
| #925 Tranche 3: task records, scheduler, submission vertical slice | `TaskStore.{hpp,cpp}`, `TaskScheduler.{hpp,cpp}`, `TranscriptExport.{hpp,cpp}`; `TaskStoreTest.cpp`, `TaskSchedulerTest.cpp` | `task-lifecycle`, `crash-recovery`, request-ID dedup and reservation tests |
| #933 Tranche 4-1: ownership graph, cascade abort, fail-fast joins | `parent_task_id`, `task_descendants`, `request_abort`, `transition` cascade; `TaskStoreTest.cpp:77`, `TaskSchedulerTest.cpp:256` | `failFast`, `allSettled`, direct parent abort cascade and ownership rejection |
| #934 Tranche 4-2: replacement and version migration | `register_handler` version check, `migrate_task`, `TaskSchedulerTest.cpp:52/120`, `TaskStoreTest.cpp:126` | migration-before-reservation, phase-boundary handover, too-old/missing definitions |
| #935 Tranche 4-3: persistent inbox boundaries | `SqliteSessionStore` inbox table, `commit_batch_with_inbox`, `claim_pending_inbox`; `SqliteSessionStoreTest.cpp:96/129` | atomic admission batch, busy queue, follow-up/steer/reject, queue modes, abort withdrawal |
| #936 Tranche 4-4: lifecycle hooks | `register_before_hook`, `register_after_hook`, diagnostics sink; `TaskSchedulerTest.cpp:306/362` | task hook order, ordinary-throw isolation, abort-through-hook |

The current C++ tests are regression evidence for those slices. They do not establish the full
frozen target: the record model has no waiting/completing hold, no structured outcome, and the
inbox table has no admission policy or submission binding.

## 2. Definitions, records, reservation and terminal receipts

`defineTask` supplies an initial checkpoint, a phase map, an abort handler, an optional migration
function and optional typed hooks. The alphabet of durable task states is:

- `pending` — eligible for scheduling, checkpoint retained;
- `running` — reserved by one invocation, checkpoint retained;
- `waiting` — parked with `on` and a `JoinPolicy`, no invocation;
- `completing` — outcome decided, waiting for ordinary owned work to drain;
- `terminal` — settled receipt.

Terminal outcomes are `completed` (result), `failed` (error snapshot), `aborted` (optional reason),
`orphaned` (blocked reason, no task code ran) and `faulted` (runtime/contract error). The observed
rows cover the first three; the scheduler-written two are listed explicitly in the ledger as
unverified C++ gaps rather than being folded into a passing state test.

The counter scenario observed one invocation, phase values `[0,1,2]`, a result of `3`, and a
terminal record `{kind:"audit.counter", version:1, status:"terminal"}`. The transfer scenario
observed `{status:"completed", result:{receipt:70}}` and a durable checkpoint throughout. These are
receipts, not just state projections.

Reservation is committed before the handler runs. The frozen C++ test already matches that part:
`TaskSchedulerTest.cpp:16` sees `running` inside the handler and confirms `resume()` gates
dispatch. C++ also has a reservation-failure test (`TaskSchedulerTest.cpp:166`) that leaves the
task pending and runs no handler. What C++ does not have is a phase runtime: its handler returns a
single checkpoint string at the end, so it cannot commit a checkpoint after each phase or reject a
commit after an abort mark appears.

The terminal receipt is atomic in the target: record, result entries, task-document retirement and
submission settlement land together, with a `completing` hold when owned work is live. C++
`transition` updates the record and marks the task’s submissions `done` in one SQLite transaction,
but it stores no result entries or task-scoped documents and has no outcome payload. The transcript
export test checks the submission is `done`, but that settlement is produced by the export handler,
not by a structured task-result receipt.

Lifecycle times are part of the record: the `audit.clock` scenario measured `startedAt:1000`,
`endedAt:4000` on a Harness clock, with the task visibly `waiting` in between. C++ has
`created_at/updated_at` only; a wait or reopen cannot recover first-run or terminal spans. Memos are
also absent from C++: the capture shows `{requested:7}` beside a replaced checkpoint and no memos on
the terminal record.

### Effect sandwich

The documented contract is intent commit, external effect, outcome/next-phase commit. Reopening at
the intent checkpoint means the effect **may** have run; the task must be idempotent, poll an
external handle, or record interruption. The crash capture makes the boundary concrete:

```text
killed after effect, before outcome commit
after reopen: checkpoint {phase:"apply", key:"transfer-7"}, memo {requested:7}
effect invocations after crash: 1, applied: 1
effect after resume: calls 2, applied 1
terminal outcome: completed {receipt:70}
third open: effect calls 2, applied 1; no rerun
```

The durable layer did not invoke the effect twice on its own; it replayed the handler, and the
application-side keyed store suppressed the second application. C++ has the same at-least-once
replay shape and a genuinely idempotent transcript-export handler
(`TaskSchedulerTest.cpp:195`). It has no generic effect key or memo, so this report does not claim
exactly-once delivery for a new side-effecting task kind.

## 3. Structured concurrency, ownership, abort and cancellation

The completing-hold scenario observed a parent in `completing` while its child was `running`, then
both completed. In C++, `transition` allows `running -> completing -> completed|failed|aborted`, but
the scheduler performs both writes back-to-back and never waits for live owned work. The state name
exists; the structured-concurrency hold does not.

The frozen joins were observed directly:

| Join | Observed order / result |
|---|---|
| `failFast` | child `a` failed; live siblings `b` and `c` received abort marks and ran their abort handlers; parent completed with `[failed, aborted, aborted]`; `b`/`c` had `abortRequested:true` |
| `allSettled` | child `b` failed; child `c` finished normally; parent completed with `[failed, completed]`; neither child was abort-marked |
| direct parent abort | parent and child both marked; child abort handler ran first; parent abort handler then read `outcomes([child]) == [aborted]` and committed `aborted`; returned `marked` |

C++ has no `waiting` state, no `on` list and no `JoinPolicy`. Its one “fail-fast” test has the
opposite direction: when a parent commits `failed`, `transition` marks non-terminal descendants
`failed` (`TaskStoreTest.cpp:77`). That is a parent-failure cascade, not a waiting join that aborts
live siblings; it cannot represent either captured join.

Ownership itself is also only partial. C++ stores `parent_task_id` and recursively marks
descendants, but `create_task` accepts any parent and has no conversation ownership, background
flag, terminal/missing owner check, or cross-conversation check. The frozen `createTask` rejections were observed at the transaction boundary:

```text
Task owner 7 is terminal
Task owner 999999 does not exist
A child task cannot be background
A child task lives in its owner's conversation 1
```

The abort protocol is similarly split. Upstream `abortTask` returns `marked`, commits the mark,
signals and joins the run invocation, waits for ordinary owned work, then runs a **fresh abort
invocation** whose handler commits the outcome. The capture shows `marked` while the record is still
`running` with `abortRequested:true`; the run receives its signal, its commit is rejected with
`This operation was aborted`, the run invocation ends, and only then does the abort handler run.
`abortTask` on an already terminal task returns `terminal`.

C++ `request_abort` immediately writes `pending|running -> aborted` and signals queued/active work;
`finish_aborted` records the checkpoint. There is no abort handler invocation, no `marked|terminal`
return distinction, and no bottom-up abort ordering. The C++ abort test still usefully proves the
durable mark survives reopen and submissions become `unanswered` (`TaskStoreTest.cpp:8`,
`TaskSchedulerTest.cpp:412`).

Cancellation of a wait is separate from abort. The frozen `waitCancellationIsNotAbort` scenario
cancels the waiter’s context, observes the task still `running` with `abortRequested:false`, and then
lets it complete. The submission equivalent leaves the submission `placed` until it settles. C++
has no waiter handle at all; its scheduler tests poll `load_task` or join on `close()`.

`abandonOnRestart` is covered explicitly: a clean reopen leaves the task pending, and after
`resume()` it is aborted with reason `restart`. C++ has only `abort_requested`; there is no flag, no
reason, and `recover_tasks` happily requeues it.

## 4. Versioning, replacement and blocked definitions

The versioning artifact distinguishes four paths:

1. **Migration before reservation.** A v1 record under a v2 definition is `ready`/`migrates:false`
   before installation and `ready`/`migrates:true` after. The migration receives
   `(input, checkpoint, fromVersion)`; the observed v2 input was `{n:2}`, checkpoint `n` advanced by
   10, and the result was `v2:11`. The log contains only `run-v2`; v1 never ran.
2. **Too-old stored version.** A v2 record under a v1 definition stays `pending`, inspection says
   `blocked/task_too_old`, and the checkpoint remains `{phase:"run",n:1}`.
3. **Missing definition.** Removing the definition leaves the task pending and `blocked/missing_task`;
   restoring it lets the same task complete without a terminalization.
4. **Failed migration.** A throwing migrate leaves the task pending, reports `cannot migrate` once,
   and a repaired v2 definition later completes it (`v2:1`). The failure is not a terminal task state.

Definition replacement is separate from migration. The handover scenario shows a running v1 task
finish `p1`, commit back to pending at the phase boundary, then run only `v2-p2` with a v1→v2
migration. C++ correctly replaces a future handler and keeps a running invocation on its old handler
(`TaskSchedulerTest.cpp:52`), but it has no phase boundary at which to hand over.

Finally, an injected storage failure at the terminal commit produced `SessionFailed` with
`Session failed after a storage error; close and reopen it`, left the task `pending` at its prior
checkpoint, and reran it to `completed` after reopen. C++ rollback tests cover the immediate
transaction (`TaskStoreTest.cpp:146`, `TaskSchedulerTest.cpp:166`) but there is no sticky Session
failure, waiter rejection or reopen-after-failure contract.

## 5. Submissions and inbox admission

The captured admission behavior is:

| Observation | Frozen result |
|---|---|
| Idle input | entry `pi.user`, status `placed` until the run settles `done`, transcript `[pi.user:hi, pi.assistant:hello]` |
| Idle passive write | entry `audit.note` with data, status `done`, zero model messages |
| Display-only write while busy | appended and `done`, no inbox item |
| Busy follow-up | `queued`, inbox mode `followUp`, placed at the final boundary after the first answer |
| Busy steer | `queued`, inbox mode `steer`; conversation abort withdraws it to `unanswered/aborted` |
| Busy reject | `ConversationBusy`, no new submission, inbox unchanged |
| Duplicate request ID | same submission ID, zero new storage batches; reusing the ID for another type throws |
| Atomic admission | one batch with `submission` and `document.change`; inbox delta path `items` |
| Queue modes | `all`: both follow-ups placed at final; `one-at-a-time`: only the first followed by a second run |
| Submission abort | queued → `aborted` + inbox removal; placed → `already_placed`; settled → `settled`; unknown/wrong conversation → `not_found`; queued record `unanswered/aborted` |
| Conversation abort | placed input `unanswered/aborted`; queued input `unanswered/aborted`; queued write remains `queued`; inbox keeps only the write |
| Submission wait cancellation | waiter rejects; submission remains `placed`; later settles `done` |

The low-level C++ inbox table already has `steer`, `follow_up`, `write`, a boundary entry ID,
`pending/placed/done`, atomic entry+inbox commit and claim-once. That is useful storage, but it is
not the frozen submission contract:

- the inbox row has no submission record and no request ID;
- `TaskStore` submissions are coupled to task creation and have a global request-ID primary key,
  no conversation scope, no type and no handle;
- no code checks whether the conversation is busy, chooses steer/follow-up, rejects with
  `ConversationBusy`, applies queue modes or places items at a final boundary;
- no submission abort, status/wait handle or inbox withdrawal exists;
- `commit_batch_with_inbox` and `TaskStore::submit` are separate transactions, so the captured
  atomic `[submission, document.change]` batch has no C++ equivalent.

This is the clearest place where the future implementation needs Session/document primitives rather
than another task-table column.

## 6. Crash and reopen

Five process-level scenarios were captured against SQLite:

| Boundary | State after reopen | Result after resume |
|---|---|---|
| Kill after generation request dispatch | scheduling `paused`; generation task `pending` at checkpoint `request`; submission `placed` | `done` with one answer; duplicate request ID returns the same submission; a third open adds no answer |
| Kill after effect, before outcome | task `pending` at checkpoint `{phase:"apply", key:"transfer-7"}`; memo present; `startedAt` present | handler reruns; effect calls 2, applied 1; terminal `{receipt:70}`; third open does not rerun |
| Kill inside abort handler | task `pending`, `abortRequested:true`; worker log `[run, abort]` | reopened log `[abort]` only; outcome `aborted/recovered` |
| Kill after terminal commit | task already terminal; submission `done`; worker log `[run]` | no rerun; duplicate request ID same submission |
| `abandonOnRestart`, clean reopen | task pending at checkpoint `run` | after resume, aborted with reason `restart`; log `[abort:restart]` |

C++ `recover_tasks` resets surviving `running` tasks to `pending`, orders parents before descendants,
and requires explicit `resume()`; those parts match the frozen open/resume gate and are covered by
`TaskStoreTest.cpp:8/102` and `TaskSchedulerTest.cpp:135`. But `recover_tasks` removes
`abort_requested` tasks from its returned work and leaves them `aborted`; there is no abort
invocation to finish a killed abort handler. Terminal records are not selected, which correctly
prevents rerun, but there is no result receipt to preserve.

The storage-failure scenario is a distinct recovery path: the Session fails rather than continuing
with a task that may have written partially; reopening is required. C++ currently propagates errors
locally from `transition` and ignores some scheduler failures, so a future implementation needs the
sticky Session failure state before it can claim this guarantee.

## 7. What this evidence does and does not prove

Proved by frozen execution:

- the public task lifecycle, checkpoint replacement, memo survival/removal, lifecycle stamps and
  terminal receipts;
- `completing` hold, `waiting` with both join policies, bottom-up abort ordering and direct-parent
  cascade;
- version migration before reservation, phase-boundary handover, blocked definitions and storage
  failure/reopen;
- submission request-ID dedup, all captured busy/idle admission modes, atomic submission+inbox
  batch, queue modes, abort/submission-wait semantics and conversation-abort effects;
- process-kill recovery at admission, effect and terminal boundaries, plus abort-mark recovery and
  abandon-on-restart.

Not proved:

- exactly-once external effects — the capture shows replay with application-level idempotence;
- crash safety under power loss, fsync failure, disk corruption or SQLite WAL loss — the captures
  use process `SIGKILL` with a live OS page cache;
- concurrent multi-process reopen, scheduler races or two writers on one database;
- the steer-at-post-tools placement message (admission/withdrawal was captured; boundary placement
  was not);
- the `background` task scope and `Conversation.abort({background:true})` (not captured);
- scheduler-written `orphaned`/`faulted` outcomes and their cleanup;
- task-graph projection/watch or UI behavior — out of this audit’s selected scope, with a hard
  ordering rule left by #932;
- any C++ parity. No row is `verified`.

## 8. Recommended single-context slices and public tests

None of these are implemented by this audit; they are ordered to keep one writer and one context.

1. **Record model, outcome and lifecycle stamp.** Add terminal outcome JSON, result entries,
   `started_at`/`ended_at`, and memos to the live record. Tests: structured receipt, memo
   first-writer-wins across two candidates, wait/reopen stamps, terminal memo removal, terminal
   commit rollback on injected storage failure.
2. **Phase runtime and terminal receipt.** Add a phase handler that can commit a replacement
   checkpoint after rereading the record, rejects after an abort mark, and returns a terminal
   outcome. Test phase-per-checkpoint, abort rejection, result-entry+record atomicity and document
   retirement (or explicitly defer documents if A10 owns them).
3. **Structured concurrency.** Add `waiting`/`on`/`policy`, `completing` hold, ordinary-owned-work
   traversal, bottom-up abort handlers and conversation/background scopes. Tests: the captured
   failFast `[failed,aborted,aborted]`, allSettled `[failed,completed]`, direct parent abort order,
   completing hold, abandon-on-restart, and wait cancellation versus abort.
4. **Blocked definitions and scheduler outcomes.** Add derived `missing_task`/`task_too_old`/
   `migration_failed`, retry on registry change, `orphaned` on abort of blocked work and `faulted`
   on handler/contract failure. Tests for each reason and a definition restoration.
5. **Admission and inbox.** Add one admission transaction that writes an entry, a submission and
   the inbox delta; add busy run detection, steer/follow-up/reject, queue modes, display-only/stale
   writes, submission status/wait/abort and conversation abort. Tests mirroring every row in
   `submissions-inbox.json` plus the missing post-tools steer boundary.
6. **Crash/reopen integration.** Reuse the five real worker boundaries as public C++ crash tests:
   kill after admission, effect, abort mark and terminal; assert observed records, no duplicate
   submission, one answer, and one applied effect with a stable key. Keep `SIGKILL` process tests
   separate from storage-failure tests; the latter must fail the Session and require reopen.

The task-graph projection remains a later read model, not a scheduler proxy. Inbox admission needs
the Session transaction/document primitives from the Session audit lane; it should not grow a
parallel document store inside `TaskStore`.

## 9. Cross-references and serial follow-ups

- **A10 / #1054 (Session, documents and storage):** terminal result entries, task-document
  retirement, atomic submission+document admission and `SessionFailed` are shared dependencies.
- **A13 / #1057 (Chord contracts):** task-graph projection/watch and cross-owner traversal belong
  there; this audit does not claim them.
- **Serial merger:** add the `audit-1055` registry record after `baselines.json` is free. The exact
  JSON patch request is in `/tmp/cch-1044-context/1055-handoff.json`; do not edit the shared
  baselines file from this lane.
- **No production code, migration or issue state changed.** The only tracker action for close-out
  is the normal parent/close-out procedure after review; this lane keeps #1055 open.

## Appendix: scenario-to-ledger coverage

Every captured scenario is referenced by at least one ledger row. The main discriminators are:

- `task-lifecycle.json`: receipt, lifecycle clocks, memos, completing hold, hook order/isolation;
- `task-versioning.json`: migration success/failure, too-old, missing, handover, ownership rejection,
  storage failure;
- `ownership-abort.json`: failFast, allSettled, direct cascade order, abort/commit ordering, wait
  cancellation;
- `submissions-inbox.json`: idle input/write, busy followUp/steer/reject, display-only, request-ID
  dedup, atomic admission, queue modes, submission abort, conversation abort, wait cancellation;
- `crash-recovery.json`: generation admission, effect sandwich, abort mark, terminal no-rerun,
  abandon-on-restart.

The ledger also carries explicit `unverified` rows where the frozen harness has a selected behavior
that this batch did not execute: scheduler-written `orphaned`/`faulted`, background ownership
boundaries, post-tools steer placement, and idle/named waits. They are gaps to close, not
“Deferred” and not evidence of parity.
