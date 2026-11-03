# #1176 Agent package reconciliation

**Status:** reconciliation index and a bounded four-row comparison subset delivered. No capability
row is promoted, no membership changes, and no shared delivery receipt is edited: registration is
serial PM work under #1183. A1–A4 remain open batches; `ToolRegistry:active-set` is blocked on a
recorded divergence, and `Agent:wait-for-idle` remains partial on a named seam blocker.

- **Authority:** [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), ADR 0075,
  Named Baseline **pi-v1.3.0 / `71023aec1546d6d677a65669c5665c0927ce0dd0`**, Linux x86-64 glibc.
- **Inventory:** 29 supported rows owned by `cch_agent_core` in
  [1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json), 33
  row-scenario links over the 30 frozen `audit-1047` scenarios.
- **Frozen evidence:** `fixtures/pi-v1.3.0/audit-1047/` — `lifecycle.json` (11),
  `tool-execution.json` (10), `queue-admission.json` (6), `cancellation.json` (3); registered
  manifest `audit-1047/manifest.json`. Expectations in this ticket are read from those artifacts,
  never derived from C++.

## Central finding: zero rows read the frozen artifact

Before this ticket, **0 of 29 `cch_agent_core` rows had a C++ case that opened an `audit-1047`
artifact**. Every row was `upstream-captured` with the same remaining work: “C++ comparison against
the registered artifact is not run; the row stays unverified.”

Three traps were ruled out explicitly:

1. **`AgentCoreEvidenceTest.cpp` is not this baseline.** Its three lifecycle/terminal goldens read
   `fixtures/pi-agent-core/`, whose README pins frozen pi commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`
   (`@earendil-works/pi-agent-core@0.83.0`). It is a valid #351/#350 evidence bundle, but it is not
   the #1044 `71023aec…` capture and cannot support a #1176 receipt.
2. **The 1047 source audit's `existingCxxTests` list is history, not observation.** It names the
   hand-written cases that were later delivered by #1106–#1113; none of them reads the artifact.
3. **Same scenario name is not a comparison.** Twenty-one rows had only a partial hand-written case;
   one row had no case at all. The four rows in the delivered subset are the first cases that read
   `expected` from `audit-1047` at test time.

The strict pre-ticket buckets were:

| Bucket | Rows |
|---|---:|
| C++ case reads `audit-1047` (whole-row receipt possible) | **0** |
| Same-scenario hand-written case asserts every frozen `expected` field as a literal | **7** |
| Same-scenario hand-written case asserts only part, or uses different inputs/texts | **21** |
| No scenario at all | **1** (`Agent:wait-for-idle`) |

## Reconciliation index

`✓` means the existing C++ case asserts that frozen `expected` field; `✗` means missing or different.
For S1–S4 the fields are now read from the artifact rather than copied. `Artifact` is under
`fixtures/pi-v1.3.0/audit-1047/`.

### Agent lifecycle and state — `lifecycle.json`, `cancellation.json`

| Row | Frozen scenario | Existing case | Frozen `expected` coverage | Route |
|---|---|---|---|---|
| `Agent:abort` | `cancellation.json`: abort during sequential execution skips the next call; abort while streaming is idempotent | `AgentBehaviorTest.cpp` suspended-tool abort; `AgentTest.cpp` abort token; `ModelRuntimeSeamTest.cpp` aborted terminal | `✓ isStreaming (idle after abort), signalWasAborted (active request token)`; `✗ executeCalls, startedToolCalls, requestCount, secondRequestSignalAborted, events, errorMessage`; `✗ signalPresentAfterRun` | A1 |
| `Agent:continue` | `queue-admission.json`: continue from an assistant tail drains steering before follow-up | `AgentTest.cpp` continue-from-tail | `✓ requestRoles, lastUserMessages, remainingQueued, agentEndMessages` | A3 |
| `Agent:default-state` | `lifecycle.json`: defaults select off thinking, parallel tools, one-at-a-time queues; off remains for a reasoning model | `AgentTest.cpp` thinking default; `AgentBehaviorTest.cpp` queue/tool defaults | `✓ thinkingLevel, steeringMode, followUpMode, toolExecution`; `✗ modelId, toolCount, messageCount, transport, isStreaming` | A1 |
| `Agent:event-lifecycle` | `lifecycle.json`: turn and event ordering for a tool turn and a final response | `AgentBehaviorTest.cpp` text/tool event counts; `AgentCoreEvidenceTest.cpp` old-baseline golden | `✗ events, finalMessageRoles, toolResultText` (counts only) | A1 |
| `Agent:finish-turn-hook` | `lifecycle.json`: finishTurn action end stops before follow-up polling | `AgentBehaviorTest.cpp` finishTurn end | `✓ finishTurnCalls, requestCount, remainingQueued`; `✗ events` | A2 |
| `Agent:prepare-next-turn-hook` | `lifecycle.json`: prepareNextTurn runs once and can append messages | `AgentBehaviorTest.cpp` once-per-turn + appended messages | `✓ prepareNextTurnCalls, secondRequestRoles, secondRequestLastUser` | A2 |
| `Agent:prepare-request-hook` | `lifecycle.json`: prepareRequest observes the pending prompt | `AgentBehaviorTest.cpp` prepareRequest observe / no-steering-poll | `✗ calls` (`roles`, `model`, `thinkingLevel` not recorded at the hook) | A2 |
| `Agent:prompt-busy` | `lifecycle.json`: second prompt rejected without replacing the run | `AgentTest.cpp` suspended-run rejection | `✓ secondPromptError` (and active run survives) | **S3** |
| `Agent:provider-error` | `lifecycle.json`: provider error emits one failed assistant lifecycle | `ModelRuntimeSeamTest.cpp` error terminal; `AgentCoreEvidenceTest.cpp` old-baseline golden | `✗ events, errorMessage, isStreaming` (terminal/count checks only) | A1 |
| `Agent:reset` | `lifecycle.json`: reset restores the prompt baseline and clears both queues | `AgentTest.cpp` reset baseline | `✓ roles, systemPrompt, hasQueuedMessages, isStreaming` | **S2** |
| `Agent:stream-injection` | `lifecycle.json`: ordinary stream injection drives updates and the final message | `AgentBehaviorTest.cpp` user lifecycle / observer order | `✓ lastAssistantText (own terminal text)`; `✗ events, streamedUpdates` (no `text_start`/`text_end` assertion) | A1 |
| `Agent:wait-for-idle` | `lifecycle.json`: agent_end listeners settle before prompt/waitForIdle; `cancellation.json`: abort/waitForIdle are idle no-ops | none | `✗ order, isStreamingDuringAgentEndListener, isStreamingAfterWaitForIdle`; `✗ signalPresent, isStreaming, messageCount` | A1 (partial, blocker) |

### Input admission — `queue-admission.json`

| Row | Frozen scenario | Existing case | Frozen `expected` coverage | Route |
|---|---|---|---|---|
| `InputQueue:admission` | queue admission is unbounded and both queues default to one-at-a-time | `AgentTest.cpp` 300+300; `ProjectionStreamTest.cpp` AgentSession admission | `✓ steeringMode, followUpMode, steeringEnqueued, followUpEnqueued, hasQueuedMessages, peekCount, peekText`; `✗ enqueueReturnsVoid` | A3 |
| `InputQueue:follow-up` | follow-up runs only after an otherwise stopping assistant turn | `AgentTest.cpp` follow-up | `✓ requestRoles, lastUserMessages, agentEndMessages, remainingQueued` | A3 |
| `InputQueue:peek-clear` | peek prefers steering and clearAllQueues removes both queues | `AgentTest.cpp` peek/clear | `✓ peekBefore, peekAfterSteeringClear, hasQueuedMessages, peekCount` | **S1** |
| `InputQueue:steering-all` | all-mode steering injects every queued message in FIFO order in one turn | `AgentTest.cpp` all-mode steering | `✓ requestRoles, requestCount, agentEndMessages, remainingQueued` | A3 |
| `InputQueue:steering-one-at-a-time` | one-at-a-time steering injects the oldest message after the tool turn | `AgentTest.cpp` one-at-a-time steering | `✓ requestRoles, lastUserMessages, agentEndMessages, remainingQueued`; `✗ events` | A3 |

### Tool execution — `tool-execution.json`

| Row | Frozen scenario | Existing case | Frozen `expected` coverage | Route |
|---|---|---|---|---|
| `NestedToolCalls:duration` | nested calls forward parent ids and separate record duration from execute duration | `NestedToolCallsTest.cpp` end duration; `ToolCallExecutorTest.cpp` nested events | `✓ endDurationPresent, recordStatus, recordDurationPresent`; `✗ eventTypes, parentIds, endDurationMatchesOutcome` | A4 |
| `NestedToolCalls:parent-ids` | same scenario | `NestedToolCallsTest.cpp` nested id/record | `✗ eventTypes, parentIds`; call-id format only | A4 |
| `ToolCallExecutor:after-tool-call` | afterToolCall content replacement drops structuredContent but keeps duration | `ToolCallExecutorTest.cpp` after-hook duration | `✓ content, durationPresent`; `✗ hasStructuredContent, details` | A4 |
| `ToolCallExecutor:argument-mutation` | prepareArguments before validation; beforeToolCall mutations execute without revalidation | `ToolCallExecutorTest.cpp` prepare + mutation | `✓ beforeValue, executedValue, executedValueType, durationPresent`; `✗ resultText` | A4 |
| `ToolCallExecutor:duration` | hook and execute activity are distinguishable in duration and wall time | `ToolCallExecutorTest.cpp` execute-only duration | `✓ durationLessThanBeforeHookDelay`; `✗ hookCalls, durationAtMostExecuteDelayPlusTolerance, wallClockStartToEndIncludesHookTime, durationLessThanWallClockStartToEnd` | A4 |
| `ToolCallExecutor:outcomes` | executed success, thrown failure, blocked and unknown calls keep distinct duration presence | `ToolCallExecutorTest.cpp` four-call batch; single blocked/unknown/failing cases | `✓ durationPresent, isError` per call (parallel, different names/texts); `✗ events, executeCalls, blockedText, unknownToolText` | A4 |
| `ToolCallExecutor:parallel-scheduling` | parallel tool ends follow completion order while result messages keep source order | `ToolCallExecutorTest.cpp` parallel order | `✓ startOrder, completionOrder, resultOrder, durationPresent`; `✗ maxConcurrent` | A4 |
| `ToolCallExecutor:prepare-arguments` | prepareArguments runs before validation | `ToolCallExecutorTest.cpp` prepare before validation | `✓ beforeValue, durationPresent`; `✗ executedValue, executedValueType, resultText` | A4 |
| `ToolCallExecutor:run-tool-call` | runToolCall runs the hooks and reports execute-only duration | `ToolCallExecutorTest.cpp` ok + blocked | `✓ ok.durationPresent/isError/beforeCalls/afterCalls/executeCalls/text`; `✓ blocked.durationPresent/isError/text` | **S4** |
| `ToolCallExecutor:truncation` | a call from a length-truncated message never executes and has no duration | `AgentBehaviorTest.cpp` truncated executor seam + fail-all | `✓ executeCalls(0), durationPresent, isError`; `✗ resultText` exact, `✗ events` | A4 |
| `ToolRegistry:active-set` | replacing state.tools narrows the executable set and declares the system tool delta | `AgentToolSurfaceTest.cpp` removed active tool | `✓ assignedTools, executed, systemDeclarations, totalMessages, durationPresent, events`; `✗ resultText` (diverges) | **blocked** |
| `ToolResult:duration-field` | outcomes duration presence; tool duration survives the public session record round trip | `ToolCallExecutorTest.cpp` outcomes; `SessionMessageJsonTest.cpp` round trip | `✓ durationPresent/isError` per call (parallel); `✓ role, durationMs, isError` shape, `✗` frozen value 4200 (case uses 1234) | A4 |

## Delivered comparison subset (S1–S4)

Five CTest cases, five passed. Each case now reads the named artifact's `expected` object through
`tests/agent/AuditArtifact.hpp` (read-only `read_artifact` / `find_scenario` / `member`); no frozen
value is copied into C++. The helper only locates and returns artifact members.

| Row | Artifact / scenario | Expected fields read | Case(s) | Normal / discriminating observation |
|---|---|---|---|---|
| `InputQueue:peek-clear` | `queue-admission.json` / “peek prefers steering and clearAllQueues removes both queues” | `peekBefore`, `peekAfterSteeringClear`, `hasQueuedMessages`, `peekCount`; inputs `steeringMessages`, `followUpMessage` | `stateful Agent queue peek prefers steering and clear removes both queues` | Peek returns steering, clearing steering leaves follow-up visible, clearing both empties. A clear-everything implementation fails the middle step. |
| `Agent:reset` | `lifecycle.json` / “reset restores the prompt baseline and clears both queues” | `roles`, `systemPrompt`, `hasQueuedMessages`, `isStreaming`; inputs `prompt`, `queuedBeforeReset` | `reset restores the System Message baseline and clears queues while retaining live policy` | Baseline is the system-only replay; queues and transient run state clear while tools/model/thinking stay live. A reset that wipes live policy fails the retained-policy assertions. |
| `Agent:prompt-busy` | `lifecycle.json` / “a second prompt while a run is active is rejected without replacing the run” | `secondPromptError`; inputs `firstPrompt`, `secondPromptWhileActive` | `stateful Agent rejects a second prompt while its active run is suspended` | Rejection carries the frozen text and the original run still completes. Reject-and-cancel fails the completion half. |
| `ToolCallExecutor:run-tool-call` | `tool-execution.json` / “runToolCall runs the hooks and reports execute-only duration” | `ok.durationPresent`, `ok.isError`, `ok.beforeCalls`, `ok.afterCalls`, `ok.executeCalls`, `ok.text`; `blocked.durationPresent`, `blocked.isError`, `blocked.text` | `run_tool_call executes successfully and records execute-only duration`; `run_tool_call returns error outcome for blocked tool call without executing` | Executed call runs both hooks once and reports text; blocked call runs neither hook and has no duration. A blocked path that fabricates a duration or still executes fails. |

**S4 input evidence (required before the blocked-text change).** The frozen capture explicitly
records the blocked reason and the expected text:

- `fixtures/pi-v1.3.0/audit-1047/scenarios.mts:1092` —
  `beforeToolCall: async () => ({ block: true, reason: "no" })`
- `fixtures/pi-v1.3.0/audit-1047/tool-execution.json`, scenario
  “runToolCall runs the hooks and reports execute-only duration”, `expected.blocked.text` = `"no"`.

C++ copies the hook's `reason` into the result (`src/agent/ToolCallExecutor.cpp:394`), so changing
the **test input** from `denied by policy` to `no` aligns the input with the capture; it is not a
production behavior change. The test still asserts `terminate = true`, a C++ field outside the
frozen `blocked` object, and that assertion is retained.

**JUnit:** `docs/research/pi-v1.3.0/deliveries/1176-agent-subset.xml`, 5 tests, 0 failures, 0
skipped, SHA-256 `d1de6b24a953f661a79d6920d4261f923349eaa9903212481bd893d95aea7313`.

## Batch routing (A1–A4)

Every uncovered comparison is assigned below. “Normal” is the scenario's own input path;
“discriminating” is the case the comparison must reject.

### A1 — Agent lifecycle and state (owner `cch_agent_core`)

- **Rows/scenarios:** `Agent:abort` (sequential abort, stream abort idempotence),
  `Agent:default-state` (defaults, thinking-off reasoning model), `Agent:event-lifecycle`,
  `Agent:provider-error`, `Agent:stream-injection`, `Agent:wait-for-idle` (settle order, idle no-op).
- **Public caller:** `agent::Agent` (`prompt`, `subscribe`, `abort`); `AgentSession::wait_for_idle`
  only if the wait-for-idle seam decision below selects the application caller.
- **Normal cases:** drive each artifact's own inputs through the public Agent and assert the
  artifact's `events`, roles, texts and state fields.
- **Discriminating cases:** second `abort` during a streamed turn produces no second terminal;
  provider error settles as `error` (not `aborted`); a stream injection's `done` text is the final
  message text, not the last delta; idle `abort` leaves `signalPresent=false`, no messages.
- **Dependency:** delivered #1108 and audit-1047; no new capture. `Agent:wait-for-idle` has a named
  seam blocker (below) and is not a receipt candidate until it is resolved.

### A2 — Turn hooks (owner `cch_agent_core`)

- **Rows/scenarios:** `Agent:prepare-request-hook`, `Agent:finish-turn-hook`,
  `Agent:prepare-next-turn-hook`.
- **Public caller:** `agent::Agent` with `AsyncAgentOptions::{prepare_request, finish_turn,
  prepare_next_turn}`.
- **Normal cases:** record the `PrepareRequestContext` call (`roles`, `model`, `thinkingLevel`);
  finishTurn end; appended next-turn messages.
- **Discriminating cases:** prepareRequest sees the pending prompt before the provider request (a
  post-provider observer fails); finishTurn end leaves the queued follow-up unclaimed; appended
  message appears before the next `turn_start`/request.
- **Dependency:** #1107; audit-1047. `audit-1107/turn-hooks.json` remains an unregistered candidate
  and is not used for a receipt here.

### A3 — Input admission (owner `cch_agent_core`)

- **Rows/scenarios:** `InputQueue:admission`, `InputQueue:steering-one-at-a-time`,
  `InputQueue:follow-up`, `InputQueue:steering-all`, `Agent:continue`.
- **Public caller:** `agent::Agent` queue methods; `AgentSession` for the admission path.
- **Normal cases:** 300+300 admission; one-at-a-time vs all-mode drains; follow-up after a stopping
  turn; continue from an assistant tail.
- **Discriminating cases:** one-at-a-time delivers exactly one message per turn while all-mode
  delivers the whole queue; continue prefers steering over follow-up; `enqueueReturnsVoid` (C++
  `void` return) is asserted, not inferred.
- **Dependency:** #1113; audit-1047.

### A4 — Tool execution (owner `cch_agent_core`)

- **Rows/scenarios:** `NestedToolCalls:duration`, `NestedToolCalls:parent-ids`,
  `ToolCallExecutor:after-tool-call`, `ToolCallExecutor:argument-mutation`,
  `ToolCallExecutor:duration`, `ToolCallExecutor:outcomes`,
  `ToolCallExecutor:parallel-scheduling`, `ToolCallExecutor:prepare-arguments`,
  `ToolCallExecutor:truncation`, `ToolResult:duration-field` (outcomes and session round trip).
- **Public caller:** `agent::ToolCallExecutor`, `agent::run_tool_call`,
  `agent::NestedToolCallRunner`, and the public session JSON record.
- **Normal cases:** the artifact's own call set and expected event/text/order fields.
- **Discriminating cases:** blocked, unknown and truncated calls have no duration and do not run
  hooks; hook time is excluded from execute duration; parallel end order is completion order while
  result messages stay in source order; nested end duration is execute-only while the record
  duration covers the pipeline.
- **Dependency:** #1106, #1109, #812; audit-1047.

## Partial rows and named blockers

### `Agent:wait-for-idle` — partial, named seam blocker

C++ `cch_agent_core::Agent` has no `waitForIdle` awaitable corresponding to pi's
`Agent.waitForIdle`; the application exposes `cch_coding_agent::AgentSession::wait_for_idle`, and
no test currently exercises the frozen listener/prompt ordering through either seam. This is a
**seam gap**, not a missing capture: the audit-1047 scenarios are sufficient. The row stays
`partial` / `upstream-captured`.

- **Owner:** `cch_agent_core` for an Agent-level seam decision; `cch_coding_agent` for the Session
  caller if the application seam is chosen.
- **Route:** A1, with the seam decision as the first step; no temporary seam is added by this
  ticket and no new capture is made.
- **Named blocker:** “Agent wait-for-idle seam decision” under #1176/A1 (registration boundary
  #1183). The two frozen fields `order` and `isStreamingDuringAgentEndListener` cannot be compared
  until the seam exists.

### `ToolRegistry:active-set` — blocked on a recorded divergence

The frozen artifact expects `resultText = "Tool a not found"`; the C++ case currently observes
`unknown tool: alpha` and pins that text as a tracked divergence (#1110). The other frozen fields
(executable set, declarations, message count, duration absence, events) are covered. A whole-row
receipt would falsely claim the frozen text. The row stays `upstream-captured` until the divergence
is resolved or explicitly accepted by the ledger owner.

- **Owner:** `cch_agent_core` (#1110 decision).
- **Route:** blocked; not A4, not a receipt candidate.

## Validation

All commands ran in this worktree on the default Debug preset.

| Check | Command | Result |
|---|---|---|
| Owning shard build | `cmake --build --preset vcpkg --target cch_tests_agent` | PASS; Parity Architecture Gate PASS |
| Focused subset | `ctest --preset vcpkg -R '<five case names>' --output-junit …/1176-agent-subset.xml` | **5 selected, 5 passed**, 0 failed/skipped |
| Owner suite | `ctest --preset vcpkg -L '^agent$'` | **241 selected, 241 passed** (1 standing realtime-clock skip) |
| Architecture | `ctest --preset vcpkg -L '^architecture$' -j4` | **36 selected, 36 passed** |
| Consolidation | `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS; 1464 rows, 107 delivered rows, 658 scenarios, 0 uncovered |
| Consolidation tests | `python3 tests/evidence/ConsolidationAuditTest.py` | 42 passed |
| Evidence verify | `python3 scripts/evidence/audit_evidence.py verify` | 57 bundles, 118 artifacts, 661 scenarios verified at `71023aec…` |
| Formatting | `scripts/format-check.sh 7735dbbfe983d6748436b71713849d061606ca7b` | clean |

No `baselines.json`, `consolidation-plan.json`, or `1058-deliveries.json` edit was made. The JUnit
artifact and this report are the handoff for serial registration; the receipt itself remains PM
work.

## Capability status

- **Four rows are promoted** through the registered receipt `1176-agent-comparisons`
  (`InputQueue:peek-clear`, `Agent:reset`, `Agent:prompt-busy`, `ToolCallExecutor:run-tool-call`),
  each with the exact artifact fields named in the subset table above.
- **A1–A4 remain `upstream-captured`** with the named gaps above.
- **`Agent:wait-for-idle` remains `partial`** on the seam blocker.
- **`ToolRegistry:active-set` remains `upstream-captured`** on the recorded text divergence.
- Membership, implementation and verification stay separate; this ticket adds no production
  behavior and no new capture.

## Remaining boundary

The 29-row inventory is not a completion percentage. The next bounded work is A1–A4 in the order
above; the two blockers need owner decisions before any receipt. #1044 remains open, and #1183 owns
the consolidated registration index.
