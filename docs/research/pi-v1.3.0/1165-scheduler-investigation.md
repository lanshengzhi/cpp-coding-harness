# #1165 scheduler visibility investigation (in progress)

The public `Harness::inspect` on main scans Storage outside the Session line and
reads the active invocation map separately. The #1088 worktree adds observation
views but has not yet landed. Its bounded reservation probe exposes a pending
record with a derived running state before the reservation commit is admitted.
The frozen v1.3.0 scheduler registers invocations within its Session commit turn.
A frozen public probe also observes a committed `waiting` record with a derived
`running` state while its phase remains active. Running cannot be inferred from
record status alone.

A candidate keeps tentative reservations private and publishes an invocation
only in the successful reservation completion, which Session invokes after
Storage settlement and publication, while the line is still active and before
queued reads start. No-op and failure must discard the reservation; failures
must retain the original error. Inspection must read committed records and a
single snapshot of published identities in the same line turn. Identity-checked
cleanup can occur under the Harness mutex: its instant during that turn provides
a valid snapshot point as long as committed records cannot change. The frozen
scheduler also ends invocation identities outside `readOnLine`.

This candidate is not yet established as safe. The public `read_committed`
contract says its callback reads committed Storage state only. A private friend
bridge could reuse the existing `PendingRead` FIFO without broadening that public
contract; similar private access friends already exist in `Session.hpp`. This
changes an Owner header and requires architecture validation. A snapshot callback
would acquire the Harness mutex on the Session line, so every opposite lock path
must first be audited. Session listeners and completion callbacks run
synchronously with the line active; they can enqueue a read but cannot
synchronously wait for it.

The following are source-identified risks, not all reproduced deadlocks:

- Publication, abort completion, conversation abort completion and shutdown
  invoke synchronous `request_stop` under the Harness mutex. Copying stop sources
  under the mutex and signalling after release preserves inline signalling but
  needs tests for races and frozen ordering. A bounded two-thread probe in
  `SchedulerVisibilityTest.cpp` proves a reverse lock cycle: a Session commit
  listener calls `resume` while shutdown holds the Harness mutex in a stop
  callback waiting for that listener. The callback timed out after 500 ms,
  releasing the mutex so the listener and close could finish. A candidate
  copies the stop sources and signals outside the mutex; the same test now
  asserts that no timeout occurs and passes. The identity/cleanup race and
  callback lifetime remain unverified. The bounded #1088 stop-reentry probe
  was copied here and rerun against the modified C++: it still detects a
  500 ms timeout waiting for a Session read during publication, followed by
  read completion. This is a defect demonstration, not complete abort-order
  validation; unlocking the mutex cannot resolve that Session-line cycle.
- `wait_for_idle` invokes its error completion under the Harness mutex. A
  reentrant completion can enter Session and oppose the proposed line-to-mutex
  lock order. Capture the error while locked, then complete after unlocking;
  a bounded two-domain test is needed.
- `wait_for_task` calls virtual `Storage::task` under the mutex; a reentrant
  backend is a possible reverse edge. Its registration and terminal publication
  must remain atomic if the call moves. This has not been reproduced.
- Waiter and failed-migration ownership changes can destroy callback-bearing
  objects under the mutex. `stop_callback` destruction can wait for its running
  callback; final ownership and a reachable reentry cycle need testing before
  asserting a deadlock. Shutdown joins task threads under the mutex after the
  active map empties; whether a joined thread still needs that lock also needs
  a bounded test.

Unlike `abort_task`, `inspect` documents no Harness lifetime requirement. Today
it completes synchronously; a FIFO implementation may remain pending through a
held Storage commit. `Harness::~Harness` shuts down workers but does not drain
pending Session reads, and Session's destructor does not close or drain. The
implementation must establish ownership through completion and prove close and
destruction behavior; a new documentation requirement alone is not evidence
that existing callers remain safe. No production seam is changed while these
conditions remain unresolved.

Independent bounded reruns of the frozen waiting, abort, close, and failure
probes succeeded. The #1088 C++ reservation probe selected 1 case, failed 1
(5 of 6 assertions passed); the stop reentry probe selected 1 and passed 1
(10 assertions), including the expected callback timeout. The independent
main-worktree reservation regression selected 1 case and failed 1 (5 of 6
assertions passed); the reverse lock probe selected 1 and passed its 11
assertions detecting the expected timeout. With the candidate lock change,
that same test selected 1 and passed 1 (11 assertions), requiring no timeout;
the reservation regression still failed. An active-phase close gate passed one
case with 10 assertions after the candidate. An injected, uniquely tagged entry
commit failure returned the original error before the active phase received its
stop signal: 1 case failed, 8 of 9 assertions passed, both before and after the
candidate. The callback is registered before the phase-ready signal; the test
observes the signal at failure return, then releases the phase before close. It
does not rule out a later signal. This differs from the frozen failure probe
and predates the candidate. A focused CTest selection of 23 task
and lock tests passed 23, failed 0, skipped 0. The existing Session close-drain
case selected 1 and passed 1 (14 assertions). The durable shard build and its
build-phase architecture gate passed. These are probe results, not full
validation or a parity claim. Modified C++ abort/close/failure ordering,
identity cleanup, callback lifetime and mutation evidence remain outstanding.
The logs `1165-evidence/reservation-red.log`, `lock-before.log`,
`lock-after.log`, `reentry-after.log`, `close-gated.log`, `failure-red.log`,
and `failure-main-red.log` retain the discriminating runs. Their original
filter tags no longer exist in the tree, so they are historical and not
reproducible as-is; `1165-evidence/README.md` indexes every log with its
original filter and current status.

The copied `std::stop_source` retains the old invocation's shared stop-state
identity, so it cannot directly signal a newly allocated replacement source.
This does not prove equivalent behavior if the old invocation finishes and
unregisters its callback between copying and signalling. The installed GCC 16
`stop_token` implementation confirms that copying shares the stop state,
requesting stop invokes registered callbacks synchronously, and a destructor
on another thread waits for an already-running callback; destruction can instead
unregister a callback before signalling. Those implementation observations do
not establish the required public ordering. Existing public gates
can hold phase and Storage operations but expose no event at that particular
cut; a deterministic identity/cleanup test needs a private test seam or a
revised design. No such seam has been introduced. Candidate production edits and
regressions remain uncommitted and are not a delivery. The visibility issue
still needs the ownership/lock prerequisite followed by the reservation and
inspection change; a separately scoped lock/lifetime prerequisite is a possible
split, but it must retain the still-open Session-line reentry problem.

## Status update: implemented, reviewed, revised

The reservation visibility fix landed on `ticket/1165` on top of the merged
#1166 failure-signaling work (merge `91e30d0cf`, fix commit `5057091d3`,
review revision in flight). The stop-source copy-out candidate recorded above
was adopted and merged by #1166 (`f787d03f7`); the superseded
lock/close/failure/reentry probes were removed from
`SchedulerVisibilityTest.cpp` in favor of the reviewed
`HarnessFailureLifecycleTest.cpp`. The candidate publication and snapshot
design described above is what shipped:

1. **Publication point.** `schedule_step()` no longer registers the
   reservation before committing. The reservation commit's completion —
   invoked on the Session line after Storage settlement, while the line is
   still active and before any read admitted later starts — publishes the
   invocation identity under the scheduler mutex, or discards it on
   failure, no-op, or close. The worker's settle wait paces reservations as
   before; the phase thread starts from the published identity's shared
   stop state after the wait observes the publication. A close race erases
   the identity so shutdown's empty-map wait finishes.
2. **Derivation gate.** `inspect()` takes one snapshot of the published
   identity ids under one lock acquisition and `inspect_task` derives: a
   committed `running` record is running on its own; an invocation
   upgrades `waiting`/`completing` records to running while the phase
   remains active (frozen waiting contract); an identity is never used to
   derive running for a scan that still sees `pending`, because the
   identity reflects a commit newer than the record it would annotate.
3. **Waiting-resume guard** (added in review revision). The
   `waiting -> pending` resume skips tasks with a live invocation. Without
   it the resume — dirtied by the waiting commit itself — committed
   `pending` while the phase was still active, reviving the exact composite
   the gate excludes. Frozen pi ends the invocation in the turn the record
   parks, so its resume never races one; cleanup dirties the scheduler
   again, so the guarded resume fires on the next pass. With the guard, a
   committed `pending` record with a live identity is unreachable, and the
   status gate becomes a coherent observation.

Lock order stays Session line -> Harness mutex; no path holds the mutex
while entering Session or invoking stop/user callbacks. The completion
notifies its settle wait under the lock so the borrowed stack
synchronization objects outlive the notification (CODING_STANDARDS
§6.2/§7.5); `reserved` keeps its release/acquire pair because the
transaction and completion may run on different threads for a deferred
backend (§9.13).

### Review round 1 (dual-axis, both BLOCK) and resolutions

- *Standards P1 — borrowed stack sync objects, notify after unlock.* Fixed:
  notify under the lock; destruction strictly follows the waiter's
  re-acquisition.
- *Standards P2 — implicit captures, memory-order justification, REQUIRE vs
  CHECK.* Fixed: explicit captures with lifetime comments, §9.13 comment at
  the `reserved` load, post-drain behavioral assertions converted to CHECK
  (§11.3); setup preconditions that guard dereferences stay REQUIRE.
- *Spec P1 — resume race hides a live invocation behind the pending gate.*
  Confirmed with a bounded RED (`resume-race-red.log`): the record flipped
  to `pending` while the phase was invoked and inspect derived a non-running
  state. Fixed by the waiting-resume guard; the regression case pins it.
- *Spec P1 — held async reservation, no-op, close coverage.* Added
  `held-release` (startup + cleanup after the line releases) and
  `close-race` (reservation queued behind a held line while shutdown
  begins: identity discarded, phase never starts, close drains). The
  reservation no-op is pinned by the pre-existing
  `HarnessOwnershipTest.cpp` "a stale reservation skips a task whose abort
  terminal already landed" (terminal lands inside the held reservation
  window; the transaction no-ops; the scheduler survives with the terminal
  record intact).
- *Spec P1 — "queue inspect on the line; the direct scan can composite
  states that never existed."* Disproof by state-pair analysis, not by test
  pass: after the gate and the resume guard, every reportable
  (scanned status, identity snapshot) cell corresponds to a real instant —
  `running` rows derive from the scan; `waiting`/`completing` + identity
  derive running and exist while the phase is active (frozen contract);
  `waiting`/`completing` without identity exist once the invocation ends;
  `pending` + identity is unreachable post-guard except as the pre-settle
  stale scan, which the gate rejects. No fabricated state is reportable.
  A FIFO inspect additionally cannot be implemented within this issue's
  scope: a synchronous line wait from the demonstrated stop-callback
  reentry window (`[issue1166][reentry]`, recorded) would wait on the very
  line its own callback holds (unbounded self-deadlock), and the Harness
  lifetime contract for a line-pending inspect (`~Harness` does not drain
  pending line reads) is unproven. Queued inspect remains #1088-coordinated
  scope, documented below.
- *Spec P2 — preserve reproducible frozen evidence; record #1088
  coordination.* The frozen waiting probe and its three original logs are
  preserved under `1165-evidence/frozen-waiting/` with a README (frozen
  checkout `71023aec`, re-run instructions), re-confirmed 3/3 fresh on
  2026-10-28. Coordination with #1088 is recorded here and relayed through
  the orchestrator: #1088 owns the FIFO/complete-scan inspection upgrade and
  must apply the `session->failed()` seal at its observation entry (per the
  #1166 boundary note); this change leaves `Harness::inspect()` a direct
  scan with its 256-task cap.

### Evidence (`docs/research/pi-v1.3.0/1165-evidence/`)

- `reservation-red-merged-main.log` — the held-reservation probe fails on
  merged main (pre-fix), discriminating RED.
- `resume-race-red.log` — the waiting-resume race fails pre-guard, bounded
  RED for the review-confirmed defect.
- `reservation-green3.log` — `[issue1165|issue1166]` 13/13, three runs
  (post-revision).
- `durable-shard.log` — full durable shard 178/178 (post-revision).
- `mutation-gate-off.log` / `mutation-gate-only.log` — the two mutation
  directions with honest interpretation (see Review round 1 in the first
  delivery; unchanged).
- `frozen-waiting/` — preserved runnable frozen probe, original + fresh
  logs.

### Validation state

Focused: post-revision `[issue1165|issue1166]` 13/13 x3, durable shard
178/178, format gate clean, Parity Architecture Gate build phase PASS.
Full Validation: 3878/3878 passed, 1 pre-existing skip, exit 0 on
`5057091d3` (pre-revision); the review revision rebuilds the durable shard
only and needs a fresh Full Validation window before delivery.

### Round 3: review slice pins and the deterministic stale-scan RED (WIP)

Production is untouched since `15724cd09`; this round delivers
discriminating tests only.

- `HeldReservationStorage` gates the reservation's `pending -> running`
  write inside `Storage::commit` itself: the gate firing is the admission
  proof the reviewers required (a held earlier read never established that
  the reservation was admitted). `[held-release]` pins the existing
  held-turn behavior: while the commit turn is held the record stays
  `pending`, and an inspect started during the hold queues its line job
  behind the held commit turn (line-job FIFO), then observes the settled
  running state -- the same public behavior a frozen `readOnLine` read
  spends behind the held turn. The 2s no-return assertion proves
  blocking only within that bounded window. `[close-race]` pins the discard
  semantics (settled write remains, identity discarded, phase never starts,
  close drains); its setup orders the release after the `closed` flag by a
  bounded margin, not by proof.
- `SlowScanStorage` models a slow-read backend (scan observes at T1, returns
  at T2) with thread attribution. `[stale-scan]` is a KNOWN, REPRODUCIBLE
  RED (3/3, `stale-scan-red.log`): the current inspection pairs a
  pre-cleanup `waiting` record with a post-cleanup identity view and
  reports a non-running state for a task that is already terminal. Frozen
  `readOnLine` admits no such interleaving. The RED stays in the suite as
  WIP pending the observation protocol design.
- Measured architectural facts: `Session::failed()`/`closed()` take the
  line mutex, so every public observation freezes during a held commit
  turn; the current inspect is "FIFO's blocking without FIFO's ordering"
  (the seal check orders against in-flight turns only, while the scan and
  the identity snapshot can still interleave with later commits -- the RED
  above). Holding the Harness mutex across the scan would not freeze record
  state (line commits never take it), so that direction was rejected
  without code.

### Residuals carried forward (unchanged, documented, not claimed fixed)

- The Session-line reentry hazard (a stop callback that waits on a
  Session-line read inside the failure window) is structural; demonstrated
  by `[issue1166][reentry]`; not solved here and not claimed solved.
- `wait_for_task` reads `Storage::task` under the scheduler mutex as one
  atomic check+register (frozen pi `TaskScheduler.waitForTask` parity). A
  Storage backend that synchronously re-entered Session from `task()` would
  oppose the line->mutex order; no backend does, and frozen pi carries the
  same exposure. `wait_for_idle` completes outside the mutex already.
- The queued line-job `inspect` is implemented (the seam and the
  reservation-visibility slice). Only a complete committed scan beyond the
  256/1024 caps remains #1088-coordinated scope.

## Inspection scan cap (issue #1088 hand-off)

`Harness::inspect` captures at most 256 task records per call
(`Storage::scan_tasks(256)` in `capture_inspection`) and derives
`waiting`/abort rows through the scheduler's `load_world()` scan (limit
1024). A repository with more records is inspected partially: #1165 does
not claim a complete committed inspection beyond that cap. The public
`inspect` contract states the bound and points at #1088 (observation and
views) for completeness beyond it, and `[inspect-cap]` pins the boundary
(257 committed tasks -> 256 inspected rows) so the behavior is a
documented bound rather than an implicit unbounded scan.
