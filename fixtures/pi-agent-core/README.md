# pi-agent-core compatibility fixtures

The committed evidence bundle for the pi-agent-core completion gate ([#330]/[#331], spec
[#349]). This directory mirrors the `fixtures/pi-ai/` strategy: every fixture is compared by
tests in this repository against the C++ surface, so the gate's evidence is one checklist away.
No fixture value is a live credential or derived from one; all strings are distinguishable
dummy values (see [Sanitization rules](#sanitization-rules)).

This file records only the capabilities landed so far (T01 [#350], T02 [#351], T03 [#352], T04 [#353], T05 [#354], T06 [#355], T07 [#356], T08 [#357], T09 [#358], T10 [#359], T11 [#360], T12 [#361]); the
capability checklist grows with each subsequent ticket (T12–T14, blockers-first per parity map [#2]), and
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

### Compaction machinery goldens (`summarization-request.json`, `compaction-persistence.jsonl`, `compaction-rebuild.json`)

The committed compaction goldens ([#358]): the machinery lives in the harness module
(`src/harness/compaction/`, mirroring pi `packages/agent/src/harness/compaction/compaction.ts`); the manual
trigger lives in the session-assembly layer (`AgentSessionRuntime::compact`, mirroring pi
`AgentSession.compact`), per the harness/AgentSession split. The trigger policy (overflow
compact-and-retry-once, threshold) is T10's half; only the machinery and the manual trigger land here.

- `summarization-request.json` — one summarization request captured through the recording fake
  `ModelRuntime` at the stream seam: `cacheRetention: "none"` and a fresh session id (injected
  deterministic `summarization-session-1`, proving each request isolates routing and avoids
  unreusable cache writes exactly like pi's `completeSimpleWithRetries`), `maxTokens` 1600
  (`floor(0.8 * reserveTokens)`), `reasoning` forwarding for a thinking model, the verbatim
  `SUMMARIZATION_SYSTEM_PROMPT`, and the `<conversation>`-wrapped summarization prompt.
- `compaction-persistence.jsonl` — the appended `compaction` entry line with pi's field set
  (`summary`, `firstKeptEntryId`, `tokensBefore`, `retainedTail` messages, `details`
  `{readFiles, modifiedFiles}`, `usage`, explicit `fromHook: false`), entry `id`/`timestamp`
  normalized to placeholders (generated values, per the `thinking-persistence.json` precedent).
- `compaction-rebuild.json` — the rebuilt context after compaction: `compactionSummary`
  (summary + `tokensBefore`) plus the retained tail messages, so the next prompt resumes exactly
  like pi (`agent.state.messages = sessionContext.messages`).

`CompactionTest` `[issue358]` compares all three; `AgentSessionCompactionTest` `[issue358]` covers the
manual-trigger lifecycle (idle persistence + context rebuild, abort-in-flight, too-small and
in-memory rejections) through the scripted fake `Models` seam.

### Automatic trigger fixture (`overflow-recovery-message.txt`)

The committed automatic-trigger golden ([#359]): the verbatim overflow-recovery failure message the
session prompt fails with when a second context overflow follows one compact-and-retry attempt,
byte-compared by `AgentSessionCompactionTest` `[issue359]`. The trigger policy lives in the
session-assembly layer (`AgentSessionRuntime::check_auto_compaction`/`run_auto_compaction`, mirroring
pi `AgentSession._checkCompaction`/`_runAutoCompaction` on top of the T09 machinery): overflow error
terminals compact and retry the turn exactly once (the error message stays in session history but is
dropped from the retry's live context), a second overflow fails with the verbatim message, threshold
compaction (`contextTokens > contextWindow - reserveTokens`, `enabled`/`reserveTokens` 16384/
`keepRecentTokens` 20000 from the `compaction` settings object with pi defaults) compacts with no
retry, and the pre-prompt check catches aborted/over-threshold responses before the next prompt.

### Turn auto-retry lifecycle golden (`auto-retry-lifecycle.json`)

The committed turn auto-retry golden ([#361], T12): the pi `AgentSessionEvent` sequence plus the
model-call count for one transient-error-then-success prompt, captured through the scripted fake
`Models` seam with `settings.retry {enabled: true, maxRetries: 3, baseDelayMs: 1}` (pi's own retry
tests use the same `baseDelayMs: 1` override). Two facts are pinned in one artifact:

- `auto_retry_start {attempt: 1, maxAttempts: 3, delayMs: 1, errorMessage: "overloaded_error"}` —
  the backoff event pi's `_prepareRetry` emits before the sleep (`delayMs = baseDelayMs * 2^(attempt-1)`);
- `auto_retry_end {success: true, attempt: 1}` — pi's `message_end` handler emits the success event
  at the first non-error assistant message, resetting the retry counter.

`modelRequests: 2` pins re-entry through the agent continuation mechanism: exactly one retry call
after the failed attempt, matching pi's retry test (`expect(created.getCallCount()).toBe(2)`).
The default-path delay (`delayMs: 2000`, `maxAttempts: 3`) and the exponential schedule
(2/4/8 ms at `baseDelayMs: 2`) are asserted in `TurnAutoRetryTest` `[issue361]` from the same
`auto_retry_start` payloads.

### Terminal matrix

The six-category terminal matrix (`model_source`, `model_validation`, `provider`, `stream`,
`auth`, `oauth`) is a test matrix in `ModelRuntimeSeamTest` (`[issue351]`), not a fixture: each
row scripts a terminal failure of one category through the fake runtime and asserts exactly one
terminal event plus an agreeing final `AssistantMessage`, with the category flowing through the
single `util::Expected` error value (the #326 six-category channel).

### Re-auth guidance goldens (`re-auth-guidance-*.txt`)

The committed verbatim re-auth guidance goldens ([#360], T11): pi's two branches at both trigger
points, byte-compared by `ReAuthGuidanceTest` `[issue360]`.

- `re-auth-guidance-preflight-no-key.txt` — the prompt preflight (pi `agent-session.ts`
  `prompt()` `hasConfiguredAuth` check) failing a keyless provider with pi's verbatim
  `formatNoApiKeyFoundMessage` text through the `auth` category of the single `util::Expected`
  channel (no second exception hierarchy);
- `re-auth-guidance-preflight-oauth.txt` — the same preflight on an OAuth-typed provider with no
  stored credential failing with pi's verbatim `Run '/login kimi-coding' to re-authenticate.`
  re-auth branch;
- `re-auth-guidance-request-no-key.txt` — request time (pi `_getRequiredRequestAuth`): an
  `auth`-category terminal from the stream is rewritten to the no-key branch in the terminal
  `AssistantMessage` (category preserved, exactly-one-terminal contract);
- `re-auth-guidance-request-oauth.txt` — request time on an `oauth`-category terminal (dead
  credentials — refresh/derivation failure) rewritten to the verbatim re-auth branch.

The guidance is owned by the session layer: `include/cch/coding_agent/AuthGuidance.hpp` ports pi
`auth-guidance.ts` (`formatNoApiKeyFoundMessage`/`getProviderLoginHelp`, unknown provider renders
as "the selected model"), the preflight check lives in
`AgentSessionRuntime::preflight_auth_guidance` (skipped for the `kDefaultModel` placeholder, which
keeps its ordinary "Unknown provider: unknown" streaming failure), and the request-time rewrite is
a session-layer stream decorator (`src/coding_agent/runtime/AuthGuidanceStreamRuntime.hpp`) that
wraps the session's `ModelRuntime` for the Agent's stream and the summarization seam — the pi-ai
`ModelRuntime`/`getAuth` surface is consumed unchanged. The fake-`ModelRuntime` seam scripts the
`auth`/`oauth` terminals; the session-level tests drive both branches through scripted clients.
The docs-path lines use the deterministic default `~/.pi/docs` (see the divergences below).

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
| 26 | Compaction machinery (harness module, mirroring pi's harness/AgentSession split): `prepareCompaction` cut-point selection with `keepRecentTokens` 20000 and split-turn handling (valid cut points are user/assistant/bash/custom/branchSummary/compactionSummary messages and branch_summary/custom_message entries — never tool results; a cut at an assistant message splits its turn, summarizing the turn prefix separately), token estimation (chars/4, fixed 4800-char image estimate, last-assistant-usage context estimate), previous-summary iteration from the last compaction, and file-operation extraction (`read`/`write`/`edit` tool calls plus the previous pi-generated compaction's `{readFiles, modifiedFiles}` details) | `packages/agent/src/harness/compaction/compaction.ts` (`findCutPoint`, `findTurnStartIndex`, `findValidCutPoints`, `estimateTokens`, `estimateContextTokens`, `prepareCompaction`, `extractFileOperations`), `utils.ts` (`extractFileOpsFromMessage`, `computeFileLists`, `formatFileOperations`, `serializeConversation`) | `src/harness/compaction/Compaction.{hpp,cpp}` (`find_cut_point`, `find_turn_start_index`, `estimate_tokens`, `estimate_context_tokens`, `prepare_compaction`, `extract_file_ops_from_message`, `compute_file_lists`, `format_file_operations`, `serialize_conversation`), `src/harness/session/SessionTree.{hpp,cpp}` (path-based `buildSessionContext`) | `CompactionTest` `[issue358]`: cut-point/turn-start edge cases, split-turn preparation with prior details, previous-summary preparation, token/usage estimation rows, file-tags + details rows |
| 27 | Summarization requests are issued through `streamSimple` with `cacheRetention: "none"` and a fresh session id per request (`completeSimpleWithRetries`), so compaction never pollutes the session's cache affinity; `maxTokens` = `min(floor(0.8 * reserveTokens), model.maxTokens)` for history and `0.5 * reserveTokens` for turn prefixes; `reasoning` forwards the thinking level only for reasoning models; aborted/error terminals map to pi's compaction errors; split-turn compactions combine history + prefix summaries and their usage | `packages/agent/src/harness/compaction/compaction.ts` (`completeSimpleWithRetries`, `generateSummaryWithUsage`, `generateTurnPrefixSummary`, `combineUsage`, `compact`) | `src/harness/compaction/Compaction.{hpp,cpp}` (`compact`, `generate_summary_with_usage`, `combine_usage`, verbatim `kSummarization*Prompt` constants), summarization seam wired to `ModelRuntime::stream_simple` by the session assembly | `CompactionTest` `[issue358]` → `summarization-request.json` (cacheRetention `none`, fresh session id, maxTokens 1600, reasoning); split-turn two-request test (distinct fresh session ids, combined usage); summarization-failed/aborted/invalid-session error rows |
| 28 | Compaction persists a `CompactionEntry` with pi's fields (`summary`, `firstKeptEntryId`, `tokensBefore`, `retainedTail`, `details {readFiles, modifiedFiles}`, `usage`, explicit `fromHook: false`) and rebuilds the live context as compactionSummary + retained tail (`agent.state.messages = sessionContext.messages`), so the next prompt resumes exactly like pi | `packages/coding-agent/src/core/agent-session.ts` `compact` (`sessionManager.appendCompaction`, `buildSessionContext`, `estimateMessagesTokens`); `packages/agent/src/harness/session/session.ts` `appendCompaction` | `src/coding_agent/runtime/AgentSessionRuntime.cpp` (`compact_impl`: `append_compaction` + `open_as_tree`/`buildSessionContext` rebuild), `src/harness/session/JsonlSessionStore.cpp` (`append_compaction` with `retainedTail`/`usage`), `src/agent/Agent.cpp` (`AgentMessageAccess::replace_messages`), `include/cch/coding_agent/Sdk.hpp` (`CompactionResult`) | `CompactionTest` `[issue358]` → `compaction-persistence.jsonl`, `compaction-rebuild.json`; `AgentSessionCompactionTest` `[issue358]` idle persistence + next-prompt context at the stream seam |
| 29 | Manual compaction trigger (`AgentSession.compact`) aborts the active run first (then waits for it to settle) and compacts; rejects when the session is closed, a compaction is in flight, no model is selected (`No model selected.\n\nThen use /model to select a model.` — login help is Native TUI presentation, ADR 0032), there is nothing to compact (`Nothing to compact (session too small)`), or the session was already compacted (`Already compacted`) | `packages/coding-agent/src/core/agent-session.ts` `compact` (`_disconnectFromAgent` + `abort()` + `waitForIdle`, `formatNoModelSelectedMessage`) | `src/coding_agent/runtime/AgentSessionRuntime.{hpp,cpp}` (`compact`, prompt-settled signal), `src/coding_agent/Sdk.cpp` (`AgentSession::compact` via `AgentSessionPromptAccess`) | `AgentSessionCompactionTest` `[issue358]`: idle compaction, abort-in-flight (gated prompt observes the cancellation and the run settles with the ordinary aborted terminal), too-small and in-memory rejections |
| 30 | Compaction settings default to pi's `DEFAULT_COMPACTION_SETTINGS` (`enabled: true`, `reserveTokens: 16384`, `keepRecentTokens: 20000`); the machinery sits in the agent module (harness) and the trigger policy in session assembly; overflow compact-and-retry-once and threshold triggers are T10's half, not present here | `packages/agent/src/harness/compaction/compaction.ts` `DEFAULT_COMPACTION_SETTINGS` | `src/harness/compaction/Compaction.hpp` (`kDefaultCompactionSettings`), `src/coding_agent/runtime/AgentSessionRuntime.cpp` (uses the defaults) | `CompactionTest`/`AgentSessionCompactionTest` `[issue358]` cut-point rows at 20000 and lifecycle rows through the SDK |
| 31 | Automatic compaction trigger policy (session assembly, pi `_checkCompaction`/`_runAutoCompaction`): context-overflow terminal errors compact and retry the turn exactly once (the overflow error message stays in session history but is removed from live state before the retry, and again after the context rebuild); a second overflow in the same prompt fails with pi's verbatim `Context overflow recovery failed after one compact-and-retry attempt…` message; a successful response whose usage already exceeds the window compacts without retrying (`willRetry = stopReason !== "stop"`); threshold compaction fires at `contextTokens > contextWindow − reserveTokens` and never retries; overflow never routes to turn auto-retry (T12's boundary); the post-run check runs after every retry exactly like pi's `while (await this._handlePostAgentRun()) await this.agent.continue()`, and the pre-prompt check (skipAbortedCheck=false) catches aborted/over-threshold responses before the next prompt | `packages/coding-agent/src/core/agent-session.ts` `_checkCompaction`, `_runAutoCompaction`, `_handlePostAgentRun`, `_overflowRecoveryAttempted` reset on user `message_start`; `packages/agent/src/agent.ts` `continue()` / `agent-loop.ts` `runAgentLoopContinue` | `src/coding_agent/runtime/AgentSessionRuntime.{hpp,cpp}` (`check_auto_compaction`, `run_auto_compaction`, post-run loop in `run_agent_loop`, pre-prompt check in `run_prompt`, `overflow_recovery_attempted_`), `src/agent/Agent.{hpp,cpp}` (`continue_run`), `src/agent/AgentLoop.{hpp,cpp}` (user-message-less `continue_with`), `src/agent/AgentMessageAccess.hpp` (`pop_trailing_assistant`) | `AgentSessionCompactionTest` `[issue359]`: overflow compact-and-retry-once (retry request context at the fake-`ModelRuntime` seam: compactionSummary + retained tail, overflowing user message last, no error terminal), second-overflow verbatim failure → `overflow-recovery-message.txt`, threshold compaction with no retry, disabled-settings suppression, pre-prompt aborted-response compaction |
| 32 | Overflow detection + threshold arithmetic: `isContextOverflow` — provider error-message patterns (excluding rate-limit/throttling patterns that would false-positive), silent overflow when `usage.input + usage.cacheRead` exceeds the window, and length-stop zero-output fill — plus `shouldCompact` (`contextTokens > contextWindow - reserveTokens`, disabled settings never compact) | `packages/ai/src/utils/overflow.ts` `isContextOverflow`/`OVERFLOW_PATTERNS`/`NON_OVERFLOW_PATTERNS`; `packages/agent/src/harness/compaction/compaction.ts` `shouldCompact` | `src/harness/compaction/Compaction.{hpp,cpp}` (`is_context_overflow`, `should_compact`) | `CompactionTest` `[issue359]`: per-pattern overflow matrix + non-overflow exclusions + silent/length usage cases + unknown-window gate; threshold boundary rows (strict `>`, custom reserveTokens, zero-window arithmetic) |
| 33 | Compaction settings surface: the nested `compaction {enabled, reserveTokens, keepRecentTokens}` settings object loads from both scopes with per-field deep merge (project wins per field), mistyped values fall back to the pi defaults at resolution, and the trigger policy resolves the effective settings exactly like pi `getCompactionSettings` | `packages/coding-agent/src/core/settings-manager.ts` `getCompactionSettings`/`getCompactionReserveTokens`/`getCompactionKeepRecentTokens` (`settings.compaction?.x ?? default`) | `include/cch/coding_agent/Settings.hpp` (`UserCompactionSettings`, `UserSettings::compaction`), `src/coding_agent/SettingsManager.cpp` (parse + per-field merge), `src/coding_agent/runtime/AgentSessionRuntime.cpp` (`effective_compaction_settings`) | `SettingsManagerTest` `[issue359]` (load/merge/mistyped-fallback/reject); `AgentSessionCompactionTest` `[issue359]` disabled-settings path |
| 34 | Re-auth guidance, both pi branches verbatim at preflight: a real model whose provider resolves no auth fails the prompt before any stream with `formatNoApiKeyFoundMessage` (no-key branch, unknown provider renders as "the selected model") or the "Credentials may have expired … Run '/login X'" branch (OAuth-typed provider with no stored credential), as an `auth`-category error through the single `util::Expected` channel; the `kDefaultModel` placeholder is skipped and keeps its ordinary "Unknown provider: unknown" streaming failure | `packages/coding-agent/src/core/auth-guidance.ts` (`formatNoApiKeyFoundMessage`, `getProviderLoginHelp`, `UNKNOWN_PROVIDER`); `packages/coding-agent/src/core/agent-session.ts` `prompt()` (`hasConfiguredAuth || (await checkAuth(provider)) !== undefined`) | `include/cch/coding_agent/AuthGuidance.hpp`, `src/coding_agent/runtime/AgentSessionRuntime.{hpp,cpp}` (`preflight_auth_guidance` in `run_prompt`, using `ModelRuntime` `has_configured_auth`/`check_auth`/`is_using_oauth` unchanged) | `ReAuthGuidanceTest` `[issue360]`: preflight no-key (keyless `alpha` via CLI `--model`) and preflight OAuth (built-in `kimi-coding`, no credential, `KIMI_API_KEY` unset) → `re-auth-guidance-preflight-{no-key,oauth}.txt` |
| 35 | Re-auth guidance, both pi branches verbatim at request time (pi `_getRequiredRequestAuth`): an `auth`/`oauth`-category terminal from `streamSimple` is rewritten to the guidance — no-key branch for `auth` terminals on non-OAuth providers, the re-auth branch for `auth` terminals on OAuth providers and for `oauth` terminals (dead credentials — refresh/derivation failures stay in `auth.json`, no deletion, no implicit login) — preserving the terminal-error-event contract (exactly one `error` terminal plus an agreeing final `AssistantMessage`) and the six-category channel (the `auth`/`oauth` code flows through the single `util::Expected` error value); the rewrite is owned by the session layer through a stream decorator applied to the Agent's runtime and the summarization seam (`_getSummarizationRequestAuth`), with the pi-ai `ModelRuntime`/`getAuth` surface consumed unchanged | `packages/coding-agent/src/core/agent-session.ts` `_getRequiredRequestAuth`/`_getSummarizationRequestAuth` (isUsingOAuth branch selection, `formatNoApiKeyFoundMessage`); `packages/ai/src/models.ts` `streamSimple` applyAuth (the `auth`/`oauth` terminal sources) | `include/cch/coding_agent/AuthGuidance.hpp`, `src/coding_agent/runtime/AuthGuidanceStreamRuntime.hpp` (wraps `ModelRuntime::stream_simple`, category-preserving rewrite), `src/coding_agent/runtime/AgentSessionRuntime.cpp` (decorator wired into the Agent and the summarization seam) | `ReAuthGuidanceTest` `[issue360]`: request-time no-key/oauth through scripted clients → `re-auth-guidance-request-{no-key,oauth}.txt`; the four decorator rows through the fake-`ModelRuntime` seam (auth → no-key, auth + OAuth provider → re-auth, oauth terminal → re-auth, non-auth/success pass-through); summarization-seam row (manual compaction fails with the guidance embedded in the compaction error) |
| 36 | Turn auto-retry policy (session assembly, pi `_handlePostAgentRun`/`_prepareRetry`): enabled by default with pi's `settings.retry` defaults (`enabled: true`, `maxRetries` 3, `baseDelayMs` 2000), exponential backoff `baseDelayMs * 2^(attempt-1)`, retry re-enters through the agent continuation mechanism (`agent.continue()` / C++ `continue_run`), the failed assistant message is removed from live state but retained in session history, and retry runs before the automatic compaction check exactly like pi | `packages/coding-agent/src/core/agent-session.ts` `_handlePostAgentRun`/`_prepareRetry`; `settings-manager.ts` `getRetrySettings` (`enabled ?? true`, `maxRetries ?? 3`, `baseDelayMs ?? 2000`); `packages/agent/src/agent.ts` `continue()` | `src/coding_agent/runtime/AgentSessionRuntime.{hpp,cpp}` (retry-first post-run loop in `run_agent_loop`, `prepare_retry`, `effective_retry_settings`, `is_retryable_error`), `include/cch/coding_agent/Settings.hpp` (`UserRetrySettings`), `src/coding_agent/SettingsManager.cpp` (parse + per-field merge), `src/coding_agent/runtime/AgentSessionRuntime.cpp` | `TurnAutoRetryTest` `[issue361]`: retry-succeeds, max-retries exhaustion, defaults + exponential backoff schedule, disabled-settings, live-state-removal + session-history retention → `auto-retry-lifecycle.json` |
| 37 | Retryability classification (`isRetryableAssistantError`): an `error` terminal whose message matches a transient provider/network pattern retries — overloaded, rate-limit/too-many-requests, 429/5xx (500/502/503/504/524), service/server/internal errors, provider-returned-error, network/connection/socket/fetch failures, DNS (`ENOTFOUND`/`EAI_AGAIN`), WebSocket close/error text, premature stream endings, "retry delay", explicit retry guidance, `ResourceExhausted` — while quota/billing/provider-limit patterns never retry (`GoUsageLimitError`/`FreeUsageLimitError`, "Monthly usage limit reached", "available balance", `insufficient_quota`, "out of budget", "quota exceeded", billing), with the non-retryable limit check winning when both match | `packages/ai/src/utils/retry.ts` `isRetryableAssistantError` + `NON_RETRYABLE_PROVIDER_LIMIT_ERROR_PATTERN`/`RETRYABLE_PROVIDER_ERROR_PATTERN` | `src/ai/utils/RetryClassifier.{hpp,cpp}` (`is_retryable_assistant_error`) | `RetryClassifierTest` `[issue361]` pattern matrix (transient retries, quota never, non-error/empty rejections, case-insensitivity); `TurnAutoRetryTest` network-retry + quota-exclusion rows |
| 38 | Context overflow never enters the retry path: `is_retryable_error` excludes `isContextOverflow` (T10) before the classifier, so overflow routes to compaction compact-and-retry-once and the two recovery paths never interfere | `packages/coding-agent/src/core/agent-session.ts` `_isRetryableError` (`isContextOverflow` first, "Context overflow is handled by compaction, not retry") | `AgentSessionRuntime::is_retryable_error` (overflow exclusion before the classifier) | `TurnAutoRetryTest` overflow-routing row (no auto_retry events, exactly one model call, overflow error retained in session history) |
| 39 | `auto_retry_start`/`auto_retry_end` events (pi `AgentSessionEvent`): `auto_retry_start {attempt, maxAttempts, delayMs, errorMessage}` fires before each backoff sleep; `auto_retry_end {success, attempt, finalError?}` fires at the first non-error assistant message after retries (success), at final-failure exhaustion (with the final error message), and at an aborted backoff ("Retry cancelled") — retry is observable and cancellable like pi | `packages/coding-agent/src/core/agent-session.ts` `_prepareRetry` (start emission), `_handleAgentEvent` message_end (success reset), `_handlePostAgentRun` (failure emission), abort catch ("Retry cancelled") | `include/cch/coding_agent/AgentSessionEvent.hpp` (`AutoRetryStartEvent`/`AutoRetryEndEvent`, `AgentSessionEvent`), `AgentSessionRuntime::subscribe_session`/`emit_session_event`, `AgentSession::subscribe_session` (SDK) | `TurnAutoRetryTest` event-sequence rows + golden → `auto-retry-lifecycle.json` |
| 40 | Abort-interruptible backoff: the prompt-scoped stop token cancels the backoff timer; an abort during the sleep emits exactly one `auto_retry_end {success: false, finalError: "Retry cancelled"}` terminal outcome, the retry never starts, and the session stays reusable for the next prompt | `packages/coding-agent/src/core/agent-session.ts` `_prepareRetry` `sleep(delayMs, this._retryAbortController.signal)` catch branch; `abort()` (`abortRetry()`) | `AgentSessionRuntime::prepare_retry` (steady_timer + `std::stop_callback`; `operation_aborted`/`stop_requested` → cancelled) | `TurnAutoRetryTest` abort-during-backoff row (one end event, one model call, error retained in session history, reusable session) |

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
- Compaction is confined to persisted (JsonlSessionStore-backed) sessions: in-memory SDK sessions
  have no session file to persist a `CompactionEntry` into or to rebuild context from, so the
  manual trigger rejects them (`compaction requires a persisted session file`). pi has no
  in-memory session target, so this is a consequence of the C++ in-memory extension, not a pi
  divergence.
- The summarization prompt's user-message timestamp is 0 (deterministic); pi stamps `Date.now()`.
  Provider wire messages never carry the timestamp, so this is not observable on the wire.
- The manual-trigger no-model error omits pi's `getProviderLoginHelp()` tail (`/login` guidance is
  Native TUI presentation, ADR 0032; the auth-guidance capability is a separate ticket).
- The no-key guidance's login-help docs lines use the deterministic default `~/.pi/docs`
  (`kDefaultAuthGuidanceDocsPath`) instead of pi's `getDocsPath()` (`<packageDir>/docs`): the
  harness cannot discover a pi install, and the committed goldens must be byte-stable. The
  message structure stays pi's verbatim `formatNoApiKeyFoundMessage`; hosts may override the
  docs path at the formatter call site.
- Request-time guidance maps every `auth`/`oauth`-category terminal to the verbatim branches;
  pi's `_getRequiredRequestAuth` rethrows non-no-key `getAuth` errors (e.g. credential-store
  failures) unchanged on the summarization path, while the C++ session layer rewrites them to
  the same guidance (the six-category channel carries the `auth` code either way; the dead-
  credential flow — refresh failure → `oauth` terminal → "Run '/login X'" — matches the #328
  record exactly).
- Threshold compaction treats an unknown (zero) `contextWindow` as no window instead of pi's
  `contextWindow ?? 0` arithmetic (which would compact on every turn, since any context exceeds
  `0 - reserveTokens`). pi's catalog models always carry a window, while the C++ placeholders
  (`kDefaultModel` and the test sentinel) carry none; error-based overflow still fires independent
  of the window, exactly like pi's `isContextOverflow` truthiness gate.
- The second-overflow recovery failure surfaces as the prompt error rather than a `compaction_end`
  event: the C++ session has no compaction event channel yet, so the prompt fails with pi's verbatim
  message and the second overflow error stays in live state and session history (pi reports the
  same failure through the event while completing the run).
- `overflow_recovery_attempted_` resets when a new user prompt starts instead of on every user
  `message_start` / non-error assistant `message_end`. The C++ loop runs to completion per prompt
  before the policy is consulted, so every reachable overflow sequence resets identically to pi.
- Turn auto-retry session events (`auto_retry_start`/`auto_retry_end`) are delivered through a
  dedicated session-event subscription (`AgentSession::subscribe_session`, pi `AgentSessionEvent`);
  the one-shot text and `--mode json` printers consume only Agent lifecycle events, so retry events
  do not appear there (the C++ session likewise has no compaction event channel — see the
  second-overflow note). Hosts subscribe the session-event channel directly.
- pi's `agent_end` events carry a computed `willRetry` field (`_willRetryAfterAgentEnd`); the C++
  `AgentEndEvent` carries no such field — retry observability flows through the session-event
  channel instead.
- The C++ reads `settings.retry {enabled, maxRetries, baseDelayMs}` with pi's defaults applied, but
  has no settings *write* API for retry yet (pi `SettingsManager.setRetryEnabled`); the toggle
  surface is deferred to the coding-agent module gate.

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
[#358]: https://github.com/lanshengzhi/cpp-coding-harness/issues/358
[#359]: https://github.com/lanshengzhi/cpp-coding-harness/issues/359
[#360]: https://github.com/lanshengzhi/cpp-coding-harness/issues/360
[#361]: https://github.com/lanshengzhi/cpp-coding-harness/issues/361
