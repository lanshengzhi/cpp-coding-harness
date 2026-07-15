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
- `pi:packages/coding-agent/src/core/agent-session.ts`
- `pi:packages/coding-agent/src/modes/rpc/rpc-mode.ts`
- `pi:packages/coding-agent/docs/rpc.md`

## AI Package Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `KnownApi`, `Api`, `KnownProvider`, `Provider`, image provider aliases | Partial: `ProviderFactoryContext` now carries separate registry key and provider/API identity; no public alias/catalog model yet | Near-term parity | C++ must not report every OpenAI-compatible adapter response as provider `openai`; full provider/model catalog remains deferred. |
| `ThinkingLevel`, `ModelThinkingLevel`, `ThinkingLevelMap`, `ThinkingBudgets` | Partial: `include/cch/ai/Content.hpp` has `ThinkingContent`; no public thinking-level model/options seam yet | Deferred parity | Requires the future model/options seam; do not add inert thinking controls to the generic request surface. |
| `CacheRetention`, `Transport`, `ProviderResponse`, `StreamOptions`, `ProviderStreamOptions`, `SimpleStreamOptions` | Partial: `include/cch/ai/ChatClient.hpp::StreamChatRequest`, `src/ai/providers/OpenAIChatClient.cpp` | Deferred parity | Current C++ request only carries context/model; temperature, maxTokens, transport, retry, cacheRetention, sessionId, callbacks, headers, metadata, and env remain out of scope. |
| `TextContent`, `ThinkingContent`, `ImageContent`, `ToolCall` | `include/cch/ai/Content.hpp` (`TextContent`, `ThinkingContent`, `ImageContent`, `ToolCallContent`) | MVP parity | C++ also keeps `raw_arguments` / validity state for streamed tool-call recovery; this is useful but should not expand into provider scratch state elsewhere. |
| `Usage`, `StopReason` | `include/cch/ai/Usage.hpp` (`Usage`, `AssistantStopReason`) | MVP parity | Contract has cache/cost fields; OpenAI-compatible adapter still only fills prompt/completion/total tokens. |
| `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `Message` | `include/cch/ai/Message.hpp` (`SystemMessage`, `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `MessageVariant`) | MVP parity with known cleanup debt | System messages and extended runtime/custom messages are C++ additions; future runtime-only message types should live outside `include/cch/ai`. |
| `Tool`, `Context` | `include/cch/ai/Tool.hpp`, `include/cch/ai/Context.hpp` | MVP parity | Tool JSON schema uses project `util::JsonValue`; all local scheduling vocabulary now stays under `include/cch/agent`. |
| `AssistantMessageEvent` and `AssistantMessageEventStream` | `include/cch/ai/StreamEvent.hpp` (`AssistantStreamEvent`) | Near-term parity | Current pi stream/result semantics live in `types.ts` and `utils/event-stream.ts`. C++ has event variants but no async-iterable stream object. |
| `stream`, `complete`, `streamSimple`, `completeSimple` | `include/cch/ai/ChatClient.hpp::StreamingChatClient` | MVP parity | C++ exposes a streaming client seam plus `complete()` helper; simple helpers remain deferred unless a current consumer appears. |
| `ApiProvider`, `registerApiProvider`, `getApiProvider`, `getApiProviders`, `unregisterApiProviders`, `clearApiProviders` | Partial: `include/cch/ai/ProviderRegistry.hpp` | Near-term parity | pi's old global registry is now compatibility-only (`compat.ts`); C++ registry is adapter construction, not a full `Models` collection. |
| `Model`, `Models`, `createModels`, `createProvider`, `getModel`, `getProviders`, `getModels`, `calculateCost`, thinking-level helpers, `modelsAreEqual` | Missing except static config/model strings | Deferred parity | Requires a dedicated C++ model seam carrying provider/API/baseUrl/reasoning/input/cost/context/max-token/compat metadata. |
| OpenAI/Anthropic/OpenRouter/Vercel compatibility option shapes | Provider implementation details under `include/cch/ai/providers/`, `src/ai/providers/`, and `src/ai/glaze/` | Partial/deferred parity | C++ has the older OpenAI completions compat subset; current pi adds routing, strict-mode, cache-control, session-affinity, long-retention, and chat-template fields. |
| Images API contracts (`ImagesOptions`, `AssistantImages`, `ImagesModel`) | Missing | Deferred parity | Image generation is out of scope for the current harness roadmap. |


### AI Contract Ownership Note

`include/cch/ai` should stay the provider-neutral LLM contract surface: content, assistant/user/tool-result messages, context, tools, usage, stream events, chat clients, provider registry, and provider adapter config. Agent scheduling (`ToolExecutionPolicy`, `ToolConcurrency`, queueing, hooks) belongs under `include/cch/agent`; coding-agent/runtime transcript messages (bash execution, custom display messages, branch/compaction summaries) belong under coding-agent/session-facing contracts. Existing C++ additions remain for compatibility, but new runtime-only concepts should not be added to `include/cch/ai`.

## Agent Package Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `StreamFn` | `include/cch/agent/AgentEvent.hpp::AgentEventSink` plus AI `AssistantEventSink` | MVP parity | C++ keeps move-only event sinks via `std::move_only_function`. |
| `ToolExecutionMode`, `QueueMode` | `ToolExecutionPolicy` and `ToolConcurrency` in `include/cch/agent/` (queue modes still missing) | MVP tool-scheduling parity / queue modes deferred | Sequential is the safe default. Bounded parallel execution requires an explicit run cap and every resolved tool to claim `ParallelSafe`; result insertion remains in assistant source order. |
| `AgentToolCall` | `include/cch/ai/Content.hpp::ToolCallContent` and `include/cch/agent/AgentTool.hpp::ToolInvocation` | MVP parity | Existing loop maps provider tool calls into tool invocations. |
| Tool hooks: `BeforeToolCall*`, `AfterToolCall*`, `PrepareNextTurn*`, `ShouldStopAfterTurn*` | `BeforeToolCallHook`, `AfterToolCallHook`, and `PrepareNextTurnHook` under `include/cch/agent/` | Partial parity | Pre/post tool hooks and validated next-turn updates are implemented; an independent `ShouldStopAfterTurn` hook remains deferred. |
| `AgentLoopTurnUpdate` | Partial: lifecycle events in `include/cch/agent/AgentEvent.hpp` | Near-term parity | Assistant stream phases are now delivered through `message_update` carrying the provider-neutral `AssistantStreamEvent`; standalone thinking/tool-call stream events were removed. |
| `AgentLoopConfig` | `include/cch/agent/AgentContext.hpp::AsyncAgentOptions` | MVP parity | Options cover max turns, model, context/LLM transforms, tool hooks and policy, steering/follow-up retrieval, and prepare-next-turn validation. Public queue control, cancellation, and retry remain deferred. |
| Agent `ThinkingLevel` | `AgentState::thinking_level` plus validated next-turn updates | Partial parity | The agent state can observe a supported thinking-level string, but full model capability and user-facing thinking controls remain deferred. |
| `CustomAgentMessages`, `AgentMessage` | `include/cch/ai/Message.hpp::MessageVariant` | MVP parity | `BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage` added as passive aggregates with LLM conversion helpers. |
| `SlashCommandInfo`, command sources | `src/coding_agent/CommandRegistry.hpp` (`CommandInfo`, `CommandContext`, `CommandRegistry`) | MVP parity with C++ adaptations | Registry-owned metadata, aliases, explicit collision errors, deterministic introspection, 6 canonical built-ins, and 2 aliases are implemented for the line-CLI adapter. CLI built-ins are dispatched before AgentSession; RPC commands are JSON records owned by `RpcMode`; the SDK exposes no command registration. `/help`, `/commands`, `/clear`, and `/exit` are line-CLI adaptations rather than direct pi parity; extension command sources remain deferred. |
| `PromptTemplate`, `expandPromptTemplate`, prompt interpretation | Public passive `PromptTemplate` value in `include/cch/coding_agent/PromptTemplate.hpp`; private owning processor and template expander in `src/coding_agent/prompt/` | MVP parity | AgentSession prompt interpretation owns an immutable skill/template snapshot only: cached skill expansion flows into template expansion, unmatched slash input passes through, and `PromptOptions::expand_prompt_templates` bypasses both stages when false. CLI and RPC command dispatch stay in their adapters; no prompt-processing free functions are published. |
| `AgentState` | `include/cch/agent/AgentContext.hpp::AgentState` | MVP parity for the supported subset | Passive state carries messages, streaming assistant state, active tools, pending tool-call IDs, model, and thinking level. |
| `AgentToolResult`, `AgentToolUpdateCallback`, `AgentTool` | `include/cch/agent/AgentTool.hpp` (`AsyncToolExecutionResult`, `AsyncAgentTool`, `ToolConcurrency`) | MVP parity | Current contract is async and value-oriented. Tools default to `Exclusive`; adapters opt into `ParallelSafe` only after concurrent-use validation. Progress/update callbacks are deferred. |
| `AgentContext` | `include/cch/agent/AgentContext.hpp` | Near-term parity | The header owns run options/results, including the closed sequential/bounded-parallel `ToolExecutionPolicy`; state remains a passive value in the same package. |
| `AgentEvent` | `include/cch/agent/AgentEvent.hpp::AgentLifecycleEvent` | MVP parity for the supported subset | Cut over to pi-aligned shapes in ticket 01: removed prompt copy, numeric turn fields, queued-message alternatives, and standalone thinking/tool-call stream events; message updates carry the current assistant message and provider-neutral stream event. Unsupported queue, compaction, retry, and progress events remain absent rather than appearing as placeholders. |

## Harness and Execution Environment Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `Result`, error helpers, typed error unions | `include/cch/util/Error.hpp`, `std::expected` aliases | MVP parity | Do not reintroduce legacy `util::Result`. |
| Resource contracts (`Skill`, `PromptTemplate`, `AgentHarnessResources`, stream option patches) | `Skill`, `PromptTemplate`, `ProjectResources`, and `ProjectResourceLoader` under `include/cch/coding_agent/` and `src/coding_agent/` | Partial parity | Immutable session skill/template snapshots, trust-gated project discovery, and host/project precedence are implemented. A general `AgentHarnessResources` bundle and stream-option patches remain deferred. |
| File error and execution error code families | `include/cch/harness/ExecutionEnv.hpp` (`FileError`, `FileErrorCode`, `ExecutionError`, `ExecutionErrorCode`); `include/cch/util/Error.hpp` | Near-term parity | Typed error families added in T4 capability parity; to_util_error() bridges for compatibility. |
| `FileInfo`, `FileSystem` | `include/cch/harness/ExecutionEnv.hpp` (`FileInfo`, `FileKind`, `BinaryData`, pi-shaped FS methods); `src/harness/WorkspaceFileSystem.hpp` | Near-term parity | Full filesystem surface (stat/list/binary/temp dirs/canonicalize) implemented; stronger workspace containment than pi default. |
| `Shell`, `ExecutionEnvExecOptions`, `ExecutionEnv` | `include/cch/harness/ExecutionEnv.hpp` (`ExecOptions`, `ShellExecResult`, `exec`); `src/util/Process.hpp` split-stream process | Near-term parity | Split stdout/stderr, cwd/env override, per-stream truncation, callback support, typed execution errors, and bash opt-in safety all landed in T4. |
| Agent harness event hooks (`BeforeAgentStartEvent`, provider request/response, tool/session/model/resource updates) | Missing | Deferred parity | U5 only creates agent lifecycle stream events; harness extension hooks come later. |
| Compaction and branch-summary preparation contracts | Session entry values plus `SessionTree::BranchSummaryHook` | Partial parity | Tree reconstruction and a branch-summary hook seam exist; runtime compaction and automatic branch-summary orchestration remain deferred. |
| `AgentHarnessOptions` / `AgentHarness` | Missing | Deferred parity | Full harness object is beyond pre-implementation cleanup. |

## Coding-Agent Runtime and CLI Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `Args`, `Mode`, `parseArgs`, `printHelp`, `isValidThinkingLevel` | CLI11 parsing and preflight under `src/cli/`, with runtime config in `src/cli/CliRuntimeConfig.hpp` | MVP parity for current flags | Current text/JSON/RPC mode combinations, session/resume rules, provider overrides, trust/resource flags, and max-turn validation are adapter-owned; richer pi flags remain deferred. |
| One-shot, REPL, resume/session flags | `src/main.cpp`, `src/coding_agent/runtime/AsyncCliRuntime.*`, CLI smoke tests | MVP parity | Text one-shot/REPL, JSON one-shot, RPC stdin, new session, and active-path resume are implemented with pre-session failures kept off machine-readable stdout. |
| JSON/RPC modes | `src/coding_agent/runtime/JsonEventPrinter.*`, `RpcMode.*`, `RpcJsonl.*`, and private `AgentSessionPromptAccess` | MVP parity / partial command set | JSON emits the v3 session header followed by direct, redacted, bounded AgentSession events. RPC emits no header; it interleaves correlated responses with flushed direct events, acknowledges prompt success after AgentSession preflight, returns preflight rejection as an error response, and emits no second execution-outcome response. Neither mode emits C++ schema metadata, sequence counters, content-status substitutions, or runtime terminal records. |
| SDK `CreateAgentSessionOptions`, `CreateAgentSessionResult`, `createAgentSession`, `AgentSession.prompt`, `AgentSession.subscribe` | `include/cch/coding_agent/Sdk.hpp`, `src/coding_agent/Sdk.cpp`, `src/coding_agent/runtime/AgentSessionRuntime.*` | MVP parity / partial | The source-level SDK creates or resumes sessions, injects host capabilities/resources, and exposes one success-or-error prompt completion plus persistent move-only subscriptions and separate live-state accessors. Prompt-result status models, per-prompt sinks, SDK command registration, runtime replacement, concurrent prompts, abort/queue APIs, compaction, and ABI stability are absent or deferred. |
| Runtime service split (`agent-session-runtime`, services, session lifecycle, event printer) | `AgentSessionRuntime`, `SessionFactory`, `RuntimeServices`, `SessionLifecycle`, event printers, and frontend adapters under `src/coding_agent/runtime/` | MVP parity for current consumers | `SessionFactory` centralizes shared CLI/SDK assembly; `AgentSessionRuntime` owns prompt expansion, live-state-first event handling, subscriber delivery, and incremental persistence; text/JSON/RPC adapters own presentation and protocol policy. |
| Settings/model resolver/project trust/resources | `ConfigLoader`, `ProviderConfigResolution`, `ProjectTrust`, `ProjectResources`, and `ProjectResourceLoader` | MVP parity for the supported subset | Provider/model defaults and resume precedence, user trust state, project resource controls, skill/template loading, and diagnostics-as-values are implemented. Global/config-driven resource directories and extension/package loading remain deferred. |

## Session Entry Contracts

| pi contract | C++ equivalent | Classification | Notes |
| --- | --- | --- | --- |
| `SessionMessageEntry` / `MessageEntry` | `SessionEntryKind::Message` plus `JsonlSessionStore::append` | MVP parity | New sessions use a v3 header and append redacted typed message entries; legacy v2 headers remain readable. AgentSession persists each completed user, assistant, and tool-result message after subscriber delivery. |
| `ModelChangeEntry` | `ModelChangeValue` and `append_model_change` | MVP value/storage parity | Typed write/read support exists; public runtime model-switch commands remain deferred. |
| `ThinkingLevelChangeEntry` | `ThinkingLevelChangeValue` and `append_thinking_level_change` | MVP value/storage parity | Typed write/read support exists; public thinking controls remain deferred. |
| `ActiveToolsChangeEntry` | `ActiveToolsChangeValue` and `append_active_tools_change` | MVP value/storage parity | Typed write/read support exists. |
| `CompactionEntry` | `CompactionEntryValue` and `append_compaction` | Partial parity | Typed storage and compaction-aware context reconstruction exist; runtime compaction execution remains deferred. |
| `BranchSummaryEntry` | `BranchSummaryEntryValue` and `append_branch_summary` | Partial parity | Typed storage, context reconstruction, and a branch-summary hook seam exist; automatic summary generation remains deferred. |
| `CustomEntry` | `CustomEntryValue` and `append_custom_entry` | MVP value/storage parity | Typed opaque state storage exists; extension behavior remains deferred. |
| `CustomMessageEntry` | `CustomMessageEntryValue` and `append_custom_message_entry` | MVP value/storage parity | Typed storage and model-context conversion support exist. |
| `LabelEntry` / `SessionInfoEntry` | `LabelEntryValue`, `SessionInfoEntryValue`, `append_label_change`, and `append_session_info` | MVP value/storage parity | Typed metadata storage exists. |
| `LeafEntry` | `LeafEntryValue`, loaded leaf tracking, and `SessionTree` leaf selection | MVP navigation parity | Stable IDs anchor active-path reconstruction and navigation. |
| `SessionTreeEntryBase` IDs (`id`, `parentId`) and leaf tracking | `SessionEntry`, `JsonlSessionStore`, and `SessionTree` | MVP parity | V3 entries carry stable IDs/parents; the store and tree track the active leaf while tolerating unknown future entries. |
| Tree context reconstruction (`buildSessionContext`) | `SessionTree::buildSessionContext` and `SessionResume` | MVP parity for resume | Resume rebuilds the durable active path with compaction-aware context; SDK v1 still rejects active branched or compacted topologies it cannot expose safely. |

## Feature Classification Summary

| Area | MVP parity | Near-term parity | Deferred parity |
| --- | --- | --- | --- |
| AI contracts | Text/thinking/image/tool-call content, messages, tools, usage, streaming chat seam, OpenAI-compatible adapter | Provider registry, fake provider registration, forwarding stream phases through agent events | Full model catalog, cost accounting, OAuth/subscription flows, image generation, provider-specific reasoning compatibility |
| Agent loop | Async loop/state, tool execution and hooks, move-only pi-shaped supported events, deterministic tool scheduling, context transforms, and internal steering/follow-up retrieval | Additional currently useful agent contracts | Public queue/cancellation APIs, tool progress events, retry, compaction, and unsupported event families |
| Harness | Workspace-guarded filesystem/shell capabilities, bash opt-in, redaction, v3 append-only entries, session tree navigation, and compaction-aware resume | Additional session operations needed by consumers | Runtime compaction/branch-summary orchestration and harness extension event pipeline |
| Coding-agent runtime | One-shot/REPL/resume CLI, CLI-owned built-ins, skill/template AgentSession prompting, live-state accessors, incremental persistence, direct JSON events, preflight-aligned narrow RPC, source-level SDK, settings, trust, and resources | Additional pi commands and replacement operations selected by real consumers | Extensions, packages, full RPC/SDK parity, public queue/cancellation, compaction/retry orchestration, and TUI integration |
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
