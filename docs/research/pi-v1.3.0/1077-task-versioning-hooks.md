# #1077 durable task definition versioning, replacement and phase hooks: delivery record

Delivered on `main` against the `pi-v1.3.0 /
71023aec1546d6d677a65669c5665c0927ce0dd0` named baseline.

## Scope

`cch::durable::Harness`, `cch::durable::Registry`, `cch::durable::TaskDefinition`
and `cch::durable::TaskRuntime` gain task definition migration before
reservation, recoverable blocked definition states (`missing_task`,
`task_too_old`, `migration_failed`) with derived `Harness::inspect()` reporting
and orphan settlement on abort, phase-boundary definition handover without
mid-flight takeover, and extension-ordered task phase hooks with ordinary error
isolation:

- **Task definition migration and hook declarations**
  ([Task.hpp](../../../src/durable/include/cch/durable/Task.hpp),
  [Task.cpp](../../../src/durable/Task.cpp)): `MigratedTaskState`,
  `MigrateHook`, `TaskHook`, `HookInvokeHook`,
  `TaskDefinitionBuilder::migrate`, `TaskDefinitionBuilder::hook`,
  `TaskDefinition::has_migrate`, `TaskDefinition::migrate`, and
  `TaskDefinition::has_hook`. Consumed builders treat `migrate` and `hook` as
  documented no-ops after `build()`.
- **Live extension task and hook registration**
  ([Workflow.hpp](../../../src/durable/include/cch/durable/Workflow.hpp),
  [WorkflowRuntime.cpp](../../../src/durable/WorkflowRuntime.cpp)):
  `Extension::tasks`, `Extension::hooks`, `TaskHookRegistration`, `task_hook`,
  in-place extension replacement by name in `Registry::install`,
  `Registry::uninstall`, task-name collision and reserved `pi.generation`
  rejection, and lock-free subscriber wake-ups to reconsider blocked tasks.
- **Reservation migration, blocked states, handover and hook isolation**
  ([Harness.hpp](../../../src/durable/include/cch/durable/Harness.hpp),
  [Records.hpp](../../../src/durable/include/cch/durable/Records.hpp),
  [Harness.cpp](../../../src/durable/Harness.cpp)):
  - `Harness::inspect()` returns `HarnessInspection` (`scheduling` and
    per-task `TaskInspectionState`: `ready` with `migrates`, `running`,
    `completing`, or `blocked` with `missing_task` / `task_too_old` /
    `migration_failed`) without running task or migration code.
  - At reservation, an older stored record runs `TaskDefinition::migrate` and
    atomically commits the migrated `version`, `input`, `checkpoint` and
    `running` state before running the newer version's phase.
  - Missing, too-old, and failed-migration definitions keep the task `pending`
    and recoverable; a failed migration is recorded per `(task, definition)` and
    reported once through `HarnessOptions::on_report` without retrying on
    unrelated registry changes, then recovers when a usable definition is
    installed. Aborting a blocked task with no active invocation settles it as
    `orphaned` with the blocked reason (`missing_task`, `task_too_old`, or
    `migration_failed`).
  - Installing a replacement definition while a phase is active does not take
    over mid-phase. At the phase boundary after durable progress, if the new
    definition can reserve the task, the invocation commits `pending` (keeping
    checkpoint and memos) and hands over to a fresh reservation under the new
    definition; if the replacement is missing or cannot reserve the task, the
    running invocation reports
    `Task <id> keeps running under its old <kind> definition` once and continues
    under its original definition.
  - `TaskRuntime::each_hook` runs matching task hooks in extension order from
    the phase's registry snapshot, reports ordinary hook errors via
    `HarnessOptions::on_report` while continuing to subsequent hooks and the
    task phase, and propagates failure immediately when the invocation's
    `stop_token()` is cancelled.

Tests: [TaskExecutionTest.cpp](../../../tests/durable/TaskExecutionTest.cpp).

## Frozen evidence and observations

Every `[issue1077]` comparison loads its registered scenario from
[audit-1055/task-versioning.json](../../../fixtures/pi-v1.3.0/audit-1055/task-versioning.json)
or
[audit-1055/task-lifecycle.json](../../../fixtures/pi-v1.3.0/audit-1055/task-lifecycle.json)
(registered in [baselines.json](../../../fixtures/pi-v1.3.0/baselines.json)):

1. `reservation migrates a stored definition before running the newer version`
   (`audit-1055/task-versioning.json`): compares `before` (`ready`,
   `migrates: false`), `afterInstall` (`ready`, `migrates: true`), `log`
   (`["run-v2"]`), `version` (`2`), `input` (`{"n": 2}`) and `outcome`
   (`completed`, `"v2:11"`).
2. `a task with no installed definition stays pending and runs when its definition returns`
   (`audit-1055/task-versioning.json`): compares `blocked` / `missing_task` and
   `pending` record status after uninstall, then verifies completion (`"v1:1"`)
   after re-installation.
3. `a task whose stored version is newer than the installed definition stays pending and blocked`
   (`audit-1055/task-versioning.json`): compares `blocked` / `task_too_old`,
   `pending` record status and preserved checkpoint `{"phase": "run", "n": 1}`,
   plus discriminating recovery to `completed` (`"v2:1"`) after installing v2.
4. `a failed migration is reported and leaves the task pending until a usable definition is installed`
   (`audit-1055/task-versioning.json`): compares `blocked` / `migration_failed`,
   single report `["cannot migrate"]` across an unrelated extension install, and
   `recoveredOutcome` (`completed`, `"v2:1"`) after installing a repaired v2.
5. `a running task hands over to a newer definition at a phase boundary`
   (`audit-1055/task-versioning.json`): compares `log`
   (`["v1-p1", "v1-p1-release", "v2-migrate-from-1", "v2-p2"]`), `version` (`2`),
   `outcome` (`completed`, `"v2"`), and memo preservation across handover.
6. `task hooks run in extension order and an ordinary throw is reported while the task continues`
   (`audit-1055/task-lifecycle.json`): compares `log`
   (`["hook-1", "hook-2", "task"]`), `reports` (`["hook boom"]`), and `outcome`
   (`completed`, `"done"`).

### Discriminating failure, cancellation and ordering cases

- **Orphan settlement across blocked reasons**: aborting a task blocked by
  `task_too_old` or `migration_failed` commits a terminal `orphaned` outcome
  carrying the exact blocked reason.
- **Incompatible replacement during a running task**: uninstalling the
  definition during `p1` and installing a v2 without `migrate` during `p2`
  reports `Task <id> keeps running under its old <kind> definition` once per
  incompatible replacement and finishes `p1 -> p2 -> p3` under v1.
- **Hook failure under invocation cancellation**: when `abort_task` signals the
  running invocation during hook 1 and hook 1 returns an error, `each_hook`
  propagates the error without reporting it as an isolated hook failure, skips
  hook 2 and the task phase body, and settles through the abort handler.
- **In-place replacement versus uninstall/reinstall hook order**: replacing an
  extension in place preserves its hook position, whereas uninstalling and
  reinstalling moves its hook to the end; conflicting task kinds and reserved
  `pi.generation` registrations are rejected without mutating the snapshot.

## Validation

| Check | Result |
| --- | --- |
| `cmake --preset vcpkg` | passed; Parity Architecture Gate PASS (12 targets, 64 dependencies, 265 sources, 3552 includes) |
| `cmake --build --preset vcpkg --target cch_tests_durable -j2` | passed; build-phase Parity Architecture Gate PASS |
| `./build/cch_tests_durable "[issue1077]"` | **passed — 9 test cases, 235 assertions** |
| `./build/cch_tests_durable "[issue1076]"` | **passed — 12 test cases, 215 assertions** |
| `ctest --preset vcpkg -L durable` | **59/59 passed** |
| `ctest --preset vcpkg -L architecture` | **43/43 passed** (including `cch_warning_gate` and `cch_owner_interface_standalone`) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | **PASS** |
| `python3 tests/evidence/ConsolidationAuditTest.py` | **39/39 passed** |
| `cmake --build --preset vcpkg -j2` and `ctest --preset vcpkg -j4` | **3741/3742 passed, 0 failed, 1 expected skip** |

## Capability status

The F3 entry and close-out summary in
[1058-implementation-frontier.json](1058-implementation-frontier.json) now list
`#1077` alongside `#1076` and reference this report. Source audit ledgers and
composite capability verification statuses in
[1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json)
remain unchanged until the remaining composite durable slices land.

## Remaining boundary

- Structured concurrency (`waiting` joins, `completing` holds, bottom-up abort
  cascades, `abandonOnRestart`, and idle/wait cancellation) belongs to `#1078`.
- Workflow `GenerationTask.onYield` and `ToolTask` before/after hooks belong to
  `#1087`.
- Full `HarnessInspection` submission listing, task graph, conversation views,
  and event streams belong to `#1088`.
