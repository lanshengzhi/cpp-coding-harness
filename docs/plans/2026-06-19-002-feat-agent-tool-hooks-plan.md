---
title: "feat: Add pre/post tool-call hooks and graceful blocking semantics"
type: "feat"
status: "completed"
date: "2026-06-19"
origin: "docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---


# feat: Add pre/post tool-call hooks and graceful blocking semantics

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.
**Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T3 checklist).

## Summary

Add `beforeToolCall` and `afterToolCall` hooks to the async agent loop, matching the pi `AgentLoopConfig` contract. The `before` hook can block a tool call and emit an error tool result; the `after` hook can override content, details, error flag, and the terminate hint; thrown hook errors become stable agent errors. The `AsyncToolExecutionResult` content shape is aligned with pi's `AgentToolResult.content` array so overrides are type-consistent. All changes preserve the existing move-only callback and passive-value architecture.

---

## Problem Frame

The pre-implementation cleanup established package-style build targets, observable agent state, and expanded lifecycle events, but the agent loop still executes every tool call unconditionally and forwards every result unchanged. The parity roadmap's T3 checklist identifies pre/post hooks as the next agent-loop seam. Without these hooks, higher-level runtime features (permission prompts, result filtering, early termination from tool results, and future steering) have no clean injection point.

---

## Requirements

### Type and contract changes

- R1. `AsyncToolExecutionResult` carries its content as `std::vector<ai::Content>` (was `std::string`) and exposes a `terminate` hint, aligning with pi's `AgentToolResult` contract.
- R2. Passive hook context/value types are added: `BeforeToolCallContext`, `BeforeToolCallResult`, `AfterToolCallContext`, `AfterToolCallResult`. They use aggregate-friendly structs and `util::JsonValue` for arguments.
- R3. `AsyncAgentOptions` gains optional `before_tool_call` and `after_tool_call` move-only hooks.

### Before-hook blocking

- R4. `beforeToolCall` runs after argument validation and before execution. If it returns `block = true`, the loop emits an error tool result with the provided reason and skips both execution and the `afterToolCall` hook.

### After-hook overrides and terminate

- R5. `afterToolCall` runs only for calls that were actually executed, and before `ToolExecutionEndEvent` / tool-result message emission. Omitted override fields keep the original executed values; provided fields replace the original values with no deep merge.
- R6. The terminate hint is true only when every finalized tool call in the assistant's batch sets it to true. Blocked calls (from the before hook) and calls whose tool execution failed are treated as `terminate = false`. When the batch terminates, the loop stops after the current turn with `AgentEndEvent{true, ...}` and `AsyncAgentRunResult.stop_reason = AssistantStopReason::ToolUse`.

### Error handling and architecture

- R7. Hook exceptions and `util::Error` failures are converted to a stable `ErrorCode::Tool` agent error, emit `AgentEndEvent{false, ...}`, and return `std::unexpected` from the run.
- R8. Public headers remain free of provider DTOs, Glaze machinery, and `src` includes. Architecture tests (`[architecture]`) continue to pass.

---

## Scope Boundaries

- This plan covers only the pre/post tool-call hook seam and the terminate batch hint.
- Parallel versus sequential tool execution, per-tool execution mode, and read-only concurrency remain deferred to the separate T3 slice "Evaluate sequential versus parallel tool execution with deterministic result insertion."
- `convertToLlm`, `transformContext`, `getSteeringMessages`, `getFollowUpMessages`, `prepareNextTurn`, and `shouldStopAfterTurn` are out of scope.
- `AbortSignal`/cancellation-token integration is out of scope; the current C++ loop has no cancellation token contract.
- Tool progress/update callbacks are out of scope.
- No change to CLI flags, session JSONL shape, workspace containment, secret redaction, or provider wire format.

### Deferred to Follow-Up Work

- Per-tool execution mode override (`AsyncAgentTool::executionMode`) and parallel scheduling: separate T3 slice.
- Steering/follow-up message queues, context transforms, and `shouldStopAfterTurn`: separate T3 slices once session entry support exists.
- Async hook signatures: if future hooks need `boost::asio::awaitable`, add a second overload then; keep the initial contract synchronous and move-only to match `AgentEventSink`.

---

## Context & Research

### Relevant Code and Patterns

- **Tool execution contract** (`include/cch/agent/AgentTool.hpp`): `ToolInvocation`, `AsyncToolExecutionResult` (currently `content: std::string`, `details`, `is_error`), and `AsyncAgentTool` interface.
- **Agent loop** (`include/cch/agent/AgentLoop.hpp`, `src/agent/AgentLoop.cpp`): `AsyncAgentLoop::continue_with` executes tool calls sequentially, builds `ai::ToolResultMessage`, and emits `ToolExecutionStartEvent` / `ToolExecutionEndEvent`.
- **Options and state** (`include/cch/agent/AgentContext.hpp`): `AsyncAgentOptions` currently holds `max_turns` and `model`; `AgentState` tracks active tools, pending tool calls, messages, and streaming message.
- **Events** (`include/cch/agent/AgentEvent.hpp`): `AgentLifecycleEvent` variant and move-only `AgentEventSink` (`std::move_only_function`).
- **AI context** (`include/cch/ai/Context.hpp`): `AiContext` with `system_prompt`, `model`, `messages`, `tools` — used as the hook context snapshot.
- **Content types** (`include/cch/ai/Content.hpp`): `Content = variant<TextContent, ThinkingContent, ImageContent>`, plus `ToolCallContent` for assistant content.
- **Tool registry** (`include/cch/agent/ToolRegistry.hpp`): owns `std::unique_ptr<AsyncAgentTool>` instances and returns raw pointers for lookup.
- **Built-in tools** (`src/tools/AsyncToolFactories.cpp`): four tools return `AsyncToolExecutionResult{std::string, ...}` today.
- **Tests** (`tests/agent/AsyncAgentLoopTest.cpp`, `tests/tools/AsyncToolsTest.cpp`): construct fake tools and inspect lifecycle events / request messages.
- **Error handling** (`include/cch/util/Error.hpp`): `util::Error` with `ErrorCode`, `message`, `detail`, `context`; `util::Expected` aliases.

### Reference Contracts (pi)

- **`pi:packages/agent/src/types.ts`**: Defines `BeforeToolCallContext`, `BeforeToolCallResult`, `AfterToolCallContext`, `AfterToolCallResult`, `AgentToolResult`, `AgentLoopConfig.beforeToolCall`, `AgentLoopConfig.afterToolCall`, and the `terminate` batch semantics.
- **`pi:packages/agent/src/agent-loop.ts`**: Implements `prepareToolCall`, `finalizeExecutedToolCall`, and `shouldTerminateToolBatch`.

### Institutional Learnings

- **Move-only callbacks are mandatory** (`tests/architecture/MoveOnlyCallbackTest.cpp`): hooks must use `std::move_only_function` so subscribers can capture unique state.
- **Passive value contracts**: hook contexts and results must be aggregate-friendly structs with no behavior, consistent with the existing AI/agent public headers.
- **No `src` or Glaze leakage into public headers** (`tests/architecture/PublicHeaderBoundaryTest.cpp`): hook types live in `include/cch/agent/`, arguments stay `util::JsonValue`, and no provider DTOs are involved.
- **`docs/solutions/` does not exist** — no formal institutional knowledge base to consult.

---

## Key Technical Decisions

- **Hooks live in `AsyncAgentOptions` (per-loop config)**, matching pi's `AgentLoopConfig.beforeToolCall` / `afterToolCall`. Per-tool hooks are deferred until a concrete consumer appears.
- **Hook signatures are synchronous move-only functions**, returning `util::Expected<T>`. This matches the existing `AgentEventSink` pattern and avoids introducing async hook overloads before a consumer needs them.
- **`AsyncToolExecutionResult::content` becomes `std::vector<ai::Content>`**, aligning with pi's `AgentToolResult.content` array. The four built-in tools are updated to return `vector<Content>` (wrapping their previous string in `TextContent`). This makes the `afterToolCall` content override type-consistent and removes an existing contract mismatch.
- **`terminate` is tracked internally, not persisted to `ToolResultMessage`**. Pi stores the hint on the tool result, not the session message. The C++ loop tracks a `terminate_batch` flag that is true only if every finalized call in the batch has `terminate = true`.
- **Hook failures are `ErrorCode::Tool` agent errors**. A thrown exception or returned `std::unexpected` from a hook aborts the run, emits `AgentEndEvent{false, ...}`, and returns `std::unexpected`. This prevents a misbehaving hook from hanging the loop or producing undefined state.
- **after-hook runs only for executed calls**, not for blocked calls or calls whose tool execution returned an error. This matches pi's `finalizeExecutedToolCall` scope.
- **after-hook overrides are field-by-field replacements**, not deep merges. This matches pi's `AfterToolCallResult` merge semantics exactly.

---

## Open Questions

### Resolved During Planning

- **Should hooks be async?** → No. Use synchronous `std::move_only_function` returning `util::Expected<T>`. Async hooks can be added later as a non-breaking extension if needed.
- **Should `terminate` be stored in `ToolResultMessage`?** → No. Track it in loop state only, matching pi's semantics.
- **Should `AsyncToolExecutionResult::content` change from `std::string` to `vector<Content>`?** → Yes. It aligns with pi and makes after-hook overrides consistent.
- **Does `afterToolCall` run on blocked calls?** → No. Blocked calls (before hook) and calls whose execution returned an error skip the after hook.
- **What is the event order for blocked calls?** → `ToolExecutionStartEvent` is emitted first, then the before hook runs, then `ToolExecutionEndEvent` is emitted with the synthetic error result.
- **What `AgentEndEvent` reason and `stop_reason` are used when terminate stops the turn?** → `AgentEndEvent{true, ai::stop_reason_to_string(ai::AssistantStopReason::ToolUse)}` and `AsyncAgentRunResult.stop_reason = ai::AssistantStopReason::ToolUse`.

### Deferred to Implementation

- Exact spacing of `TurnEndEvent` relative to the last `ToolExecutionEndEvent` when terminate stops the turn: finalize during implementation based on existing event ordering tests.
- Whether to expose the hook reason string in `ToolExecutionEndEvent` for blocked calls: decide based on existing CLI printer needs.
- Whether to add an architecture test specifically asserting that `AsyncAgentOptions` hooks are move-only: add if the existing `MoveOnlyCallbackTest` pattern does not already cover it.

---

## Implementation Units

### U1. Align `AsyncToolExecutionResult` and define hook contracts

**Goal:** Refactor the tool result value type to match pi's `AgentToolResult` and add the passive hook context/result types to the public agent header.

**Requirements:** R1, R2, R3, R8.

**Dependencies:** None.

**Files:**

- Modify: `include/cch/agent/AgentTool.hpp`
- Modify: `include/cch/agent/AgentContext.hpp`
- Modify: `src/tools/AsyncToolFactories.cpp`
- Modify: `tests/tools/AsyncToolsTest.cpp`
- Modify: `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Change `AsyncToolExecutionResult::content` from `std::string` to `std::vector<ai::Content>`.
- Add `bool terminate{false}` to `AsyncToolExecutionResult`.
- Update built-in tools in `src/tools/AsyncToolFactories.cpp` to wrap their previous string output in `ai::TextContent`.
- Update `tests/tools/AsyncToolsTest.cpp` and `tests/agent/AsyncAgentLoopTest.cpp` fake tools to return `vector<Content>`.
- Define `BeforeToolCallContext`, `BeforeToolCallResult`, `AfterToolCallContext`, `AfterToolCallResult` in `AgentTool.hpp`.
- Add optional hook fields to `AsyncAgentOptions` using `std::move_only_function` returning `util::Expected<...>`. Because `std::move_only_function` is not copyable, `AsyncAgentOptions` becomes move-only; verify that all existing call sites (`src/AsyncCliRuntime.cpp`, tests) construct or move it rather than copying.

**Patterns to follow:**

- Existing `std::move_only_function` usage in `AgentEventSink`.
- Existing aggregate-friendly struct style in `ToolInvocation` and `AsyncToolExecutionResult`.

**Test scenarios:**

- Happy path: `AsyncToolExecutionResult` is default-constructible, move-constructible, and aggregate-friendly; `terminate` defaults to false.
- Happy path: fake tool returns `vector<ai::Content>` with a single `TextContent`; agent loop builds the same `ToolResultMessage` as before.
- Edge case: built-in `read_file` tool still produces a text result after wrapping its output in `TextContent`.
- Edge case: `AsyncAgentOptions` is move-only but can still be default-constructed and passed by value to `AsyncAgentLoop`.
- Architecture: `AgentTool.hpp` and `AgentContext.hpp` compile without including any `src/` or Glaze headers.

**Verification:**

- `./build/cpp_harness_tests "[tools][async]"` passes.
- `./build/cpp_harness_tests "[agent][async]"` passes.
- `./build/cpp_harness_tests "[architecture]"` passes.

---

### U2. Integrate `beforeToolCall` blocking hook

**Goal:** Run the optional before hook before each tool execution and honor a `block` result by emitting an error tool result instead of executing the tool.

**Requirements:** R4, R7.

**Dependencies:** U1.

**Files:**

- Modify: `src/agent/AgentLoop.cpp`
- Modify: `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Emit `ToolExecutionStartEvent` for the call before invoking any hook, so CLI/event consumers always see a start/end pair for every tool call requested by the assistant.
- After argument validation and tool lookup, if `options_.before_tool_call` is set, call it with a `BeforeToolCallContext` containing the assistant message, tool call block, parsed arguments, and current `ai::AiContext` snapshot.
- If the hook returns `block = true`, build an error `ToolResultMessage` using `reason` (or a default blocked message) and skip both `tool->execute` and the `afterToolCall` hook.
- If the hook returns an error, abort the run with `ErrorCode::Tool` and emit `AgentEndEvent{false, ...}`.
- If the hook throws, catch and convert to `ErrorCode::Tool` with the exception message.
- `ToolExecutionEndEvent` is emitted for blocked calls using the synthetic error result.

**Patterns to follow:**

- Existing `error_tool_result` helper in `AgentLoop.cpp`.
- Existing `CCH_TRY_VOID` macro usage for error propagation.

**Test scenarios:**

- Happy path: before hook returns `block = true` for `read_file`; the tool is not executed, the final context contains an error `ToolResultMessage`, and `ToolExecutionEndEvent` shows `is_error = true`.
- Happy path: before hook returns `block = true` with a custom reason; the error tool result contains the custom reason.
- Happy path: before hook returns no-block; the tool executes normally.
- Error path: before hook returns `std::unexpected`; the run fails with `ErrorCode::Tool` and emits `AgentEndEvent{false, ...}`.
- Error path: before hook throws `std::runtime_error`; the run fails with `ErrorCode::Tool` and a stable message.

**Verification:**

- `./build/cpp_harness_tests "[agent][async]"` passes with new before-hook tests.
- `./build/cpp_harness --fake --session /tmp/u2.jsonl "read README.md"` still works when no hook is configured.

---

### U3. Integrate `afterToolCall` override hook and terminate batch

**Goal:** Run the optional after hook after each tool execution, apply field-by-field overrides, and stop the run after the turn when every finalized tool call in the batch requests terminate.

**Requirements:** R5, R6, R7.

**Dependencies:** U1, U2.

**Files:**

- Modify: `src/agent/AgentLoop.cpp`
- Modify: `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- After `tool->execute` returns successfully, if `options_.after_tool_call` is set, call it with an `AfterToolCallContext` containing the assistant message, tool call, args, executed result, current error flag, and context snapshot. Blocked calls and calls whose execution produced an error `AsyncToolExecutionResult` skip the after hook.
- Apply overrides: `content` replaces the full content vector; `details` replaces details; `is_error` replaces the error flag; `terminate` replaces the terminate hint.
- Build the final `ToolResultMessage` from the possibly-overridden values.
- Track a loop-local `terminate_batch` flag: initialize true, then `terminate_batch = terminate_batch && result.terminate` for each finalized call. Blocked calls and failed executions force `terminate = false` for that call.
- After all tool calls finish, if `terminate_batch` is true and at least one executed call existed, emit `TurnEndEvent`, emit `AgentEndEvent{true, ai::stop_reason_to_string(ai::AssistantStopReason::ToolUse)}`, set `AsyncAgentRunResult.stop_reason = ai::AssistantStopReason::ToolUse`, and return the run result instead of starting another turn.
- Hook errors/exceptions convert to `ErrorCode::Tool` agent errors, same as before-hook failures.

**Patterns to follow:**

- Existing tool-result construction path in `AgentLoop.cpp`.
- pi's `shouldTerminateToolBatch` semantics (all must agree).

**Test scenarios:**

- Happy path: after hook overrides `content` with a new `TextContent` vector; the `ToolResultMessage` and `ToolExecutionEndEvent` reflect the override.
- Happy path: after hook overrides `is_error` from false to true; the tool result is marked as error.
- Happy path: after hook sets `terminate = true` for the only tool call in the batch; the loop stops after the turn with `AgentEndEvent{true, ...}` and `AsyncAgentRunResult.stop_reason == ai::AssistantStopReason::ToolUse`.
- Happy path: after hook sets `terminate = true` for one of two calls but the other keeps `terminate = false`; the loop continues to the next turn.
- Edge case: blocked calls (before hook) skip the after hook and force `terminate = false`, preventing batch termination even if other executed calls request it.
- Edge case: a tool execution that returns `is_error = true` skips the after hook and forces `terminate = false`.
- Error path: after hook returns an error; the run aborts with `ErrorCode::Tool`.
- Error path: after hook throws; the run aborts with `ErrorCode::Tool`.

**Verification:**

- `./build/cpp_harness_tests "[agent][async]"` passes with new after-hook and terminate tests.
- `./build/cpp_harness --fake --session /tmp/u3.jsonl "read README.md"` still works when no hook is configured.

---

### U4. Documentation and parity roadmap update

**Goal:** Update the parity roadmap to mark the hook item complete and adjust README/AGENTS.md only if public boundaries or behavior changed.

**Requirements:** Roadmap hygiene, R8.

**Dependencies:** U1, U2, U3.

**Files:**

- Modify: `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`
- Modify: `README.md` (only if the Architecture boundaries section needs the new hook seam)
- Modify: `AGENTS.md` (only if agent-loop routing guidance needs a new row)

**Approach:**

- In the parity roadmap, mark the T3 item "Add pre/post tool-call hooks and graceful blocking semantics" as complete (`[x]`) and add a pointer to this plan.
- Update README's Architecture boundaries section if agent-loop hooks are worth listing alongside event sinks and observable state.
- Update AGENTS.md's routing table if a new "agent loop hooks" entry improves discoverability.
- Run the full test suite and a manual fake-provider smoke test.

**Patterns to follow:**

- Existing README Architecture boundaries list style.
- Existing AGENTS.md routing table row style.

**Test scenarios:**

- Test expectation: none — documentation-only changes. The integration smoke test is manual verification.
- Integration (manual): `./build/cpp_harness --fake --session /tmp/u4.jsonl "read README.md"` produces unchanged output.
- Integration (manual): `./build/cpp_harness --fake --repl --session /tmp/u4-repl.jsonl` with two prompts preserves history.

**Verification:**

- Parity roadmap T3 hook item is marked complete and references this plan.
- Full `ctest` passes.
- `./build/cpp_harness --fake --session /tmp/u4.jsonl "hello"` produces expected semantic event lines.

---

## System-Wide Impact

- **Interaction graph:** `AsyncToolExecutionResult` change touches `src/tools/AsyncToolFactories.cpp`, `src/agent/AgentLoop.cpp`, `tests/tools/AsyncToolsTest.cpp`, and `tests/agent/AsyncAgentLoopTest.cpp`. The new hooks are consumed only by `AgentLoop.cpp` and configured through `AsyncAgentOptions`.
- **Error propagation:** Hook failures become `ErrorCode::Tool` errors, emitted as `AgentEndEvent` and returned as `std::unexpected`. This is consistent with existing provider/validation error paths.
- **State lifecycle risks:** The `terminate_batch` flag is local to one turn; it does not persist across turns or into session state. A partial failure mid-batch (hook throws on the second call) aborts the entire run rather than leaving half-finalized state.
- **API surface parity:** `AsyncAgentOptions` gains two optional hook fields; existing callers that default-construct it are unaffected. `AsyncToolExecutionResult` changes its `content` type — this is a source-breaking change for any out-of-tree tool implementations, but in-tree tools and tests are updated together.
- **Integration coverage:** Cross-layer scenario — CLI `--fake` → `AgentLoop` with no hooks → built-in tools → unchanged output. Hook scenarios are covered in unit tests with a fake streaming client and fake tool.
- **Unchanged invariants:** Move-only event sinks remain move-only. Glaze stays isolated to AI/provider serialization. Workspace containment, secret redaction, bash opt-in, and session permissions are untouched. No `util::Result`, Boost.JSON domain contract, or legacy sync tool surface is reintroduced.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Changing `AsyncToolExecutionResult::content` to `vector<Content>` breaks any out-of-tree tool implementations. | In-tree tools and tests are updated in the same PR; the public header change is source-breaking by design and aligns with pi parity. |
| Synchronous hook signatures may not satisfy future async permission-prompt use cases. | Documented as a deliberate scope boundary; async hooks can be added as an overload without removing the synchronous contract. |
| Hook exceptions escaping the coroutine frame could crash the process. | Wrap each hook invocation in try/catch and convert to `util::Error`. |
| Terminate batch logic could be misinterpreted as "any call requests terminate." | Implement and test the "all finalized calls must agree" semantics exactly as pi defines it. |

---

## Documentation / Operational Notes

- Update the parity roadmap (`docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`) to mark the T3 hook item complete and reference this plan.
- Update `README.md` Architecture boundaries if the agent-loop hook seam is worth surfacing.
- Update `AGENTS.md` routing table if a new row improves agent discoverability for hook work.
- No monitoring, rollout, or migration concerns — this is a development-only agent-loop contract change.

---

## Sources & References

- **Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T3 checklist)
- **Prerequisite plan:** `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- Related code:
  - `include/cch/agent/AgentTool.hpp`, `include/cch/agent/AgentLoop.hpp`, `include/cch/agent/AgentContext.hpp`, `include/cch/agent/AgentEvent.hpp`
  - `src/agent/AgentLoop.cpp`
  - `src/tools/AsyncToolFactories.cpp`
  - `tests/agent/AsyncAgentLoopTest.cpp`, `tests/tools/AsyncToolsTest.cpp`
  - `tests/architecture/MoveOnlyCallbackTest.cpp`, `tests/architecture/PublicHeaderBoundaryTest.cpp`
- pi reference contracts:
  - `pi:packages/agent/src/types.ts`
  - `pi:packages/agent/src/agent-loop.ts`
