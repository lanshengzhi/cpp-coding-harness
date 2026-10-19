# #1047 Agent execution, tool timing and input admission audit

**Status:** audit complete. Frozen behavior is captured as `upstream-captured`; this ticket did
not change production code and did not run a C++ parity suite. No row is marked `verified`.

- **Baseline:** `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
- **Capture host:** Linux x86-64, glibc 2.44, Node v26.12.0, tsx 4.22.1
- **Private checkout:** `/tmp/cch-1047-pi-frozen` (depth-1 clone of the tag; clean tree; `HEAD`
  verified against the baseline before every capture)
- **Integration base:** `bdf16856e9a279e62de7ee47dfc715f67b5f8737` (`integration/1044-pi-v1.3.0`,
  including #1045 authority and #1046 evidence infrastructure)
- **Ticket branch:** `ticket/1047-audit`
- **Machine ledger:** [1047-capability-ledger.json](1047-capability-ledger.json) — 32 capability
  rows, 5 exclusions, 12 gaps, consumer chains and the exact artifact digests.
- **Evidence family:** `fixtures/pi-v1.3.0/audit-1047/` — four artifacts, 30 scenarios.

The report answers the A03 acceptance criteria; the JSON ledger is the authoritative inventory.

## Evidence bundle

| Artifact | Scenarios | SHA-256 |
|---|---:|---|
| `lifecycle.json` | 11 | `2aef0aa9b2d85e7267e3ab1d0b9a865e47a61aa0b71b801374cb4a8fe974eebd` |
| `tool-execution.json` | 10 | `fbc55adb5947c3e722fdf6902ec0e6e057a3947f2d154982f3466dbacb080a35` |
| `queue-admission.json` | 6 | `48039a377d500663012497882bf5fbdca384d6fcc60ec74c1f6075982693f609` |
| `cancellation.json` | 3 | `45d932a1915958d0aa27365f0bdffa1a3c6c5f4e1de78d5f2be41b385e08c37e` |

`manifest.json` SHA-256: `0d146159a27ceaf1e657cd22997c1ebdc972818215054a860d88226e7d58d59a`.
Capture generator SHA-256: `738de5b2947b1913c086b3592d7ab422ab7863477d8a56046cc5ee167ef3e791`.

Exact capture command (already run from the repository root; the runner also re-execs itself
under the frozen checkout's `tsx` for workspace path resolution):

```bash
PI_CHECKOUT=/tmp/cch-1047-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1047-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1047/capture-agent.mts
# captured audit-1047 (4 artifacts, 30 scenarios) for pi-v1.3.0 (71023aec…)
```

`python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1047/manifest.json`
accepted the bundle and returned the registry record quoted in the handoff. Serial registration
is intentionally left to the parent/serial merger; this lane did not edit `baselines.json`.

## What was run

Every observation is a real call against public frozen entries, not a symbol check:

- `Agent` from `packages/agent/src/index.ts` with scripted `streamFn` and real public lifecycle
  subscriptions, `prompt`/`continue`/`abort`/`waitForIdle`/queue methods, and real tools whose
  `execute` is the observed activity.
- The public `runToolCall` export with its real hooks.
- The coding-agent `NestedToolCallRunner` path and `SessionManager` public session record.
- A temporary local session directory only for the session round trip; no provider or catalog
  access.
- Scenario assertions run inside the capture. Artifacts and `manifest.json` are written only
  after all guards and assertions succeed.

Dummy credentials/scripted inference, local-only resources and declared normals follow the #1046
convention. Timing scenarios keep raw milliseconds in `observed` but make `expected` assert
presence, absence and threshold relations rather than a volatile number.

## 1. Lifecycle, stream injection, defaults and loop hooks

### Ordered lifecycle

For a tool turn followed by a final response, the frozen public event order is:

```text
agent_start
turn_start
message_start:user
message_end:user
message_start:assistant
message_end:assistant:toolUse
tool_execution_start:echo-1
tool_execution_end:echo-1:duration=present:error=false
message_start:toolResult
message_end:toolResult:echo-1:duration=present
turn_end:assistant:tools=1
turn_start
message_start:assistant
message_end:assistant:stop
turn_end:assistant:tools=0
agent_end:messages=4
```

The final transcript roles are `system, user, assistant, toolResult, assistant`. Tool start/end
remain paired for executed, blocked, unknown and length-truncated calls. `agent_end.messages`
counts only messages produced by the invocation, not the prior transcript.

### Stream injection

A scripted stream emitting `start, text_start, text_delta, text_end, done` produces one
`message_start:assistant`, three `message_update` events with the matching assistant sub-event
types, `message_end:assistant:stop`, one turn end and `agent_end:messages=2`; the final text is
the `done` message's text. This is ordinary `streamFn` injection.

**Explicit exclusion:** the separate `streamProxy` protocol is not captured here. It is an AI
transport protocol and belongs to the AI audit; ordinary stream injection into `Agent` is in
scope and is covered above.

### Defaults

The frozen defaults observed with no initial state are:

| Setting | Frozen value |
|---|---|
| `thinkingLevel` | `off` |
| `model.id` | `unknown` |
| `tools` / `messages` | empty |
| `steeringMode` | `one-at-a-time` |
| `followUpMode` | `one-at-a-time` |
| `toolExecution` | `parallel` |
| `transport` | `auto` |
| `isStreaming` | `false` |

The default remains `thinkingLevel: off` even when the initial model is reasoning-capable. This
is not a model-clamping side effect: it is the Agent default.

### `agent_end` settlement

The observed order is:

```text
agent_end:listener_start
agent_end:listener_end
waitForIdle:resolved
prompt:resolved
```

`agent.state.isStreaming` is still `true` while the `agent_end` listener runs and becomes `false`
by the time `waitForIdle` resolves. A later C++ test must assert this order, not only eventual
idle.

### `reset`, busy prompt and error terminal

- `reset()` restores the initial system message (`roles = ["system"]`, `systemPrompt = "sys"`),
  clears both queues (`hasQueuedMessages = false`) and leaves the Agent idle.
- A second `prompt()` while a run is active is rejected; the active run remains active. Frozen
  text is `Agent is already processing a prompt. Use steer() or followUp() to queue messages, or
  wait for completion.`
- A provider `error` terminal emits `message_start:assistant`, `message_end:assistant:error`,
  `turn_end`, `agent_end:messages=2`; `state.errorMessage` is the provider's error text. It does
  not turn into an aborted turn or a thrown run failure.

### Loop hooks

- `prepareRequest` runs before the provider request. It sees the pending Agent messages
  (`roles = ["user"]` for a first prompt) and the effective model/thinking (`unknown`, `off`); the
  system prompt is carried separately from `context.messages`. Its return can replace request
  state.
- `prepareNextTurn` runs exactly once after a tool turn. A `{ messages: [user("prepared")] }`
  return is emitted as a new user message and appears in the next request as
  `system, user, assistant, toolResult, user`.
- `finishTurn` returning `{ action: "end" }` stops before another request: one request, one
  `finishTurn` call, queued follow-up remains queued. `end` does not poll the follow-up queue.

## 2. Tool preparation, execution, duration and outcomes

### The timing contract

A public tool with a 60 ms `beforeToolCall` hook, a 10 ms `execute`, and a 50 ms `afterToolCall`
hook produced:

```json
{
  "durationMs": 10,
  "wallClockMs": 121,
  "hooks": {"before": 1, "after": 1}
}
```

Therefore:

- `durationMs` is measured with `performance.now()` (a monotonic clock) around **`execute` only**.
- `beforeToolCall`, `prepareArguments`, validation and `afterToolCall` are excluded.
- The same field is on `tool_execution_end` and on the executed tool-result message.
- It is **present** for a successful execute and for an execute that throws (the execute duration
  still exists).
- It is **absent** for a call blocked by `beforeToolCall`, an unknown tool, and a call from a
  length-truncated assistant message. No zero-duration sentinel is written.
- `runToolCall` returns `{ ok, result, durationMs, startedAt }` for an executed call and a
  blocked outcome with **no `durationMs` key at all**.
- Nested `tool_execution_end.durationMs` equals the nested execute duration, while the nested
  record's duration includes the complete nested pipeline (hooks and preparation). The two
  clocks are distinct.

The exact frozen execute-only measurement is in `packages/agent/src/agent-loop.ts`
`executePreparedToolCall`; the final message carries the value through
`packages/ai/src/utils/event-stream.ts`.

**How a later C++ test distinguishes the clocks:** force a long hook (for example >100 ms) and a
short execute (for example 10 ms), then assert the tool-end/tool-result duration is near the
execute delay and strictly less than the start-to-end wall span. A test that only checks a
positive duration cannot distinguish the frozen contract from the existing TUI wall-clock timer.

### Outcome classification and exact text

One assistant message with four calls captured:

| Call | Executed | `durationMs` | Result text |
|---|---:|---|---|
| `ok` | 1 | present | `a` |
| `throws` | 1 | present | thrown diagnostic (isError) |
| `blocked` (`reason: "denied"`) | 0 | absent | `denied` |
| `missing` (unknown tool) | 0 | absent | `Tool missing not found` |

Event order is start/end per call, then tool-result messages, with all four result messages after
the four end events in the captured lifecycle. A length-truncated assistant message emits start
and end for the call with `duration=absent:error=true` and this exact result text:

```text
Tool call "echo" was not executed: the response hit the output token limit, so its arguments may be truncated. Re-issue the tool call with complete arguments.
```

### Preparation order and mutation

`prepareArguments` runs before validation. A tool that normalized `"before"` to `"beforeafter"`
and a `beforeToolCall` that changed the string `"123"` to the number `123` both executed; the
mutated number was not revalidated. The observed text was `123`, the value type was `number`.
The combined order is: `prepareArguments` → schema validation → `beforeToolCall` → execute.

### Parallel scheduling and source order

With two tools whose `executionMode` is omitted, both start before either end
(`maxConcurrent = 2`). Completion order was `fast, slow`; tool-result messages were
`slow, fast` (assistant source order). `tool_execution_end` follows completion order. This also
proves that an omitted per-tool execution mode is parallel, not sequential.

### `afterToolCall` replacement and nested calls

Content-only `afterToolCall` replacement produced `content = "replaced"`, no
`structuredContent`, `details = {}`, and retained `durationMs = present`. The audit records this
as the frozen public shape; the C++ message type models `details` rather than
`structuredContent`.

A nested call emits `tool_execution_start` and `tool_execution_end` with
`parentToolCallId = "caller"` for both, the nested outcome is recorded under `nestedCalls`, and
the record duration is at least the end-event execute duration.

### Registry behavior

`state.tools` is the executable set. Replacing it with `[b]` (and then mutating the replacement
array) leaves `state.tools = ["b"]`; the next request receives a system message declaring
`toolsAdded: [a, b]` followed by a delta system message `toolsRemoved: [a]`. A model-issued call
to the removed `a` never executes and yields `Tool a not found` with no `durationMs`. The final
transcript has six messages (two system declarations, user, assistant toolUse, toolResult,
assistant stop).

This is both a prompt-declaration and an execution-boundary property: hiding a tool from the
prompt is not sufficient.

## 3. Input admission, steering and follow-up

### Admission

Frozen queues accepted 300 steering plus 300 follow-up messages without rejection; both
`steer()` and `followUp()` return `void`. The default modes are `one-at-a-time` for both. The
frozen behavior has **no capacity result**. The C++ defaults (256 messages / 16 MiB, enqueue
returning capacity errors) must be removed under the ADR 0022 amendment; this is a migration
obligation, not a permanent exemption.

### Steering

- `one-at-a-time`: two queued messages after a tool turn produce three requests with request
  roles:
  `[system,user]`, `[system,user,assistant,toolResult,user]`,
  `[system,user,assistant,toolResult,user,assistant,user]`; the last users are
  `go, steer1, steer2`; `agent_end.messages = 7`; both consumed.
- `all`: both queued messages are injected in FIFO order in the next turn; request roles end with
  `... user,user`; two requests total; `agent_end.messages = 6`.
- Follow-up: one queued follow-up runs only after an otherwise stopping assistant turn, not
  before the tool result; request roles are `[user]`, then `[user,assistant,user]`.
- Peek prefers steering (`steer1`), then follow-up (`follow`); `clearAllQueues` removes both.
- `continue()` from an assistant tail drains steering first and follow-up second; request roles
  `[assistant,user]`, then `[assistant,user,assistant,user]`.

## 4. Cancellation

- Abort during sequential execution: the first call's `execute` observes the signal and returns;
  the second call never starts. The aborted run still makes a second provider completion request
  with `signal.aborted = true`, producing `message_end:assistant:aborted` and
  `errorMessage = "Request was aborted"`. Lifecycle settles with four new messages and
  `isStreaming = false`.
- Abort while streaming: two `abort()` calls are idempotent; one aborted assistant terminal is
  produced. `agent.signal` is undefined after settlement; `signalWasAborted` is true for listeners
  during the run.
- Abort and `waitForIdle()` while idle are no-ops: no signal, no messages, no streaming state.

## 5. C++ consumer chain and implementation status

The authoritative per-row mapping is in the [ledger](1047-capability-ledger.json). The important
chains are:

| Frozen surface | Current C++ producer | Handoff/consumer | Status |
|---|---|---|---|
| Agent lifecycle | `src/agent/AgentRun.cpp` (`run_turns`, `emit_agent_event`) | `AgentSession::Impl::apply_event` → `ProjectionStream` patches | implemented; tool end has no duration |
| Tool start/end timing | `src/agent/ToolCallExecutor.cpp` (`execute_single_call`) | `ToolExecutionEndEvent` → `AgentSession` projection → TUI | missing `durationMs` |
| Tool-result message | `src/ai/include/cch/ai/Message.hpp` (`ToolResultMessage`) | `AgentSession`/`ChatContainer` | missing `durationMs` |
| Session record | `src/agent/harness/session/SessionMessageJson.hpp` (`MessageDto`) | `EntrySerializer` → `SessionJournal`/JSONL/SQLite | missing `durationMs` |
| Visible timer | `src/coding_agent/tui/ToolExecutionComponent.cpp` | `ToolRenderContext.started_at_ms/ended_at_ms` → `BashRenderer` "Took" | wall clock, not monotonic execute-only |
| Active tools | `Agent::set_active_tools`/`snapshot` | `McpSessionManager` → `Agent` declaration | declaration-only; executor still resolves the full registry |
| Input admission | `Agent::steer`/`follow_up` → `AgentSession::steer`/`follow_up` | capacity error at both layers | capped/rejecting, needs removal |
| Nested calls | `src/agent/NestedToolCalls.cpp` | `NestedToolCallRunner` record | record duration exists; end-event duration does not |
| Concrete tools | `src/agent/ToolRegistry.cpp` (read/bash/edit/write) | application harness | ownership gap for #1052, not moved here |

`AgentSession` has the frozen-facing `prompt`, `abort`, `steer`, `follow_up`, `wait_for_idle`
and projection seams. `ChatContainer` consumes `ToolExecutionEndEvent` at
`src/coding_agent/tui/ChatContainer.cpp`; `EntrySerializer` is the session DTO consumer.

## 6. Gaps and implementation-slice recommendations

The ledger has the full gap list with evidence. The blockers for the implementation lane are:

1. **Tool duration (blocker).** Add an optional duration to `ToolExecutionEndEvent` and
   `ToolResultMessage`, measured monotonically around `execute` only. Keep it absent for
   blocked/unknown/truncated outcomes. Do not compute it from event receipt time.
2. **Session round trip (blocker).** Add optional `durationMs` to the tool-result DTO and both
   serialize/deserialize paths, so a 4200-style value survives append/read. Do not synthesize a
   value for absent calls.
3. **TUI clock (high).** Make the visible "Took" value consume the frozen duration (or prove it is
   intentionally not the contract), and add the long-hook/short-execute test described above.
4. **`prepareArguments` (blocker).** Add the hook before validation and preserve no-revalidation
   for `beforeToolCall` mutations.
5. **Default concurrency (blocker).** `Tool::concurrency` currently defaults to `Exclusive`
   despite its own comment and the frozen omitted-mode parallel behavior. Default to
   `ParallelSafe`, keep explicit sequential overrides, and test mixed batches.
6. **Admission (blocker).** Remove the 256-message/16-MiB caps and the capacity result from the
   Agent and `AgentSession` facade. Update callers/tests that assert rejection.
7. **Active-tool execution (high).** Resolve model-issued and nested calls against the active
   executable set; a removed name must return `Tool <name> not found` without executing, and the
   transcript must declare the tool delta.
8. **Loop hooks (medium).** Add `prepareRequest`, `finishTurn` continue/end, and message
   replacement for `prepareNextTurn`.
9. **`reset` and thinking default (medium/low).** Add `reset`; change the default thinking level
   to `off` before clamping.
10. **Diagnostics and public `runToolCall` (medium).** Align unknown-tool/block/busy text and
    decide the C++ public outcome seam carrying `durationMs`.
11. **Package ownership (info).** Concrete tools remain under `cch_agent_core`; record the target
    `cch_coding_agent` ownership and let #1052 decide. This audit does not move code.

### Passive AI fields and the boundary

The non-AI timing requirement is carried by `tool-execution`/`tool-result` messages and the
session DTO, not by the assistant message. `ToolResultMessage.usage` is an AI-owned field and is
not part of this audit's timing claim. Assistant `message_update`/thinking timing, provider retry,
transport selection, `thinkingBudgets`, `onPayload`/`onResponse`/`onProviderStreamEvent` and the
`streamProxy` protocol are excluded (rows in the ledger's `excluded` list). An AI-owned message
shape cannot remove the non-AI `durationMs` requirement.

## 7. Validation performed

- `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1047/manifest.json`
  → accepted; registry record digest quoted in the handoff.
- Capture run → `captured audit-1047 (4 artifacts, 30 scenarios) for pi-v1.3.0 (71023aec…)`.
- `git -C /tmp/cch-1047-pi-frozen rev-parse HEAD` → `71023aec1546d6d677a65669c5665c0927ce0dd0`;
  clean status checked by the capture runner.
- `python3 -m json.tool docs/research/pi-v1.3.0/1047-capability-ledger.json` → valid JSON.
- No C++ build/tests were run: this is an audit lane and the ledger rows are intentionally
  `upstream-captured`/`unverified`, not `verified`.

## 8. Unresolved factual blockers and handoff

- Serial registry registration is pending the parent/serial merger; the lane did not edit
  `fixtures/pi-v1.3.0/baselines.json`.
- The C++ public duration type/owner seam is not decided: the frozen value fits an optional
  non-negative integer millisecond field on the end event and result message, but the exact C++
  type and the public `runToolCall` outcome shape are implementation decisions.
- The active-tool execution fix may affect the MCP surface and nested-call policy; the ledger
  records both consumers but does not choose a policy beyond the frozen executable-set rule.
- Concrete filesystem/shell package relocation belongs to #1052; no code was moved.
- Integration was merged before the audit; the final branch commit and exact handoff paths are in
  `/tmp/cch-1044-context/1047-handoff.json`.
