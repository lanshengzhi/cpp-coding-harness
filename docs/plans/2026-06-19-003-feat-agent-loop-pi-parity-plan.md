---
title: "feat: Agent loop context transform, steering, follow-up, and parallel tool execution with OAuth deferral documentation"
type: "feat"
status: "completed"
date: "2026-06-19"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---


# feat: Agent loop context transform, steering, follow-up, and parallel tool execution with OAuth deferral documentation

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.

## Problem Frame and Scope

`docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` tracks the migration of this C++ agent harness toward pi's module and contract parity. T2 `pi-ai` parity is functionally complete except for an explicit deferral note, and T3 `pi-agent-core` parity recently landed tool-call hooks (`docs/plans/2026-06-19-002-feat-agent-tool-hooks-plan.md`). The remaining T3 gaps are the control seams that let callers steer, extend, and re-contextualize the loop without modifying `src/agent/AgentLoop.cpp` internals, plus the sequential/parallel tool execution decision.

This plan covers:

1. **T2 cleanup:** explicitly document that OAuth and subscription-provider flows are not supported.
2. **T3 loop control seams:** `transform_context`, `convert_to_llm`, `get_steering_messages`, `get_follow_up_messages`, and `prepare_next_turn`.
3. **T3 tool execution mode:** per-tool and per-run `sequential` vs `parallel` execution, with parallel results re-inserted in assistant source order.
4. **T3 hygiene:** architecture tests guarding move-only callbacks, and routing-document updates.

Out of scope: abort/cancellation signals (pi's `AbortSignal` has no direct C++ equivalent yet), `should_stop_after_turn` (can be added later on top of `prepare_next_turn`), tool execution streaming updates, and any new provider adapters.

## Origin Traceability

- `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`
  - T2 / "Defer OAuth and subscription-provider flows until model/provider registry contracts are stable."
  - T3 / "Add context transform, conversion-to-LLM, steering messages, follow-up messages, and prepare-next-turn seams."
  - T3 / "Evaluate sequential versus parallel tool execution with deterministic result insertion."
  - T3 / "Preserve move-only event sink semantics while adding missing lifecycle events."
- `pi:packages/agent/src/types.ts`
- `pi:packages/agent/src/agent-loop.ts`
- `docs/plans/2026-06-19-002-feat-agent-tool-hooks-plan.md` (recently landed hook contract)

## Key Technical Decisions

1. **Keep new callbacks move-only.** The existing `AgentEventSink`, `BeforeToolCallHook`, and `AfterToolCallHook` are `std::move_only_function`. All new loop-control callbacks will follow the same pattern so the public contract does not accidentally reintroduce copyable `std::function` requirements.
2. **Default behavior is pass-through.** `transform_context` and `convert_to_llm` default to identity. This preserves current loop behavior when callers do not configure them.
3. **Steering and follow-up are pull-based queues.** The caller provides callbacks that return vectors of messages. The loop drains steering after each turn and follow-up only when the agent would otherwise stop, matching `pi:packages/agent/src/agent-loop.ts`.
4. **Default tool execution stays sequential.** `AsyncAgentOptions::tool_execution_mode` defaults to `Sequential` to preserve today's deterministic behavior and existing tests. `Parallel` is opt-in, keeping the origin's "evaluate" intent alive until a concrete caller justifies the concurrency cost.
5. **Per-tool `execution_mode` lives in the agent layer.** `ToolExecutionMode` is defined in `cch::ai` for header reachability, but the per-tool override is exposed through `AsyncAgentTool`, not `ai::Tool`. `ai::Tool` remains a provider-schema value; scheduling policy stays in the agent layer.
6. **Parallel execution re-orders events but not transcript order.** `tool_execution_end` may emit in completion order, but `ToolResultMessage` entries are appended to the transcript in the order the assistant emitted the tool calls. This keeps the LLM context deterministic.
7. **High-privilege callbacks are guarded.** Every new callback is wrapped in `try/catch`, has bounded output, and operates on a copy of messages where mutation is allowed only at explicit extension points.

## Implementation Units

### U1. Document OAuth and subscription-provider deferral

**Goal:** Make the unsupported status of OAuth/subscription-provider flows explicit so that no placeholder API or README wording implies they are already implemented.

**Requirements:**

- Satisfies T2 deferral item from `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`.
- Aligns with `pi:packages/ai/src/oauth.ts` (empty/deferred in pi reference) and `pi:packages/coding-agent/docs/providers.md`.

**Dependencies:** None.

**Files:**

- `README.md`
- `include/cch/ai/ProviderRegistry.hpp`

**Approach:** Add a short paragraph to README's provider/model section stating that OAuth, subscription-provider, and dynamic API-key resolution flows are intentionally deferred. Add a matching code comment near `ProviderFactoryContext`/`ProviderRegistry` noting that `api_key_env` covers static environment keys only and OAuth is not yet supported.

**Test expectation:** none — documentation-only change.

**Verification:** README no longer reads as if OAuth is supported; `ProviderRegistry.hpp` carries a visible deferral comment.

---

### U2. Add `transform_context` and `convert_to_llm` seams

**Goal:** Let callers transform the message context before each LLM call and convert agent-internal messages to LLM-compatible messages, without special-casing inside the loop.

**Requirements:**

- Satisfies T3 / "context transform, conversion-to-LLM" from the parity TODO.
- Matches `pi:packages/agent/src/types.ts` `transformContext` and `convertToLlm` contracts.

**Dependencies:** None; U1 is hygiene-only and may proceed in parallel.

**Files:**

- `include/cch/agent/AgentContext.hpp`
- `include/cch/agent/AgentLoop.hpp`
- `src/agent/AgentLoop.cpp`
- `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Define named move-only callback aliases in `include/cch/agent/AgentContext.hpp`:
  - `TransformContextHook`: `std::vector<ai::MessageVariant>` → `util::Expected<std::vector<ai::MessageVariant>>`.
  - `ConvertToLlmHook`: `std::vector<ai::MessageVariant>` → `util::Expected<std::vector<ai::MessageVariant>>`.
- Add `std::optional<TransformContextHook>` and `std::optional<ConvertToLlmHook>` fields to `AsyncAgentOptions`; absent means identity.
- In `AgentLoop::continue_with`, before building the `StreamChatRequest`, apply `transform_context` then `convert_to_llm` to a copy of `context.messages`. Use the transformed copy only for the request; keep the original transcript intact.
- Wrap each callback in `try/catch`. If a callback throws or returns an error, emit `AgentEndEvent` and abort the run with that error, matching hook-error behavior.
- If `convert_to_llm` returns an empty vector, emit a failed `AgentEndEvent` with `ErrorCode::Validation` rather than making a doomed provider call.

**Test scenarios:**

- **Happy path:** default identity path produces the same LLM request messages as before.
- **Transform pruning:** `transform_context` drops messages older than a synthetic boundary; the next LLM request contains only the retained messages, while the final transcript still contains all messages.
- **Convert filtering:** `convert_to_llm` drops custom/non-LLM message shapes and maps the rest to standard user/assistant/tool messages.
- **Error path:** a failing `transform_context` aborts the run and emits a failed `AgentEndEvent`.
- **Error path:** a failing `convert_to_llm` aborts the run and emits a failed `AgentEndEvent`.
- **Empty-result path:** `convert_to_llm` returning an empty vector aborts with `ErrorCode::Validation`.
- **Exception path:** a throwing `transform_context` or `convert_to_llm` is caught and aborts the run cleanly.
- **Architecture:** `AsyncAgentOptions`, `TransformContextHook`, and `ConvertToLlmHook` remain non-copyable and moveable.

**Verification:** `AsyncAgentLoopTest` covers identity, pruning, filtering, and error paths; `MoveOnlyCallbackTest` covers the new callback members.

---

### U3. Add steering and follow-up message queues

**Goal:** Allow external callers to inject messages mid-run (steering) or after the agent would otherwise stop (follow-up), matching pi's pull-based queue contract.

**Requirements:**

- Satisfies T3 / "steering messages, follow-up messages" from the parity TODO.
- Matches `pi:packages/agent/src/types.ts` `getSteeringMessages` and `getFollowUpMessages`.

**Dependencies:** U2.

**Files:**

- `include/cch/agent/AgentContext.hpp`
- `src/agent/AgentLoop.cpp`
- `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Define `GetSteeringMessagesHook` and `GetFollowUpMessagesHook` as move-only callbacks returning `util::Expected<std::vector<ai::MessageVariant>>`.
- Add `std::optional<GetSteeringMessagesHook>` and `std::optional<GetFollowUpMessagesHook>` to `AsyncAgentOptions`; absent means no queued messages.
- Restructure the turn loop to maintain a `pending_messages` queue:
  - At turn start, if `pending_messages` is non-empty, emit a generic `MessageEvent` pair (or extend `MessageStartEvent`/`MessageEndEvent` to carry `ai::MessageVariant`) for each queued message, validate that the message count and total byte size are within bounds, and append them to `context.messages` before calling the LLM.
  - After each turn (and after tool results are appended), poll `get_steering_messages`; validate and enqueue any returned messages for the next iteration.
  - When the agent has no tool calls, the pending queue is empty, and `get_steering_messages` returns nothing, poll `get_follow_up_messages`; validate and enqueue returned messages and continue the outer loop.
  - If both callbacks return empty, the run ends normally.
- Wrap callbacks in `try/catch`; a throwing callback aborts the run.

**Test scenarios:**

- **Happy path:** empty callbacks leave the existing single-turn and multi-turn behavior unchanged.
- **Steering injection:** a steering message returned after the first tool-result turn is emitted and appended before the second LLM request.
- **Follow-up continuation:** a follow-up message returned after a final assistant text response triggers an additional turn.
- **Follow-up empty:** when follow-up returns nothing, the run ends after the final response.
- **Error path:** a callback returning an error aborts the run and emits a failed `AgentEndEvent`.
- **Exception path:** a throwing callback is caught and aborts the run cleanly.
- **Ordering:** steering messages are emitted in the order returned by the callback.
- **Lifecycle events:** new `MessageStartEvent`/`MessageEndEvent` emission for queued messages is tied to the origin's "missing lifecycle events" goal.

**Verification:** `AsyncAgentLoopTest` demonstrates steering, follow-up, empty, error, and ordering cases.

---

### U4. Add `prepare_next_turn` seam

**Goal:** Let callers adjust context, model, and thinking level between turns without modifying `AgentLoop` internals.

**Requirements:**

- Satisfies T3 / "prepare-next-turn seams" from the parity TODO.
- Matches `pi:packages/agent/src/types.ts` `prepareNextTurn` and `AgentLoopTurnUpdate`.

**Dependencies:** U3.

**Files:**

- `include/cch/agent/AgentContext.hpp`
- `include/cch/agent/AgentLoop.hpp`
- `src/agent/AgentLoop.cpp`
- `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Define `PrepareNextTurnContext` carrying the current turn's assistant message, the tool results from this turn, the full `ai::AiContext`, and the steering/follow-up messages that were just enqueued (if any).
- Define `AgentLoopTurnUpdate` with optional fields:
  - `append_messages`: `std::optional<std::vector<ai::MessageVariant>>` — messages to append to the transcript (safer than replacing the whole context).
  - `model`: `std::optional<std::string>` — validated against the provider registry before use.
  - `thinking_level`: `std::optional<std::string>` — validated against an allowed set; stored in `AgentState::thinking_level` and propagated to future provider requests once T2 provider parity adds reasoning/thinking options.
- Add `PrepareNextTurnHook` alias returning `util::Expected<std::optional<AgentLoopTurnUpdate>>`, and add `std::optional<PrepareNextTurnHook>` to `AsyncAgentOptions`.
- Invoke `prepare_next_turn` after `TurnEndEvent` and after steering/follow-up polling but before the next LLM request. Apply returned values only when present; update `options_.model`, `context.model`, and `AgentState::model` for model changes.
- Wrap the callback in `try/catch`; a throwing callback aborts the run.

**Test scenarios:**

- **No update:** returning `std::nullopt` leaves context, model, and thinking level unchanged.
- **Model swap:** returning a different model causes the next LLM request to use that model and updates observable state.
- **Thinking level change:** returning a different thinking level is preserved in `AgentState`.
- **Append-only mutation:** returning `append_messages` appends to the transcript without rewriting history.
- **Validation failure:** returning an unknown model or thinking level is rejected with `ErrorCode::Validation`.
- **Error path:** a callback error aborts the run.
- **Exception path:** a throwing callback is caught and aborts the run cleanly.

**Verification:** `AsyncAgentLoopTest` covers no-op, model swap, thinking-level change, context mutation, and error paths.

---

### U5. Add tool execution mode contracts

**Goal:** Expose sequential vs parallel execution as a first-class contract on both the run options and individual tools.

**Requirements:**

- Satisfies the contract portion of T3 / "Evaluate sequential versus parallel tool execution" from the parity TODO.
- Matches `pi:packages/agent/src/types.ts` `ToolExecutionMode` and per-tool `executionMode`.

**Dependencies:** None ( foundational contract; can precede U6/U7).

**Files:**

- `include/cch/ai/Tool.hpp`
- `include/cch/agent/AgentTool.hpp`
- `include/cch/agent/AgentContext.hpp`
- `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Add `enum class ToolExecutionMode { Sequential, Parallel }` in `include/cch/ai/Tool.hpp` (under `cch::ai`) so both `cch::ai` and `cch::agent` can use it without reversing the module graph.
- Add `ToolExecutionMode tool_execution_mode{ToolExecutionMode::Sequential}` to `AsyncAgentOptions` to preserve current deterministic behavior.
- Add a virtual `std::optional<ToolExecutionMode> execution_mode() const` method to `AsyncAgentTool` so tool implementations can advertise a preferred mode; default returns `std::nullopt`.
- The loop resolves effective mode per batch: if the run default is `Sequential` or any tool in the batch advertises `Sequential`, the batch runs sequentially; otherwise it runs in parallel when `tool_execution_mode == Parallel`.

**Test scenarios:**

- **Default sequential:** `AsyncAgentOptions` defaults to `Sequential`.
- **Per-tool override:** a tool advertising `Sequential` forces sequential execution even when the run default is `Parallel`.
- **Run-mode override:** setting `AsyncAgentOptions::tool_execution_mode` to `Parallel` enables parallel execution for a batch where all tools advertise `Parallel` or no mode.
- **Architecture:** `ToolExecutionMode` is usable from `cch::ai` without including `cch::agent` headers.

**Verification:** `AsyncAgentLoopTest` asserts tool definition propagation and option defaults.

---

### U6. Implement sequential tool execution path

**Goal:** Provide an explicit sequential path that preserves the current one-by-one execution semantics and serves as the fallback when parallel mode is not safe.

**Requirements:**

- Satisfies the sequential path of T3 / "Evaluate sequential versus parallel tool execution".
- Preserves existing hook, error, and terminate-batch behavior.

**Dependencies:** U5.

**Files:**

- `src/agent/AgentLoop.cpp`
- `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Extract the existing tool-execution body into an internal `execute_tool_calls_sequential` helper.
- Route to it when `options_.tool_execution_mode == ToolExecutionMode::Sequential` or when any tool call targets a tool whose `execution_mode` is `Sequential`.
- Keep current behavior: `ToolExecutionStartEvent`/`ToolExecutionEndEvent` per call, results appended in source order, terminate-batch logic unchanged.

**Test scenarios:**

- **Single tool:** one sequential call executes and emits start/end events.
- **Multiple tools:** two sequential calls execute in source order; the second call starts only after the first is fully finalized.
- **Hook ordering:** `before_tool_call` and `after_tool_call` run in source order with no interleaving.
- **Terminate batch:** all finalized calls in the batch having `terminate=true` stops the run after the batch.
- **Mixed outcome:** an error or a call whose `terminate` hint is `false` prevents terminate-batch, matching existing tests.

**Verification:** `AsyncAgentLoopTest` covers sequential routing, ordering, hook ordering, and terminate semantics.

---

### U7. Implement parallel tool execution path

**Goal:** Allow independent tool calls to run concurrently while keeping the transcript deterministic and honoring per-tool sequential overrides.

**Requirements:**

- Satisfies the parallel path of T3 / "Evaluate sequential versus parallel tool execution".
- Matches `pi:packages/agent/src/agent-loop.ts` `executeToolCallsParallel` semantics: preflight sequentially, execute allowed tools concurrently, emit end events in completion order, append result messages in assistant source order.

**Dependencies:** U5, U6.

**Files:**

- `src/agent/AgentLoop.cpp`
- `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Implement `execute_tool_calls_parallel`:
  1. **Preflight phase:** for each tool call, run argument validation and `before_tool_call` sequentially. If blocked/invalid, produce an immediate finalized outcome.
  2. **Execution phase:** for each prepared tool, spawn a `boost::asio::co_spawn` awaitable on a bounded executor/strand to call `tool->execute`, run `after_tool_call`, and finalize. Wrap each spawned coroutine so exceptions are captured and converted to an error `ToolResultMessage`, matching the existing `error_tool_result` path. Collect awaitables in a vector keyed to source order.
  3. **Await all** using `boost::asio::experimental::parallel_group` or an equivalent awaitable-of-tuple pattern. If one coroutine fails with an exception, still await the remaining coroutines before surfacing the error to preserve deterministic transcript order.
  4. **Emit `ToolExecutionEndEvent`** as each coroutine completes.
  5. **Append `ToolResultMessage`** entries in the original assistant source order.
- Enforce a configurable `max_parallel_tools` cap (default e.g. 8) and apply the existing per-tool timeout to each parallel task.
- If any tool advertises `Sequential`, fall back to U6's sequential path for the entire batch.

**Test scenarios:**

- **Two parallel tools:** both execute; the slow tool finishes last but its result message appears in source order in the transcript.
- **Completion-order events:** `ToolExecutionEndEvent` order reflects actual completion, not source order.
- **One failure, one success:** the successful tool still completes; both results appear in source order; terminate-batch is false.
- **Sequential override:** a batch containing one sequential tool falls back to sequential execution.
- **Run-mode override:** setting `AsyncAgentOptions::tool_execution_mode` to `Sequential` forces sequential execution even when all tools advertise `Parallel`.
- **Hook safety:** `before_tool_call` runs sequentially for all calls before any `execute` begins; `after_tool_call` runs inside each parallel coroutine after execution.

**Verification:** `AsyncAgentLoopTest` covers parallel execution, ordering, failure isolation, sequential fallback, and run-mode override.

---

### U8. Update architecture tests for move-only callback discipline

**Goal:** Ensure the new loop-control callbacks do not weaken the move-only event discipline established in `tests/architecture/MoveOnlyCallbackTest.cpp`.

**Requirements:**

- Satisfies T3 / "Preserve move-only event sink semantics while adding missing lifecycle events".
- Matches the architecture rule in `AGENTS.md` §2.3.

**Dependencies:** U2, U3, U4.

**Files:**

- `tests/architecture/MoveOnlyCallbackTest.cpp`

**Approach:** Add static assertions that `AsyncAgentOptions`, `AgentEventSink`, `TransformContextHook`, `ConvertToLlmHook`, `GetSteeringMessagesHook`, `GetFollowUpMessagesHook`, and `PrepareNextTurnHook` are not copy-constructible/copy-assignable and are move-constructible/move-assignable.

**Test scenarios:**

- `AsyncAgentOptions` as a whole is non-copyable and moveable (implicitly covering both existing hooks and the new callbacks).
- Each named callback alias is non-copyable and moveable.

**Verification:** `MoveOnlyCallbackTest.cpp` compiles and passes.

---

### U9. Update routing documents and parity TODO

**Goal:** Keep `AGENTS.md` and the parity TODO aligned with the new seams.

**Requirements:**

- Satisfies T10 / "Keep README aligned with the current implemented subset and the next parity target" and "Update this TODO document when a slice is completed".

**Dependencies:** U2–U8.

**Files:**

- `AGENTS.md`
- `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`
- `README.md` (only if behavior changes materially; U1 already touches it)

**Approach:**

- In `AGENTS.md`, add a one-line note under the agent loop routing entry pointing to the new control seams.
- In the parity TODO, mark the T2 OAuth deferral and the covered T3 sub-items as completed, and update the T3 sequential/parallel item to reflect what is now implemented vs still deferred.

**Test expectation:** none — documentation-only change.

**Verification:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` accurately reflects the new baseline.

## Dependencies and Sequencing

```text
U1 ─┐
U2 ─┬─► U3 ─► U4 ─┐
    │              ▼
U5 ─┴──────────► U6 ─► U7
                 ▲
U8 after U2–U4.
U9 after U2–U8.
```

U1 and U5 are foundational and can start independently. U2, U3, and U4 are serial only to avoid file-conflict churn; they are logically independent except that U4's ordering depends on steering/follow-up polling from U3. U5 is required by U6 and U7. U7 depends on U6 because parallel mode falls back to sequential execution when a batch contains a sequential tool. U8 can run as soon as U2–U4 land. U9 is last.

## Deferred to Follow-Up Work

- **Abort/cancellation signals:** pi passes `AbortSignal` to hooks and tool execution. C++ has no direct equivalent wired through the current loop. Defer until a coherent cancellation strategy is designed.
- **`should_stop_after_turn` hook:** Can be added later on top of `prepare_next_turn`; the present plan covers the more general seam.
- **Tool execution streaming updates (`tool_execution_update`):** pi supports partial result streaming from long-running tools. Out of scope here.
- **Extended runtime message types:** pi's custom/branch/compaction messages belong to T4/T5 parity, not this loop-control slice.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Parallel tool execution introduces non-determinism in tests | Use fake tools with controlled delays; assert transcript order independently of event order; keep the default `Sequential`. |
| Parallel tool execution causes resource exhaustion | Cap `max_parallel_tools`; run parallel coroutines on a bounded executor/strand; apply per-tool timeouts and cancellation. |
| Exceptions escape spawned parallel coroutines | Wrap each coroutine to capture exceptions and convert them to error tool results; await siblings before surfacing errors. |
| New callbacks accidentally become copyable | `MoveOnlyCallbackTest.cpp` static assertions on `AsyncAgentOptions` and every named callback alias. |
| `transform_context`/`convert_to_llm` silently corrupt the transcript or leak secrets | Apply them only to a request copy; wrap in `try/catch`; validate output size and message count; document that they are privileged runtime seams. |
| `prepare_next_turn` redirects to an arbitrary model/thinking level | Validate `model` against the provider registry and `thinking_level` against an allowed set before applying updates. |
| Steering/follow-up queues inject unbounded or attacker-controlled messages | Validate message role/content types; cap count and byte size per turn; append only after existing tool results. |
| A mutating tool self-declares `Parallel` and races with peers | Treat advertised mode as a hint; future runtime capability tags can force sequential for mutating/stateful tools. |

## Documentation Plan

- `README.md` — update provider/model section with the OAuth deferral note.
- `AGENTS.md` — update agent loop routing entry to mention new control seams.
- `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` — mark completed T2/T3 items and clarify remaining gaps.
