---
title: "refactor: Track pi module parity in C++"
type: "refactor"
status: active
date: "2026-06-16"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---

<!-- markdownlint-disable MD013 MD025 -->

# refactor: Track pi module parity in C++

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.

## Purpose

This TODO checklist is the durable roadmap for evolving `cpp-coding-harness` into a C++ implementation of pi's agent harness architecture. The goal is **module and contract parity**, not a mechanical TypeScript-to-C++ translation.

The current C++ project already covers the core loop: provider request, tool calls, local tool execution, semantic events, CLI output, and JSONL sessions. The remaining work is to expand that seed along pi's package boundaries while preserving the repository's C++ architecture rules: passive value contracts, capability seams, move-only events, and localized serialization machinery.

## Pre-Implementation Research Update (2026-06-16)

Research for this roadmap found that several structural seams should be refactored before starting the T2–T9 parity slices. The focused plan is `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`.

Confirmed pre-implementation decisions:

- Split the monolithic CMake library into package-like targets before broad parity work.
- Replace the hand-written CLI parser with CLI11, which is already declared in `vcpkg.json`.
- Move the scripted fake provider out of CLI runtime and expose it through a provider/model registry path.
- Replace blocking shell execution with true `boost::asio` async process I/O.

The first implementation pass should execute that cleanup plan and produce the T0 contract inventory before expanding provider, agent, session, resource, or TUI parity.

## Non-Negotiable Migration Rules

- [ ] Treat pi as the reference for module boundaries and interface contracts; do not invent new large subsystems without first checking the corresponding pi package.
- [ ] Express contracts idiomatically in C++ with aggregate-friendly structs, `std::variant`, `std::expected`, interfaces, RAII ownership, and `util::JsonValue` for dynamic facts.
- [ ] Keep provider DTOs, Glaze mappings, transport details, and parser helpers out of domain-facing headers.
- [ ] Preserve current CLI/session/tool behavior unless a TODO item explicitly changes the contract and adds tests for the new behavior.
- [ ] Add or tighten tests before broad structural moves; use tests to protect semantic parity rather than old class names.
- [ ] Keep all implementation-plan paths repo-relative; label reference paths with `pi:` instead of using absolute paths.

## Module Parity Map

| pi reference area | C++ target area | Current state | Contract references to inspect first |
| --- | --- | --- | --- |
| `pi:packages/ai` | `include/cch/ai/`, `src/ai/`, `tests/ai/` | Partial: messages, content, usage, stream events, OpenAI-compatible provider, SSE, Glaze DTO boundary exist. | `pi:packages/ai/src/types.ts`, `pi:packages/ai/src/stream.ts`, `pi:packages/ai/src/api-registry.ts`, `pi:packages/ai/src/providers/` |
| `pi:packages/agent` | `include/cch/agent/`, `src/agent/`, `include/cch/harness/`, `src/harness/`, `tests/agent/`, `tests/harness/` | Partial: async loop, event sink, async tools, execution env, and JSONL store exist; richer agent state/hooks/session tree are missing. | `pi:packages/agent/src/types.ts`, `pi:packages/agent/src/agent-loop.ts`, `pi:packages/agent/src/harness/types.ts`, `pi:packages/agent/src/harness/agent-harness.ts` |
| `pi:packages/coding-agent` core/CLI | `src/AsyncCliRuntime.*`, `src/main.cpp`, future CLI/core modules, `tests/cli/` | MVP only: fake/real provider wiring, one-shot/REPL/resume, semantic event lines. | `pi:packages/coding-agent/src/core/`, `pi:packages/coding-agent/src/cli/`, `pi:packages/coding-agent/docs/usage.md`, `pi:packages/coding-agent/docs/json.md`, `pi:packages/coding-agent/docs/rpc.md` |
| `pi:packages/coding-agent` resources | future resource/config/extension modules | Mostly absent: settings, packages, extensions, skills, prompts, themes, project trust. | `pi:packages/coding-agent/docs/settings.md`, `pi:packages/coding-agent/docs/extensions.md`, `pi:packages/coding-agent/docs/skills.md`, `pi:packages/coding-agent/docs/prompt-templates.md`, `pi:packages/coding-agent/docs/packages.md` |
| `pi:packages/tui` | future `include/cch/tui/`, `src/tui/`, `tests/tui/` or equivalent | Absent: current CLI is line-oriented only. | `pi:packages/tui/src/`, `pi:packages/coding-agent/docs/tui.md`, `pi:packages/coding-agent/docs/themes.md`, `pi:packages/coding-agent/docs/keybindings.md` |

## Current C++ Baseline to Preserve

- [ ] `include/cch/ai` remains the public AI contract surface for content blocks, messages, context, tools, usage, stream events, and chat clients.
- [ ] `src/ai/providers` remains provider-specific implementation code, with Glaze DTOs isolated under `include/cch/ai/glaze` and `src/ai/glaze`.
- [ ] `include/cch/agent` remains the public agent orchestration surface for loop, context, events, tools, and registry behavior.
- [ ] `include/cch/harness` remains the execution/session capability boundary.
- [x] `src/AsyncCliRuntime.*`, `src/main.cpp`, and `src/coding_agent/runtime/` split current runtime wiring into CLI parsing, runtime services, session lifecycle, event printing, and agent execution seams.
- [ ] `tests/architecture` continues to guard public header boundaries, no `src` public leakage, no old sync tool surface, no Boost.JSON domain contract, and move-only callbacks.

## TODO Roadmap

### T0. Reference Contract Inventory

- [x] Build a contract inventory that maps pi public types, events, commands, and session entries to existing or missing C++ equivalents.
  - **References:** `pi:packages/ai/src/types.ts`, `pi:packages/agent/src/types.ts`, `pi:packages/agent/src/harness/types.ts`, `pi:packages/coding-agent/docs/session-format.md`.
  - **Output:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`.
  - **Done when:** every future TODO item can cite either an existing C++ contract or a named missing contract.
- [x] Classify pi features into MVP parity, near-term parity, and intentionally deferred parity.
  - **Done when:** README and this TODO agree on which gaps are intentional: full TUI, extensions, package installation, OAuth, multi-provider registry, session tree, compaction, SDK/RPC, and sandbox/container integration.
- [x] Add a recurring implementation checklist for each future parity slice.
  - **Done when:** each slice starts by reading the relevant `pi:` contract, adding tests, implementing the smallest C++ seam, and updating docs only if behavior or public boundaries change.

### T1. Package-Style Library Boundaries

- [x] Split `cpp_harness_lib` into package-like targets such as AI, agent, harness, tools, coding-agent runtime, and future TUI.
  - **Decision:** confirmed in `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`.
  - **References:** `CMakeLists.txt`, `pi:package.json`, `pi:packages/*/package.json`.
  - **Done when:** dependencies flow in the same direction as pi packages: coding-agent/runtime depends on agent, harness, and tools; tools bridge agent contracts to harness capabilities; providers stay under AI.
- [x] Add architecture tests for forbidden dependency directions.
  - **Files:** `tests/architecture/ArchitectureSurfaceScanTest.cpp`, `tests/architecture/PublicHeaderBoundaryTest.cpp`, `tests/architecture/CMakeDependencyTest.cpp`.
  - **Done when:** tests fail if agent code starts depending on CLI/runtime code, or public contracts start including private implementation headers.
- [x] Document the package mapping in README.
  - **Files:** `README.md`.
  - **Done when:** contributors can map a pi package to the corresponding C++ module before editing code.

### T2. `pi-ai` Contract Parity

- [ ] Complete message/content parity for text, image, thinking, tool-call, tool-result, diagnostics, usage, stop reasons, and provider metadata.
  - **Files:** `include/cch/ai/Content.hpp`, `include/cch/ai/Message.hpp`, `include/cch/ai/Usage.hpp`, `include/cch/ai/StreamEvent.hpp`, `tests/ai/MessageContractTest.cpp`.
  - **References:** `pi:packages/ai/src/types.ts`.
  - **Test scenarios:** round-trip each content block type; preserve tool call IDs and raw/parsed arguments; preserve error/aborted stop reasons.
- [ ] Align streaming event semantics with pi's assistant message event protocol.
  - **Files:** `include/cch/ai/StreamEvent.hpp`, `src/ai/providers/SseParser.cpp`, `tests/ai/providers/SseParserTest.cpp`.
  - **References:** `pi:packages/ai/src/types.ts`, `pi:packages/ai/src/stream.ts`, `pi:packages/ai/src/utils/event-stream.ts`.
  - **Test scenarios:** start, text delta/end, thinking delta/end, toolcall delta/end, done, and error events produce the expected final assistant message.
- [x] Introduce a provider/model registry seam before adding more providers.
  - **Files:** `include/cch/ai/ProviderRegistry.hpp`, `src/ai/ProviderRegistry.cpp`, `src/ai/providers/`, `tests/ai/ProviderRegistryTest.cpp`.
  - **References:** `pi:packages/ai/src/api-registry.ts`, `pi:packages/ai/src/models.ts`, `pi:packages/ai/src/env-api-keys.ts`, `pi:packages/ai/src/providers/register-builtins.ts`.
  - **Done when:** OpenAI-compatible/Kimi wiring is one registered provider path, not hardcoded throughout CLI runtime.
- [ ] Add provider compatibility options only behind provider adapters.
  - **Files:** `src/ai/providers/OpenAIChatClient.cpp`, `src/ai/glaze/ProviderDtos.hpp`, `tests/ai/providers/OpenAIChatClientTest.cpp`.
  - **References:** `pi:packages/ai/src/providers/openai-completions.ts`, `pi:packages/ai/src/providers/openai-responses.ts`, `pi:packages/ai/src/providers/transform-messages.ts`.
  - **Done when:** custom base URLs, tool-result naming, reasoning/thinking compatibility, timeout/retry options, and session affinity live in provider-specific code.
- [ ] Defer OAuth and subscription-provider flows until model/provider registry contracts are stable.
  - **References:** `pi:packages/ai/src/oauth.ts`, `pi:packages/coding-agent/docs/providers.md`.
  - **Done when:** the deferral is documented, and no placeholder API implies OAuth is already supported.

### T3. `pi-agent-core` Loop, State, and Tool Parity

- [x] Expand agent state to match pi's public state concepts without copying TypeScript declaration merging.
  - **Files:** `include/cch/agent/AgentContext.hpp`, `include/cch/agent/AgentLoop.hpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`.
  - **Test scenarios:** active tools, messages, streaming message, pending tool calls, error message, model, and thinking level are observable through a C++-appropriate state seam.
- [ ] Add pre/post tool-call hooks and graceful blocking semantics.
  - **Files:** `include/cch/agent/AgentTool.hpp`, `src/agent/AgentLoop.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`, `pi:packages/agent/src/agent-loop.ts`.
  - **Test scenarios:** before-hook blocks a tool with an error tool result; after-hook overrides content/details/isError/terminate; thrown hook errors become stable agent errors.
- [ ] Add context transform, conversion-to-LLM, steering messages, follow-up messages, and prepare-next-turn seams.
  - **Files:** `include/cch/agent/AgentLoop.hpp`, `include/cch/agent/AgentContext.hpp`, `src/agent/AgentLoop.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`.
  - **Done when:** continuation and multi-turn control can be implemented without special cases in CLI runtime.
- [ ] Evaluate sequential versus parallel tool execution with deterministic result insertion.
  - **Files:** `include/cch/agent/AgentTool.hpp`, `src/agent/AgentLoop.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`.
  - **Test scenarios:** read-only tools may complete out of order while tool-result messages are appended in assistant source order; mutating tools remain serialized unless classified safe.
- [x] Preserve move-only event sink semantics while adding missing lifecycle events.
  - **Files:** `include/cch/agent/AgentEvent.hpp`, `tests/architecture/MoveOnlyCallbackTest.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`, `pi:packages/coding-agent/docs/extensions.md`.
  - **Done when:** event order can support future UI/extensions without requiring copyable callbacks.

### T4. Harness Execution Environment and Session Tree Parity

- [ ] Expand execution environment from current workspace/file/shell methods toward pi's `FileSystem` and `Shell` capability contracts.
  - **Files:** `include/cch/harness/ExecutionEnv.hpp`, `include/cch/harness/LocalExecutionEnv.hpp`, `src/harness/`, `tests/harness/AsyncLocalExecutionEnvTest.cpp`.
  - **References:** `pi:packages/agent/src/harness/types.ts`.
  - **Test scenarios:** absolute path, join path, file info, list dir, canonical path, exists, create/remove directory, temp file/dir, read/write binary, streaming stdout/stderr, stable error codes.
- [ ] Keep workspace containment and secret redaction stronger than the pi default where the C++ harness already has it.
  - **Files:** `src/tools/PathGuard.hpp`, `src/util/Redactor.hpp`, `tests/tools/AsyncToolsTest.cpp`, `tests/harness/AsyncLocalExecutionEnvTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/security.md`, `pi:packages/coding-agent/docs/containerization.md`.
  - **Done when:** parity work does not weaken symlink escape checks, bash opt-in, private session permissions, or redaction boundaries.
- [ ] Migrate JSONL sessions from the current linear message format toward pi's tree-entry format.
  - **Files:** `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `tests/harness/session/JsonlSessionStoreTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/session-format.md`, `pi:packages/agent/src/harness/types.ts`.
  - **Test scenarios:** load legacy C++ sessions; write v3-style tree entries; preserve leaf ID; ignore unknown entries safely; keep resume ordering stable.
- [ ] Add model change, thinking-level change, active-tools change, custom, custom-message, label, compaction, and branch-summary entry support incrementally.
  - **Files:** `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `tests/harness/session/JsonlSessionStoreTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/session-format.md`.
  - **Done when:** each entry type has parse/write/context-building tests before being used by CLI or extensions.
- [ ] Add branch navigation and context reconstruction only after entry compatibility is proven.
  - **References:** `pi:packages/coding-agent/docs/sessions.md`, `pi:packages/coding-agent/docs/compaction.md`.
  - **Done when:** branch/fork/leaf behavior has deterministic tests and does not break simple `--resume`.

### T5. Built-In Tools and Coding-Agent Runtime Parity

- [ ] Reconcile built-in tool schemas and behavior with pi's read, write, edit, and bash tools.
  - **Files:** `include/cch/tools/ToolFactories.hpp`, `src/tools/AsyncToolFactories.cpp`, `tests/tools/AsyncToolsTest.cpp`.
  - **References:** `pi:packages/coding-agent/src/core/bash-executor.ts`, `pi:packages/coding-agent/src/core/output-guard.ts`, `pi:packages/coding-agent/src/core/exec.ts`.
  - **Test scenarios:** schema shape, path validation, truncation, edit ambiguity, timeout, exit code, stderr/stdout representation, and secret filtering.
- [ ] Add pi-style extended runtime messages only after session entry support exists.
  - **Files:** `include/cch/ai/Message.hpp`, `include/cch/harness/session/`, `tests/harness/session/JsonlSessionStoreTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/session-format.md`, `pi:packages/coding-agent/src/core/messages.ts`.
  - **Done when:** bash execution, custom, compaction summary, and branch summary messages have explicit C++ variants or an intentional custom-message escape hatch.
- [x] Split CLI/runtime wiring into pi-coding-agent-like responsibilities.
  - **Files:** `src/AsyncCliRuntime.hpp`, `src/AsyncCliRuntime.cpp`, `src/main.cpp`, `src/coding_agent/runtime/`, `tests/cli/CliSmokeTest.cpp`.
  - **References:** `pi:packages/coding-agent/src/core/agent-session-runtime.ts`, `pi:packages/coding-agent/src/core/agent-session-services.ts`, `pi:packages/coding-agent/src/core/agent-session.ts`, `pi:packages/coding-agent/src/cli/args.ts`.
  - **Done when:** argument parsing, model resolution, tool registration, session lifecycle, event printing, and agent execution are separate seams.
- [ ] Add configuration and model resolution before expanding provider count.
  - **Files:** future config/runtime files, `tests/cli/`.
  - **References:** `pi:packages/coding-agent/src/config.ts`, `pi:packages/coding-agent/src/core/settings-manager.ts`, `pi:packages/coding-agent/src/core/model-registry.ts`, `pi:packages/coding-agent/src/core/model-resolver.ts`, `pi:packages/coding-agent/docs/settings.md`, `pi:packages/coding-agent/docs/models.md`.
  - **Done when:** provider/model/base-url/api-key-env defaults can be resolved through configuration rather than ad hoc CLI conditionals.
- [ ] Add slash-command and prompt-processing seams after runtime/session boundaries are stable.
  - **References:** `pi:packages/coding-agent/src/core/slash-commands.ts`, `pi:packages/coding-agent/src/core/prompt-templates.ts`, `pi:packages/coding-agent/docs/usage.md`, `pi:packages/coding-agent/docs/prompt-templates.md`.
  - **Done when:** commands can be tested without starting an interactive TUI.

### T6. Resources, Skills, Extensions, and Packages

- [ ] Design a C++ extension boundary before implementing extensions.
  - **References:** `pi:packages/coding-agent/docs/extensions.md`, `pi:packages/coding-agent/examples/extensions/`.
  - **Decision to record:** native C++ plugin, process/RPC extension, embedded JS/TS bridge, or intentionally unsupported for the C++ harness.
  - **Done when:** the decision preserves security posture and does not leak dynamic extension concerns into core AI/agent contracts.
- [ ] Implement skill loading as file/resource discovery before model-visible integration.
  - **References:** `pi:packages/coding-agent/docs/skills.md`, `pi:packages/agent/src/harness/skills.ts`.
  - **Test scenarios:** global/project skill discovery, relative reference resolution, disabled model invocation, prompt formatting, and duplicate handling.
- [ ] Implement prompt template loading and invocation.
  - **References:** `pi:packages/coding-agent/docs/prompt-templates.md`, `pi:packages/agent/src/harness/prompt-templates.ts`.
  - **Done when:** templates can be expanded deterministically in tests without provider calls.
- [ ] Implement package discovery/installation only after resource loading exists.
  - **References:** `pi:packages/coding-agent/docs/packages.md`.
  - **Scope boundary:** do not add npm/git package installation silently; it is a supply-chain surface and needs an explicit design/test plan.
- [ ] Add project trust and resource enable/disable controls before loading project-local executable resources.
  - **References:** `pi:packages/coding-agent/src/cli/project-trust.ts`, `pi:packages/coding-agent/src/core/trust-manager.ts`, `pi:packages/coding-agent/docs/security.md`.
  - **Done when:** untrusted project resources cannot execute by accident.

### T7. TUI, Themes, Keybindings, and Interactive UX

- [ ] Decide whether the C++ port should include native TUI parity or keep line-oriented CLI plus JSON/RPC modes first.
  - **References:** `pi:packages/tui/src/`, `pi:packages/coding-agent/docs/tui.md`, `pi:packages/coding-agent/docs/keybindings.md`, `pi:packages/coding-agent/docs/themes.md`.
  - **Done when:** the decision is captured before adding terminal rendering abstractions.
- [ ] If native TUI parity is approved, introduce terminal abstraction and diff rendering before widgets.
  - **Future files:** `include/cch/tui/`, `src/tui/`, `tests/tui/`.
  - **Test scenarios:** render diff stability, terminal size changes, key parsing, East Asian width, truncation, and ANSI style boundaries.
- [ ] Add components incrementally: text, box, input/editor, markdown, select list, loader, image, settings list.
  - **References:** `pi:packages/tui/src/components/`.
  - **Done when:** each component has pure rendering tests before it is wired into an interactive app.
- [ ] Add themes and keybindings after terminal and component contracts exist.
  - **References:** `pi:packages/coding-agent/docs/themes.md`, `pi:packages/coding-agent/docs/keybindings.md`.
  - **Done when:** keybindings stay configurable; no hardcoded shortcut checks enter runtime code.

### T8. SDK, RPC, and Machine-Readable Modes

- [ ] Add JSON event stream mode as the first machine-readable surface.
  - **Files:** `src/main.cpp`, `src/AsyncCliRuntime.*`, `tests/cli/CliSmokeTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/json.md`.
  - **Test scenarios:** stable event objects for model request, assistant deltas, tool call, tool result, errors, max turns, and completion.
- [ ] Add RPC mode only after runtime services are separated from CLI printing.
  - **References:** `pi:packages/coding-agent/docs/rpc.md`.
  - **Done when:** stdin/stdout JSONL requests can drive sessions without TUI assumptions.
- [ ] Add embeddable SDK surface after agent/session/resource seams are stable.
  - **References:** `pi:packages/coding-agent/docs/sdk.md`, `pi:packages/coding-agent/src/core/sdk.ts`.
  - **Done when:** a host application can create a session, register tools/resources, send prompts, receive events, and shut down cleanly without depending on CLI globals.

### T9. Security, Platform, and Distribution

- [ ] Keep documenting that workspace guard is not a sandbox.
  - **Files:** `README.md`.
  - **References:** `pi:packages/coding-agent/docs/security.md`, `pi:packages/coding-agent/docs/containerization.md`.
  - **Done when:** every new shell/file/package/extension feature states its trust boundary.
- [ ] Add platform compatibility plans for Windows, Termux, tmux, terminal setup, and shell aliases only when those surfaces are in scope.
  - **References:** `pi:packages/coding-agent/docs/windows.md`, `pi:packages/coding-agent/docs/termux.md`, `pi:packages/coding-agent/docs/tmux.md`, `pi:packages/coding-agent/docs/terminal-setup.md`, `pi:packages/coding-agent/docs/shell-aliases.md`.
  - **Done when:** platform-specific behavior has tests or documented manual validation.
- [ ] Treat package manager, dependency, release, and binary distribution work as separate supply-chain plans.
  - **References:** `pi:README.md`, `pi:AGENTS.md`, `pi:scripts/`, `pi:packages/coding-agent/package.json`.
  - **Done when:** no package installation or self-update feature lands without reviewable dependency and trust controls.

### T10. Documentation and Progress Hygiene

- [ ] Keep README aligned with the current implemented subset and the next parity target.
  - **Files:** `README.md`.
  - **Done when:** README never claims support for deferred pi features.
- [ ] Keep AGENTS.md aligned with the roadmap and reference-module routing.
  - **Files:** `AGENTS.md`.
  - **Done when:** agents know to inspect pi reference contracts before changing a parity area.
- [ ] Update this TODO document when a slice is completed, split, deferred, or superseded.
  - **Files:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`.
  - **Done when:** future sessions can resume work without re-discovering the migration strategy.
- [ ] For completed implementation slices, prefer adding a focused plan or solution note instead of bloating this roadmap with execution history.
  - **Done when:** this document stays a roadmap, not a changelog.

## Suggested Attack Order

0. Execute `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md` to create the T0 inventory, split package-style build targets, move fake provider wiring into the registry seam, adopt CLI11, make shell execution truly async, and prepare session/event seams.
1. T0 Reference Contract Inventory.
2. T1 Package-Style Library Boundaries.
3. T2 `pi-ai` stream/provider/model registry parity.
4. T3 `pi-agent-core` loop hooks, state, and tool execution parity.
5. T4 session tree and richer execution environment parity.
6. T5 coding-agent runtime split, config, and tool behavior parity.
7. T8 JSON/RPC surfaces before a large TUI investment.
8. T6 resources/extensions/packages once trust and runtime seams exist.
9. T7 native TUI only after machine-readable/runtime seams are stable.
10. T9/T10 continuously as security, platform, distribution, and docs guardrails.

## Per-Slice Definition of Done

For any checklist item that changes behavior or public contracts:

- [ ] Relevant pi contract files or docs were read and cited in the implementation plan or PR notes.
- [ ] Public C++ headers expose passive value contracts or narrow capability interfaces, not provider DTOs or implementation helpers.
- [ ] Tests cover both C++ behavior and pi-semantic compatibility for the slice.
- [ ] Architecture tests still pass for public boundary rules.
- [ ] README or AGENTS.md is updated if the user-facing behavior, module map, or agent routing changed.
- [ ] Deferred pi parity is stated explicitly rather than hidden behind placeholder APIs.
