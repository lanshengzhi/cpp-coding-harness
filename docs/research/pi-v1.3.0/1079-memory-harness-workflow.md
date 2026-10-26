# #1079 durable memory Harness workflow: delivery record

Delivered on `main` (commit `355e53a5`) against the `pi-v1.3.0 /
71023aec1546d6d677a65669c5665c0927ce0dd0` named baseline.

## Scope

`cch::durable::Harness` gains a bounded model/tool workflow over the
existing task runtime. The workflow lazily creates a root conversation and
five built-in documents, resolves registered tools and static prompt
sections from the live registry snapshot, consumes an AI-owned
`ModelStreamFactory`, executes injected tools, and commits a task-backed
settled receipt with the final entry identity.

- **Public API** (`src/durable/include/cch/durable/Workflow.hpp`):
  `ToolRegistration`, `Extension`, `PromptSection`, thread-safe `Registry`,
  `HarnessSettings`, `RootOptions`, `Submission`, `SettledSubmission`.
- **Workflow runtime** (`src/durable/WorkflowRuntime.cpp`): `pi.generation`
  task definition wired into `Harness::Impl::init()`; `root()` creates
  conversation 1 with `pi.agent:v1`, `pi.inbox:v1`, `pi.live:v1`,
  `pi.provider:v1`, `pi.usage:v1` in one transaction; `submit()` commits
  `pi.user` + task atomically then resumes; `run()` assembles `ai::AiContext`
  from the registry snapshot (tools_added/system sections only on change),
  streams via `ai::ModelStreamFactory`, commits `pi.assistant` + terminal
  outcome (answer entry id) atomically, executes real tools with
  `durationMs`, commits `pi.tool-result` entries.
- **Entry provenance**: `EntryRecord` carries `model` (the model-message
  payload) and `by_task_id` (the owning execution task). JSONL and SQLite
  backends persist both fields through serialization and restore.
- **Bug fix found by testing**: receipt waiters hung when final-answer
  persistence failed; added `wait_error` + `subscribe_close` handler
  completing all waiters with "Session failed after a storage error; close
  and reopen it" / "Session is closed".

Implementation: [WorkflowRuntime.cpp](../../../src/durable/WorkflowRuntime.cpp),
[WorkflowRuntime.hpp](../../../src/durable/WorkflowRuntime.hpp),
[Workflow.hpp](../../../src/durable/include/cch/durable/Workflow.hpp),
[Harness.cpp](../../../src/durable/Harness.cpp),
[Session.cpp](../../../src/durable/Session.cpp).

Tests: [HarnessWorkflowTest.cpp](../../../tests/durable/HarnessWorkflowTest.cpp).

## Frozen evidence and observations

Frozen observations are in
[audit-1056/workflow.json](../../../fixtures/pi-v1.3.0/audit-1056/workflow.json)
(normal workflow transcript, root atomism, late-registry transition) and
[audit-1079/held-tool.json](../../../fixtures/pi-v1.3.0/audit-1079/held-tool.json)
(held-tool ordering vs settlement), captured by
[capture-1079.mts](../../../fixtures/pi-v1.3.0/audit-1079/capture-1079.mts)
against a clean `pi` checkout and registered in
[baselines.json](../../../fixtures/pi-v1.3.0/baselines.json).

### Non-comparable: receipt duration

The frozen audit-1056 receipt records a wall-clock `durationMs` measured by
the JavaScript runtime. The C++ receipt exposes `startedAt`/`endedAt` from
the task clock but does not populate a comparable duration field. The tests
assert timestamp presence and ordering, not the frozen duration value.

## Validation

| Check | Result |
| --- | --- |
| `cmake --preset vcpkg` | passed; Parity Architecture Gate PASS |
| `cmake --build --preset vcpkg --target cch_tests_durable -j2` | passed; build-phase Gate PASS |
| `./build/cch_tests_durable "[issue1079]"` | **passed — 5 test cases, 212 assertions** |
| `ctest --preset vcpkg -L durable` | **48/48 passed** |
| `ctest --preset vcpkg -L architecture` | **43/43 passed** |
| `ctest --preset vcpkg -j2` (full suite) | **3728/3728 passed**, 0 failed; 1 expected skip |
| Parity Architecture Gate (build phase) | PASS |

## Capability status

The F1 `evidence.missing` text in
[1058-implementation-frontier.json](1058-implementation-frontier.json) now
lists `#1079` and describes the delivered slice. The consolidated ledger
rows for `cch_durable:generation-admission`, `cch_durable:harness-open-close`
and `cch_durable:extension-sections` record `#1079` as the implementation
ticket with updated `cppSeam`/`cppEvidence` text. Verification stays
`upstream-captured` until a pinned C++ comparison delivery lands.

## Remaining boundary

- Submission dedup/inbox policies (#1080), workflow reopen (#1082),
  dynamic registry hooks (#1087), event views/task graph/inspect (#1088)
  remain separate slices.
- The `pi.generation` task kind is reserved; caller-supplied definitions
  with the same kind are rejected at `Harness::open`.
