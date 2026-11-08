# #1234 background ownership wait follow-up — 2026-11-08

## Scope and observation

At `314d50e8c292cd874110a76ed68cc9785340aa32`, the audit-1203
background ownership test records failures, releases its gate and closes the
Harness before its assertions. Its polling deadlines do not bound individual
`consume()` calls: `tests/durable/Outcome.hpp` waits on a condition variable
without a deadline and its completion captures the outcome, mutex, condition
variable and flag by reference. This applies to `abort_conversation()`,
`wait_for_task()`, and other consumed operations in the scenario, including
`close()`.

A minimal independent probe compiled against the actual `Outcome.hpp` created
an `AsyncResult<void>` whose producer retained its completion without calling
it, and passed it to `consume()`. `timeout 2s` exited 124. This is an injected
missing completion, not evidence that the normal Harness scenario currently
hangs. At the same base `ctest --preset vcpkg -LE architecture -L issue1203
-j1` passed 3/3; its normal pass does not distinguish the missing completion.

## Safety boundary and next slice

A timed return from `consume()` would leave its callback referring to dead stack
objects. Merely heap-owning those callback objects also does not settle ownership:
`Harness::abort_conversation_pass` retains a raw `Harness*` in pending state.
`Harness::close()` calls `shutdown()`, which joins workers and waits without a
deadline for active invocations; a stuck operation cannot be assumed to drain
on gate release. There is no demonstrated safe in-process timeout that both
returns and destroys these objects while an arbitrary callback is outstanding.

The focused CTest case is already a separate process, with the configured default
300-second CTest timeout, but that only kills the process; it is not evidence of
graceful gate release or `close()` on a stalled operation. A possible next slice
is an explicitly bounded child-process protocol, following
`tests/durable/CrashRecoveryTest.cpp`: child owns all Harness state and reports
outcomes without Catch assertions; parent enforces a deadline, kills and reaps a
hung child, and asserts the result. It must test injected stalls at both the
conversation abort and task wait (and close), preserve the frozen comparisons,
and distinguish process termination from graceful cleanup. Fork/exec safety,
parent-death cleanup and the parent-side deadline need explicit review.
No implementation or durable regression test is claimed by this record.

## Frozen evidence and receipt impact

`fixtures/pi-v1.3.0/audit-1203/ownership.json` expects the background child to
remain running after conversation abort, then reach terminal with aborted/test
after direct abort. Those observations must stay unchanged. Receipt
`1203-durable-captures` in `1058-deliveries.json` pins
`tests/durable/HarnessOwnershipTest.cpp` and its three-case JUnit result; a test
source change requires the exact issue1203 selection to be rerun and the
reviewed digest and revalidation recorded. `cmake/tests/DurableTests.cmake` is
also pinned by other receipts, so a discovery change has a separate blast radius.
This note changes neither frozen evidence nor receipt inputs.

## Remaining boundary

The requested bounded cleanup fix, persistent failing discrimination, full
validation, and implementation review remain outstanding. This record is a
partial investigation, not a resolution of the independent review finding.
