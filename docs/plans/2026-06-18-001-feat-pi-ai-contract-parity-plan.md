---
title: "feat: Achieve pi-ai message/content/stream/provider parity"
type: "feat"
status: "completed"
date: "2026-06-18"
origin: "docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---

<!-- markdownlint-disable MD013 MD025 -->

# feat: Achieve pi-ai message/content/stream/provider parity

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.
**Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T2 checklist).

## Summary

Complete pi-ai contract parity by separating ToolCall from the Content variant to match pi's semantic model, adding diagnostics/cacheWrite1h/missing-type-fields, introducing the `OpenAICompletionsCompat` struct with all pi-defined compatibility flags, and implementing provider-specific transformations in the OpenAI provider adapter. All changes stay within the established passive-value/Glaze-isolated architecture.

**2026-06-24 drift note:** pi-ai has since moved stream/API-registry contracts into `types.ts`, `utils/event-stream.ts`, `models.ts`, `compat.ts`, and `api/lazy.ts`; `src/stream.ts` and `src/api-registry.ts` are absent in the current checkout. pi's `OpenAICompletionsCompat` also now includes routing, strict-mode, cache-control, session-affinity, long-retention, and chat-template fields beyond the 10 fields implemented by this completed plan. Treat those as deferred follow-up parity, not as behavior this plan delivered.

---

## Problem Frame

The pre-implementation cleanup (`docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`) established the structural seams — package-style CMake targets, provider registry, move-only event sinks, async shell execution — that make ai-contract parity feasible. The parity roadmap's T2 checklist identifies the remaining gaps: message/content types that diverge from pi's model, missing provider compatibility flags, and incomplete stream event semantics. Without closing these gaps, every subsequent provider or agent slice must work around the same mismatches.

---

## Requirements

- R1. ToolCall is separated from the `Content` variant — `Content = TextContent | ThinkingContent | ImageContent` and `AssistantContent = TextContent | ThinkingContent | ToolCallContent`. `AssistantMessage.content` uses `AssistantContent`; `UserMessage` and `ToolResultMessage` continue using `Content`.
- R2. Public contracts remain passive aggregate value types. No provider DTO or Glaze machinery leaks into domain headers. Architecture tests (`[architecture]`) continue to pass.
- R3. `AssistantMessage` gains `diagnostics` (`std::optional<std::vector<DiagnosticEntry>>`), `Usage` gains `cache_write_1h` (`std::optional<std::int64_t>`), and all existing forward-compatible fields (`text_signature`, `thinking_signature`, `redacted`, `thought_signature`) are preserved.
- R4. `OpenAICompletionsCompat` is introduced with all pi-defined fields (`supportsStore`, `supportsDeveloperRole`, `supportsReasoningEffort`, `supportsUsageInStreaming`, `maxTokensField`, `requiresToolResultName`, `requiresAssistantAfterToolResult`, `requiresThinkingAsText`, `requiresReasoningContentOnAssistantMessages`, `thinkingFormat`) and wired into `OpenAIStreamConfig` and `ProviderFactoryContext`.
- R5. Provider compatibility transformations (thinking-format mapping, tool-result naming, developer-role substitution, max-tokens field selection, reasoning-effort parameter wiring) are implemented in `src/ai/providers/OpenAIChatClient.cpp` and `src/ai/glaze/ProviderDtos.hpp` without leaking into domain types.
- R6. Stream event protocol semantics are verified against pi's `AssistantMessageEvent` contract. Existing 12-event protocol (`AssistantStartEvent` through `AssistantErrorEvent`) already has 1:1 alignment; no new event types are needed, but gaps in event payload fields are closed.
- R7. Glaze DTO mappings (`AiJson.hpp`) are updated for all new and changed types, with round-trip tests covering every new field and serialization path.

---

## Scope Boundaries

- This plan does not add new LLM providers (Anthropic, Google, etc.). Only the existing OpenAI-compatible provider path is extended.
- OAuth and subscription-provider flows remain explicitly deferred (per T2 checklist).
- `AnthropicMessagesCompat` and `OpenAIResponsesCompat` structs are deferred until corresponding providers exist.
- `StreamOptions` (temperature, maxTokens, signal, transport, sessionId, callbacks, headers, metadata) is deferred to T5 (runtime config parity) — only the compat flags that directly affect provider wire format are in scope.
- `UserMessage.content` string-mode support (pi allows `string | content[]`) is deferred — the C++ path already uses `vector<Content>` and a string shortcut can be added later without breaking the contract.
- T3–T8 work (agent hooks, session tree write, config/model resolution, extensions, TUI, SDK/RPC) is out of scope.
- No change to workspace containment, secret redaction, bash opt-in, or session permissions.

### Deferred to Follow-Up Work

- `AnthropicMessagesCompat` and `OpenAIResponsesCompat`: implement when corresponding provider paths exist (future T2+ slice).
- `StreamOptions` full interface (temperature, maxTokens, transport, sessionId, callbacks, headers, retry budget): T5 runtime config parity.
- `UserMessage` string content shortcut: implement when a consumer (e.g., CLI prompt construction) benefits from the simpler API.
- Provider retry/timeout at the HTTP transport level: these require the `StreamOptions` contract first (T5).

---

## Context & Research

### Relevant Code and Patterns

- **Content types** (`include/cch/ai/Content.hpp`): `Content = variant<TextContent, ThinkingContent, ImageContent, ToolCallContent>`. Already has `text_signature`, `thinking_signature`, `redacted`, `thought_signature` forward-compatible fields from pre-implementation cleanup.
- **Message types** (`include/cch/ai/Message.hpp`): `MessageVariant = variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage>`. `AssistantMessage` carries `api`, `provider`, `model`, `response_model`, `response_id`, `usage`, `stop_reason`, `error_message`, `timestamp`.
- **Usage** (`include/cch/ai/Usage.hpp`): Has `input`, `output`, `cache_read`, `cache_write`, `total_tokens`, and full `UsageCost` sub-object. Missing `cache_write_1h`.
- **Stream events** (`include/cch/ai/StreamEvent.hpp`): 12-event `AssistantStreamEvent` variant with 1:1 alignment to pi's `AssistantMessageEvent` protocol. Naming convention is C++ PascalCase vs pi camelCase — cosmetic only.
- **Agent lifecycle events** (`include/cch/agent/AgentEvent.hpp`): 13-type `AgentLifecycleEvent` variant wrapping AI-level stream events with turn metadata. Move-only event sink (`std::move_only_function`).
- **Provider client** (`include/cch/ai/providers/OpenAIChatClient.hpp`, `src/ai/providers/OpenAIChatClient.cpp`): `OpenAIStreamConfig` with basic fields (base_url, api_key, model, timeout). No compat flags. SSE chunk parsing → stream event conversion done inline.
- **Provider registry** (`include/cch/ai/ProviderRegistry.hpp`, `src/ai/ProviderRegistry.cpp`): `ProviderFactoryContext` with basic fields. Registered providers: `"fake"`, `"openai-compatible"`.
- **Glaze DTOs** (`include/cch/ai/glaze/AiJson.hpp`): `ContentDto` flat-struct-with-discriminator pattern. `to_dto()`/`*_from_dto()` free functions in `detail` namespace. `require_field<T>()` helper for required-field validation.
- **Provider wire DTOs** (`src/ai/glaze/ProviderDtos.hpp`): OpenAI-specific request/response DTOs — implementation layer only.

### Reference Contracts (pi)

- **`pi:packages/ai/src/types.ts`**: Defines `TextContent`, `ThinkingContent`, `ImageContent`, `ToolCall`, `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `Usage`, `StopReason`, `OpenAICompletionsCompat`, `AnthropicMessagesCompat`, `OpenAIResponsesCompat`, `StreamOptions`, `AssistantMessageDiagnostic`.
- **`pi:packages/ai/src/types.ts` + `pi:packages/ai/src/utils/event-stream.ts`**: `AssistantMessageEvent` protocol — start, text_start/delta/end, thinking_start/delta/end, toolcall_start/delta/end, done, error.
- **`pi:packages/ai/src/providers/openai-completions.ts`**: Provider adapter implementing compat transformations.

### Institutional Learnings

- **Never re-create structural seams**: the pre-implementation cleanup (`docs/plans/2026-06-16-002`) established package targets, provider registry, move-only events, and Glaze isolation. All new work extends these seams.
- **ContentDto flat-discriminator pattern is intentional**: chosen over Glaze's variant inference for stable model-visible discriminators (KTD5 from coroutine-glaze plan). New types must follow this pattern.
- **Post-review vulnerability checklist**: from the cleanup plan's post-merge fixes (commits `eff78c9`, `4087164`, `a122c44`, `ac6934f`, `4a8702d`, `0e8c875`): verify no Glaze/Provider DTO leakage, registry lifetime correctness, event sink safety, session durability, workspace containment, and CLI threading for every AI contract change.
- **Kimi Code plan** (`docs/plans/2026-06-11-001`) established the pattern for provider-specific compatibility through the OpenAI-compatible seam: API keys from env vars only, stripped from subprocesses, live tests opt-in gated.
- **No `docs/solutions/` directory exists** — this project has not yet accumulated a formal bug/pattern knowledge base. Capture concrete serialization issues or JSON quirks that surface during implementation.

---

## Key Technical Decisions

- **Separate ToolCall from Content variant** (aligns with pi): pi's `ToolCall` is a message-level element (`AssistantMessage.content = (TextContent | ThinkingContent | ToolCall)[]`), not a content block. C++ mirrors this with `AssistantContent = variant<TextContent, ThinkingContent, ToolCallContent>`. `UserMessage` and `ToolResultMessage` keep `Content = variant<TextContent, ThinkingContent, ImageContent>` unchanged.
- **OpenAICompletionsCompat as a passive struct in provider header**: the struct is a value type in `include/cch/ai/providers/OpenAIChatClient.hpp`, not a domain type in `include/cch/ai/`. It is provider-specific configuration, not a universal AI contract.
- **Compat transformations stay in `src/ai/providers/`**: `requiresToolResultName`, `requiresThinkingAsText`, `thinkingFormat` mapping, and all other wire-format adaptations are implemented in the provider adapter (`OpenAIChatClient.cpp` + `ProviderDtos.hpp`), never in domain types or Glaze DTOs.
- **Keep the 12-event stream protocol as-is**: the C++ `AssistantStreamEvent` variant already has 1:1 alignment with pi's `AssistantMessageEvent`. Only payload fields are extended (e.g., `diagnostics` on `AssistantDoneEvent`), not event types.
- **Characterization coverage before behavior changes**: add tests for existing compat-adjacent behavior (Kimi trailing-slash normalization, tool-call argument accumulation, SSE chunk dispatch) before introducing new transformations. This ensures regressions in the existing fake-provider and CLI paths are caught immediately.

---

## Open Questions

### Resolved During Planning

- **Align ToolCall model with pi?** → Yes, separate `ToolCallContent` from the `Content` variant, introduce `AssistantContent`.
- **Full OpenAICompat fields in one pass?** → Yes, implement all 10 pi-defined fields.
- **Test posture: characterization before changes?** → Yes, characterization coverage on existing stream/transform behavior before adding compat transformations.

### Deferred to Implementation

- Exact `DiagnosticEntry` fields: pi's `AssistantMessageDiagnostic` shape (which fields, which types) will be finalized against the pi source during implementation.
- `thinkingFormat` enum values and their exact wire-format mappings: defined during implementation from pi's `openai-completions.ts`.
- Whether `AssistantContent` needs its own Glaze DTO or reuses a modified `ContentDto` with a discriminator check: decided during implementation when the full serialization impact is visible.
- Exact `AgentLifecycleEvent` payload extensions for diagnostics: determined by which consumers (CLI printer, session writer) need the data.

---

## High-Level Technical Design

> *This illustrates the intended approach and is directional guidance for review, not implementation specification. The implementing agent should treat it as context, not code to reproduce.*

After this plan, the type hierarchy should be:

```
Content = TextContent | ThinkingContent | ImageContent
AssistantContent = TextContent | ThinkingContent | ToolCallContent

UserMessage.content = vector<Content>
AssistantMessage.content = vector<AssistantContent>
ToolResultMessage.content = vector<Content>
```

Provider compatibility flow:

```
User config (CLI11 / RuntimeConfig)
  → ProviderFactoryContext { ..., OpenAICompletionsCompat compat }
  → ProviderRegistry.create("openai-compatible", ctx)
  → OpenAIChatClient(config with compat flags)
  → request_to_openai(context, compat)  // in ProviderDtos.hpp / OpenAIChatClient.cpp
     • requiresToolResultName → adds "name" to tool result messages
     • requiresThinkingAsText → wraps thinking in <thinking> text blocks
     • thinkingFormat → selects reasoning parameter scheme
     • maxTokensField → uses "max_completion_tokens" vs "max_tokens"
     • supportsDeveloperRole → maps system messages to "developer" role
     • requiresReasoningContentOnAssistantMessages → adds empty reasoning_content
     • requiresAssistantAfterToolResult → inserts synthetic assistant message
     • supportsStore → includes "store" field
     • supportsReasoningEffort → includes "reasoning_effort" parameter
     • supportsUsageInStreaming → includes stream_options.include_usage
```

---

## Implementation Units

### U1. Separate ToolCall from Content variant, introduce AssistantContent

**Goal:** Align the C++ content model with pi's semantic model where `ToolCall` is a message-level element, not a content block.

**Requirements:** R1, R2.

**Dependencies:** None.

**Files:**

- Modify: `include/cch/ai/Content.hpp`, `include/cch/ai/Message.hpp`
- Modify: `include/cch/ai/glaze/AiJson.hpp`
- Modify: `src/agent/AgentLoop.cpp`
- Modify: `src/coding_agent/runtime/EventPrinter.cpp`
- Modify: `src/tools/AsyncToolFactories.cpp`
- Test: `tests/ai/MessageContractTest.cpp`
- Test: `tests/agent/AsyncAgentLoopTest.cpp`

**Approach:**

- Split `ToolCallContent` out of the `Content` variant — `Content` retains `TextContent | ThinkingContent | ImageContent`.
- Introduce `AssistantContent = std::variant<TextContent, ThinkingContent, ToolCallContent>`.
- Update `AssistantMessage::content` to `std::vector<AssistantContent>`.
- `UserMessage::content` and `ToolResultMessage::content` continue as `std::vector<Content>`.
- Update the Glaze `ContentDto` discriminator logic to handle `AssistantContent` serialization (either via a new `AssistantContentDto` or by extending `ContentDto` with a discriminator check — decided during implementation).
- `AgentLoop::build_assistant_message()` now accumulates `AssistantContent` instead of `Content` for assistant turns.
- `EventPrinter::print_agent_event()` continues to iterate content blocks unchanged (both `Content` and `AssistantContent` visitors work through the variant).
- `text_from_content()` helper is updated or a parallel `text_from_assistant_content()` is added for the agent loop's text-extraction path.

**Patterns to follow:**

- Existing `std::variant` usage in `Content.hpp` and `Message.hpp`.
- Existing `std::get_if` visitor patterns in `AgentLoop.cpp` and `EventPrinter.cpp`.
- Glaze `ContentDto` flat-discriminator pattern in `AiJson.hpp`.

**Test scenarios:**

- Happy path: `AssistantMessage` with mixed `TextContent`, `ThinkingContent`, and `ToolCallContent` in `content` vector survives round-trip JSON serialization.
- Happy path: agent loop with fake provider produces correct `AssistantContent` sequence.
- Edge case: `UserMessage.content` and `ToolResultMessage.content` remain `vector<Content>` — they do not accept `ToolCallContent`.
- Edge case: `text_from_content()` (now scoped to `Content`) and the new text-extraction for `AssistantContent` produce identical output for text-only content.
- Integration: CLI fake one-shot (`--fake "hello"`) produces unchanged semantic event output.

**Verification:**

- `./build/cpp_harness_tests "[ai][u2]"` passes with updated content-block and round-trip tests.
- `./build/cpp_harness_tests "[agent][async]"` passes — agent loop consumes `AssistantContent` correctly.
- `./build/cpp_harness --fake --session /tmp/u1.jsonl "read README.md"` produces unchanged output.

---

### U2. Add diagnostics, cacheWrite1h, and missing type fields

**Goal:** Close remaining gaps in message/usage type definitions to match pi's contract.

**Requirements:** R3, R7.

**Dependencies:** U1 (ToolCall separation must land first — `AssistantMessage` changes are in the same header).

**Files:**

- Modify: `include/cch/ai/Message.hpp` (add `DiagnosticEntry`, `diagnostics` field)
- Modify: `include/cch/ai/Usage.hpp` (add `cache_write_1h`)
- Modify: `include/cch/ai/glaze/AiJson.hpp`
- Modify: `src/ai/glaze/ProviderDtos.hpp` (if provider wire DTOs reference usage/metrics)
- Test: `tests/ai/MessageContractTest.cpp`
- Test: `tests/ai/GlazeRoundTripTest.cpp`

**Approach:**

- Add `DiagnosticEntry` struct to `Message.hpp` with fields matching pi's `AssistantMessageDiagnostic` (type, message, details, timestamp — exact fields finalized against pi source during implementation).
- Add `std::optional<std::vector<DiagnosticEntry>> diagnostics` to `AssistantMessage`.
- Add `std::optional<std::int64_t> cache_write_1h` to `Usage`.
- Update Glaze DTOs: `MessageDto` gains optional diagnostics array; `UsageDto` gains optional `cache_write_1h`.
- Verify that all existing forward-compatible fields (`text_signature`, `thinking_signature`, `redacted`, `thought_signature`) pass round-trip unchanged.

**Patterns to follow:**

- `require_field<T>()` in `AiJson.hpp` detail namespace for required diagnostic fields.
- Existing `UsageCost` sub-object pattern for the new `cache_write_1h` field.
- `stop_reason_from_json()` / `stop_reason_to_json()` enum mapping pattern for any diagnostic enum fields.

**Test scenarios:**

- Happy path: `AssistantMessage` with `diagnostics` populated round-trips through JSON.
- Happy path: `Usage` with `cache_write_1h` populated round-trips through JSON.
- Edge case: `AssistantMessage` with `diagnostics = std::nullopt` round-trips (backward-compatible).
- Edge case: `Usage` with `cache_write_1h = std::nullopt` round-trips (backward-compatible).
- Error path: malformed diagnostic entry JSON produces typed `util::Error`.
- Integration: existing session JSONL files without diagnostics load without error.

**Verification:**

- `./build/cpp_harness_tests "[ai][u2]"` passes with new diagnostic and usage round-trip tests.
- `./build/cpp_harness_tests "[harness][session]"` passes — session store handles new fields.

---

### U3. Add OpenAICompletionsCompat struct and wire into provider config

**Goal:** Introduce the passive `OpenAICompletionsCompat` struct with all pi-defined fields and thread it through the provider configuration path.

**Requirements:** R2, R4.

**Dependencies:** U1 (ToolCall separation — provider config header references AI types).

**Files:**

- Modify: `include/cch/ai/providers/OpenAIChatClient.hpp` (add `OpenAICompletionsCompat`, extend `OpenAIStreamConfig`)
- Modify: `include/cch/ai/ProviderRegistry.hpp` (extend `ProviderFactoryContext`)
- Modify: `src/ai/ProviderRegistry.cpp` (pass compat through factory)
- Test: `tests/ai/ProviderRegistryTest.cpp`
- Test: `tests/ai/providers/OpenAIChatClientTest.cpp`

**Approach:**

- Define `OpenAICompletionsCompat` as a passive struct in `OpenAIChatClient.hpp` with all pi-defined fields: `supportsStore`, `supportsDeveloperRole`, `supportsReasoningEffort`, `supportsUsageInStreaming`, `maxTokensField`, `requiresToolResultName`, `requiresAssistantAfterToolResult`, `requiresThinkingAsText`, `requiresReasoningContentOnAssistantMessages`, `thinkingFormat`.
- All fields use `std::optional<bool>` or `std::optional<enum>` to distinguish "not set" (auto-detect from URL) from explicit true/false — matching pi's "auto-detected from URL" defaults.
- Add `OpenAICompletionsCompat compat` field to `OpenAIStreamConfig`.
- Extend `ProviderFactoryContext` with `OpenAICompletionsCompat openai_compat` (default-constructed — all fields `std::nullopt`).
- The provider registry factory passes `context.openai_compat` into `OpenAIStreamConfig`.
- No compat transformations are implemented yet — U3 is struct definition and wiring only.

**Patterns to follow:**

- Existing passive-struct pattern in `OpenAIStreamConfig` and `ProviderFactoryContext`.
- `using` aliases for enum types (e.g., `using ThinkingFormat = enum class ...`).

**Test scenarios:**

- Happy path: `OpenAIStreamConfig` with populated compat fields constructs without error.
- Happy path: `ProviderFactoryContext` with default compat (all nullopt) produces a valid client.
- Edge case: all compat fields nullopt → provider behaves identically to pre-U3 (no transformations applied).
- Edge case: `thinkingFormat` enum covers all 9 pi-defined variants.
- Architecture: `OpenAICompletionsCompat` lives in `include/cch/ai/providers/` (provider header), not `include/cch/ai/` (domain header).

**Verification:**

- `./build/cpp_harness_tests "[ai][provider]"` passes — registry and client construction unchanged.
- Architecture test `[architecture]` passes — no provider DTO leaked into domain headers.

---

### U4. Implement provider compatibility transformations

**Goal:** Implement the compat-flag-driven transformations in the OpenAI provider adapter so that each flag changes the wire format as specified by pi.

**Requirements:** R5, R6.

**Dependencies:** U3 (compat struct must exist before transformations are implemented).

**Files:**

- Modify: `src/ai/providers/OpenAIChatClient.cpp`
- Modify: `src/ai/glaze/ProviderDtos.hpp`
- Test: `tests/ai/providers/OpenAIChatClientTest.cpp`

**Approach:**

- Add characterization tests first: capture current Kimi trailing-slash normalization, tool-call argument accumulation, and SSE chunk → stream event dispatch behavior before introducing compat transformations.
- Implement transformations in `request_to_openai()` and related helpers in `OpenAIChatClient.cpp`:
  - `requiresToolResultName` → adds `"name"` field to tool-result message DTOs.
  - `requiresThinkingAsText` → converts `ThinkingContent` blocks to `TextContent` with `<thinking>` delimiters in the wire DTO.
  - `thinkingFormat` → selects the correct reasoning parameter scheme (`reasoning_effort`, `reasoning: { effort }`, `thinking: { type }`, `enable_thinking`, `chat_template_kwargs.enable_thinking`, `thinking: string`, `reasoning: { effort }`).
  - `maxTokensField` → uses `"max_completion_tokens"` instead of `"max_tokens"`.
  - `supportsDeveloperRole` → maps system-role messages to `"developer"` role in the wire DTO.
  - `requiresReasoningContentOnAssistantMessages` → adds empty `reasoning_content` to replayed assistant messages when reasoning is enabled.
  - `requiresAssistantAfterToolResult` → inserts a synthetic assistant message between tool results and the next user message.
  - `supportsStore` → includes `"store"` field.
  - `supportsReasoningEffort` → includes `"reasoning_effort"` parameter.
  - `supportsUsageInStreaming` → includes `stream_options: { include_usage: true }`.
- Each transformation is a focused helper function or inline block, not a monolithic `apply_all_compat()` — this keeps each flag independently testable.

**Patterns to follow:**

- Existing `handle_chunk` lambda in `OpenAIChatClient.cpp` — add compat-awareness to the chunk → event translation.
- `ToolCallAccumulator` pattern for streaming argument accumulation.
- Kimi-specific trailing-slash normalization and tool-request construction as the canonical precedent for provider-specific wire-format adaptation.

**Test scenarios:**

- Happy path: `requiresToolResultName = true` → tool-result JSON includes `"name"` field.
- Happy path: `requiresThinkingAsText = true` → thinking content is emitted as text deltas with `<thinking>` wrappers.
- Happy path: each `thinkingFormat` variant produces the correct JSON parameter shape.
- Happy path: `maxTokensField = "max_completion_tokens"` → request JSON uses `"max_completion_tokens"`.
- Edge case: all compat flags nullopt → wire format identical to pre-U4 behavior (characterization tests pass).
- Edge case: `requiresReasoningContentOnAssistantMessages = true` → replayed assistant messages include empty `reasoning_content`.
- Edge case: `supportsDeveloperRole = true` → system messages are serialized with `"role": "developer"`.
- Error path: unknown `thinkingFormat` value → treated as `"openai"` (safe default).
- Integration: fake provider path is unaffected (fakes don't use compat flags).

**Verification:**

- `./build/cpp_harness_tests "[ai][provider]"` passes with new compat-flag test cases.
- `./build/cpp_harness --fake --session /tmp/u4.jsonl "hello"` produces unchanged output.

---

### U5. Glaze DTO updates, round-trip coverage, and architecture guards

**Goal:** Update all Glaze DTO mappings for the new types and fields, add comprehensive round-trip tests, and verify architecture boundaries.

**Requirements:** R2, R7.

**Dependencies:** U1, U2, U4 (all type changes must land before DTO updates are finalized).

**Files:**

- Modify: `include/cch/ai/glaze/AiJson.hpp`
- Modify: `src/ai/glaze/ProviderDtos.hpp` (if U4 added new wire DTO fields)
- Test: `tests/ai/GlazeRoundTripTest.cpp`
- Test: `tests/ai/MessageContractTest.cpp`
- Test: `tests/architecture/ArchitectureSurfaceScanTest.cpp` (may need updates for new types)

**Approach:**

- Update `ContentDto` in `AiJson.hpp` to handle the new `AssistantContent` variant — either via a separate `AssistantContentDto` with its own discriminator or by extending `ContentDto` with a discriminator guard that rejects `ToolCallContent` in `Content` contexts and accepts it in `AssistantContent` contexts.
- Update `MessageDto` for `diagnostics` array serialization.
- Update `UsageDto` for `cache_write_1h`.
- Add round-trip tests for every new field and type combination:
  - `AssistantContent` with each variant (text, thinking, tool-call).
  - `AssistantMessage` with and without diagnostics.
  - `Usage` with and without `cache_write_1h`.
  - `OpenAICompletionsCompat` struct round-trip (even though it's a config struct, not serialized to session — test aggregate-friendliness).
- Verify that all existing round-trip tests still pass with the new DTO shapes.
- Run full architecture test suite and fix any new boundary violations.

**Patterns to follow:**

- `to_dto()` / `*_from_dto()` in `detail` namespace.
- `require_field<T>()` for required fields.
- `json_contract_error()` for typed errors with source context.
- Existing `MessageContractTest` aggregate-friendliness checks (`std::is_aggregate_v`, `std::is_copy_constructible_v`, etc.).

**Test scenarios:**

- Happy path: `AssistantContent` round-trips for text, thinking, and tool-call variants.
- Happy path: `AssistantMessage` with all optional fields populated (diagnostics, response_model, response_id, error_message) round-trips.
- Happy path: `Usage` with all fields (including cache_write_1h) round-trips.
- Happy path: `ToolResultMessage` with `Content` (not `AssistantContent`) round-trips unchanged.
- Edge case: empty `diagnostics` vector round-trips as `[]`.
- Edge case: `ContentDto` rejects `ToolCallContent` discriminator with typed error.
- Error path: malformed `AssistantContent` discriminator produces typed `util::Error`.
- Architecture: `[architecture]` test suite passes — no new provider DTO or Glaze machinery leaked into domain headers.

**Verification:**

- `./build/cpp_harness_tests "[ai][u2]"` passes — all round-trip and contract tests.
- `./build/cpp_harness_tests "[architecture]"` passes — no boundary regressions.

---

### U6. Documentation, parity roadmap update, and final integration smoke test

**Goal:** Update README, AGENTS.md, and the parity roadmap to reflect completed T2 work. Verify end-to-end CLI behavior.

**Requirements:** R7 (verification), parity roadmap completeness.

**Dependencies:** U1–U5 (all implementation must land first).

**Files:**

- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`

**Approach:**

- In README, update the AI package description to note OpenAICompletionsCompat and provider compatibility flags.
- In AGENTS.md, add routing guidance for provider compatibility work under the existing AI/provider registry row.
- In the parity roadmap, mark all T2 items as complete (`[x]`) and add a pointer to this plan.
- Run the full integration smoke test: `./build/cpp_harness --fake --repl --session /tmp/t2-smoke.jsonl` with prompts that exercise text output, tool calls, and multi-turn conversation. Verify semantic event output is unchanged.

**Patterns to follow:**

- Existing README Architecture boundaries section format.
- Existing AGENTS.md routing table format.
- Parity roadmap checklist format.

**Test scenarios:**

- Test expectation: none — documentation-only changes. The integration smoke test is manual verification, not automated.
- Integration (manual): `./build/cpp_harness --fake --session /tmp/t2.jsonl "read README.md"` produces expected output.
- Integration (manual): `./build/cpp_harness --fake --repl --session /tmp/t2-repl.jsonl` with two prompts preserves history.

**Verification:**

- README accurately describes the new AI contract surface.
- AGENTS.md routes provider-compatibility changes to the correct files.
- Parity roadmap T2 checklist items are marked complete.
- Full `ctest` passes.

---

## System-Wide Impact

- **Interaction graph:** `Content.hpp` type split affects `AgentLoop.cpp` (build_assistant_message), `EventPrinter.cpp` (content visitors), `AsyncToolFactories.cpp` (read tool result construction), `AiJson.hpp` (Glaze DTOs), and all test files that construct `AssistantMessage` instances. `OpenAICompletionsCompat` is consumed by `OpenAIChatClient.cpp` and threaded through `ProviderRegistry` and `RuntimeServices`.
- **Error propagation:** Compat-transformation errors (e.g., unknown `thinkingFormat`) are treated as provider-level errors — emitted via `AssistantErrorEvent` and returned as `std::unexpected` from the client's stream method. Malformed DTOs produce typed `util::Error` via `require_field` / `json_contract_error`.
- **State lifecycle risks:** `DiagnosticEntry` is append-only metadata — it should not affect resume or session reconstruction. `cache_write_1h` is informational only. `OpenAICompletionsCompat` is configuration, not session state.
- **API surface parity:** All public type changes in `include/cch/ai/` are backward-compatible at the Glaze JSON level (new optional fields only). The `Content` → `AssistantContent` split is a C++ type-level change; session JSONL format is unaffected because discriminators remain stable.
- **Integration coverage:** Cross-layer scenario — CLI `--fake` → ProviderRegistry → fake client → AgentLoop → EventPrinter → stdout. This path must produce identical output before and after all units. CLI `--resume` of pre-U1 session files must load without error.
- **Unchanged invariants:** Move-only event sinks are preserved. Glaze isolated to impl layer. No `boost/json` or `util::Result` reintroduced. Workspace containment, secret redaction, bash opt-in, and private session permissions unchanged. `ProviderRegistry` API unchanged (only `ProviderFactoryContext` extended with optional compat field).

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `Content` / `AssistantContent` split breaks downstream consumers (CLI printer, tool factories, session writer) that iterate `Content` vectors without visiting all variant alternatives. | All compile-time errors from the variant split are caught at build time. Runtime behavior is verified by the existing CLI smoke tests and agent loop tests. |
| Glaze DTO changes silently break JSONL session compatibility — pre-U1 session files fail to load. | Session JSONL tests (`JsonlSessionStoreTest`) cover v2 message round-trips. Add a specific test loading a pre-U1 golden session file before the U1 merge. |
| Compat transformation interacts poorly with the existing Kimi-specific code paths (trailing-slash normalization, tool-request construction). | Characterization tests on existing Kimi behavior land in U3/U4 before any compat transformation code. Kimi path is exercise with each compat flag in isolation. |
| New fields on `AssistantMessage` / `Usage` increase serialized JSON size, potentially hitting session line-length or output-truncation limits. | All new fields are optional and default to omitted. Session JSONL line-length limits are not changed. |
| `thinkingFormat` enum with 9 variants is fragile — adding a new variant requires touching multiple switch/if-else chains. | Each variant is mapped in a single `apply_thinking_format()` helper. Future variants only add a case to that function. |

---

## Documentation / Operational Notes

- README Architecture boundaries section: update AI package description to include `OpenAICompletionsCompat` and provider compatibility flags.
- AGENTS.md routing table: add `include/cch/ai/providers/OpenAIChatClient.hpp` compat flags row under the existing Provider/model registry entry.
- Parity roadmap: mark T2 `pi-ai` items as complete, add pointer to this plan.
- No monitoring, rollout, or migration concerns — this is a development-only type-system change.

---

## Sources & References

- **Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T2 checklist)
- **Prerequisite plan:** `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- Related code:
  - `include/cch/ai/Content.hpp`, `include/cch/ai/Message.hpp`, `include/cch/ai/Usage.hpp`, `include/cch/ai/StreamEvent.hpp`
  - `include/cch/ai/glaze/AiJson.hpp`, `src/ai/glaze/ProviderDtos.hpp`
  - `include/cch/ai/providers/OpenAIChatClient.hpp`, `src/ai/providers/OpenAIChatClient.cpp`
  - `include/cch/ai/ProviderRegistry.hpp`, `src/ai/ProviderRegistry.cpp`
  - `src/agent/AgentLoop.cpp`, `src/coding_agent/runtime/EventPrinter.cpp`
  - `tests/ai/MessageContractTest.cpp`, `tests/ai/GlazeRoundTripTest.cpp`, `tests/ai/providers/OpenAIChatClientTest.cpp`, `tests/ai/ProviderRegistryTest.cpp`
- pi reference contracts:
  - `pi:packages/ai/src/types.ts`
  - `pi:packages/ai/src/utils/event-stream.ts`
  - `pi:packages/ai/src/providers/openai-completions.ts`
  - `pi:packages/ai/src/providers/transform-messages.ts`
