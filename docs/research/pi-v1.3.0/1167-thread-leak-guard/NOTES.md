# #1167 — durable test failures leak live threads under `-fno-exceptions`

Status: **open**. This slice records the mechanism, the audit, and a rejected
guard; it does not establish assert-level failure safety. A follow-up slice
owns that guarantee.

## Mechanism

Durable test TUs build with `-fno-exceptions` (ADR 0042,
`cmake/ProjectSetup.cmake`). A Catch2 `REQUIRE` failure throws from the Catch2
library (built with exceptions) through the test frame; that frame has no
cleanup landing pads, so **no local destructor runs on the failing path**. A
test that fails while its `Harness` owns live threads leaks the scheduler
worker and every execution thread, and later cases can then crash far from the
cause. Issue #1166 supplied the symptom: each failing case passed in isolation
while 9/12 full-suite runs SIGSEGV'd.

## Rejected guard: cleanup at `testCaseEnded`

A `LiveHarness` scope class (singleton registry plus a Catch2
`EventListenerBase` listener closing still-registered scopes at
`testCaseEnded`) was implemented across the durable shard and then rejected:

- The listener runs after the test frame is abandoned and recurses **below**
  it on the same stack (`testCaseEnded -> delete scope -> ~Harness ->
  Impl::shutdown -> join`), so a still-running worker or phase reads stack
  memory that the recursion has already overwritten.
- Recorded cores show the worker faulting inside
  `schedule_step -> reconcile -> MemoryStorage::scan_conversations` on
  corrupted frame bytes (`core-backtrace-frame-capture.txt`).
- Isolated runs of the guard looked green (40/40), which is exactly why the
  mechanism was not adopted: a cleanup hook that runs after the failure cannot
  establish "a failed assertion leaves no live threads or mutexes", and
  presenting it as such would give later authors false confidence.
- Full-shard exposure: with the guard in place, one completed run had two
  failures (the thread-count witness and `CrashRecoveryTest`'s
  "a killed generation worker recovers admission without a duplicate"), and a
  separate SEGV came from a block-scoped guard outliving its raw pointer in
  `HarnessWorkflowTest`. All of it is reverted in this slice.

Evidence files:

| File | What it shows |
| --- | --- |
| `core-backtrace-frame-capture.txt` | Worker faulting on corrupted frame bytes while the listener joins below the abandoned frame |
| `red-frame-capture-victim-sees-leak.log` | Next case sees 2 threads where 1 is expected |
| `red-frame-capture-ghost-tasks.log` | Surviving task ids after the leaked worker keeps stepping |
| `red-frame-capture-segv.log` | A run that died instead of reporting |
| `red-narrowed-probe.log` | Canonical RED on the narrowed tree (this slice) |

## Kept in this slice

- `tests/durable/HarnessFailureCleanupProbeTest.cpp`: hidden `[.]` pair.
  The probe deliberately REQUIRE-fails with a live harness and phase; the
  victim then checks the property "no live threads after a failed assertion"
  and currently fails with `3 == 1`. Run it with
  `./build/cch_tests_durable '[issue1167]' --order decl`; the pair is hidden
  from every discovery spec. Catch2 randomizes run order by default, so the
  victim reports an explicit ordering failure rather than passing vacuously if
  it runs first.
- `CODING_STANDARDS.md` §11.10: the `-fno-exceptions` failure semantics and
  the rules that follow (release-then-assert, bounded waits and stop-aware
  phases, fork-child protocol, heap-owned thread-visible state), with the
  guard rejection recorded as a non-guarantee.
- `HarnessOwnershipTest.cpp`: `Gate::wait_for` plus a 60 s bound on the
  `migration_gate` wait. That wait has no stop token, so a failed assertion
  before the release could hang the teardown join; the bound converts a hang
  into a failing assertion. This is the one real liveness fix found by the
  audit.

## Audit

All 23 durable TUs (~274 cases at audit time) were read. Findings:

- Every case that opens a `Harness` (188 bindings across 14 files) is exposed
  to the leak above; the guard retrofit for them was reverted, so the exposure
  remains documented rather than fixed.
- One unbounded test-side wait: `HarnessOwnershipTest`'s `migration_gate`
  (fixed here).
- `CrashRecoveryTest` already uses the sanctioned fork-child protocol (the
  child drives the harness and `_exit`s, never running Catch2 assertions), so
  it needs no guard and was excluded from the retrofit.
- `HarnessFailureLifecycleTest`'s phase loops were already stop-aware; its
  release-then-assert shape is the precedent §11.10 cites.

## Side findings

- `ctest -R cch_tests_durable` selects nothing **by design**: durable cases are
  registered individually (one CTest test per Catch2 case), so the binary name
  is not a test name. Use `ctest --preset vcpkg -L durable` (294 tests on this
  base) or run `./build/cch_tests_durable` directly. No defect, no issue
  filed.
- The `CrashRecoveryTest` failure seen in one full-shard run occurred only
  with the rejected retrofit; on the narrowed tree the full shard passes twice
  and `[issue1084]` passes (5 cases, 75 assertions) plus three isolated runs of
  the named case. Not reproduced without the retrofit; its assertions are
  untouched.

## Validation (narrowed tree, base `6fe4ba04b`)

- `./build/cch_tests_durable` (full durable shard, direct binary):
  - run A: 295 cases | 289 passed | 6 skipped | 0 failed; 11131 assertions.
  - run B: 295 cases | 289 passed | 6 skipped | 0 failed; 10684 assertions.
  (assertion totals differ between runs because some cases assert
  conditionally; case counts are stable)
- `ctest --preset vcpkg -L durable`: 294 selected, 294 passed, 0 failed,
  0 skipped (`catch_discover_tests` registers one test per case, so the count
  tracks the discovery specs rather than the binary's 295 cases).
- `./build/cch_tests_durable '[issue1167]' --order decl`: exit 42; 2 cases,
  2 failed; 8 assertions, 6 passed, 2 failed — the deliberate failure plus the
  victim's `3 == 1` leak. Reproduced 8/8 runs.
- `ctest --preset vcpkg -L architecture`: 43/43 passed, after building the full
  preset (`catch_discover_tests` registers a shard's cases only once that shard
  builds, so a shard-only tree showed 36).
- `scripts/format-check.sh origin/main`: clean.

## Remaining boundary (follow-up slice)

A general guarantee needs a helper that makes the failure path safe instead of
merely observable:

1. release-then-assert: drain the invocation and close the harness before any
   `REQUIRE` that can abort the case, so no test-owned thread is live when the
   assertion can fail;
2. heap-owned thread-visible state: storage, gates, event vectors, and ready
   flags shared by value, so a thread that outlives a failure cannot read
   overwritten frame memory;
3. a shape that makes (1) and (2) mechanical rather than a convention each
   author must remember.

Until then, the hidden probe documents the hazard and the victim stays RED.
