# #1076 durable task execution: delivery record

Delivered on `task/1044-1076-review` on top of the merged #1076 slice
(merge commit `37736f8bbccbebfec8a73612df2e445faeb4bc9f`, task commits
`66714473e` and `7716aab3b`) against the `pi-v1.3.0 /
71023aec1546d6d677a65669c5665c0927ce0dd0` named baseline.

## Scope

`cch::durable::Harness` runs registered `TaskDefinition`s through their phases
against the durable `Session`, and `cch::durable::TaskRuntime` gives a phase or
abort handler its memo and commit channels. The delivered behavior is:

- **Definitions and the builder.** `TaskDefinitionBuilder::initial/phase/abort`
  accumulate handlers; `build()` validates the kind and moves the definition
  out. `build()` consumes the builder: a second `build()` returns a
  `support::ErrorCode::Validation` error instead of dereferencing a moved-out
  implementation, and the mutators are documented no-ops after a successful
  build (`valid()` reports whether a definition is still owned).
- **Phase scheduling and checkpoints.** The worker starts pending tasks,
  runs the phase named by the checkpoint, and commits `running` checkpoints or
  `terminal` outcomes through `TaskRuntime::commit`. A phase that returns
  without durable progress faults the task with
  `Task <kind> phase <name> returned without durable progress`.
- **Memos.** `TaskRuntime::memo` is first-writer-wins, survives checkpoint
  replacement, and is cleared on terminal settlement.
- **Abort.** `Harness::abort_task` durably marks a running task and returns
  `"marked"` (or `"terminal"` for an already-terminal task). The mark rejects
  further phase memo and commit writes, the abort handler runs with
  abort-commit authority, and a handler that returns an error or returns
  successfully without a terminal commit faults the task with the handler's
  message or
  `Abort handler of task <id> returned without a terminal outcome`.
- **Missing definitions.** Aborting a task whose definition is not registered
  commits an `orphaned` / `missing_task` outcome.
- **Atomic terminal receipt.** The terminal `TaskRecord`, including its
  outcome and result, is written by a single `Storage::commit` batch.
- **Resume.** `open_tasks()` reconciles surviving `running` tasks to `pending`;
  a reopened Harness resumes from the durable checkpoint without rerunning
  completed phases.

Implementation: [Harness.cpp](../../../src/durable/Harness.cpp),
[Task.cpp](../../../src/durable/Task.cpp),
[Harness.hpp](../../../src/durable/include/cch/durable/Harness.hpp),
[Task.hpp](../../../src/durable/include/cch/durable/Task.hpp).
Tests: [TaskExecutionTest.cpp](../../../tests/durable/TaskExecutionTest.cpp).

## Frozen evidence and observations

Frozen observations are in
[audit-1076/task-outcomes.json](../../../fixtures/pi-v1.3.0/audit-1076/task-outcomes.json),
captured by
[capture-1076-task-outcomes.mts](capture-1076-task-outcomes.mts) and registered
in [baselines.json](../../../fixtures/pi-v1.3.0/baselines.json). Every test case
loads its scenario by name through `FrozenScenario` and compares against that
artifact's own `expected` object.

### `invocations` is non-comparable, not asserted as phase count

The frozen counter scenario records `invocations: 1`, and the capture computes
it as `runtimes.size` — the number of distinct JavaScript `TaskRuntime` object
identities a phase body observed. That quantity has no reliable C++ analogue
here: `Harness::Impl::run_task_execution` constructs `HarnessTaskRuntime` as a
stack local for each phase invocation, so an identity taken from the runtime
reference would be a stack address that may be reused across sequential
invocations. Asserting the phase count (`3`) against frozen `invocations` (`1`)
would compare a different quantity and would be satisfied by a respelling of
the observation.

The former unread `invocation_count` local was removed. The observation is
recorded here as **non-comparable** rather than silently substituted, and the
counter test asserts the frozen quantities it can: `phasesSeen` `[0, 1, 2]`,
`result` `3`, `status` `terminal`, `outcomeStatus` `completed`, timestamp
presence, and empty terminal memos. The pre-existing #1055 artifact records the
same `invocations: 1` for the same reason.

### Threading of test assertions

Harness phase and abort handlers run on Harness execution threads. Catch2
assertion macros are not thread-safe for reporting, so the tests never place
`REQUIRE`/`CHECK` inside a phase or abort callback: a callback records an
observation (an atomic flag, or a returned `Validation` error that faults the
task) and the test thread asserts it after `wait_for_task` returns. Optional
receipt members (`state.outcome`, `outcome->error`, `outcome->result`) and
frozen scenario members are `REQUIRE`-checked before dereference on the test
thread.

### Runtime reference lifetime across the async memo producer

The memo scenario's `p1` handler captures `TaskRuntime&` in the producer it
returns. That borrow is explicit and valid because the Harness owns the runtime
for the whole phase invocation and only destroys it after the returned
`AsyncResult` delivers its terminal completion, so the producer's callbacks
cannot outlive the referent.

### `initial_log`

The resume scenario records the pre-interruption phase log in `initial_log` and
asserts it after the first `close()`: exactly `["p1"]`, because the interrupted
`p2` returns from its stop branch before logging. The reopened log is asserted
against the frozen `reopenedPhasesRan` `["p2:1"]`.

## Validation

All checks below were run in this review worktree after the changes, on the
merged #1076 baseline `37736f8bb`.

| Check | Result |
| --- | --- |
| `cmake --preset vcpkg` (review worktree configure, cold) | passed; Parity Architecture Gate PASS — 12 targets, 61 dependencies, 265 sources, 3537 includes |
| `cmake --build --preset vcpkg --target cch_tests_durable -j2` | passed; build-phase Parity Architecture Gate PASS |
| `./build/cch_tests_durable "[issue1076]"` | **passed — 12 test cases, 215 assertions** |
| `ctest --preset vcpkg -L durable -j2` | **44/44 passed** |
| `ctest --preset vcpkg -L architecture -j2` | **43/43 passed** (36 architecture cases plus the 7 `cch_tests_cli_arch` cases) |
| `cmake --build --preset vcpkg -j2` (full build) | **passed** — 307/307 steps, build-phase Gate PASS |
| `ctest --preset vcpkg -j4` (full suite) | **3715/3715 passed, 0 failed**; 1 expected skip (Native TUI differential capture exposes deterministic Pike cells and ANSI) |
| `scripts/format-check.sh 37736f8bb` | clean |
| `git diff --check 37736f8bb` | clean (no whitespace errors) |
| JSON validity of `1058-implementation-frontier.json` | valid; F3 `relatedIssues` = `["#925", "#932", "#1076"]` |

After merging into `integration/1044-pi-v1.3.0` the full build and full suite
were re-run on the merged tree: build 307/307 steps passed, and
`ctest --preset vcpkg` reported 3715/3715 passed with the same single expected
skip. The merged tree is byte-identical to the reviewed branch tip
(`git diff 6f858dcb3 <merge>` is empty), so the reviewed and merged results
describe the same tree.

### Pre-existing runner-sensitive flakes, not caused by this change

Under concurrent load the merged tree intermittently failed two cases that this
diff does not touch: `Process Terminal atomic stop and default stop detect
postdrain EIO on closed master` (`tests/tui/ProcessTerminalTest.cpp`, from
`ef59d18ca`) and `Projection publishes 100 message-update chunks inside the
issue cost bound over a long history` (`tests/coding_agent/ProjectionStreamTest.cpp`,
from the #617 projection work). Both are wall-clock or PTY-timing sensitive and
both passed on isolated rerun; a later single-job full run passed all 3715
cases. The host was running several concurrent agent sessions during the
concurrent runs. The recorded full-suite result is the clean run.

### Interrupted runs before the final result

Earlier architecture runs in this worktree were stopped deliberately while
diagnosing why the label selected only 36 of the main checkout's 43 cases. The
cause was the cold build tree: `cch_tests_cli_arch` had not been built, so its 7
architecture cases were not registered. After building that target the label
selected 43 cases and the complete run above passed.

### Runner-sensitive rerun detail (preserved from #1076)

The merged #1076 delivery recorded two runner-sensitive failures in a first full
suite run — a PTY post-drain `EIO` and a projection timing case. Both passed on
isolated rerun and on the subsequent full run; the full-suite counts above are
from that run. They remain runner-resourcing conditions, not retuned bounds.

## Capability status

The F3 entry in [1058-implementation-frontier.json](1058-implementation-frontier.json)
records this verified slice in its `evidence.missing` text and now lists
`#1076` in `relatedIssues`. Definition version migration/replacement, task phase
hooks and hook isolation remain unverified in C++; no consolidated-ledger row is
promoted to verified without a complete consolidation regeneration.

## Remaining boundary

- Definition version migration, replacement, and task hook isolation stay open
  F3 acceptance items.
- The frozen `invocations` observation stays non-comparable until this harness
  exposes a stable per-invocation runtime identity.
