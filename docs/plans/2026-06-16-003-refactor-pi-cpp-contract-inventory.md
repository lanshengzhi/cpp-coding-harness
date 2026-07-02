---
title: "refactor: pi C++ contract inventory"
type: "inventory"
status: active
date: "2026-06-16"
origin: "docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---


# refactor: pi C++ contract inventory

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.

## Purpose

This inventory is the T0 reference map for evolving `cpp-coding-harness` toward pi module and contract parity. It records which pi public contracts already have C++ equivalents, which contracts are near-term seams for the pre-implementation cleanup, and which are intentionally deferred until later roadmap slices.

Classification:

- **MVP parity:** already represented in the current C++ harness or required to preserve current behavior.
- **Near-term parity:** introduced or prepared by `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`.
- **Deferred parity:** intentionally not part of the pre-implementation cleanup.

## Source References Read

- `pi:packages/ai/src/types.ts`
- `pi:packages/ai/src/utils/event-stream.ts`
- `pi:packages/ai/src/models.ts`
- `pi:packages/ai/src/compat.ts`
- `pi:packages/ai/src/api/lazy.ts`
- `pi:packages/ai/src/providers/all.ts`
- Historical references `pi:packages/ai/src/stream.ts` and `pi:packages/ai/src/api-registry.ts` are absent in the current pi checkout; their stream/API-registry contracts now live in the files above.
- `pi:packages/agent/src/types.ts`
- `pi:packages/agent/src/harness/types.ts`
- `pi:packages/coding-agent/docs/session-format.md`
- `pi:packages/coding-agent/src/cli/args.ts`
- `pi:packages/coding-agent/src/core/sdk.ts`

## AI Package Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `KnownApi`, `Api`, `KnownProvider`, `Provider`, image provider aliases | Partial: `ProviderFactoryContext` now carries separate registry key and provider/API identity; no public alias/catalog model yet | Near-term parity | C++ must not report every OpenAI-compatible adapter response as provider `openai`; full provider/model catalog remains deferred. |
| `ThinkingLevel`, `ModelThinkingLevel`, `ThinkingLevelMap`, `ThinkingBudgets` | Partial: `include/cch/ai/Content.hpp` has `ThinkingContent`; no public thinking-level model/options seam yet | Deferred parity | Requires the future model/options seam; do not add inert thinking controls to the generic request surface. |
| `CacheRetention`, `Transport`, `ProviderResponse`, `StreamOptions`, `ProviderStreamOptions`, `SimpleStreamOptions` | Partial: `include/cch/ai/ChatClient.hpp::StreamChatRequest`, `src/ai/providers/OpenAIChatClient.cpp` | Deferred parity | Current C++ request only carries context/model; temperature, maxTokens, transport, retry, cacheRetention, sessionId, callbacks, headers, metadata, and env remain out of scope. |
| `TextContent`, `ThinkingContent`, `ImageContent`, `ToolCall` | `include/cch/ai/Content.hpp` (`TextContent`, `ThinkingContent`, `ImageContent`, `ToolCallContent`) | MVP parity | C++ also keeps `raw_arguments` / validity state for streamed tool-call recovery; this is useful but should not expand into provider scratch state elsewhere. |
| `Usage`, `StopReason` | `include/cch/ai/Usage.hpp` (`Usage`, `AssistantStopReason`) | MVP parity | Contract has cache/cost fields; OpenAI-compatible adapter still only fills prompt/completion/total tokens. |
| `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `Message` | `include/cch/ai/Message.hpp` (`SystemMessage`, `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `MessageVariant`) | MVP parity with known cleanup debt | System messages and extended runtime/custom messages are C++ additions; future runtime-only message types should live outside `include/cch/ai`. |
| `Tool`, `Context` | `include/cch/ai/Tool.hpp`, `include/cch/ai/Context.hpp` | MVP parity with known cleanup debt | Tool JSON schema uses project `util::JsonValue`; `ToolExecutionMode` belongs to agent scheduling and is a future cleanup target. |
| `AssistantMessageEvent` and `AssistantMessageEventStream` | `include/cch/ai/StreamEvent.hpp` (`AssistantStreamEvent`) | Near-term parity | Current pi stream/result semantics live in `types.ts` and `utils/event-stream.ts`. C++ has event variants but no async-iterable stream object. |
| `stream`, `complete`, `streamSimple`, `completeSimple` | `include/cch/ai/ChatClient.hpp::StreamingChatClient` | MVP parity | C++ exposes a streaming client seam plus `complete()` helper; simple helpers remain deferred unless a current consumer appears. |
| `ApiProvider`, `registerApiProvider`, `getApiProvider`, `getApiProviders`, `unregisterApiProviders`, `clearApiProviders` | Partial: `include/cch/ai/ProviderRegistry.hpp` | Near-term parity | pi's old global registry is now compatibility-only (`compat.ts`); C++ registry is adapter construction, not a full `Models` collection. |
| `Model`, `Models`, `createModels`, `createProvider`, `getModel`, `getProviders`, `getModels`, `calculateCost`, thinking-level helpers, `modelsAreEqual` | Missing except static config/model strings | Deferred parity | Requires a dedicated C++ model seam carrying provider/API/baseUrl/reasoning/input/cost/context/max-token/compat metadata. |
| OpenAI/Anthropic/OpenRouter/Vercel compatibility option shapes | Provider implementation details under `include/cch/ai/providers/`, `src/ai/providers/`, and `src/ai/glaze/` | Partial/deferred parity | C++ has the older OpenAI completions compat subset; current pi adds routing, strict-mode, cache-control, session-affinity, long-retention, and chat-template fields. |
| Images API contracts (`ImagesOptions`, `AssistantImages`, `ImagesModel`) | Missing | Deferred parity | Image generation is out of scope for the current harness roadmap. |


### AI Contract Ownership Note

`include/cch/ai` should stay the provider-neutral LLM contract surface: content, assistant/user/tool-result messages, context, tools, usage, stream events, chat clients, provider registry, and provider adapter config. Agent scheduling (`ToolExecutionMode`, queueing, hooks) belongs under `include/cch/agent`; coding-agent/runtime transcript messages (bash execution, custom display messages, branch/compaction summaries) belong under coding-agent/session-facing contracts. Existing C++ additions remain for compatibility, but new runtime-only concepts should not be added to `include/cch/ai`.

## Agent Package Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `StreamFn` | `include/cch/agent/AgentEvent.hpp::AgentEventSink` plus AI `AssistantEventSink` | MVP parity | C++ keeps move-only event sinks via `std::move_only_function`. |
| `ToolExecutionMode`, `QueueMode` | Missing | Deferred parity | Parallel/read-only tool scheduling is a later T3 slice. |
| `AgentToolCall` | `include/cch/ai/Content.hpp::ToolCallContent` and `include/cch/agent/AgentTool.hpp::ToolInvocation` | MVP parity | Existing loop maps provider tool calls into tool invocations. |
| Tool hooks: `BeforeToolCall*`, `AfterToolCall*`, `PrepareNextTurn*`, `ShouldStopAfterTurn*` | Missing | Deferred parity | Pre/post hooks and next-turn transforms follow after U5 event/state seam. |
| `AgentLoopTurnUpdate` | Partial: lifecycle events in `include/cch/agent/AgentEvent.hpp` | Near-term parity | U5 expands lifecycle events to include thinking/tool-call stream phases. |
| `AgentLoopConfig` | `include/cch/agent/AgentContext.hpp::AsyncAgentOptions` | MVP parity | Current options cover max turns, tools, and event sink; richer queue/hook/options remain deferred. |
| Agent `ThinkingLevel` | Missing | Near-term parity | U5 introduces an observable field if needed by current state seam; provider-specific model support remains later T2/T3. |
| `CustomAgentMessages`, `AgentMessage` | `include/cch/ai/Message.hpp::MessageVariant` | MVP parity | `BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage` added as passive aggregates with LLM conversion helpers. |
| `SlashCommandInfo`, command sources | `include/cch/coding_agent/PromptProcessing.hpp::CommandRegistry` | MVP parity | Built-in command registry with session-lifecycle commands (`/session`, `/quit`, `/new`, `/resume`); extension/prompt/skill sources deferred to T6. |
| `PromptTemplate`, `expandPromptTemplate` | `include/cch/coding_agent/PromptProcessing.hpp` | MVP parity | `PromptTemplate` struct and `expand_prompt_template()` with bash-style arg substitution. File-based template discovery deferred to T6. |
| `AgentState` | Missing | Near-term parity | U5 adds passive `AgentState` for active tools, messages, streaming message, pending tool calls, model, and thinking level. |
| `AgentToolResult`, `AgentToolUpdateCallback`, `AgentTool` | `include/cch/agent/AgentTool.hpp` (`AsyncToolExecutionResult`, `AsyncAgentTool`) | MVP parity | Current contract is async and value-oriented. Progress/update callbacks are deferred. |
| `AgentContext` | `include/cch/agent/AgentContext.hpp` | Near-term parity | Current header is options/result-only; U5 decides whether state lives here or in a sibling header. |
| `AgentEvent` | `include/cch/agent/AgentEvent.hpp::AgentLifecycleEvent` | Near-term parity | Current events preserve CLI output; U5 adds new variants without removing old ones. |

## Harness and Execution Environment Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `Result`, error helpers, typed error unions | `include/cch/util/Error.hpp`, `std::expected` aliases | MVP parity | Do not reintroduce legacy `util::Result`. |
| Resource contracts (`Skill`, `PromptTemplate`, `AgentHarnessResources`, stream option patches) | Missing | Deferred parity | Skills/templates/resources are T6 after runtime/session boundaries stabilize. |
| File error and execution error code families | `include/cch/harness/ExecutionEnv.hpp` (`FileError`, `FileErrorCode`, `ExecutionError`, `ExecutionErrorCode`); `include/cch/util/Error.hpp` | Near-term parity | Typed error families added in T4 capability parity; to_util_error() bridges for compatibility. |
| `FileInfo`, `FileSystem` | `include/cch/harness/ExecutionEnv.hpp` (`FileInfo`, `FileKind`, `BinaryData`, pi-shaped FS methods); `src/harness/WorkspaceFileSystem.hpp` | Near-term parity | Full filesystem surface (stat/list/binary/temp dirs/canonicalize) implemented; stronger workspace containment than pi default. |
| `Shell`, `ExecutionEnvExecOptions`, `ExecutionEnv` | `include/cch/harness/ExecutionEnv.hpp` (`ExecOptions`, `ShellExecResult`, `exec`); `src/util/Process.hpp` split-stream process | Near-term parity | Split stdout/stderr, cwd/env override, per-stream truncation, callback support, typed execution errors, and bash opt-in safety all landed in T4. |
| Agent harness event hooks (`BeforeAgentStartEvent`, provider request/response, tool/session/model/resource updates) | Missing | Deferred parity | U5 only creates agent lifecycle stream events; harness extension hooks come later. |
| Compaction and branch-summary preparation contracts | Missing | Deferred parity | Session tree parse support lands first; compaction/branch behavior remains later T4. |
| `AgentHarnessOptions` / `AgentHarness` | Missing | Deferred parity | Full harness object is beyond pre-implementation cleanup. |

## Coding-Agent Runtime and CLI Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `Args`, `Mode`, `parseArgs`, `printHelp`, `isValidThinkingLevel` | `src/main.cpp` hand parser; `src/AsyncCliRuntime.hpp::AsyncCliRuntimeConfig` | Near-term parity | U3 migrates parsing to CLI11 while preserving current flags and validation. |
| One-shot, REPL, resume/session flags | `src/main.cpp`, `src/AsyncCliRuntime.cpp`, CLI smoke tests | MVP parity | Current line-oriented CLI behavior must remain stable. |
| JSON/RPC modes | Missing | Deferred parity | T8 after runtime services are separated from event printing. |
| SDK `CreateAgentSessionOptions`, `CreateAgentSessionResult`, `createAgentSession` | Missing | Deferred parity | U7 splits runtime internals but does not expose an SDK. |
| Runtime service split (`agent-session-runtime`, services, session lifecycle, event printer) | Partial: `src/AsyncCliRuntime.*` | Near-term parity | U7 extracts only seams with current CLI/test consumers. |
| Settings/model resolver/project trust/resources | Missing | Deferred parity | Follow-up T5/T6 once provider registry and runtime split exist. |

## Session Entry Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `SessionMessageEntry` / `MessageEntry` | `include/cch/harness/session/SessionEntry.hpp::SessionEntryKind::Message` | MVP parity | Current store writes/loads v2 message entries. |
| `ModelChangeEntry` | Missing | Near-term parity | U8 parses/preserves entry kind; context reconstruction remains deferred. |
| `ThinkingLevelChangeEntry` | Missing | Near-term parity | U8 parses/preserves entry kind. |
| `ActiveToolsChangeEntry` | Missing | Near-term parity | Present in harness TS types; U8 includes it in C++ enum/payload parsing. |
| `CompactionEntry` | Missing | Near-term parity | U8 parse-only; compaction semantics deferred. |
| `BranchSummaryEntry` | Missing | Near-term parity | U8 parse-only; branch summary generation deferred. |
| `CustomEntry` | Missing | Near-term parity | U8 parse-only for extension state; extension behavior deferred. |
| `CustomMessageEntry` | Missing | Near-term parity | U8 parse-only; conversion into model context deferred. |
| `LabelEntry` / `SessionInfoEntry` | Missing | Near-term parity | U8 parse-only metadata preservation. |
| `LeafEntry` | Missing from session-format docs headings but present in `pi:packages/agent/src/harness/types.ts` | Near-term parity | U8 includes `Leaf` so future tree navigation has an ID anchor. |
| `SessionTreeEntryBase` IDs (`id`, `parentId`) and leaf tracking | Partial: linear entries only | Near-term parity | U8 adds optional `parent_id`/`leaf_id` fields. |
| Tree context reconstruction (`buildSessionContext`) | Missing | Deferred parity | Future T4 slice; U8 must fail closed or avoid silently mis-resuming nontrivial trees. |

## Feature Classification Summary

| Area | MVP parity | Near-term parity | Deferred parity |
| --- | --- | --- | --- |
| AI contracts | Text/thinking/image/tool-call content, messages, tools, usage, streaming chat seam, OpenAI-compatible adapter | Provider registry, fake provider registration, forwarding stream phases through agent events | Full model catalog, cost accounting, OAuth/subscription flows, image generation, provider-specific reasoning compatibility |
| Agent loop | Async loop, tool execution, move-only lifecycle event sink, max-turn behavior | Observable state and thinking/tool-call lifecycle events | Hooks, context transforms, parallel tool execution, queue modes, update callbacks |
| Harness | Workspace-guarded file tools, bash opt-in, redaction, JSONL v2 sessions | True async process I/O, parse-only session tree entry model | Full filesystem/shell parity, compaction, branch summaries, harness extension event pipeline |
| Coding-agent runtime | One-shot/REPL/resume CLI, semantic event lines | CLI11 parsing, provider/model registry wiring, runtime service split | Settings, resources, skills, extensions, prompt templates, JSON/RPC, SDK |
| TUI/platform/distribution | Line-oriented CLI only | None in cleanup | Native TUI, themes, keybindings, packages, self-update, platform-specific terminal integrations |

## Recurring Checklist for Future Parity Slices

Before implementing any future parity slice:

1. Read the relevant `pi:` contract file or docs listed in this inventory and in the roadmap.
2. Identify whether the C++ target should be a passive value contract, a capability interface, or provider/runtime implementation detail.
3. Add or update tests that protect pi-semantic behavior rather than old class names.
4. Keep provider DTOs, Glaze machinery, and private implementation helpers out of public domain headers.
5. Preserve move-only event sinks and `std::expected` error flow.
6. Update README/AGENTS/roadmap only when behavior, public boundaries, or routing changes.
7. State deferrals explicitly instead of adding placeholder APIs that imply unsupported pi behavior exists.
