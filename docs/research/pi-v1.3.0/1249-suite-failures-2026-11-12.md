# #1249 suite-failure investigation, 2026-11-12

## Scope and status

The full Debug suite at `5c134df` failed three cases under the default
four-job run: `bounded parallel cancellation leaves queued tool
capabilities unstarted` (`ToolCallExecutorTest.cpp`), `a tool held by a
gate keeps its slot running and a late subscriber sees committed state
only` (`HarnessEventStreamTest.cpp`), and `generation publishes the
attempt before the model stream and usage with the final answer`
(`HarnessWorkflowTest.cpp`). All three reproduced under four-process
load (16/400, 2/400 and 3/400) and pass at one job. Each is a test
harness race, not a product defect; all three are fixed in `1ed1405`
with discriminating evidence. A fourth, pre-existing intermittent —
`Process Terminal atomic stop and default stop detect postdrain EIO on
closed master` — reproduced during full validation, is diagnosed below,
and is **not** fixed here (its fix belongs to the TUI owner lane).

## Failure 1: bounded parallel cancellation — test defect

Probe (temporary `INFO`, removed after capture) recorded, in failing
runs, `starts: call-1 call-2 call-3 call-4` with `invoked: call-1
call-3`: start events are emitted in source order even when the
executed set is not the first two calls. The preparation permit
serializes start emission, but the concurrency-permit acquisition that
follows each start races across the test harness's four-thread pool, so
any two calls can win the permits. The frozen and issue contracts
(#40 "stop at documented safe points and do not start unrelated work
after cancellation", #497 paired lifecycle, audit-1047 scenario 4
source-ordered starts and source-ordered result messages) all hold in
the failing runs: exactly two tools run, nothing starts after the
cancellation, results keep source order, lifecycle events stay paired.
Only the test's assumption that call-1/call-2 specifically execute was
wrong. The fix asserts `duration_ms` presence exactly for the calls the
tool state recorded as invoked.

## Failure 2: late-subscriber durable event stream — test defect

Probe recorded `updates_at_attach=1` and `late_updates=1`: the later
stream attached after the first of the gated tool's two pre-gate
progress commits, and the second commit — the `details` report —
published after the attach, so the late subscriber received one
`tool_execution_update` batch where the frozen `lateDuringHold` expects
none. The divergence is the documented one
(`HarnessEventStreamTest.cpp` collapse helpers: frozen progress commits
are paced, the C++ writer commits each report immediately), so the
frozen emptiness is only deterministic when the test synchronizes on
both pre-gate commits. The fix waits for both updates before attaching;
the gate then guarantees no further publication until release.

## Failure 3: generation answer/usage commit merge — test defect

The failing check read `0` because the test's own commit listener had
not yet observed the final publication when `wait()` returned. A probe
recording every publication showed the run's final commit carries the
answer entry, the `pi.usage:v1` document change, the terminal task
record and the `done` submission in one publication — the merged
publication the test expects. The race is in handoff timing: the
Harness's internal commit listener (registered at open) calls
`Submission::Registry::observe`, which resolves the submission waiter
synchronously from inside the publication loop, so a later-registered
listener has not run for the settling publication when the waiter wakes.
Frozen pi has the same listener structure but defers waiter
continuations to a microtask after the synchronous publish loop
(`Submissions.#observe` → `Waiters.resolve`), so the continuation runs
after every listener. The Session contract documents only that a commit
listener "runs in the Session's mutation line"; it does not order
waiter wakeups after later listeners, so the test's assumption was not
guaranteed. The fix snapshots the merged-publication count when the
listener observes the settled-submission publication (the run's final
commit) and waits for that snapshot after `wait()`. The synchronous
handoff divergence from pi's microtask ordering is recorded here for the
owner: any consumer combining `subscribe_commits` with a submission wait
observes it.

## Validation of the fixes

Focused: all three pass. Reproduction loops on the fixed binaries at
the same four-process load: cancellation 0/400, late subscriber 0/800,
generation 0/600. Owning labels run serially: `agent` 2132/2132,
`durable` 385/385. Running the two label suites concurrently reddened
three unrelated runner-sensitive cases (`Native TUI fullscreen
scroll-to-end indicator…`, `Durable local environment reports file
process and watch outcomes`, `a killed generation worker recovers
admission without a duplicate`); each passed alone and serially, and
each is documented elsewhere as load-sensitive.

Receipts: `ToolCallExecutorTest.cpp` and `HarnessWorkflowTest.cpp` are
pinned by the #1176, #1184 and #1203 receipts; each receipt's exact
recorded selection was rebuilt and rerun at `1ed1405` into a fresh
JUnit artifact (10/10, 5/5, 3/3), the changed input digests and
validation records were refreshed, and the prior validations are
retained under `revalidation1249` (`2d10d05`).
Consolidation build and check passed; `ConsolidationAuditTest.py` 94/94
and `ReceiptBoundaryTest.py` 24/24 (both with `CCH_LANE_NAME`; the
unidentified first invocation failed the lane-lock gate as documented);
`receipt_boundary.py` checked 32 receipts / 808 claims / 966
comparisons; `audit_evidence.py verify` verified 84 bundles / 180
artifacts / 1,012 scenarios. Format and whitespace checks passed.

Full Validation per contract (incremental build, separate
`cch_parity_gate_build` invocation, then the unfiltered suite at the
default four jobs with `TERM_PROGRAM=herdr`) ran twice over the fixed
code (`1ed1405`..`2d10d05`; `888dc2e` is docs-only). Run 1, ended
19:52:57 JST 2026-11-12: 4,608 selected, 12 skipped, one failure —
the pre-existing `Process Terminal` flake below; that run's
`build/Testing/Temporary/LastTestsFailed.log` lists only case 1049.
Run 2, started 19:57 JST and ended 20:02:17 JST on `888dc2e` with the
same command and environment: 4,608/4,608 passed with 11 Catch2 skips;
the verbose `build/Testing/Temporary/LastTest.log` records `Test
Passed.` for all 4,608 cases, the Herdr PTY evidence case executed (15
assertions, so `TERM_PROGRAM=herdr` held), and case 1049 passed. Note
CTest does not rewrite `LastTestsFailed.log` on a green run, so that
file still names 1049 from run 1.

Run 2 green does not resolve the intermittents: one clean full run is
also consistent with the pre-existing `Process Terminal` race
(14/200 under load) still being present — case 1049 simply won the
race that once — and the three fixed tests are validated by their
reproduction loops above (0/400, 0/800, 0/600), not by either full
run. The full gate is green on this tree modulo that still-open flake.

Review follow-up on the same day: the review of `888dc2e` found the
new settled-submission wait in `HarnessWorkflowTest.cpp` could abort
on its `REQUIRE` before `harness->close()` — under `-fno-exceptions`
no local destructor runs and the registered commit/watch callbacks
capture the abandoned stack frame (CODING_STANDARDS 11.10). The test
now snapshots its storage reads, stops the watch, and closes the
harness before any assertion; the #1184 and #1203 receipts, which pin
that file, are rebound under `revalidation1249ReviewFix` with their
exact selections rerun (10/10, 3/3).

## Remaining boundary: Process Terminal post-drain EIO flake (not fixed)

`Process Terminal atomic stop and default stop detect post-drain EIO on
closed master` (`ProcessTerminalTest.cpp:2600`) reproduced 14/200 at
four workers on this tree and fails the full gate; it is pre-existing
(documented on clean `6b99d79` in 1086-sqlite-continuation, and carried
in 1117-print-mode-truncation and 1182-tui-reconciliation) and
untouched by this change (`cch_tests_tui` is byte-identical to the
base). Mechanism, newly diagnosed: the test closes the PTY master and
then calls `stop()`, expecting the stop drain to surface the closed
master as `ErrorCode::Process` with "could not write terminal output".
Under load the worker's asynchronous close detection
(`handle_pty_close`) wins the race, tears the session down
(`modes.started = false`) and restores modes before the test thread
reaches `stop()`; `stop()` then returns the early `Validation` "Process
Terminal must be started for final alternate restore stop" instead.
Both sections of the case race the same way, and the file is pinned by
delivery receipts. A deterministic fix needs either a test seam that
defers the worker's close detection until after `stop()` begins, or a
reviewed acceptance of the documented teardown outcome in place of the
drain-EIO assertion — a TUI owner design decision, recommended as its
own ticket rather than folded into this lane.
