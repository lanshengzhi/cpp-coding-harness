---
title: "refactor: pi C++ contract inventory"
type: "refactor"
status: active
date: "2026-06-16"
origin: "docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---

<!-- markdownlint-disable MD013 MD025 -->

# refactor: pi C++ contract inventory

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.
**Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`.

## Purpose

This inventory maps the pi public contracts needed by the parity roadmap to the current C++ contract surface. It is a planning artifact for later implementation slices, not a claim that all listed pi behavior is implemented.

Classification terms:

- **MVP parity:** already present or required to preserve the current harness behavior while refactoring.
- **Near-term parity:** needed by the T2-T5 parity slices and should be represented by explicit C++ seams before higher-level features depend on it.
- **Intentionally deferred:** valid pi behavior, but outside the current pre-implementation cleanup scope.

## AI contracts (`pi:packages/ai`)

Reference files: `pi:packages/ai/src/types.ts`, `pi:packages/ai/src/stream.ts`, `pi:packages/ai/src/api-registry.ts`, `pi:packages/ai/src/models.ts`, `pi:packages/ai/src/providers/register-builtins.ts`, `pi:packages/ai/src/utils/event-stream.ts`.

| pi contract | Current C++ equivalent | Classification | Notes / gap |
| --- | --- | --- | --- |
| `TextContent`, `ThinkingContent`, `ImageContent`, `ToolCall` | `include/cch/ai/Content.hpp` (`TextContent`, `ThinkingContent`, `ImageContent`, `ToolCallContent`) | Near-term parity | Core variants exist. Later slices should verify field names, raw/parsed tool arguments, and thinking/image semantics against pi. |
| `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `Message` | `include/cch/ai/Message.hpp` (`SystemMessage`, `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `MessageVariant`) | MVP parity | C++ has a `SystemMessage` variant and typed timestamps. Future work should cover diagnostics, provider/model response metadata, and extended coding-agent messages. |
| `Usage`, `StopReason` | `include/cch/ai/Usage.hpp` (`Usage`, `UsageCost`, `AssistantStopReason`) | MVP parity | Existing tests cover basic contracts. Parity slices should check pi stop reasons `stop`, `length`, `toolUse`, `error`, `aborted`. |
| `Tool`, JSON schema contracts, `Context` | `include/cch/ai/Tool.hpp`, `include/cch/ai/Context.hpp` | MVP parity | C++ uses `util::JsonValue` and passive schema structs. Keep provider DTOs out of these headers. |
| `AssistantMessageEvent` / assistant message event stream | `include/cch/ai/StreamEvent.hpp` | Near-term parity | C++ already has start/text/thinking/tool/done/error event structs. U5 should forward thinking/tool-call deltas through the agent lifecycle without exposing sensitive payloads by default. |
| `StreamFunction`, `stream`, `streamSimple`, stream options | `include/cch/ai/ChatClient.hpp` (`StreamingChatClient`, `StreamChatRequest`) | Near-term parity | The C++ seam is provider-neutral but narrower than pi. Later provider work should add option compatibility only behind adapters. |
| `Api`, `Provider`, `Model`, `ImagesModel`, thinking-level maps, cost helpers | No first-class public model registry yet | Near-term parity | U4 introduces a provider registry; full model catalog, thinking-level clamping, cost calculation, image generation, and provider metadata remain later T2/T5 work. |
| `registerApiProvider`, `getApiProvider`, `getApiProviders`, `clearApiProviders` | Missing | Near-term parity | U4 should add the smallest C++ registry seam: provider-name key plus provider-neutral factory context unless pi contract inspection requires otherwise. |
| OAuth/subscription provider flows and image APIs | Missing | Intentionally deferred | Out of scope until provider/model registry and runtime configuration are stable. |

## Agent contracts (`pi:packages/agent`)

Reference files: `pi:packages/agent/src/types.ts`, `pi:packages/agent/src/agent-loop.ts`.

| pi contract | Current C++ equivalent | Classification | Notes / gap |
| --- | --- | --- | --- |
| `AgentLoopConfig`, model/tool/message loop options | `include/cch/agent/AgentLoop.hpp`, `include/cch/agent/AgentContext.hpp` (`AsyncAgentOptions`) | MVP parity | Current loop supports model requests, tool calls, max turns, and event sinks. It lacks most pi hook/context transform seams. |
| `AgentState` (`systemPrompt`, `model`, `thinkingLevel`, `tools`, `messages`, streaming state, pending tool calls) | Missing as a public value type | Near-term parity | U5 should add a passive observable `AgentState` without making callbacks copyable. |
| `AgentEvent` (`agent_start`, `turn_start`, message/tool updates, provider payload/response, `agent_end`) | `include/cch/agent/AgentEvent.hpp` | Near-term parity | C++ has lifecycle events for model/turn/message/tool start/end. U5 should add thinking/tool-call stream events while preserving existing CLI output. |
| `AgentTool`, `AgentToolResult`, `AgentToolUpdateCallback` | `include/cch/agent/AgentTool.hpp`, `include/cch/agent/ToolRegistry.hpp` | MVP parity | Async tool execution exists. Partial tool updates and richer details are not yet exposed. |
| `beforeToolCall`, `afterToolCall`, `shouldStopAfterTurn`, `prepareNextTurn` | Missing | Intentionally deferred for this cleanup | T3 follow-up work should add hooks with tests for blocking, overrides, thrown errors, and termination semantics. |
| `transformContext`, `convertToLlm`, steering/follow-up messages | Missing | Intentionally deferred for this cleanup | Needs a dedicated T3 slice after state/event seams exist. |
| Sequential vs. parallel tool execution modes | Current loop is effectively serialized | Intentionally deferred for this cleanup | Parallel read-only tool execution is deferred until tool classification and deterministic insertion rules are designed. |

## Harness and execution contracts (`pi:packages/agent/src/harness`)

Reference files: `pi:packages/agent/src/harness/types.ts`, `pi:packages/agent/src/harness/agent-harness.ts`.

| pi contract | Current C++ equivalent | Classification | Notes / gap |
| --- | --- | --- | --- |
| `FileSystem` (`absolutePath`, `joinPath`, `fileInfo`, `listDir`, `canonicalPath`, `exists`, directory/temp APIs, binary read/write) | `include/cch/harness/ExecutionEnv.hpp` (`AsyncExecutionEnv` file read/write/edit subset) | Near-term parity | Current C++ env covers the tool-facing subset with containment. Full filesystem capability parity is later T4 work. |
| `Shell` / `ExecutionEnvExecOptions` / stdout-stderr execution result | `include/cch/harness/ExecutionEnv.hpp` (`AsyncShellResult`), `src/util/Process.hpp` | Near-term parity | Current process runner is synchronous internally. U6 should make the process boundary awaitable, preserve output limits/redaction, and terminate process trees on timeout. |
| Structured harness error codes (`FileError`, `ExecutionError`, `SessionError`, etc.) | `include/cch/util/Error.hpp` (`ErrorCode`, `Error`, `Expected`) | MVP parity | C++ uses project-local error enums and `std::expected`. Do not reintroduce `util::Result` or Boost.JSON domain contracts. |
| Resource contracts (`Skill`, `PromptTemplate`, `AgentHarnessResources`) | Missing | Intentionally deferred | Skills/resources/extensions are T6 scope after runtime/session trust boundaries exist. |
| Harness lifecycle events (`before_agent_start`, provider payload/response, tool call/result, session tree/compact, model/thinking/tools/resource updates) | Partially represented by `AgentLifecycleEvent` | Near-term parity | U5 covers agent-level event expansion. Harness/session/resource event parity is later T3/T4/T6 work. |

## Session contracts

Reference files: `pi:packages/agent/src/harness/types.ts`, `pi:packages/coding-agent/docs/session-format.md`.

| pi contract | Current C++ equivalent | Classification | Notes / gap |
| --- | --- | --- | --- |
| v3 `SessionHeader` (`type: session`, `version`, `id`, `timestamp`, `cwd`, optional `parentSession`) | `include/cch/harness/session/SessionEntry.hpp` (`SessionMetadata`), `src/harness/session/JsonlSessionStore.cpp` | Near-term parity | Current C++ writes v2 header. U8 should parse future/tree metadata without breaking v2 resume. |
| `SessionTreeEntryBase` (`id`, `parentId`, `timestamp`) | Missing from current `SessionEntry` | Near-term parity | U8 should add tree IDs/parent IDs as passive optional fields. |
| `MessageEntry` | `SessionEntryKind::Message`, `LoadedSession::messages` | MVP parity | Legacy message resume is the current canonical behavior and must remain stable. |
| `thinking_level_change`, `model_change`, `active_tools_change` | Missing | Near-term parity | U8 should parse and preserve entries; using them to reconstruct context is later T4 work. |
| `compaction`, `branch_summary`, `custom`, `custom_message`, `label`, `session_info`, `leaf` | Missing | Near-term parity for parse/preserve, intentionally deferred for full behavior | Parse-only support can prevent unknown-entry failures. Branch navigation, compaction, and context reconstruction are follow-up slices. |
| Session tree context building and branch navigation | Missing | Intentionally deferred | Do not claim full v3 resume compatibility until reconstruction semantics have tests. |

## Coding-agent runtime and CLI contracts (`pi:packages/coding-agent`)

Reference files: `pi:packages/coding-agent/src/cli/args.ts`, `pi:packages/coding-agent/src/core/sdk.ts`, `pi:packages/coding-agent/src/core/agent-session-runtime.ts`, `pi:packages/coding-agent/docs/json.md`, `pi:packages/coding-agent/docs/rpc.md`.

| pi contract | Current C++ equivalent | Classification | Notes / gap |
| --- | --- | --- | --- |
| CLI mode parsing (`text`, `json`, `rpc`), continue/resume, provider/model/api key, system prompt, thinking level, tools/resources, working directory, permissions | `src/main.cpp`, `src/AsyncCliRuntime.hpp` (`AsyncCliRuntimeConfig`) | Near-term parity | Current C++ supports text mode plus fake/repl/workspace/session/resume/max-turns/bash/model/base-url/api-key-env. U3 should migrate current behavior to CLI11 without adding unsupported modes. |
| Runtime/session orchestration (`createAgentSession`, session services, event stream, prompts) | `src/AsyncCliRuntime.cpp` | Near-term parity | U7 should split only current testable responsibilities: argument config, provider/model resolution, tool registration, session lifecycle, event printing, and agent execution. |
| SDK embedding (`CreateAgentSessionOptions`, `CreateAgentSessionResult`) | Missing | Intentionally deferred | SDK/RPC embedding is T8 scope after runtime seams are separated from CLI printing. |
| JSON event stream and RPC mode | Missing | Intentionally deferred | Add only after event/state/session boundaries are stable. |
| Settings, model resolution, slash commands, prompt templates | Missing | Intentionally deferred | T5/T6 work; avoid placeholder APIs in this cleanup. |

## Built-in tool contracts

Reference files: `pi:packages/coding-agent/src/core/bash-executor.ts`, `pi:packages/coding-agent/src/core/output-guard.ts`, `pi:packages/coding-agent/src/core/exec.ts`.

| pi contract | Current C++ equivalent | Classification | Notes / gap |
| --- | --- | --- | --- |
| Read/write/edit/bash tool schema and behavior | `include/cch/tools/ToolFactories.hpp`, `src/tools/AsyncToolFactories.cpp`, `tests/tools/AsyncToolsTest.cpp` | MVP parity | Current tools exist with workspace containment, exact edit matching, output limiting, bash opt-in, and environment redaction. |
| Bash execution output guard and timeout semantics | `src/tools/OutputLimiter.hpp`, `src/util/Process.hpp`, `src/harness/AsyncLocalExecutionEnv.cpp` | Near-term parity | U6 should make shell execution truly async and preserve process-tree timeout semantics. |
| Tool permission prompts and hooks | Missing | Intentionally deferred | Requires agent hook and runtime permission surfaces first. |

## Per-slice implementation checklist

Every future parity slice should begin with this checklist:

1. Read the relevant `pi:` reference contract files and cite them in the plan, PR notes, or implementation summary.
2. Identify the smallest C++ public seam needed for the slice; keep domain headers aggregate-friendly and provider DTO-free.
3. Add or update tests for the pi-semantic behavior before broad structural moves.
4. Keep implementation adapters under `src/...` or provider/glaze layers; do not leak private include paths into `include/cch/...`.
5. Preserve current CLI/session/tool behavior unless the slice explicitly changes it.
6. Update README, AGENTS.md, and this roadmap only when behavior, module boundaries, or agent routing changes.

## Immediate follow-up mapping for the cleanup plan

| Cleanup unit | Inventory dependency |
| --- | --- |
| U2 target split | Uses package mapping from AI/agent/harness/runtime sections above. |
| U3 CLI11 migration | Uses current C++ CLI subset from the runtime matrix; does not implement pi JSON/RPC modes. |
| U4 provider registry | Uses AI registry/model rows; starts with provider-name key and factory context. |
| U5 lifecycle/state | Uses agent `AgentState` / `AgentEvent` rows and AI assistant-message event rows. |
| U6 async process | Uses harness `Shell` / execution result rows while preserving stronger C++ containment/redaction. |
| U7 runtime split | Uses coding-agent runtime rows; extracts only current testable responsibilities. |
| U8 session tree prep | Uses session tree rows; parse/preserve now, reconstruct later. |
| U9 docs | Updates README/AGENTS/TODO to reflect implemented module boundaries and remaining deferred parity. |
