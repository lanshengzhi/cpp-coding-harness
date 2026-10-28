# 1166 — Durable failure signaling and active-invocation lifecycle

Worktree: `worktrees/cpp-coding-harness/1166`, branch `ticket/1166`.
Base: main @ 49850636d. Commits (bottom-up):

1. `36a296fec` — tests: durable failure-lifecycle probes (RED on main)
2. `9da8e720c` — test: release-then-assert probe restructure (-fno-exceptions)
3. `f787d03f7` — fix(1165): copy stop sources out of the scheduler mutex
   (reviewed candidate from the #1165 investigation, adopted as an
   independent labeled step; see that commit message for the four
   original call-order sites and their races)
4. `aad4b2cd8` — fix(1166): signal active invocations + seal inspect

## Frozen contract (pinned by `[issue1166][failure]`)

Event order, exactly as the frozen probe observed 3/3 on the frozen
checkout (`/tmp/cch-1088-frozen-failure-{1,2,3}.log`):

```
phase, fail-call, storage-reject, abort-enter, inspect-rejected,
abort-return, commit-rejected
```

Properties: the active invocation is signalled before the failing
commit returns; the Session is sealed for every later operation with
`Session failed after a storage error; close and reopen it` (never the
raw storage error); the failing caller still receives the ORIGINAL
storage error (`injected 1166 failure` in the probe).

## Mechanism

`Session::Impl::fail()` sets `failed`/`closed` and runs the close
listeners synchronously on the failing commit's thread before the
rejection reaches the caller. The Harness `subscribe_close` listener
now copies the active invocations' `stop_source`s out of the scheduler
mutex and `request_stop()` after releasing it (the #1165 discipline),
so the phase's stop callback runs inline between the storage rejection
and the returned error — the frozen `fail -> close -> beforeClose ->
TaskScheduler.join()` ordering. `Harness::inspect()` seals on
`session->failed()` with the same header-inline
`session_failed_error()` the Session uses
(`src/durable/SessionErrors.hpp`).

## RED -> GREEN ladder (3 runs per state, logs in this directory)

| State | failure probe | lock-cycle probe |
|---|---|---|
| unmodified main | 3 assertion failures (events order, signalled-before-return, sealed callback inspect) | 1 failure (listener `resume()` exhausts its 5 s budget) |
| + #1165 candidate (`f787d03f7`) | 3 failures (unchanged) | GREEN |
| + #1166 (`aad4b2cd8`) | GREEN, 3/3; full durable shard 172/172 cases, 5296 assertions | GREEN |

## Finding: `-fno-exceptions` test-TU landmine (ADR 0042)

The durable test shards build with `-fno-exceptions`. Catch2 `REQUIRE`
failure throws from the Catch2 static library *through* the test frame;
frames in a `-fno-exceptions` TU have no cleanup landing pads, so **no
destructors run on the failing path**. A durable test that REQUIRE-fails
while its Harness owns live threads leaks the worker and execution
threads with dangling captures; when the next test case reuses the freed
memory (malloc address reuse), the runaway threads read garbage
(`storage` member, `closed` flag) and crash far from the cause — or a
leaked line mutex blocks the next test forever.

Evidence: 9/12 full-suite runs SIGSEGV'd while each test passed in
isolation; cores (`/tmp/cch1166-core*`) show the previous test's
execution thread (`run_task_execution` on a freed `Harness::Impl`)
looping into the next test. Fix adopted for the probes:
**release-then-assert** — drain the invocation and close the harness
(recording outcomes) before any REQUIRE can fail; documented in the
test file header. Existing durable tests share this exposure on their
failure paths; worth a follow-up note for the suite.

## Boundaries (out of scope, reported not fixed)

- The Session-line reentry hazard (a stop callback that *waits* on a
  Session-line read inside the failure window) is structural and
  demonstrated by `[issue1166][reentry]` (500 ms bounded timeout); not
  solved here.
- `Harness::inspect()` keeps its direct-scan semantics; the change is a
  sealing gate only. The #1088 observation interface should apply the
  same `session->failed()` gate at its entry point.

## Validation

- Focused: `cmake --preset vcpkg` + `cmake --build --preset vcpkg
  --target cch_tests_durable`; `[issue1166]` 10/10 stable; full
  `cch_tests_durable` shard 172/172 cases green.
- Full tier (whole project + all shards): NOT run — requires the
  exclusive CPU window requested from the orchestrator.
