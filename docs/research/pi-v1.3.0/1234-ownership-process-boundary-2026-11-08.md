# #1234 ownership wait process boundary — 2026-11-08

## Scope and red observation

The audit-1203 background ownership comparison can block inside `consume()`
even when its surrounding polling loops have deadlines. Before introducing the
process boundary, a hidden probe in `HarnessOwnershipTest.cpp` created a pending
`AsyncResult<void>` whose producer retained its completion, and then called
`consume()`. The rebuilt durable shard run as
`timeout --signal=KILL 3s build/cch_tests_durable '[issue1234-wait-probe]'`
was killed with exit 137. Its discriminating operation, retained in the current
`ownership_injected_stall` helper, was:

```cpp
std::optional<support::AsyncResult<void>::completion_type> retained;
support::AsyncResult<void> pending{support::AsyncResult<void>::producer_type{
        [&retained](support::AsyncResult<void>::completion_type completion) noexcept {
            retained.emplace(std::move(completion));
        }}};
(void)consume(std::move(pending));
```

This injects a missing completion; it does not claim a normal Harness operation
has reproduced the stall.

The persistent regression injects that retained completion immediately before
conversation abort and task wait in the isolated scenario. For close, the phase
itself retains the completion while the test calls `close()` with that
invocation active. A pipe marker confirms the child reached the intended stage;
the parent must observe a timeout, SIGKILL and a reaped child for each one.
The first two injections model a blocked caller rather than a fault injected
inside the production Harness operation.

## Delivered behavior

The registered frozen comparison runs in an exec'd child of the CTest process.
The parent uses a 60-second deadline to reach the selected stage; for injected
stalls it then allows three seconds, sends SIGKILL on expiry and tries to reap
for a further five seconds. Failures report the child status and last stage:
`a` for conversation abort, `w` for task wait, and `c` for close; `a` can also
mark the active phase in the close injection.
The child sets a parent-death signal before exec, with a parent identity check
closing the fork/prctl race. This assumes the test executable has no
setuid/setgid bits or file capabilities that would clear that signal on exec;
the reviewed path has no separate parent-death integration test. Between fork
and exec the child only closes a descriptor, sets the Linux parent-death signal,
checks its parent, executes, or exits; it does not run the Harness or allocate
C++ state. The marker pipe is read nonblocking. The child reports errors and
frozen comparison failures through exit codes without Catch assertions. Codes
21–25 identify scenario loading, task construction, open, an operation, and
close; codes 26–31 identify the individual frozen outcome comparisons. Codes
90–94 identify subprocess setup or marker failures. On the ordinary path the
child retains the existing gate release, Harness close and frozen comparisons.
Forced process termination is not graceful Harness cleanup, and the five-second
reap limit reports failure if the OS cannot deliver a reap in that interval.

## Validation and receipt

The focused `issue1203|issue1234` CTest selection passed 4/4, including all three
injected stalls. The exact `issue1203` selection passed 3/3 and its JUnit result
is pinned by the `1203-durable-captures` receipt. The prior result is retained;
the scenario-to-test mappings and frozen fixture bytes are unchanged.
Consolidation build/check passed, the receipt boundary checked 32 receipts, 808
claims and 966 comparisons, and named evidence verified 84 bundles, 180 artifacts
and 1012 scenarios.

The first full CTest run (`-j4`) selected 4578 tests: 4565 passed, 11 skipped,
and two unrelated cases failed. `bounded parallel cancellation leaves queued
tool capabilities unstarted` observed unexpected duration assignments; `Remote
deterministic execution refuses a replay with a different seed or fingerprint`
aborted after its assertions with `Resource deadlock avoided`. Both passed when
rerun together at `-j1`; that rerun does not establish their underlying cause.
A full run on the revised implementation at `-j2` selected 4578 tests: 4566
passed, 11 skipped, and the same bounded parallel cancellation case failed.
That case then passed 20 consecutive isolated executions at `-j1`; its cause
remains undetermined. A subsequent full serial run (`-j1`) selected 4578 tests:
4566 passed, 11 skipped, and `/mcp navigates into a server menu and runs pi's
reconnect action` exited with SIGSEGV. That MCP case passed immediately in
isolation (0.15s). Because full CTest requires every non-skipped test to pass,
this unmitigated suite-level failure blocks clean lane exit and pushing to
origin main.

Two independent reviews identified the original child assertions, the shared
three-second startup/stall budget, the synthetic close injection and leaked
Catch2 temporary directories. The implementation now uses exit codes, waits
for a stage marker before timing the injected stall, and exercises close with an
active blocked phase. The temporary-directory limitation remains below. The
receipt's `testedRevision` is the checkout base, as specified by the delivery
contract; input digests bind the modified test bytes. An independent recheck
of test source SHA-256 `db2f4d3f56a46a71b1c35801dd68de7000721536f4088a6607c3fbd85e18da7d`
found no remaining blocking or major issue in those fixes.

## Remaining boundary

The injections do not establish that production Harness operations can be
cancelled or safely abandoned in-process. Process termination cannot run the
child's gate release or close when a real operation stalls. An exec'd Catch2
child creates its own temporary test environment; SIGKILL prevents its normal
destructor from removing that directory. The existing CTest outer timeout
remains a separate last-resort bound.
