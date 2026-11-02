# #1173 — durable public agent event stream and lifecycle ordering

## Scope

Delivers the frozen durable public agent event stream through the public
Harness surface, under [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and the [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044)
delivery, with [1173](https://github.com/lanshengzhi/cpp-coding-harness/issues/1173)
as its ticket. Named baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

[#1088](1088-observation-views.md) delivered committed conversation/document/
task-graph views and explicitly excluded `watchEvents` from that slice.
[#1056](../../../fixtures/pi-v1.3.0/audit-1056/observability.json) recorded one
frozen event-stream scenario. This slice delivers `Harness::watch_events`, the
frozen event vocabulary and its ordering, the live tool-slot presentation that
gives tool events their call/task/entry linkage, and the generation hand-over
that gives a model/tool round its frozen turn boundaries. Whole-row
verification, capability promotion and the consolidated ledger rebuild stay
with [#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180).

## Frozen contract

Source of truth at the pinned revision:

- `packages/durable/src/harness/events.ts` — `watchEvents`, `AgentEvent`, the
  `translate` order (tool starts, progress, retry/deferred, tool ends, entries,
  task failures, turn and run boundaries, submissions, document state, then what
  began), the listener failure and cancellation paths, and the frozen
  `CommittedWatch` buffering (`MAX_PENDING_WATCH_FRAMES = 100`, snapshot
  replacement).
- `packages/durable/src/harness/generation.ts` — `startToolRound` (one `pi.tool`
  task per offered call, owned by the generation), the `tools` phase and
  `finishToolRound` (the successor generation that gives each round its turn
  boundary).
- `packages/durable/src/harness/tool.ts` / `live.ts` — the live tool slot
  (`callId`, `name`, `status`, `taskId`, `output`, `details`, `entry`) and
  `finishSlot`.
- `packages/durable/src/session/observation.ts` — `CommittedWatch` start, stop,
  cancellation, listener error and the `SnapshotEvent` at attachment.

The independent capture in `fixtures/pi-v1.3.0/audit-1173/` records thirteen
scenarios from frozen pi at the named revision through the public Harness and
`watchEvents`: a normal model/tool round (per-batch types, flattened summaries,
collapsed tool progress, counts, tool call/task/entry linkage, settlement), a
tool held by a gate (snapshot and task states during the hold, a second
subscriber's deliveries), a tool held at `completing` by live owned work, an
abort during a held tool (aborted result retained, settlement, a later run), a
retryable provider error (`auto_retry_start`/`auto_retry_end`), a terminal
provider error, a throwing tool (frozen harness error text), a stopped stream
(deliveries stop, work continues, an unstarted stream drops its buffer), replay
of buffered batches on start, the 100-batch overflow replacement, a listener
failure, and an aborted attachment context. The capture resolves the baseline
from `baselines.json`, refuses a checkout whose `HEAD` is not the registered
revision or whose tracked tree is dirty, re-executes under that checkout's
`tsx`, and writes the artifact and manifest itself. Expected event sequences,
batch boundaries, snapshots and settlements come from that capture, not from
C++. Its normalizations are identity, batched frames, tool-task ID presence
rather than value, and watch end by reason.

The existing `audit-1056` observability scenario ("the agent event stream reports
run, turn, message, tool and submission lifecycle in order") remains registered
and unchanged.

## Delivered behavior

- `Harness::watch_events(conversation, stop_token)` returns an
  `AgentEventStream`: the constant `snapshot` event at attachment, then one
  batch per committed publication, in commit order, with the frozen event
  vocabulary. Events are the frozen JSON objects (`type` plus the frozen
  members), so a consumer reads the same `toolCallId`/`taskId`/`entry`/
  `record`/`inputs`/`usage`/`message` fields the frozen caller reads.
- The stream translates committed state only: conversation view before/after,
  the publication's record changes, its view operations, and the `completing`
  generation hold read on the same line as the snapshot. It introduces no
  scheduler and mutates nothing.
- A model-issued tool call is now its own durable `pi.tool` task, owned by the
  generation, exactly the frozen `startToolRound` shape. The generation waits on
  the round's tasks, `finishToolRound` applies the round's controls and the
  `postTools` boundary in one commit, and the run passes to a successor
  generation. That hand-over produces the frozen turn boundaries and the
  generating task's `completing` hold.
- `pi.live.tools` slots are published from the committed round: `pending` when
  the round opens, `running` with the task at intent, `output`/`details` from
  the tool's `ToolExecutionContext::report_output`/`report_details` sinks, and
  `done` with the result entry at settlement (or without one for a faulted,
  orphaned or interrupted call). A tool that reports output without returning
  content keeps that output as its result content, as frozen does.
- Buffering, replay and end rules follow frozen `CommittedWatch`: an unstarted
  stream buffers up to 100 batches, overflow replaces them with one snapshot of
  the newest view, `start` is single-use, `stop` drops buffered batches and
  ends one observer only, a listener failure ends the stream with
  `ListenerError` and reports it, and an aborted attachment context ends the
  stream with `Cancelled` before attachment (rejecting the acquisition) or
  after it (ending deliveries).
- A subscriber failure or stop cannot become a second scheduler or abort
  persistent work: the frozen evidence shows the run settling `done` with its
  transcript complete after a listener failure, after `stop`, and after its
  attachment context is cancelled.

## Validation

Measured on this branch, base `23f9dd8f7` (`origin/main`), Debug preset.

- Focused event-stream selection `ctest --preset vcpkg -L issue1173`:
  **11 selected, 11 passed, 0 failed**, 413 assertions
  (`/tmp/cch-1173-focused.log`).
- Durable shard `ctest --preset vcpkg -L durable`: **341 selected, 100% passed**,
  with the pinned cross-implementation skips (`/tmp/cch-1173-durable5.log`).
- Frozen selection `ctest --preset vcpkg -L frozen`: **5 selected, 5 passed**
  (the four #1088 observation scenarios plus the audit-1054 schema baseline);
  no frozen expectation was changed (`/tmp/cch-1173-frozen.log`).
- Architecture `ctest --preset vcpkg -L architecture` after a full build:
  **43 selected, 43 passed**, including `cch_warning_gate`
  (`/tmp/cch-1173-architecture2.log`, `/tmp/cch-1173-warning-gate3.log`).
- Consolidation: `consolidate_audits.py build` rebuilt 1464 rows and
  `check` passed; the two consolidation CTest cases passed 2/2
  (`/tmp/cch-1173-consolidation.log`).
- Repository Full Validation: `cmake --build --preset vcpkg` succeeded, then
  `TERM_PROGRAM=herdr ctest --preset vcpkg` selected **4186 tests: 4184 passed,
  2 failed, 0 skipped** (`/tmp/cch-1173-full2.log`). The two failures are
  load-sensitive timing bounds, not this change's cases: `Projection publishes
  100 message-update chunks inside the issue cost bound` is the quarantined
  #632/#617 bound, and `bounded parallel cancellation leaves queued tool
  capabilities unstarted` is an agent tool-executor timing case; both passed
  individually and repeatedly on rerun (3/3 and 2/2). The earlier full run on
  the same tree additionally showed four codemode worker timing cases that also
  passed individually (`/tmp/cch-1173-full.log`).
- Formatting: `scripts/format-check.sh origin/main` reports clean after
  `git clang-format`.
- Frozen evidence: `python3 scripts/evidence/audit_evidence.py check --manifest
  fixtures/pi-v1.3.0/audit-1173/manifest.json` passes and prints the registry
  record. Registration in `baselines.json` and the classification of this
  family's 13 scenarios as explicit orphan observations are the serial merger's
  steps (issue #1154's manual-classification friction); until they land the
  family stays out of the consolidation index by design.
- The delivery receipt for `1090-durable-images` pins
  `cmake/tests/DurableTests.cmake`, which this change edits. Its exact selection
  was rerun on this tree into
  `docs/research/pi-v1.3.0/deliveries/1173-revalidation-1090-1104.xml`
  (36 selected, 36 passed), the pinned digest and validation record were
  refreshed, and `revalidation1173` records why the scenario mapping is
  unchanged.

## Capability status

No capability row is promoted. `cch_durable:event-stream` gains a public
implementation and independent frozen evidence; promotion and receipt
reconciliation stay with #1180.

## Remaining boundary

- A model-issued call's arguments reach `tool_execution_start` from its tool
  task's intent checkpoint, as frozen; the C++ audit-1173 capture confirms the
  `taskId` linkage. A **nested** call's `tool_execution_update`/`end` events and
  the `nestedTools` presentation are not published: the C++ nested path records
  results in the caller's task documents but has no live nested slot, so the
  frozen `nestedTools` snapshot and nested tool events are not claimed.
- Frozen `ToolExecutionApi.createTask` has no C++ counterpart in
  `ToolExecutionContext`, so the capture's "tool held at completing by live
  owned work" scenario is not reachable through the C++ public tool caller. The
  scheduler's completing hold itself is covered by existing durable tests; the
  frozen scenario is registered and remains unclaimed until a tool-task-creation
  seam exists.
- Frozen progress pacing (adaptive commits, bounded output retention with
  `droppedBytes`/`droppedLines`, diagnostics) is not ported. The C++ writer
  commits each report immediately and retains the reported output without the
  bounded window; the comparison unit is the reported progress (accumulated
  output and last details), not the frozen commit partition.
- Streaming partial assistant messages (`message_update`, `generation.message`)
  are not published: the durable generation commits whole assistant messages.
  `message_start`/`message_end` cover committed entries.
- Compaction status presentation (`live.compactions`, `compactionFailure`,
  `compaction_start`/`compaction_end`) and `deferred_poll` are translated only
  from state the C++ live document publishes; this slice publishes no
  compaction statuses, so those events do not appear.
- The frozen `entry_appended`, `inbox_update`, `agent_changed` and `task_failed`
  branches are implemented, but no independent frozen capture selects them;
  they are not claimed as verified.
- Recovery now resumes the round's durable `pi.tool` tasks (frozen shape) rather
  than re-running the model call inline. The existing recovery tests were
  adjusted where their fixtures assumed the inline path; the replay policy of a
  recovered nested call is still enforced by the resumed task.
