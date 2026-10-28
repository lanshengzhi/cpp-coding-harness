# #1088 durable observation views

## Scope

Issue #1088 delivers durable conversation views, document-change frames, the
task graph, ready/running/waiting/blocked inspection, and their watches as
projections of committed records against pi-v1.3.0 /
`71023aec1546d6d677a65669c5665c0927ce0dd0`. The agent event stream
(`watchEvents`) is excluded: it is not derived from committed records and is
not in the issue's reuse list. No capability row is promoted by this slice.

## Delivered behavior

- `Harness::conversation_view`, `watch_conversation`, `task_graph`, and
  `watch_task_graph` hydrate and register in one Session-line read, build one
  shared ownership `World`, and derive every observation from committed
  records. They do not execute task code or add a display scheduler.
- Inspection requests complete committed task, conversation, and submission
  sets (`SIZE_MAX`). The Storage contract requires exactly
  `min(limit, committed count)`, or an error instead of a short page. Runtime
  detection of a silently short-paging third-party backend is not claimed.
- A watch has an `Idle`/`Claimed`/`Entered`/`Stopped` admission state under
  the mount mutex. `stop()` and `end_watchers` mark and cancel a claimed
  delivery and return without waiting for cancellation or an entered callback.
  A claimed delivery rechecks and cancels; an entered callback may finish. No
  user callback runs under the mount lock.
- Reopen consistency and failed-session watch termination are covered without
  running task code or changing committed state. Uncommitted and rolled-back
  transactions do not appear in views or watches.
- Review fixes include an empty value from a moved-from `ViewWatch`, a
  non-throwing `std::from_chars` task-graph decoder, documented asynchronous
  Harness/Session lifetime contracts, and removal of the unasked
  `Harness::commit` delegate.

## Validation

Two-axis review: the first Standards review found four P1s (moved-from
`ViewWatch::value()` dereference, `std::stoll`, undocumented raw `this` across
asynchronous reads, and unbounded stop/end waits); Spec was PASS with one P2
(removing the unasked `Harness::commit`). The fixes are in `ae294e20b`; the
delta Standards and Spec reviews both passed with no findings.

Focused final counts: `issue1088` 22/22, `issue1165` 15/15, `issue1063`
16/16, `[watch]` 8/8, durable shard 228 selected (225 passed, 3 skipped,
0 failed), frozen 5/5, and architecture 43/43. Logs are
`/tmp/cch-1088-final-{watch,issue1088,issue1165,issue1063,durable,frozen}.log`
and `/tmp/cch-1088-final-architecture.log`.

The first repository Full Validation on `8898b55bf` built successfully and
selected 3,931 tests: 3,929 passed, 2 failed, 1 skipped. Both failures were
`committed ledger differs from a deterministic rebuild`: the plan correction
had landed without rebuilding the ledger. The deterministic rebuild is
`dd1ae3e78` (1,464 rows; 37 corrections; 36 relative links resolve), and both
consolidation tests then passed 2/2. The final Full Validation on `dd1ae3e78`
built successfully and selected 3,931 tests: 3,930 passed, 0 failed, 1 skipped
(the Native TUI differential capture), exit 0, in 175.10 s. Raw output:
`/tmp/cch-1088-full2-ctest.log`. The earlier failure is retained above; it was
not a flaky rerun.

Merge and merged-tree focused results are recorded after integration.

## Capability status

No capability row is promoted. The consolidation plan gains the
`limit-1088-watch-events-exclusion` correction and the deterministic ledger
rebuild; `cch_durable:event-stream` remains explicitly not claimed. Rows for
the delivered committed-record views remain at their existing membership and
verification status unless a separate evidence route supports a change.

## Remaining boundary

The agent event stream (`watchEvents`) is outside this slice. SQLite
reopen-failure is a documented boundary, not a separate #1088 observation
contract, and no combined failure+reopen test is claimed. Silent short-paging
by a third-party backend and atomicity across an external Storage writer or
process are not detected or claimed. `stop()`/`end_watchers` do not wait for
or join a running callback, and cross-watch stop cycles are not claimed. This
record covers the committed-record observation slice only.

## Evidence detail

The following detail records the evidence behind the summary above.

`Session::read_committed` is the public counterpart of frozen pi's `readOnLine`.
Hydration and watch registration run together on the Session mutation line, so
a publication cannot fall between those steps. View mounts advance from commit
publications; they introduce no display scheduler and do not execute task phases.

A controlled public-caller test uses `Harness::session().commit` to write
`live.generation`, then commits its removal and a usage update. It checks every
frame by applying its ops to the prior value, checks that tampered ops fail that
comparison, and compares the final frame with fresh hydration. This exercises
real committed writes through the observation seam; it does not establish that
the generation producer emits the frozen sequence. The case passed 17
assertions under a 90-second process timeout. Four earlier non-frozen cases
passed 333 assertions. After splitting the frozen cases, separate bounded runs
passed the task graph (26 assertions), inspect states across SQLite reopen (45),
and placed-submission inspection (21). A later non-frozen run passed seven cases
and 371 assertions, including stop and move-assignment checks; that run predates
the deliberately failing reservation and read-order regression tests. The frozen
conversation comparison now passes; no capability row has been promoted.

The frozen capture's conversation watch receives three frames. An independent,
time-bounded trace of the frozen checkout found an intermediate request commit
that writes `live.generation`, followed by a final commit that removes it and
updates usage. Before #1163 the current C++ integration produced two frames,
with verbs `sp` and `dp`, because its task-only intermediate commits did not
touch the view. The merged #1163 producer change made the frozen comparison
pass; the frozen assertion remains intact, and a passing controlled observation
test still does not substitute for that integration comparison.

A blocked task remains live, so `wait_for_idle` cannot finish while a task is
blocked on a missing definition. The test that observes this state waits for
other tasks to leave the live inspection rather than claiming idle. Test-side
polling deadlines do not cover a blocked asynchronous `start()` or synchronous
inspection: focused test processes require an external timeout with time for
diagnostics and cleanup.

`watchEvents`, the agent event stream, is outside this committed-state slice:
it is not derived from committed records and is absent from the issue's reuse
list. The first acceptance criterion concerns inspection before and after reopen
without executing task code or adding a display scheduler; this exclusion does
not remove that requirement. The boundary is also recorded in the consolidation
plan. This does not claim verification of the event stream.

Inspection now completes the committed scan. In one Session-line job it
requests every task, conversation and submission (`SIZE_MAX`), builds one
ownership `World`, and derives every row and waiting/abort membership from
that capture; no per-row scan remains. Focused `[inspect-complete]` covers
Memory plus SQLite and JSONL reopen with 257 pending + 1 terminal record,
`[inspect-world-complete]` covers 1100 live children, and
`[inspect-scan-error]` proves a backend that rejects the complete request
fails the inspection instead of returning a partial page. The Storage
contract now requires exactly `min(limit, committed count)` or an error;
silent short-paging by a third-party backend is not runtime-detectable and
remains an explicit contract boundary. The three scans under the Session
line are not an atomic snapshot across an external writer or process.

A committed snapshot alone is insufficient to prove matching runtime state:
the scheduler reserves an invocation before committing its running state and
clears it separately after execution. #1165's line-job capture reports the
committed record plus the invocation identity at one instant; the reservation
regression now passes. A bounded public caller probe holds the Session
mutation line while the scheduler reserves a pending task; the merged #1165
implementation returns the coherent committed pending row rather than the old
inconsistent pair. The independently threaded read-order probe now passes via
the #1164 FIFO barrier. Existing synchronous stop callbacks under the Harness
mutex still need a lock-order review before nesting that mutex with the
mutation line; that is a static risk, not a reproduced deadlock. Failure and
close ordering for the barrier are covered by the merged #1165/#1166 tests.
The architecture selection passed on the final code: `ctest --preset vcpkg
-L architecture` selected 43, passed 43, 0 failed, exit 0, after a full
`cmake --build --preset vcpkg` (JUnit `/tmp/cch-1088-final-architecture.xml`,
raw `/tmp/cch-1088-final-architecture.log`). Repository Full Validation is
recorded above.

Watch cancellation's cross-thread claim-to-entry race is resolved. A watcher
has an `Idle`/`Claimed`/`Entered`/`Stopped` admission state guarded by the mount
mutex. `stop()` marks the stop and transitions `Idle` or `Claimed` directly to
`Stopped`; it never waits for a delivery to observe the cancellation and never
waits for an `Entered` callback to finish. A claimed delivery rechecks the
stop, cancels, and does not call its listener; one that entered first may
finish after `stop()` returns. The deterministic `[watch-claim-race]` case
pauses the publication thread between claim and entry, waits for `stop()` to
return while the publication is still paused, then releases it and observes no
callback. `[watch-self-stop]` proves an entered callback may stop its own watch
without deadlocking. The private hook is null-safe, test-scoped, and not a
public contract. A test-only follow-up (`67c406e6f`) replaced the
assertion-driven wait and busy-yield hook with a condition-variable gate,
installed the hook only after the watch exists, and releases/joins/clears
before either `FAIL` or the final checks. Review follow-up removed the
`std::condition_variable` from production `stop()`/`end_watchers`, added the
moved-from `ViewWatch::value()` guard, replaced `std::stoll` with
`std::from_chars`, documented the async lifetime contract on the public graph
and watch declarations, and removed the unasked `Harness::commit` delegate.
Evidence: `/tmp/cch-1088-watch-red.log`, `/tmp/cch-1088-watch-green.log`,
`/tmp/cch-1088-watch-hygiene-green.log`, and
`/tmp/cch-1088-final-{watch,issue1088,issue1165,issue1063,durable}.log`.
Focused after the review follow-up: `issue1088` 22/22, `issue1165` 15/15,
`issue1063` 16/16, `[watch]` 8/8, and the durable shard 228 selected (225
passed, 3 skipped, 0 failed). The new `[watch-move]` and
`[task-graph-decode]` cases cover the moved-from value and malformed/overflow
key errors. `stop()` still does not join a running callback, and cross-watch
stop cycles are not claimed.

The complete frozen split/integration selection passed: `ctest --preset vcpkg
-L frozen` selected 5 and passed 5, exit 0, with JUnit output in
`/tmp/cch-1088-final-frozen.xml` and raw output in
`/tmp/cch-1088-final-frozen.log`. The
selection is the four registered observation scenarios (conversation watch,
inspect states, placed-submission inspection, task graph) plus the audit-1054
SQLite schema baseline; the four #1088 scenarios are 4/4 and the schema
baseline is 1/1. No frozen expectation was changed. This proves those five
registered scenarios at the current checkout, not every remaining capability
row, the event stream, SQLite reopen failure, architecture receipts, or
repository Full Validation.

Full-table scans copy O(n) records; the strengthened scan contract
requires third-party Storage implementations to return complete pages rather
than silently truncate them. Each scan is a single read; separate calls are
not automatically atomic.

After merging main at `f3e1e15b5` (merge commit `8bf804ff7`), #1164's FIFO
barrier and #1165's line-job inspection made both former RED probes pass:
focused `issue1088` 14/14. The complete-scan slice then replaced the
256/1024 caps with one complete capture: focused `issue1088` 18/18 and
`issue1165` 15/15. `[inspect-complete]` covers Memory plus SQLite and JSONL
reopen with 257 pending + 1 terminal record; `[inspect-world-complete]`
covers 1100 live children; `[inspect-scan-error]` proves a capped backend
fails the inspection instead of returning a partial page. RED and GREEN raw
output: `/tmp/cch-1088-scan-red*.log`, `/tmp/cch-1088-scan-green.log`,
`/tmp/cch-1088-focused-issue1088.log`, `/tmp/cch-1088-focused-issue1165.log`.
The passing frozen case replays every observed frame; this result does not
verify every remaining observation boundary.

The reservation probes now constrain the completed #1165 implementation
rather than an outstanding fix: a public frozen-pi task commits a waiting
record while its phase remains active, so inspect reports the committed
record plus the invocation identity at one instant; record status alone
cannot replace invocation state. The C++ stop-callback reentry probe still
shows a committed read waiting for the callback's publication line to return
(`/tmp/cch-1088-reentry.log`); this is an existing reentrancy risk, not an
actual unbounded hang. Frozen abort, close, and injected Storage failure
probes ran three times: abort signalled synchronously before its reentrant
inspection and abort settled; close and failure signalled before reentrant
inspection rejected and before the outer operation settled. Logs and probe
sources are under `/tmp/cch-1088-frozen-*`. JavaScript promise callbacks
cannot establish parity for a synchronously blocking C++ stop callback.
External concurrent Storage writers are not serialized by a Session, and
silent short-paging by a third-party backend is not runtime-detectable; the
Storage contract requires an error instead.

SQLite reopen-failure coverage is not a separate #1088 observation contract:
reopen consistency is covered by `[inspect-complete]`
(`tests/durable/HarnessObservationTest.cpp:817`: Memory, SQLite and JSONL
reopen, 257 rows, before == after) and by `a reopened Harness derives every
observation from committed records without running task code` (`:587`).
Failed-commit rollback and sequence preservation across reopen are covered by
`tests/durable/SqliteStorageTest.cpp:110`; failed-Session trigger rollback,
reopen and recovery are covered by `tests/durable/SqliteStorageTest.cpp:159`;
failed-Session watch termination and rejection by the observation APIs are
covered by `[durable][observation][failure][issue1088]`
(`tests/durable/HarnessObservationTest.cpp:1740`). `[inspect-scan-error]`
(`tests/durable/HarnessObservationTest.cpp:942`) covers a backend that cannot
serve the complete scan, not a reopen failure. No combined failure+reopen test
is claimed.

The delivery summary above records the reviewed inputs, focused and Full
results, and the consolidation rebuild. Merge and merged-tree focused results
are added after integration. Only evidence-supported capability rows may be
promoted; this slice promotes none. Consolidation must be regenerated and
checked after plan changes, and any delivery record requires its reviewed
inputs, frozen scenario mapping, revision and validation receipt.
