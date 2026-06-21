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
| `pi:packages/coding-agent` resources | `include/cch/coding_agent/`, `src/coding_agent/`, `src/coding_agent/runtime/`, future extension/package modules | Partial: config/model defaults, project trust/resource controls, prompt expansion, project-local skill discovery/loading, skill prompt formatting, `/skill:name` invocation, and prompt-template loading exist; extensions, packages, and global/config-driven skill dirs remain missing. | `pi:packages/coding-agent/docs/settings.md`, `pi:packages/coding-agent/docs/extensions.md`, `pi:packages/coding-agent/docs/skills.md`, `pi:packages/coding-agent/docs/prompt-templates.md`, `pi:packages/coding-agent/docs/packages.md` |
| `pi:packages/tui` | future `include/cch/tui/`, `src/tui/`, `tests/tui/` or equivalent | Absent: current CLI is line-oriented only. | `pi:packages/tui/src/`, `pi:packages/coding-agent/docs/tui.md`, `pi:packages/coding-agent/docs/themes.md`, `pi:packages/coding-agent/docs/keybindings.md` |

## Current C++ Baseline to Preserve

- [x] `include/cch/ai` remains the public AI contract surface for content blocks, messages, context, tools, usage, stream events, and chat clients.
- [x] `src/ai/providers` remains provider-specific implementation code, with Glaze DTOs isolated under `include/cch/ai/glaze` and `src/ai/glaze`.
- [x] `include/cch/agent` remains the public agent orchestration surface for loop, context, events, tools, and registry behavior.
- [x] `include/cch/harness` remains the execution/session capability boundary.
- [x] `src/AsyncCliRuntime.*`, `src/main.cpp`, and `src/coding_agent/runtime/` split current runtime wiring into CLI parsing, runtime services, session lifecycle, event printing, and agent execution seams.
- [x] `tests/architecture` continues to guard public header boundaries, no `src` public leakage, no old sync tool surface, no Boost.JSON domain contract, and move-only callbacks.

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
  - **Result:** see the recurring checklist in `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`.

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

- [x] Complete message/content parity for text, image, thinking, tool-call, tool-result, diagnostics, usage, stop reasons, and provider metadata.
  - **Files:** `include/cch/ai/Content.hpp`, `include/cch/ai/Message.hpp`, `include/cch/ai/Usage.hpp`, `include/cch/ai/StreamEvent.hpp`, `tests/ai/MessageContractTest.cpp`.
  - **References:** `pi:packages/ai/src/types.ts`.
  - **Test scenarios:** round-trip each content block type; preserve tool call IDs and raw/parsed arguments; preserve error/aborted stop reasons.
- [x] Align streaming event semantics with pi's assistant message event protocol.
  - **Files:** `include/cch/ai/StreamEvent.hpp`, `src/ai/providers/SseParser.cpp`, `tests/ai/providers/SseParserTest.cpp`.
  - **References:** `pi:packages/ai/src/types.ts`, `pi:packages/ai/src/stream.ts`, `pi:packages/ai/src/utils/event-stream.ts`.
  - **Test scenarios:** start, text delta/end, thinking delta/end, toolcall delta/end, done, and error events produce the expected final assistant message.
- [x] Introduce a provider/model registry seam before adding more providers.
  - **Files:** `include/cch/ai/ProviderRegistry.hpp`, `src/ai/ProviderRegistry.cpp`, `src/ai/providers/`, `tests/ai/ProviderRegistryTest.cpp`.
  - **References:** `pi:packages/ai/src/api-registry.ts`, `pi:packages/ai/src/models.ts`, `pi:packages/ai/src/env-api-keys.ts`, `pi:packages/ai/src/providers/register-builtins.ts`.
  - **Done when:** OpenAI-compatible/Kimi wiring is one registered provider path, not hardcoded throughout CLI runtime.
- [x] Add provider compatibility options only behind provider adapters.
  - **Files:** `src/ai/providers/OpenAIChatClient.cpp`, `src/ai/glaze/ProviderDtos.hpp`, `include/cch/ai/providers/OpenAICompletionsCompat.hpp`, `tests/ai/providers/OpenAIChatClientTest.cpp`.
  - **References:** `pi:packages/ai/src/providers/openai-completions.ts`, `pi:packages/ai/src/providers/openai-responses.ts`, `pi:packages/ai/src/providers/transform-messages.ts`.
  - **Done when:** custom base URLs, tool-result naming, reasoning/thinking compatibility, timeout/retry options, and session affinity live in provider-specific code.
  - **Implementation plan:** `docs/plans/2026-06-18-001-feat-pi-ai-contract-parity-plan.md`.
- [x] Defer OAuth and subscription-provider flows until model/provider registry contracts are stable.
  - **References:** `pi:packages/ai/src/oauth.ts`, `pi:packages/coding-agent/docs/providers.md`.
  - **Done when:** the deferral is documented, and no placeholder API implies OAuth is already supported.
  - **Implementation plan:** `docs/plans/2026-06-19-003-feat-agent-loop-pi-parity-plan.md`.

### T3. `pi-agent-core` Loop, State, and Tool Parity

- [x] Expand agent state to match pi's public state concepts without copying TypeScript declaration merging.
  - **Files:** `include/cch/agent/AgentContext.hpp`, `include/cch/agent/AgentLoop.hpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`.
  - **Test scenarios:** active tools, messages, streaming message, pending tool calls, error message, model, and thinking level are observable through a C++-appropriate state seam.
- [x] Add pre/post tool-call hooks and graceful blocking semantics.
  - **Files:** `include/cch/agent/AgentTool.hpp`, `include/cch/agent/AgentContext.hpp`, `src/agent/AgentLoop.cpp`, `src/tools/AsyncToolFactories.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`, `tests/tools/AsyncToolsTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`, `pi:packages/agent/src/agent-loop.ts`.
  - **Test scenarios:** before-hook blocks a tool with an error tool result; after-hook overrides content/details/isError/terminate; thrown hook errors become stable agent errors.
  - **Implementation plan:** `docs/plans/2026-06-19-002-feat-agent-tool-hooks-plan.md`.
- [x] Add context transform, conversion-to-LLM, steering messages, follow-up messages, and prepare-next-turn seams.
  - **Files:** `include/cch/agent/AgentLoop.hpp`, `include/cch/agent/AgentContext.hpp`, `src/agent/AgentLoop.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`.
  - **Done when:** continuation and multi-turn control can be implemented without special cases in CLI runtime.
  - **Implementation plan:** `docs/plans/2026-06-19-003-feat-agent-loop-pi-parity-plan.md`.
- [x] Evaluate sequential versus parallel tool execution with deterministic result insertion.
  - **Files:** `include/cch/agent/AgentTool.hpp`, `src/agent/AgentLoop.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`.
  - **Test scenarios:** tools may complete out of order while tool-result messages are appended in assistant source order; tool-level sequential overrides and the run default keep mutating/stateful tools serialized.
  - **Implementation plan:** `docs/plans/2026-06-19-003-feat-agent-loop-pi-parity-plan.md`.
- [x] Preserve move-only event sink semantics while adding missing lifecycle events.
  - **Files:** `include/cch/agent/AgentEvent.hpp`, `tests/architecture/MoveOnlyCallbackTest.cpp`, `tests/agent/AsyncAgentLoopTest.cpp`.
  - **References:** `pi:packages/agent/src/types.ts`, `pi:packages/coding-agent/docs/extensions.md`.
  - **Done when:** event order can support future UI/extensions without requiring copyable callbacks.

### T4. Harness Execution Environment and Session Tree Parity

- [x] Expand execution environment from current workspace/file/shell methods toward pi's `FileSystem` and `Shell` capability contracts.
  - **Files:** `include/cch/harness/ExecutionEnv.hpp`, `include/cch/harness/LocalExecutionEnv.hpp`, `src/harness/`, `src/util/Process.hpp`, `tests/harness/AsyncLocalExecutionEnvTest.cpp`, `tests/harness/WorkspaceFileSystemTest.cpp`.
  - **References:** `pi:packages/agent/src/harness/types.ts`.
  - **Test scenarios:** absolute path, join path, file info, list dir, canonical path, exists, create/remove directory, temp file/dir, read/write binary, streaming stdout/stderr, stable error codes.
  - **Implementation plan:** `docs/plans/2026-06-19-004-refactor-execution-env-capability-parity-plan.md`.
  - **Status:** FileSystem and Shell capability contracts added with default not_supported impls; local env implements full pi-shaped FS ops (WorkspaceFileSystem) and split-stream shell exec; 172 tests pass.
- [x] Keep workspace containment and secret redaction stronger than the pi default where the C++ harness already has it.
  - **Files:** `src/harness/WorkspaceFileSystem.hpp`, `src/util/Process.hpp`, `tests/harness/AsyncLocalExecutionEnvTest.cpp`, `tests/harness/WorkspaceFileSystemTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/security.md`, `pi:packages/coding-agent/docs/containerization.md`.
  - **Done when:** parity work does not weaken symlink escape checks, bash opt-in, private session permissions, or redaction boundaries.
  - **Status:** PathGuard moved under harness ownership (WorkspaceFileSystem); symlink escape, O_NOFOLLOW, no-follow metadata, workspace-contained temps, secret env filtering with expanded heuristics all preserved; bash remains opt-in.
- [x] (Remaining T4 session tree work deferred to follow-up plan after this capability seam stabilizes.)
- [x] Migrate JSONL sessions from the current linear message format toward pi's tree-entry format.
  - **Files:** `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `tests/harness/session/JsonlSessionStoreTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/session-format.md`, `pi:packages/agent/src/harness/types.ts`.
  - **Implementation plan:** `docs/plans/2026-06-19-005-feat-session-tree-write-support-plan.md`.
  - **Status:** v3 session header (`"type":"session"`, version 3), 8-char random hex entry IDs, and write support for all 9 non-message tree entry types (model_change, thinking_level_change, active_tools_change, custom, custom_message, label, compaction, branch_summary, session_info) implemented. Resume gate removed. 20 session tests pass (7 legacy + 13 new round-trip tests).
- [x] Add model change, thinking-level change, active-tools change, custom, custom-message, label, compaction, and branch-summary entry support incrementally.
  - **Files:** `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `tests/harness/session/JsonlSessionStoreTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/session-format.md`.
  - **Status:** All 9 entry types have per-type append methods with round-trip tests.
- [x] Add branch navigation and context reconstruction.
  - **Files:** `include/cch/harness/session/SessionTree.hpp`, `src/harness/session/SessionTree.cpp`, `tests/harness/session/SessionTreeTest.cpp`, `CMakeLists.txt`.
  - **References:** `pi:packages/coding-agent/docs/sessions.md`, `pi:packages/coding-agent/docs/session-format.md`, `pi:packages/coding-agent/docs/compaction.md`.
  - **Implementation plan:** `docs/plans/2026-06-20-010-feat-session-tree-navigation-plan.md`.
  - **Status:** SessionTree with in-memory index, leaf tracking, getBranch/getChildren/root, buildSessionContext() with compaction-aware context reconstruction, branchWithSummary() hook seam, open_as_tree()/append_leaf() in JsonlSessionStore. 26 new tests pass (381 total).
  - **References:** `pi:packages/coding-agent/docs/sessions.md`, `pi:packages/coding-agent/docs/compaction.md`.
  - **Done when:** branch/fork/leaf behavior has deterministic tests and does not break simple `--resume`.

### T5. Built-In Tools and Coding-Agent Runtime Parity

- [x] Reconcile built-in tool schemas and behavior with pi's read, write, edit, and bash tools.
  - **Files:** `include/cch/tools/ToolFactories.hpp`, `src/tools/AsyncToolFactories.cpp`, `tests/tools/AsyncToolsTest.cpp`.
  - **References:** `pi:packages/coding-agent/src/core/bash-executor.ts`, `pi:packages/coding-agent/src/core/output-guard.ts`, `pi:packages/coding-agent/src/core/exec.ts`.
  - **Test scenarios:** schema shape, path validation, truncation, edit ambiguity, timeout, exit code, stderr/stdout representation, and secret filtering.
  - **Implementation plan:** `docs/plans/2026-06-20-001-feat-t5-tool-config-parity-plan.md`.
  - **Status:** edit tool uses `edits[]` array with multi-edit semantics; read appends continuation hints on truncation; bash accepts timeout in seconds and strips ANSI; write creates parent directories implicitly. All tests pass.
- [x] Add pi-style extended runtime messages only after session entry support exists.
  - **Files:** `include/cch/ai/Message.hpp`, `include/cch/ai/glaze/AiJson.hpp`, `src/harness/session/JsonlSessionStore.cpp`, `tests/ai/MessageContractTest.cpp`, `tests/harness/session/JsonlSessionStoreTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/session-format.md`, `pi:packages/coding-agent/src/core/messages.ts`.
  - **Implementation plan:** `docs/plans/2026-06-20-004-feat-extended-runtime-messages-plan.md`.
  - **Status:** `BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage` added as passive aggregates with DTO round-trip, LLM conversion helpers, and redaction visitor cases. 15 new tests pass.
- [x] Split CLI/runtime wiring into pi-coding-agent-like responsibilities.
  - **Files:** `src/AsyncCliRuntime.hpp`, `src/AsyncCliRuntime.cpp`, `src/main.cpp`, `src/coding_agent/runtime/`, `tests/cli/CliSmokeTest.cpp`.
  - **References:** `pi:packages/coding-agent/src/core/agent-session-runtime.ts`, `pi:packages/coding-agent/src/core/agent-session-services.ts`, `pi:packages/coding-agent/src/core/agent-session.ts`, `pi:packages/coding-agent/src/cli/args.ts`.
  - **Done when:** argument parsing, model resolution, tool registration, session lifecycle, event printing, and agent execution are separate seams.
- [x] Add configuration and model resolution before expanding provider count.
  - **Files:** `include/cch/coding_agent/Config.hpp`, `src/coding_agent/ConfigLoader.cpp`, `tests/coding_agent/ConfigLoaderTest.cpp`.
  - **References:** `pi:packages/coding-agent/src/config.ts`, `pi:packages/coding-agent/src/core/settings-manager.ts`, `pi:packages/coding-agent/src/core/model-registry.ts`, `pi:packages/coding-agent/src/core/model-resolver.ts`, `pi:packages/coding-agent/docs/settings.md`, `pi:packages/coding-agent/docs/models.md`.
  - **Implementation plan:** `docs/plans/2026-06-20-001-feat-t5-tool-config-parity-plan.md`.
  - **Status:** `~/.cpp-harness/config.json` loaded with provider/model/base-url/api-key-env defaults; env var chains resolved; CLI overrides take precedence; session resume preserves stored provider/model. All tests pass.
- [x] Add slash-command and prompt-processing seams after runtime/session boundaries are stable.
  - **Files:** `include/cch/coding_agent/PromptProcessing.hpp`, `src/coding_agent/PromptProcessing.cpp`, `src/coding_agent/PromptExpander.cpp`, `src/coding_agent/runtime/AgentSessionRunner.cpp`, `src/AsyncCliRuntime.cpp`, `tests/coding_agent/BuiltinCommandsTest.cpp`, `tests/coding_agent/PromptExpanderTest.cpp`.
  - **References:** `pi:packages/coding-agent/src/core/slash-commands.ts`, `pi:packages/coding-agent/src/core/prompt-templates.ts`, `pi:packages/coding-agent/docs/usage.md`, `pi:packages/coding-agent/docs/prompt-templates.md`.
  - **Implementation plan:** `docs/plans/2026-06-20-005-feat-slash-command-prompt-processing-plan.md`.
  - **Status:** `CommandRegistry` dispatch framework, `process_prompt()` pipeline in REPL/runner/RPC, and `expand_prompt_template()` with bash-style arg substitution are implemented. 22 new tests pass.
- [ ] Implement pi-aligned built-in slash commands.
  - **References:** `pi:packages/coding-agent/src/core/slash-commands.ts` (25 built-in commands).
  - **Current gap:** Only 4 CLI-shaped placeholder commands exist (`/session`, `/quit`, `/new`, `/resume`), and `/new`/`/resume` only print restart-instruction text rather than performing session operations. Missing pi-aligned commands include: `/help`, `/clear`, `/compact`, `/exit`, `/model`, `/cost`, `/stats`, `/theme`, `/doctor`, `/ide`, `/output-style`, and others. README incorrectly claims `/help`, `/clear`, `/compact`, `/exit` are available.
  - **Done when:** each implemented command matches pi's behavior and has test coverage; README accurately reflects the implemented set.

### T6. Resources, Skills, Extensions, and Packages

- [x] Design a C++ extension boundary before implementing extensions.
  - **References:** `pi:packages/coding-agent/docs/extensions.md`, `pi:packages/coding-agent/examples/extensions/`.
  - **Implementation plan:** `docs/plans/2026-06-20-011-design-extension-boundary-plan.md`.
  - **Decision:** Option E — formalize existing hook system as SessionExtension callback boundary. pi-style TypeScript extensions intentionally unsupported. Options A (dlopen), C (JS bridge) excluded. Option B (RPC) as viable mid-term upgrade path.
  - **Done when:** the decision preserves security posture and does not leak dynamic extension concerns into core AI/agent contracts.
- [x] Implement skill loading plus model-visible/user-invocable integration.
  - **References:** `pi:packages/coding-agent/docs/skills.md`, `pi:packages/agent/src/harness/skills.ts`.
  - **Test scenarios:** project skill discovery, reusable global-style `includeRootFiles` loader behavior, relative reference resolution, disabled model invocation, prompt formatting, and duplicate handling.
  - **Implementation plans:** `docs/plans/2026-06-20-006-feat-skill-file-discovery-plan.md`, `docs/plans/2026-06-20-007-feat-skill-model-visible-integration-plan.md`.
  - **Status:** `Skill`/diagnostic contracts, frontmatter parsing, recursive/deduplicating loader, trust-gated runtime startup loading, `<available_skills>` context injection, and `/skill:name [args]` expansion are implemented. Global `~/.cpp-harness/skills`, config-driven skill directories, and live skill reload are deferred.
- [x] Implement prompt template loading and invocation.
  - **Files:** `include/cch/coding_agent/PromptTemplateLoader.hpp`, `src/coding_agent/PromptTemplateLoader.cpp`, `tests/coding_agent/PromptTemplateLoaderTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/prompt-templates.md`, `pi:packages/agent/src/harness/prompt-templates.ts`.
  - **Implementation plan:** `docs/plans/2026-06-20-009-feat-prompt-template-loading-plan.md`.
  - **Status:** PromptTemplateLoader with YAML frontmatter parsing and directory scanning. expand_prompt_template() with $1/$@/$ARGUMENTS/${N:-default}/${@:N}/${@:N:L} substitution. Runtime wiring in RuntimeServices. 26 tests pass.
- [ ] Implement package discovery/installation only after resource loading exists.
  - **References:** `pi:packages/coding-agent/docs/packages.md`.
  - **Scope boundary:** do not add npm/git package installation silently; it is a supply-chain surface and needs an explicit design/test plan.
- [x] Add project trust and resource enable/disable controls before loading project-local executable resources.
  - **References:** `pi:packages/coding-agent/src/cli/project-trust.ts`, `pi:packages/coding-agent/src/core/trust-manager.ts`, `pi:packages/coding-agent/docs/security.md`.
  - **Implementation plan:** `docs/plans/2026-06-20-008-feat-project-trust-resource-controls-plan.md`.
  - **Status:** Passive trust/resource contracts, marker detection, trust store/resolution, user config defaults, `--approve`/`--no-approve`/`--no-skills`, resource load plans, trust-gated project skill loading, and stderr diagnostics are implemented. Interactive trust prompts, project settings parsing, global resources, extension execution, and package installation remain deferred.

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

- [x] Add JSON event stream mode as the first machine-readable surface.
  - **Files:** `src/main.cpp`, `src/AsyncCliRuntime.*`, `src/coding_agent/runtime/JsonEventPrinter.*`, `tests/coding_agent/runtime/JsonEventPrinterTest.cpp`, `tests/cli/CliSmokeTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/json.md`.
  - **Test scenarios:** stable event objects for model request, assistant deltas, tool call, tool result, errors, max turns, and completion.
  - **Implementation plan:** `docs/plans/2026-06-20-002-feat-json-event-stream-mode-plan.md`.
  - **Status:** `--mode json` emits JSONL stdout with a session header, pi-named C++ schema v1 lifecycle subset, correlated tool execution events, and final `runtime_terminal`; text mode remains default. RPC and SDK remain deferred.
- [x] Add RPC mode only after runtime services are separated from CLI printing.
  - **Files:** `src/main.cpp`, `src/AsyncCliRuntime.*`, `src/coding_agent/runtime/AgentSessionRunner.*`, `src/coding_agent/runtime/RpcJsonl.*`, `src/coding_agent/runtime/RpcMode.*`, `tests/cli/CliSmokeTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/rpc.md`.
  - **Implementation plan:** `docs/plans/2026-06-20-003-feat-t8-jsonl-rpc-mode-plan.md`.
  - **Status:** `--mode rpc` reads strict JSONL commands on stdin and emits JSONL responses/events on stdout for a narrow v1 command set: `prompt`, `get_state`, `get_last_assistant_text`, and `shutdown`. Full pi RPC command parity, SDK, extension UI, compaction, resources, and session tree operations remain deferred.
  - **Done when:** stdin/stdout JSONL requests can drive sessions without TUI assumptions.
- [x] Add embeddable SDK surface after agent/session/resource seams are stable.
  - **Files:** `include/cch/coding_agent/Sdk.hpp`, `src/coding_agent/Sdk.cpp`, `tests/coding_agent/SdkSessionTest.cpp`.
  - **References:** `pi:packages/coding-agent/docs/sdk.md`, `pi:packages/coding-agent/src/core/sdk.ts`.
  - **Implementation plan:** `docs/plans/2026-06-21-001-feat-t8-embeddable-sdk-surface-plan.md`.
  - **Status:** SDK v1 is available as a same-process C++23 source-level API. Host apps can create/resume sessions, register tools/resources, send prompts, subscribe to events, and close cleanly without CLI/RPC globals. Full pi SDK parity (session replacement runtime, concurrent prompts, compaction, in-memory sessions, ABI stability) is deferred.
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
6. T5 coding-agent runtime split, config, and tool behavior parity (pi-aligned slash commands still pending).
7. T8 JSON/RPC surfaces before a large TUI investment.
8. Continue T6 with project trust/resource enablement, prompt-template loading, and an extension boundary; skill discovery/model-visible invocation is already in place, while package installation remains a later supply-chain plan.
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
