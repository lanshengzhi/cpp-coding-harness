# #1084 SIGKILL recovery

## Scope

The process tests compare SQLite-backed public Harness and Session behavior with the four SIGKILL observations in [audit-1055](../../../fixtures/pi-v1.3.0/audit-1055/crash-recovery.json), captured on Linux against pi-v1.3.0 revision `71023aec1546d6d677a65669c5665c0927ce0dd0`. They preserve that registered evidence family. Each killed worker signals a committed boundary through a pipe; the parent verifies `WIFSIGNALED` and `SIGKILL`, reaps it, and starts a separate recovery process. Every child has a bounded wait and is reaped before test assertions.

## Delivered behavior

- At generation request dispatch, a killed worker leaves one placed submission and one generation task. A second process reopens the store, submits the same request ID and receives the persisted submission ID, resumes generation and settles one answer. A third process finds one answer and does not dispatch another request.
- An effect handler records each invocation and uses a stable key for its external application. After the first effect and before its outcome commit, SIGKILL leaves one invocation and one application; recovery invokes it again, with two invocations and one application, and commits the terminal receipt.
- After terminal task and display-only write settlement, a killed worker leaves the terminal receipt and done submission. Recovery does not run the task again, and a duplicate write request resolves to the same submission ID.
- When killed inside the abort handler, the task retains its abort mark. Recovery runs only the abort handler and commits `aborted/recovered`.

The effect is idempotent because the example uses a stable application key; the scheduler does not provide exactly-once external effects. The process tests do not prove power-loss durability, fsync failures, or concurrent process writers.

## Validation

The first test was committed as `498597a` and failed 1/1: the recovered child observed `running` rather than `pending`, and the effect call count stayed one. Frozen pi's scheduler reconciles running tasks during open; the C++ public API documents an explicit `Harness::open_tasks()` step. Commit `bafa085` added that step and the test passed. This remains an observable open-timing difference, not a claim of automatic-open parity. No production change was made.

The initial shard-only architecture selection passed **36/36** in 590.74 seconds; it omitted seven architecture cases from the unbuilt CLI architecture shard. After building the full default Debug preset, `ctest --preset vcpkg -L architecture --output-on-failure` selected and passed **43/43** in 53.68 seconds. The default Debug durable shard build also passed the build-phase Parity Architecture Gate under strict no-exception mode. After review fixes, `ctest --preset vcpkg -L issue1084 --output-on-failure` selected and passed **4/4** cases in 0.16 seconds. Full Validation remains pending authorization before delivery.

## Capability status

The tests establish the listed process-crash behavior through the documented C++ recovery sequence. They do not update the shared capability registry or independently verify other recovery families.

## Remaining boundary

Automatic reconciliation during `Harness::open` remains different from frozen pi. These tests call `open_tasks()` explicitly before comparing the paused scheduling state and do not establish automatic-open parity. The effect example does not exercise the frozen memo (`requested: 7`) or compare the ready-boundary key and receipt as separate observations. The generation test checks a terminal task on the third reopen as a local invariant; the frozen generation scenario records the settled submission and answer count instead. A live kernel page cache remains available throughout the SIGKILL observations.
