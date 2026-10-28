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
and `failure-main-red.log` retain the discriminating runs.

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

## Status update: implemented (pending Full Validation)

The reservation visibility fix landed on `ticket/1165` on top of the merged
#1166 failure-signaling work (merge `91e30d0cf`). The stop-source copy-out
candidate recorded above was adopted and merged by #1166 (`f787d03f7`); the
superseded lock/close/failure/reentry probes were removed from
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

Lock order stays Session line -> Harness mutex (the completion runs on the
line and takes the mutex; no path holds the mutex while entering Session or
invoking stop/user callbacks). No new threads, no unbounded waits, gates
released before joins.

### Evidence (`docs/research/pi-v1.3.0/1165-evidence/`)

- `reservation-red-merged-main.log` — the held-reservation probe fails on
  merged main (pre-fix), discriminating RED.
- `reservation-green3.log` — `[issue1165|issue1166]` 10/10, three runs.
- `durable-shard.log` — full durable shard 175/175.
- `mutation-gate-off.log` — publication moved but gate removed: the probe
  passes; the settle->publish gap closes inside the settle callback, so
  this probe can no longer bracket it. The gate's necessity is by
  construction (scan and identity snapshot are two unsynchronized reads)
  and by `mutation-gate-only.log`.
- `mutation-gate-only.log` — old pre-commit registration with the new gate:
  the probe passes; the gate alone suppresses the symptom. The publication
  move is pinned by the frozen parity contract and the #1088 line-time
  observation seam, not by this probe.

New public tests in `tests/durable/SchedulerVisibilityTest.cpp`: the held
reservation probe (kept), waiting-while-phase-active (frozen contract),
rejected reservation (no phase start, sealed inspect), and terminal identity
cleanup (no live-work row while the identity is still registered against a
terminal record). All follow the release-then-assert discipline from the
#1166 `-fno-exceptions` findings.

### Residuals carried forward (unchanged, documented, not claimed fixed)

- The Session-line reentry hazard (a stop callback that waits on a
  Session-line read inside the failure window) is structural; demonstrated
  by `[issue1166][reentry]`; not solved here and not claimed solved.
- `wait_for_task` reads `Storage::task` under the scheduler mutex as one
  atomic check+register (frozen pi `TaskScheduler.waitForTask` parity). A
  Storage backend that synchronously re-entered Session from `task()` would
  oppose the line->mutex order; no backend does, and frozen pi carries the
  same exposure. `wait_for_idle` completes outside the mutex already.
- Queued/asynchronous `inspect` was not implemented: the Harness lifetime
  contract for a Session-line-pending inspect remains unproven
  (`~Harness` does not drain pending line reads), and a blocking FIFO
  inspect would deadlock in the demonstrated stop-callback reentry window.
  The direct-scan inspect with the derivation gate is the smallest sound
  change.
- `Harness::inspect()` keeps its direct-scan semantics and 256-task cap;
  complete-scan and FIFO inspection belong to #1088's observation scope.
