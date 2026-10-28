# #1088 durable observation views: work in progress

This work implements conversation views, document-change frames, the task graph,
and inspection as projections of committed state. It remains unverified as a
delivery: the frozen integration comparison fails and the required focused and
repository Full Validation receipts have not been completed.

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
conversation comparison remains failing and no capability row has been promoted.

The frozen capture's conversation watch receives three frames. An independent,
time-bounded trace of the frozen checkout found an intermediate request commit
that writes `live.generation`, followed by a final commit that removes it and
updates usage. The current C++ integration produced two frames, with verbs `sp`
and `dp`; its task-only intermediate commits did not touch the view. Issue
#1163 tracks the independent producer gap. The frozen assertion remains intact;
a passing controlled observation test does not substitute for that integration
comparison.

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
Focused validation, architecture receipts and Full Validation remain
outstanding.

Watch cancellation also has an unverified cross-thread boundary: delivery checks cancellation under
the mount lock, but the listener begins after that lock is released. A stop can
return in between. Frozen JavaScript checks disposal and invokes its listener
on one event loop; the existing tests do not determine equivalence at this
boundary. Full-table scans copy O(n) records; the strengthened scan contract
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
Storage contract requires an error instead. The watch claim-to-entry race,
SQLite reopen failure, architecture and Full Validation, and evidence-
supported capability ledger updates remain unverified.

Before delivery, verify the remaining observation and concurrency cases; run focused
validation and the required architecture checks; coordinate repository Full
Validation; and record the actual results. Only evidence-supported capability
rows may be promoted. Consolidation must be regenerated and checked after plan
changes, and any delivery record requires its reviewed inputs, frozen scenario
mapping, revision and validation receipt. No such receipt has yet been created.
