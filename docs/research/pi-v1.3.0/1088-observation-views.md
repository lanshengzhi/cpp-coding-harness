# #1088 durable observation views: work in progress

This work implements conversation views, document-change frames, the task graph,
and inspection as projections of committed state. Focused observation cases
and the architecture selection pass, but it remains unverified as a delivery:
repository Full Validation and evidence-supported capability-ledger updates
have not been completed.

`Session::read_committed` is the public counterpart of frozen pi's `readOnLine`.
Hydration and watch registration run together on the Session mutation line, so
a publication cannot fall between those steps. View mounts advance from commit
publications; they introduce no display scheduler and do not execute task phases.

A controlled public-caller test uses `Harness::commit` to write
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
#1164's FIFO barrier. Existing synchronous stop callbacks under the Harness
mutex still need a lock-order review before nesting that mutex with the
mutation line; that is a static risk, not a reproduced deadlock. Failure and
close ordering for the barrier are covered by the merged #1165/#1166 tests.
The architecture selection passed on the final code: `ctest --preset vcpkg
-L architecture` selected 43, passed 43, 0 failed, exit 0, after a full
`cmake --build --preset vcpkg` (JUnit `/tmp/cch-1088-architecture.xml`, raw
`/tmp/cch-1088-architecture.log`). Repository Full Validation remains
outstanding.

Watch cancellation's cross-thread claim-to-entry race is resolved. A watcher
now has an `Idle`/`Claimed`/`Entered`/`Stopped` admission state guarded by the
mount mutex; `stop()` marks the stop and waits only while a delivery is
`Claimed`, never for an `Entered` callback to finish. A delivery that observes
the stop before entering cancels and notifies; one that enters first may finish
after `stop()` returns. The deterministic `[watch-claim-race]` case pauses the
publication thread between claim and entry, marks the stop, then releases the
publication: RED observed one callback after stop (`CHECK(calls.load() == 0)`
reported `1 == 0` at the then-line 1572), GREEN observes none. `[watch-self-stop]`
proves an entered callback may stop its own watch without deadlocking. The
private hook is null-safe and test-scoped; no public API changed. A test-only
follow-up (`67c406e6f`) replaced the assertion-driven wait and busy-yield hook
with a condition-variable gate, installed the hook only after the watch exists,
and releases/joins/clears before either `FAIL` or the final checks. Evidence:
`/tmp/cch-1088-watch-red.log`, `/tmp/cch-1088-watch-green.log` and
`/tmp/cch-1088-watch-hygiene-green.log`. Focused after GREEN: `issue1088`
20/20, `issue1165` 15/15, `issue1063` 16/16, `[watch]` 7/7, and the durable
shard 226 selected (223 passed, 3 skipped, 0 failed). `stop()` still does not
join a running callback, and cross-watch stop cycles are not claimed.

The complete frozen split/integration selection passed: `ctest --preset vcpkg
-L frozen` selected 5 and passed 5, exit 0, with JUnit output in
`/tmp/cch-1088-frozen.xml` and raw output in `/tmp/cch-1088-frozen.log`. The
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
(`tests/durable/HarnessObservationTest.cpp:816`: Memory, SQLite and JSONL
reopen, 257 rows, before == after) and by `a reopened Harness derives every
observation from committed records without running task code` (`:586`).
Failed-commit rollback and sequence preservation across reopen are covered by
`tests/durable/SqliteStorageTest.cpp:110`; failed-Session trigger rollback,
reopen and recovery are covered by `tests/durable/SqliteStorageTest.cpp:159`;
failed-Session watch termination and rejection by the observation APIs are
covered by `[durable][observation][failure][issue1088]`
(`tests/durable/HarnessObservationTest.cpp:1685`). `[inspect-scan-error]`
(`tests/durable/HarnessObservationTest.cpp:941`) covers a backend that cannot
serve the complete scan, not a reopen failure. No combined failure+reopen test
is claimed.

Before delivery, verify the remaining observation and concurrency cases and
coordinate repository Full Validation; record the actual results. Only
evidence-supported capability rows may be promoted. Consolidation must be
regenerated and checked after plan changes, and any delivery record requires
its reviewed inputs, frozen scenario mapping, revision and validation receipt.
No such receipt has yet been created.
