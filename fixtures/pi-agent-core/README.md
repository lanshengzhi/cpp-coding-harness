# pi-agent-core compatibility fixtures

The committed evidence bundle for the pi-agent-core completion gate ([#330]/[#331], spec
[#349]). This directory mirrors the `fixtures/pi-ai/` strategy: every fixture is compared by
tests in this repository against the C++ surface, so the gate's evidence is one checklist away.
No fixture value is a live credential or derived from one; all strings are distinguishable
dummy values (see [Sanitization rules](#sanitization-rules)).

This file records only the capabilities landed so far (T01 [#350], T02 [#351], T03 [#352], T04 [#353], T05 [#354], T06 [#355], T07 [#356], T08 [#357]); the
capability checklist grows with each subsequent ticket (T09–T14, blockers-first per parity map [#2]), and
rows below cover only what the ticket that last touched this file landed.

## Pinned baseline

- **Frozen pi commit:** `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (the parity map [#2]
  baseline). The local pi checkout is `../pi`; `pi:` references resolve from that root.
- **Published artifact:** `@earendil-works/pi-agent-core@0.83.0`, published at pi tag
  `v0.83.0`. The harness consumer pinned here is `packages/agent/src/harness/agent-harness.ts`
  (`createStreamFn`) with the loop order of `packages/agent/src/agent-loop.ts` (`runLoop`).

## Sanitization rules

- Every credential-like string in these fixtures is a distinguishable `dummy-*` token; the
  model/session values are `gpt-test`, `session-golden`, and header values like `value`.
- No fixture value was copied from a live service, a real `auth.json`, or a real `models.json`.
- Live `~/.pi/agent/auth.json` and `~/.pi/agent/models.json` are compatibility/evidence inputs
  and must never be pasted into fixtures, logs, issues, or commits (parity map [#2]).

## Fixture inventory

### Option-forwarding goldens (`stream-simple-options-*.json`)

The per-turn `streamSimple` option set as the C++ Agent forwards it, captured through the
recording fake `ModelRuntime` (`tests/support/FakeModelRuntime.hpp`) and named exactly like pi's
harness-consumer set (`agent-harness.ts` `createStreamFn`): `reasoning`, `sessionId`,
`cacheRetention`, `timeoutMs`, `maxRetries`, `maxRetryDelayMs`, `headers`, `signal`. The model
identity is included because the fake records it with the call.

- `stream-simple-options.json` (#351): the full host-configured set — `reasoning: "high"`,
  `sessionId`, `cacheRetention` resolved `"short"`, `timeoutMs`, `maxRetries`, `maxRetryDelayMs`,
  one `headers` entry, and the active prompt signal.
- `stream-simple-options-default.json` (#351): every knob at its default — `off` thinking
  forwards no `reasoning` (pi `createLoopConfig` `off → undefined`), no session id, no timeout or
  retry overrides, empty headers, `cacheRetention` still resolving to the pi default `"short"`
  (unset → `Short` via `resolve_cache_retention`, ADR 0033), and the signal present.

`transport` stays fixed per adapter (the C++ `SimpleStreamOptions` has no transport member, per
#329) and `metadata`/`onPayload`/`onResponse`/`thinkingBudgets` are absent with no placeholder.

### Tool-result shape golden (`tool-result-shape.json`)

The committed tool-result shape golden ([#354]): one assistant message carrying three tool calls run
through the `ToolCallExecutor` — a success, a call whose arguments fail JSON Schema validation
(ADR 0007, `is_error: true` with the bounded contract diagnostic), and a sibling success. Each
result is serialized in pi's `ToolResultMessage` wire shape (`role`, `toolCallId`, `toolName`,
`content`, `details` omitted when absent, `isError`, `timestamp`), proving `is_error` grouping and
per-call failure isolation (ADR 0008) in one committed artifact. Timestamps are the deterministic
executor-level default `0` (the Agent loop stamps real time on commitment).

### Loop lifecycle goldens (`loop-*.json`)

The ordered `AgentLifecycleEvent` stream (plus per-call forwarded options where they prove a
hook effect), serialized canonically and byte-compared.

- `loop-lifecycle.json` (#351): one rich run covering `agent_start` → `turn_start` →
  `message_start/update/end` → `tool_execution_*` → `turn_end` → `agent_end`, with the
  steer drain point (a steering message steered from the `turn_end` observer drains at the end
  of turn 1 and injects before the turn-2 request), the follow-up drain point (a follow-up
  drains after the last tool-call turn and runs as turn 3), prepare-next-turn (its first update
  flips the thinking level `medium → high`, visible in the recorded `reasoning` of stream calls
  2/3), tool-call continuation (turn 1 → 2) and follow-up continuation (turn 2 → 3).
- `loop-terminal-error.json` (#351): the `error` terminal ordering — synthesized assistant
  start from the authoritative final message (the #326 terminal-before-start recovery), exactly
  one assistant lifecycle, `turn_end` with `stopReason: "error"`, then `agent_end`.
- `loop-terminal-aborted.json` (#351): the `aborted` terminal ordering under cancellation
  (ADR 0020) — same shape with `stopReason: "aborted"`.

### Thinking-level clamp golden (`thinking-level-clamp.json`)

The committed thinking clamp/resolution golden ([#352]): the per-turn `reasoning` option as the C++
Agent's creation-time and model-switch clamping produces it, captured through the recording fake
`ModelRuntime`. Two turns pin both required clamp points in one artifact:

- turn 1: a partial-map reasoning model (`gpt-partial`, off/low/high/xhigh mapped, `max` absent →
  supported off..xhigh) with the requested `"max"` level — **creation-time clamp** yields
  `reasoning: "xhigh"` on the wire;
- turn 2: the prepare-next-turn hook switches to a non-reasoning model (`gpt-basic`), which
  **re-clamps** `"xhigh"` to `"off"` — `reasoning` is undefined, so an unsupported level never
  reaches the wire.

### Thinking-persistence golden (`thinking-persistence.json`)

The committed thinking-persistence golden ([#353]): one `setThinkingLevel("high")` on a fresh session
whose resolution chain landed the first available model with configured auth (a reasoning
`models.json` model). Two facts are pinned in one artifact:

- the `thinking_level_change` entry shape the session file carries (`type` + `thinkingLevel`; the
  generated `id`/`timestamp` and the generic `parentId` null-vs-omitted tree metadata are stripped
  here and pinned by the T07 session-wire contract — see `session-roundtrip.jsonl` below);
- the settings default write (`defaultThinkingLevel: "high"` in the global `settings.json`), so
  resume restores the level exactly like pi.

### Session wire round-trip golden (`session-roundtrip.jsonl`)

The committed 11-entry-type JSONL round-trip golden ([#356]): a session built with the frozen pi
`createJsonlSessionStore`/`createSessionRepository` (`packages/agent/src/harness/session/`), dumped as
JSONL, and byte-compared by `SessionRoundTripGoldenTest` — every entry line parses and re-serializes
byte-identically (`EntrySerializer::serialize_entry`), proving pi's exact field presence,
null-vs-missing distinctions, ordering (`id, parentId, timestamp, type, …`), and active-path
semantics: root `parentId: null` (not absent), `leaf.targetId: null` for a root move, a label
cleared by an undefined label, `custom.data` absent vs explicit `null`, `compaction`
`firstKeptEntryId`/`retainedTail`/`usage`/`fromHook` absent-vs-present, and `active_tools_change`
carrying pi's `activeToolNames` field. Per-entry-type projection machinery is complete here; the
derived-state completion of `buildSessionContext` (`thinkingLevel`/`model`/`activeToolNames`) and
#327 resume re-resolution landed in T08 [#357] (see the derived-state fixture below).

Coverage per entry type: `message` (user + assistant with usage), `model_change`,
`thinking_level_change`, `active_tools_change`, `label` (set + cleared), `custom` (data object /
explicit null / absent), `custom_message` (string content + details, block content incl. an image,
`display: false`), `compaction` (full: `firstKeptEntryId` + `retainedTail` + `usage` + `details` +
`fromHook`; minimal: `summary` + `tokensBefore`), `branch_summary` (`usage` + `details` + `fromHook`),
`session_info` (sanitized name), `leaf` (target string + explicit null). Captured at pi baseline
`83114817`; entry ids/timestamps are pi's capture-time values (structure is the contract).

### Context projection fixtures (`projection-*.json`, `projection-*-session.jsonl`)

pi's `buildContext` and `convertToLlm` for two deterministic branches, captured from the frozen pi
tests and compared by `SessionRoundTripGoldenTest`:

- `projection-session.jsonl` + `projection-context.json` (+ `projection-context-llm.json`): a branch
  whose compaction carries `retainedTail`, followed by a `custom` entry, two `custom_message`
  entries (string + blocks), and a post-compaction message. Context = `compactionSummary` +
  retained tail, the `custom` entry **omitted by default**, `custom_message` → CustomMessage,
  post-compaction message last.
- `projection-branch-session.jsonl` + `projection-branch-context.json` (+ `-llm`): a branch with a
  `branch_summary` (from `moveTo`) and a post-move message. Context includes the branchSummary
  message.

The C++ `SessionTree::buildSessionContext` is compared against `projection-context.json`, and the
model-facing conversion (pi `convertToLlm` semantics, verbatim prefix/suffix constants) against
`projection-context-llm.json`, both driven into the Agent through the recording fake `ModelRuntime`
so the model sees exactly what pi's model sees.

### Derived session state golden (`derived-session-state.json`)

The committed derived-state golden ([#357]): pi's `deriveSessionContextState` output
(`thinkingLevel`, `model {provider, modelId}`, `activeToolNames`) for two branches, compared by
`SessionRoundTripGoldenTest` and driven into the Agent's turn options at the fake-`ModelRuntime`
seam:

- `fullBranch`: user root → `model_change` (openai/gpt-4.1) → assistant message
  (anthropic/claude-sonnet-4-5) → `thinking_level_change` (high) → `active_tools_change`
  (read/bash/edit/write). The assistant message lands after the `model_change`, so its
  provider/model wins — exactly the pi harness test "tracks model and thinking level changes in
  built context" (`loaded.model` = the assistant's anthropic/claude-sonnet-4-5), `thinkingLevel`
  = the last entry's `"high"`, `activeToolNames` = the last tools entry's copy.
- `defaults`: a header-only branch — pi's derived defaults `thinkingLevel: "off"`, `model: null`,
  `activeToolNames: null`.

Resume re-resolution (T08) re-resolves the derived model identity against the live runtime with pi's
`restoreModelFromSession` fallback message (`Could not restore model p/m (model no longer exists|no
auth configured). Using fp/fm.`) and continues through the resolution chain; the derived thinking
level flows into the Agent's per-turn `reasoning` option at the fake-`ModelRuntime` seam, and the
session file persists only the `model_change {provider, modelId}` line (no auth material, #327).

### Terminal matrix

The six-category terminal matrix (`model_source`, `model_validation`, `provider`, `stream`,
`auth`, `oauth`) is a test matrix in `ModelRuntimeSeamTest` (`[issue351]`), not a fixture: each
row scripts a terminal failure of one category through the fake runtime and asserts exactly one
terminal event plus an agreeing final `AssistantMessage`, with the category flowing through the
single `util::Expected` error value (the #326 six-category channel).

### Tool scheduling golden (`tool-scheduling.json`)

The committed tool-scheduling golden ([#355]): one run against the recording fake `ModelRuntime`
with deterministic per-tool delays, serializing the full per-call event stream (start events with
argument payloads, end events with `isError`, tool-result message events with tool call ids,
turn boundaries with stop reasons). Three turns pin the four T06 behaviors in one artifact:

- turn 1 (`alpha`/`beta`, both `ParallelSafe`, delays 20/5 ms): the **parallel default** — both
  `tool_execution_start` events precede any end, ends arrive in completion order (`beta`, `alpha`),
  and tool-result messages land later in assistant source order;
- turn 2 (`Length` stop): **truncated fail-all** — every call gets a per-call `is_error` result in
  source order and no tool executes (pi `failToolCallsFromTruncatedMessage`);
- turn 3 (`gamma` is `Exclusive`): the **per-tool sequential override** serializes the whole batch
  with full per-call lifecycle in source order, and the after hook's all-true **terminate** hint
  ends the loop after that turn.

`agent_end.messageCount` (10) pins the invocation-local transcript length.

## Capability-to-source checklist

One line per scoped capability, tying it to the frozen pi source, the resolution record, the C++
surface, and the committed evidence. Resolution records: [#326]
`7d813af3650dfa4fd098e90e321fce24`, [#329] `746839885c04cf195984af7112f2ea88`, [#330]
`6d06d3172ff3383ed3188a9bef4be587`, [#331] `3a9243c1cb711cecbde25396a5af53cd`.

### Supported Capabilities (this ticket's scope)

| # | Capability | Frozen pi source | C++ surface | Evidence (tests → fixtures) |
| --- | --- | --- | --- | --- |
| 1 | Per-turn harness-consumer option set through `streamSimple`: `reasoning` (thinking level, `off` → undefined), `sessionId`, `cacheRetention` (default `"short"`), `timeoutMs`, `maxRetries`, `maxRetryDelayMs`, `headers`, `signal` | `packages/agent/src/harness/agent-harness.ts` `createStreamFn`; `packages/agent/src/agent.ts` `createLoopConfig` (`thinkingLevel === "off" ? undefined : thinkingLevel`) | `include/cch/agent/AgentContext.hpp` (`AsyncAgentOptions`), `src/agent/AgentLoop.cpp` (per-turn forwarding) | `ModelRuntimeSeamTest` `"Agent forwards the full harness-consumer option set…"` + `"default turns forward pi's harness-consumer defaults…"`; `AgentCoreEvidenceTest` → `stream-simple-options.json`, `stream-simple-options-default.json` |
| 2 | `transport` fixed per adapter; `metadata`/`onPayload`/`onResponse`/`thinkingBudgets` omitted with no placeholder | `agent-harness.ts` `createStreamFn` (options 2/3/4/5 per [#329]) | `include/cch/ai/RequestOptions.hpp` (`SimpleStreamOptions` has no such members — architecture-pinned) | `ArchitectureSurfaceScanTest` `"streamSimple exposes only the supported caller option set"` |
| 3 | Terminal-error-event contract: exactly one `error`/`aborted` terminal event plus a final `AssistantMessage` with agreeing `stopReason`/`errorMessage` (incl. terminal-before-start synthesized start) | #326; `packages/agent/src/agent-loop.ts` `streamAssistantResponse` (`done`/`error` → `message_start` synthesis) | `src/agent/AgentLoop.cpp` (synthesize-start + terminal turn), `tests/support/FakeModelRuntime.hpp` (exactly-one-terminal script) | `ModelRuntimeSeamTest` terminal/aborted/matrix rows; `AgentCoreEvidenceTest` → `loop-terminal-error.json`, `loop-terminal-aborted.json` |
| 4 | Six-category channel (`model_source`, `model_validation`, `provider`, `stream`, `auth`, `oauth`) through the single `util::Expected` error value; no second exception hierarchy | #326 | `include/cch/util/Error.hpp` (`ErrorCode` six categories), `tests/support/FakeModelRuntime.hpp` (`terminal_failure_code`/`last_terminal_failure`) | `ModelRuntimeSeamTest` `"six-category terminal matrix yields exactly one terminal event plus a final AssistantMessage each"` |
| 5 | Loop lifecycle order: `agent_start` → `turn_start` → `message_start/update/end` → `tool_execution_*` → `turn_end` → prepare-next-turn → stop-after-turn → steering → follow-up → `agent_end`; tool-call and follow-up continuation; `agent_end.messages` invocation-local | ADR 0014; `packages/agent/src/agent-loop.ts` `runLoop` | `src/agent/AgentLoop.cpp` | `AgentCoreEvidenceTest` → `loop-lifecycle.json`, `loop-terminal-error.json`, `loop-terminal-aborted.json`; existing `AsyncAgentLoopTest`/`AgentTest` ordering suites |
| 6 | Built-in model-facing tool set is exactly `read`/`bash`/`edit`/`write` with pi names; `edit` replaces `edit_file` (no `edit_file` surface remains anywhere, including serialization/legacy paths); `grep`/`find`/`ls` absent with no placeholder | `packages/agent/src/harness/tools/index.ts` (read/bash/edit/write); `edit.ts` `createEditTool` (`name: "edit"`); `packages/coding-agent/src/core/tools/{grep,find,ls}.ts` (Deferred) | `include/cch/tools/ToolFactories.hpp` (`make_async_edit_tool`), `src/tools/AsyncToolFactories.cpp`, `include/cch/coding_agent/Sdk.hpp` (`SdkBuiltinTools::edit`) | `AsyncToolsTest` `"async edit tool …"` suite; `ArchitectureSurfaceScanTest`; `SdkSessionTest` tool-registry rows |
| 7 | `edit` implements pi's edit-diff semantics: every edit matched against the original file (not incrementally), exact-then-fuzzy matching (NFKC, per-line trailing whitespace stripped, smart quotes/dashes/spaces normalized), CRLF/LF detection and restoration, BOM preservation, overlap rejection, pi's not-found/duplicate/empty/no-change messages, and details `{diff, patch, firstChangedLine}` | `packages/agent/src/harness/tools/edit-diff.ts` (`applyEditsToNormalizedContent`, `fuzzyFindText`, `normalizeForFuzzyMatch`, `generateDiffString`, `generateUnifiedPatch`, `detectLineEnding`, `stripBom`) and `edit.ts` `createEditTool` | `src/tools/EditDiff.{hpp,cpp}`, `src/tools/AsyncToolFactories.cpp` (`AsyncEditTool`) | `AsyncToolsTest` pi-shaped scenarios: disjoint edits + details shape, overlap rejection, missing/duplicate messages, BOM+CRLF preservation, fuzzy smart-quote/dash matching, empty/no-change errors, contract/execution agreement |
| 8 | Tool arguments validated against each tool's JSON Schema as the executable contract before policy hooks and execution; malformed/schema-invalid args fail as error tool results for their calls | ADR 0007; `edit.ts` TypeBox schemas | `src/agent/ToolArgumentPreparation.{hpp,cpp}` (`prepare_tool_arguments`), `src/agent/ToolCallExecutor.cpp` (validation before hooks/execution) | `ToolCallExecutorTest` `[tool-arguments]` suite (coercion, boundaries, formats, malformed-JSON redaction); `AgentCoreEvidenceTest` → `tool-result-shape.json` (validation failure row) |
| 9 | Tool results group with `is_error` exactly as pi's `ToolResultMessage` shape; committed golden proves it | `packages/ai/src/types.ts` `ToolResultMessage` (`isError`, role `toolResult`, `toolCallId`, `toolName`, `content`, `details?`, `timestamp`) | `include/cch/ai/Message.hpp` (`ToolResultMessage`), `src/ai/glaze/AiJson.hpp` (`to_dto`), `src/agent/ToolCallExecutor.cpp` | `AgentCoreEvidenceTest` `"tool-result shape golden …"` → `tool-result-shape.json` |
| 10 | Per-call failure isolation (Tool Call Outcome): one failing call produces an error result while sibling calls in the same assistant message complete normally; batch termination only from all-true explicit termination | ADR 0008; `packages/agent/src/types.ts` `AgentToolResult.terminate` | `src/agent/ToolCallExecutor.cpp` (sequential + bounded parallel), `src/agent/AgentLoop.cpp` | `AgentCoreEvidenceTest` → `tool-result-shape.json`; `ToolCallExecutorTest` isolation/termination rows |
| 11 | Concrete `bash` tool registration stays gated by `--enable-bash` authorization; user Bash stays independent (ADR 0026) | ADR 0026; `packages/agent/src/harness/tools/bash.ts` (tool exists; authorization is assembly policy) | `include/cch/coding_agent/Sdk.hpp` (`SdkBuiltinTools::bash`), `src/cli/CliParse.cpp` (`--enable-bash`), `src/coding_agent/runtime/SessionFactory.cpp` | `SdkSessionTest` `"SDK disabled bash is absent …"/"SDK enabled bash appears …"`; `SessionFactoryUserShellTest` |
| 12 | `beforeToolCall` fires after argument validation for every prepared call, in pi's order; `afterToolCall` runs for every executed outcome (success, error result, throwing tool) before `tool_execution_end` and may override content/details/isError/terminate | `packages/agent/src/agent-loop.ts` `prepareToolCall` / `finalizeExecutedToolCall`; ADR 0008 (hook failures isolate per call; after hooks run for error outcomes) | `src/agent/ToolCallExecutor.cpp` (sequential + bounded parallel), `src/agent/ExecutionShared.hpp` (`invoke_agent_hook`) | `ToolCallExecutorTest` `"afterToolCall runs for an error execution result (sequential/parallel path)"`, `"afterToolCall runs for a throwing tool"`, `"beforeToolCall hook failure finalizes only its call…"`, `"afterToolCall hook failure finalizes only its call"`; `AsyncAgentLoopTest` `"beforeToolCall hook failure finalizes only its call"`, `"afterToolCall hook failure finalizes only its call"`, `"afterToolCall hook exception becomes a per-call tool error"` |
| 13 | Tool calls in one assistant message execute in parallel by default (pi `toolExecution` default `"parallel"`, no explicit cap); a per-tool sequential override — any call to a tool whose adapter declares `Exclusive` (pi `executionMode: "sequential"`) — serializes the whole batch through the sequential path; bounded caps (incl. 0 = no cap) and sequential policy stay available | `packages/agent/src/harness/types.ts` (`toolExecution` default `"parallel"`); `packages/agent/src/agent-loop.ts` `executeToolCalls` (`hasSequentialToolCall`); `packages/agent/src/types.ts` `AgentTool.executionMode` | `include/cch/agent/AgentContext.hpp` (`BoundedParallelToolExecution` default `0`, `ToolExecutionPolicy`), `src/agent/ToolCallExecutor.cpp` (`execute()` routing) | `AsyncAgentLoopTest` `"default tool execution runs a parallel-safe batch concurrently"`, `"tool execution policy defaults to bounded parallel"`, `"an exclusive tool serializes the whole batch with full per-call lifecycle…"`, `"bounded parallel zero means no explicit concurrency cap"`; `ToolCallExecutorTest` `"default policy executes parallel-safe calls concurrently"`, `"an exclusive tool serializes the whole batch with full per-call lifecycle"` |
| 14 | A `length`-truncated assistant message fails every one of its tool calls with pi's verbatim error result and executes none of them; hooks do not fire and the batch never terminates | `packages/agent/src/agent-loop.ts` `failToolCallsFromTruncatedMessage` (verbatim message, `terminate: false`) | `src/agent/AgentLoop.cpp` (length branch before the executor seam) | `AsyncAgentLoopTest` `"length-truncated fail-all matches pi's message and emits source-order errors"`, `"length-truncated tool calls emit errors without crossing the executor seam"`; `AgentCoreEvidenceTest` → `tool-scheduling.json` turn 2 |
| 15 | All-true `terminate` batch hint ends the loop exactly like pi: batch terminates iff every finalized result carries an explicit terminate hint; error results carry no implicit ban (ADR 0008) but plain failure outcomes never terminate | `packages/agent/src/agent-loop.ts` `shouldTerminateToolBatch` (`every(result.terminate === true)`); ADR 0008 | `src/agent/ToolCallExecutor.cpp` (`make_batch_result`), `src/agent/AgentLoop.cpp` (`has_more_tool_calls`) | `AsyncAgentLoopTest` `"all-true terminate batch ends the loop after one turn"`, `"an error result with an explicit terminate hint still terminates the batch"`, `"terminate batch continues when one call declines"`; `ToolCallExecutorTest` `"an error result with an explicit terminate hint terminates the batch"`, `"batch termination requires every call to carry the hint"`; `AgentCoreEvidenceTest` → `tool-scheduling.json` turn 3 |
| 16 | Seven-level thinking set including `"max"` with `"medium"` as the default (pi `DEFAULT_THINKING_LEVEL`), replacing the legacy `"off"` default; an unset level requests `"medium"` | `packages/coding-agent/src/core/defaults.ts` (`DEFAULT_THINKING_LEVEL: "medium"`); `model-resolver.ts`; ADR 0034 | `include/cch/agent/Agent.hpp` (`AgentInitialState::thinking_level` default), `src/agent/AgentLoop.cpp` (constructor normalizes empty → `"medium"`), `src/coding_agent/runtime/AgentSessionRuntime.cpp` (fresh-session fallback) | `ModelRuntimeSeamTest` `"the Agent holds kDefaultModel…"` (default `"medium"` clamps to `"off"` on a non-reasoning model); `AgentTest` thinking-state suites; `stream-simple-options-default.json` (clamped default turn) |
| 17 | Creation-time and model-switch clamping via `getSupportedThinkingLevels`/`clampThinkingLevel` (null = unsupported; xhigh/max require explicit mapping); an unsupported level can never reach the wire | `packages/ai/src/models.ts` `getSupportedThinkingLevels`/`clampThinkingLevel`; `packages/coding-agent/src/core/sdk.ts` (creation clamp) and `agent-session.ts` (re-clamp on model switch / `setThinkingLevel`) | `include/cch/ai/Model.hpp` + `src/ai/SimpleOptions.cpp` (`clamp_thinking_level_string`), `src/agent/AgentLoop.cpp` (constructor clamp + `apply_turn_update` re-clamp) | `ModelRuntimeSeamTest` `"creation-time thinking clamping covers every level against full, partial, and null thinking maps"` (7×3 matrix), `"model switch re-clamps…"`; `SimpleOptionsTest` string-level clamp; `AgentCoreEvidenceTest` → `thinking-level-clamp.json` |
| 18 | Per turn, thinking `off` produces an undefined `reasoning` option and any other (clamped) level forwards as-is | `agent-harness.ts` `createStreamFn`; `agent.ts` `createLoopConfig` (`thinkingLevel === "off" ? undefined : thinkingLevel`) | `src/agent/AgentLoop.cpp` (`stream_reasoning`) | `ModelRuntimeSeamTest` option-forwarding tests; `AgentCoreEvidenceTest` → `stream-simple-options-default.json` (clamped default, no reasoning) |
| 19 | The Agent holds the concrete unknown `kDefaultModel` with no special-casing until a real model resolves; streaming against it fails through normal provider lookup exactly like pi | `packages/agent/src/agent.ts` (`DEFAULT_MODEL`); `sdk.ts` `if (!model) thinkingLevel = "off"` | `include/cch/agent/AgentContext.hpp` (`kDefaultModel`, `AsyncAgentOptions::model` default), `src/agent/AgentLoop.cpp` (no placeholder substitution) | `ModelRuntimeSeamTest` `"the Agent holds kDefaultModel with no special-casing…"` |
| 20 | The first real model resolves through pi's exact precedence chain — CLI `--model`/`--provider` → scoped models (`--models`/`enabledModels`, new sessions only; saved default in scope wins) → resumed session `model_change {provider, modelId}` re-resolved against the live runtime → settings `defaultProvider`/`defaultModel` → first available model with configured auth → `kDefaultModel`; the settings-default and resume levels require configured auth (`model && hasConfiguredAuth`) and the final fallback is availability-based, so nothing configured never silently wins | `packages/coding-agent/src/core/model-resolver.ts` `findInitialModel`/`restoreModelFromSession`; `main.ts` `buildSessionOptions`; `sdk.ts` | `src/coding_agent/runtime/SessionFactory.cpp` (`resolve_cli_request_model`, `resolve_sdk_public_model`, `runtime_default_model`, live availability refresh), `include/cch/coding_agent/ModelRuntime.hpp` (`has_configured_auth`, `get_available`) | `ModelResolutionTest` (`[model-resolution]`/`[issue353]`): one test per precedence level — CLI `--model`, scoped first/`saved-in-scope`, resume restore, resume-without-auth fallback + `resume_model_unresolved` diagnostic, unauthenticated settings default skipped, first-available-with-auth (CLI + SDK public), nothing-configured → `kDefaultModel` streaming failure `"Unknown provider: unknown"`; T08 `[issue357]` rows pin pi's `restoreModelFromSession` fallback-message text for both reasons |
| 21 | Thinking-level changes persist as a `thinking_level_change` session entry plus the global settings default (`supportsThinking() || level !== "off"`), so resume restores the level exactly like pi — session creation resolves the level as resumed `thinking_level_change` → settings `defaultThinkingLevel` → `DEFAULT_THINKING_LEVEL`, clamped against the resolved first real model (T03 clamping applied through the resolution path); T08 gates the resumed-entry branch with pi's `hasThinkingEntry` scan | `packages/coding-agent/src/core/agent-session.ts` `setThinkingLevel` (`appendThinkingLevelChange` + `setDefaultThinkingLevel` gated on `supportsThinking()`); `packages/coding-agent/src/core/settings-manager.ts` `setDefaultThinkingLevel`; `sdk.ts` thinking-level chain | `src/coding_agent/runtime/AgentSessionRuntime.{hpp,cpp}` (`set_thinking_level`, config `default_thinking_level`), `include/cch/agent/Agent.hpp` + `src/agent/Agent.cpp` (`Agent::set_thinking_level` clamp), `src/coding_agent/SettingsManager.cpp` (`set_default_thinking_level`), `src/coding_agent/Sdk.cpp` (`AgentSession::set_thinking_level`), `include/cch/coding_agent/Sdk.hpp` | `ModelResolutionTest` `[thinking-persistence]`/`[issue353]` (entry + settings write, resume restore, resumed-without-entry uses settings default, fresh session requests settings default, reasoning-model `off` gate, clamp + invalid + no-op); `AgentCoreEvidenceTest`-style golden `thinking-persistence.json`; `SettingsManagerTest` `[issue353]`; `ModelRuntimeSeamTest` `[issue353]`; `SessionTreeTest`/`SessionRoundTripGoldenTest` `[issue357]` derived-state rows (`hasThinkingEntry` flag) |
| 22 | Full pi v3 session wire contract: all eleven entry types (`message`, `thinking_level_change`, `model_change`, `active_tools_change`, `compaction`, `branch_summary`, `custom`, `custom_message`, `label`, `session_info`, `leaf`) round-trip with pi's exact field presence, null-vs-missing distinctions, ordering (`id, parentId, timestamp, type, …`), and active-path semantics (root `parentId: null`, `leaf.targetId: null`, label clear via undefined label, `custom.data` absent vs `null`, `compaction` `firstKeptEntryId`/`retainedTail`/`usage`/`fromHook` absent-vs-present, `activeToolNames` wire field); committed golden proves byte-level interoperability | `packages/agent/src/harness/session/session.ts` (entry shapes), `jsonl-store.ts` `appendEntry` (JSON.stringify semantics), `harness/types.ts` (entry types incl. `CompactionEntry.retainedTail/usage`, `CustomEntry.data?`, `LeafEntry.targetId: string \| null`) | `include/cch/harness/session/SessionEntry.hpp` (value structs), `src/harness/session/EntrySerializer.{hpp,cpp}` (DTOs in pi field order, `NullableString` null-vs-missing, `serialize_entry` round-trip writer), `src/harness/session/JsonlSessionStore.cpp` (append path) | `SessionRoundTripGoldenTest` `[issue356][golden]` (byte-exact re-serialization of every golden line) + `[issue356]` field/null/active-path rows → `session-roundtrip.jsonl` |
| 23 | Context projection per entry type matches pi: `custom` omitted from model context by default, `custom_message` → CustomMessage, `branch_summary` → branchSummary message (only when it carries a summary), `compaction` → compactionSummary + retained tail (pi `defaultContextEntryTransform` + `sessionEntryToContextMessages`), `label` → `getLabel` (last-wins, trimmed, clearable), `session_info` → session name (last non-blank trimmed name) | `packages/agent/src/harness/session/session.ts` `sessionEntryToContextMessages` / `defaultContextEntryTransform` / `HydratedSessionState` (`getLabel`, `getSessionName`) | `src/harness/session/SessionTree.{hpp,cpp}` (`buildSessionContext`, `emitCompactionMessages`, `emitEntryMessage`, `get_label`, `get_session_name`) | `SessionRoundTripGoldenTest` `[projection]` rows → `projection-context.json`, `projection-branch-context.json` |
| 24 | Rebuilt session context drives into the Agent through the fake-`ModelRuntime` seam and the model sees exactly what pi's model sees: the recorded `streamSimple` context carries the projected messages, and the model-facing conversion matches pi `convertToLlm` byte-for-byte (custom → user text, branchSummary → user with `BRANCH_SUMMARY_PREFIX/SUFFIX`, compactionSummary → user with `COMPACTION_SUMMARY_PREFIX/SUFFIX`, bash excluded when `excludeFromContext`) | `packages/agent/src/harness/messages.ts` `convertToLlm` + prefix/suffix constants (verbatim `</summary>` branch suffix, `\n</summary>` compaction suffix) | `src/agent/AgentLoop.cpp` (context → `streamSimple`), `include/cch/ai/Message.hpp` (LLM conversion helpers + constants), `src/ai/api/MessageConversion.cpp` (adapter conversion) | `SessionRoundTripGoldenTest` `[agent][projection]` rows → `projection-context-llm.json`, `projection-branch-context-llm.json` |
| 25 | `buildSessionContext` derives `thinkingLevel` (default `"off"`, last `thinking_level_change` wins), `model` (last `model_change` **or** assistant message's provider/model wins), and `activeToolNames` (last `active_tools_change` wins, copied) from the active branch exactly like pi's `deriveSessionContextState`; resume re-resolves only the derived `model_change {provider, modelId}` against the live runtime and produces pi's `restoreModelFromSession` fallback message when the model is gone (`model no longer exists`) or unauthenticated (`no auth configured`), then continues through the resolution chain; the derived thinking level and model flow into the Agent's turn options at the fake-`ModelRuntime` seam, and the session file persists only `provider`/`modelId` (no auth material, #327 / ADR 0031) | `packages/agent/src/harness/session/session.ts` `deriveSessionContextState`; `packages/coding-agent/src/core/model-resolver.ts` `restoreModelFromSession`; `packages/coding-agent/src/core/sdk.ts` (`hasThinkingEntry` gating, `modelFallbackMessage`) | `src/harness/session/SessionTree.{hpp,cpp}` (`buildSessionContext` derived state), `include/cch/harness/session/SessionResume.hpp` (`thinking_level`/`has_thinking_level_entry`), `src/coding_agent/runtime/SessionLifecycle.cpp` (resumed-entry gating), `src/coding_agent/runtime/SessionFactory.cpp` (`resume_restore_failure_reason`, `resume_model_unresolved` diagnostic) | `SessionTreeTest` `[issue357]` derived-state rows (every entry type, last-wins, assistant-message override, defaults); `SessionRoundTripGoldenTest` `[issue357]` → `derived-session-state.json` + turn-options seam; `ModelResolutionTest` `[issue357]` resume fallback-message rows (both reasons) + session-file containment row |

### Recorded divergences preserved (unchanged by this ticket)

- Bounded steer/follow-up queues remain the recorded ADR 0022 divergence (pi unbounded).
- The six-category terminal payload stays a recorded #326 C++ enrichment.
- No default turn limit stays ADR 0015 (explicit `max_turns` caps only).
- `transport` is fixed per adapter instead of a per-request option (the codex adapter is
  WebSocket-first with narrow SSE fallback, the Responses/Anthropic family plain SSE), matching
  the pi-ai wire goldens and [#329].
- C++ tool adapters keep ADR 0016's explicit parallel-safety opt-in (`concurrency()` defaults to
  `Exclusive`, the built-ins stay exclusive because their shared execution environment has no
  concurrent-use contract); a batch containing such a tool runs through pi's sequential override.
  This is the C++-flavored expression of pi's `executionMode`, not a scheduling divergence.
- The C++ session header carries `provider`/`model` extension fields beyond pi's v3 header
  (`type`/`version`/`id`/`timestamp`/`cwd`/`parentSession?`/`metadata?`). pi's `parseHeader`
  tolerates unknown fields, and #327 resume re-resolution derives the model from `model_change`,
  so the header values are not authoritative; this is recorded as a tolerated C++ extension per
  ADR 0009 (narrowing it to pi's exact header is deferred to the coding-agent module).
- C++ resume parsing requires assistant `usage` (the #17/#19 contract), which pi's type marks
  optional; pi-written sessions in practice always carry usage, and pi reads C++ files fine.

## Gate notes

- The fake `ModelRuntime` is the only seam used here (no live keys, no network validation);
  scripted responses and terminal categories drive every golden deterministically.
- E2E (agent-harness-through-`streamSimple`, TUI/CLI full-chain acceptance) belongs to the
  pi-tui / pi-coding-agent gates; this gate covers harness-level unit evidence.

[#2]: https://github.com/lanshengzhi/cpp-coding-harness/issues/2
[#326]: https://github.com/lanshengzhi/cpp-coding-harness/issues/326
[#329]: https://github.com/lanshengzhi/cpp-coding-harness/issues/329
[#330]: https://github.com/lanshengzhi/cpp-coding-harness/issues/330
[#331]: https://github.com/lanshengzhi/cpp-coding-harness/issues/331
[#349]: https://github.com/lanshengzhi/cpp-coding-harness/issues/349
[#350]: https://github.com/lanshengzhi/cpp-coding-harness/issues/350
[#351]: https://github.com/lanshengzhi/cpp-coding-harness/issues/351
[#352]: https://github.com/lanshengzhi/cpp-coding-harness/issues/352
[#353]: https://github.com/lanshengzhi/cpp-coding-harness/issues/353
[#354]: https://github.com/lanshengzhi/cpp-coding-harness/issues/354
[#355]: https://github.com/lanshengzhi/cpp-coding-harness/issues/355
[#356]: https://github.com/lanshengzhi/cpp-coding-harness/issues/356
[#357]: https://github.com/lanshengzhi/cpp-coding-harness/issues/357
