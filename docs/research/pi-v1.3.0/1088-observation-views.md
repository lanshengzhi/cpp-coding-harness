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

Inspection remains incomplete. Its current scans cap tasks and submissions at
256 and read the ownership world separately for each task. Frozen inspection
runs on the Session line. A committed snapshot alone is insufficient to prove
matching runtime state: the scheduler reserves an invocation before committing
its running state and clears it separately after execution. Existing synchronous
stop callbacks under the Harness mutex also need a lock-order review before
nesting that mutex with the mutation line. These are statically identified
interleavings and risks, not claims of a reproduced deadlock. A bounded public
caller probe held the Session mutation line while the scheduler reserved a
pending task: `inspect()` returned the committed pending record with a derived
running state, independently confirmed against Storage before releasing the
line. The regression assertion fails; a compatible fix remains outstanding.
A separate bounded test held an asynchronous Storage commit and showed that
`read_committed` returned the earlier snapshot before that commit settled.
Frozen `readOnLine` queues behind prior commits; issue #1164 tracks that Session
barrier. Both the reservation probe and the independently threaded read-order
probe compiled and failed their strict assertions under a 90-second external
timeout, as expected for the present defects. Failure and close ordering for
the barrier remain untested. The latest report edits have no build impact;
focused validation, architecture receipts and Full Validation remain outstanding.
Watch cancellation
also has an unverified cross-thread boundary: delivery checks cancellation under
the mount lock, but the listener begins after that lock is released. A stop can
return in between. Frozen JavaScript checks disposal and invokes its listener
on one event loop; the existing tests do not determine equivalence at this
boundary. Full-table scans copy O(n) records; the strengthened scan contract requires third-party Storage
implementations to return complete pages rather than silently truncate them.
Each scan is a single read; separate calls are not automatically atomic.

Before delivery, resolve or explicitly coordinate the #1163 integration
blocker; verify the remaining observation and concurrency cases; run focused
validation and the required architecture checks; coordinate repository Full
Validation; and record the actual results. Only evidence-supported capability
rows may be promoted. Consolidation must be regenerated and checked after plan
changes, and any delivery record requires its reviewed inputs, frozen scenario
mapping, revision and validation receipt. No such receipt has yet been created.
