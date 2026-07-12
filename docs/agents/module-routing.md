# Detailed module routing reference

This file is the detailed Route reference for [`AGENTS.md`](../../AGENTS.md). The root `AGENTS.md` remains the entry point for every agent run. Read this file when you need the complete module routing matrix, when you update routing, or when an active/historical plan mentions provider, tool, session, CLI, runtime, public-boundary, documentation, or pi-parity routing concepts. The historical section map below keeps older plan references to numbered `AGENTS.md` sections resolvable.

## How to use this reference

1. Start in [`AGENTS.md`](../../AGENTS.md): run the Start Gate checks, apply the Guardrails, and classify the task.
2. If the target seam is already obvious, go directly to the files listed in the matching row below.
3. If the target seam is unclear, or if you are editing route documentation, use this table as the source of truth for task family, preferred entry points, routing notes, and validation slice.
4. Keep edits minimal. Updating this reference must not change C++ behavior, build configuration, CLI behavior, provider behavior, tool behavior, runtime behavior, or session behavior.

## Route matrix

| Task family | Preferred entry points | Routing notes | Validation slice |
| --- | --- | --- | --- |
| Agent loop / turn flow / tool-call orchestration | `include/cch/agent/`, `src/agent/AgentLoop.cpp`, `tests/agent/` | `AsyncAgentLoop` owns the provider-neutral loop. Lifecycle events flow through `AgentLifecycleEvent`. Configure loop behavior through `AsyncAgentOptions`. | `tests/agent/` tags; add tests for new lifecycle events or tool-execution modes. |
| AI messages, content, tool schema, and usage contracts | `include/cch/ai/`, `tests/ai/` | Keep public message and content types as passive value contracts. Preserve aggregate-friendly domain structures. | `[ai]` tests; architecture tests if public types change. |
| OpenAI-compatible provider / SSE / HTTP transport | `include/cch/ai/providers/`, `src/ai/providers/`, `src/ai/glaze/`, `tests/ai/providers/` | Keep provider wire DTOs and Glaze mappings in implementation/serialization layers. Preserve the `StreamingChatClient` / `StreamTransport` seams. | `[ai][provider]` tests; do not run live provider validation by default. |
| Provider/model registry | `include/cch/ai/ProviderRegistry.hpp`, `src/ai/ProviderRegistry.cpp`, `src/ai/providers/FakeChatClient.*`, `include/cch/ai/providers/OpenAICompletionsCompat.hpp` | Register fake and OpenAI-compatible providers here. CLI/runtime resolves provider and model settings instead of hard-coding clients. | Provider registry tests; fake-provider smoke tests. |
| Config / model resolution | `include/cch/coding_agent/Config.hpp`, `src/coding_agent/ConfigLoader.cpp`, `src/coding_agent/ProviderConfigResolution.cpp`, `tests/coding_agent/ConfigLoaderTest.cpp`, `tests/coding_agent/ProviderConfigResolutionTest.cpp` | `~/.cpp-harness/config.json` loads provider/model/base-url/api-key-env, `default_project_trust`, and project resource defaults. `resolve_provider_settings()` centralizes CLI/config/resume precedence. | Config loader and provider resolution tests. |
| JSON serialization / deserialization | `src/ai/glaze/`, `src/util/Json.hpp`, `tests/ai/GlazeRoundTripTest.cpp` | Domain-facing APIs use `include/cch/util/JsonValue.hpp`. Keep Glaze DTOs and JSON adapters in `src/` implementation layers, converting only at physical boundaries. | `[ai]` round-trip tests; architecture tests if public types change. |
| Built-in tools: read/write/edit/bash | `include/cch/tools/ToolFactories.hpp`, `src/tools/AsyncToolFactories.cpp`, `tests/tools/` | Tools are exposed through `AsyncAgentTool`. File and process capabilities must go through the execution environment. When adding a tool, define the `ai::Tool` schema, implement `AsyncAgentTool`, register through `AsyncToolRegistry`, and add focused tests. | `[tools][async]` tests. |
| Slash commands / prompt processing | `include/cch/coding_agent/PromptProcessing.hpp`, `src/coding_agent/PromptProcessing.cpp`, `src/coding_agent/PromptExpander.cpp`, `tests/coding_agent/BuiltinCommandsTest.cpp`, `tests/coding_agent/PromptExpanderTest.cpp`, `tests/coding_agent/SkillIntegrationTest.cpp` | `CommandRegistry` owns session-lifecycle built-ins. `expand_prompt_template()` supports bash-style argument replacement. `process_prompt()` connects REPL/runner/RPC flow and expands `/skill:<name>` before command dispatch. | `[coding_agent]` tests for command/expander/skill integration. |
| Project trust / resource controls | `include/cch/coding_agent/ProjectTrust.hpp`, `include/cch/coding_agent/ProjectResources.hpp`, `src/coding_agent/ProjectTrust.cpp`, `src/coding_agent/ProjectResources.cpp`, `src/coding_agent/ProjectResourceLoader.*`, `tests/coding_agent/Project*Test.cpp`, `tests/cli/CliSmokeTest.cpp` | `ProjectResourceLoader` is the shared startup resource-loading seam. CLI, RPC, and SDK session creation consume loaded resources from this seam instead of scanning project directories at call sites. | Project trust/resource tests; CLI smoke test if CLI startup behavior changes. |
| Skills / resource loading | `include/cch/coding_agent/Skill.hpp`, `src/coding_agent/ProjectResourceLoader.*`, `src/coding_agent/SkillLoader.*`, `src/coding_agent/PromptTemplateLoader.*`, `src/coding_agent/SkillFormatting.*`, `src/coding_agent/runtime/AgentSessionRuntime.*`, `src/coding_agent/runtime/SessionFactory.*`, `tests/coding_agent/Skill*Test.cpp`, `tests/coding_agent/PromptTemplateLoaderTest.cpp`, `tests/coding_agent/ProjectResourceLoaderTest.cpp` | Project-local `.cpp-harness/skills/**/SKILL.md` parsing is delegated to `SkillLoader`; project-local `.cpp-harness/prompts/*.md` parsing is delegated to `PromptTemplateLoader`. Startup policy, trust gating, host/project duplicate precedence, and loader diagnostic normalization belong in `ProjectResourceLoader`. | Skill/template/loader tests. |
| Workspace, path guard, and shell execution | `include/cch/harness/ExecutionEnv.hpp`, `include/cch/harness/LocalExecutionEnv.hpp`, `src/harness/` (including private `SyncLocalExecutionEnv.*`), `src/tools/PathGuard.hpp`, `src/util/Process.*`, `tests/harness/` | This seam owns path containment and safety checks. Async shell I/O must go through `ProcessRunner`. Keep private execution-env implementation details out of public include surfaces. | `[harness]` tests; add tests for any new containment or redaction behavior. |
| Session JSONL / resume / tree navigation | `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `src/harness/session/SessionTree.cpp`, `src/harness/session/SessionResume.cpp`, `tests/harness/session/`, `tests/coding_agent/runtime/SessionLifecycleTest.cpp` | Session storage is an append-only, redacted typed transcript. Resume uses `SessionResume` to return agent-ready active-path history; `SessionTree` owns tree indexing, branch navigation, and compaction-aware context reconstruction. When adding session fields, preserve append-only JSONL and tolerate unknown entries. | `[harness][session]` tests; session lifecycle tests. |
| CLI / REPL / runtime wiring | `src/main.cpp`, `src/cli/` (`CliParse`, `CliPreflight`, `CliConfig`, `CliRuntimeConfig`), `src/coding_agent/runtime/AsyncCliRuntime.*`, `src/coding_agent/runtime/SessionFactory.*`, `src/coding_agent/runtime/RuntimeServices.*`, `tests/cli/` | CLI11 argument parsing and preflight live in `src/cli/`. CLI session creation routes startup resources through `SessionFactory` and `ProjectResourceLoader`; `RuntimeServices` receives already-loaded skills/templates and must not scan project directories or print resource diagnostics directly. | `[cli]` tests; README build/run examples for manual smoke checks. |
| Embeddable SDK / host resources | `include/cch/coding_agent/Sdk.hpp`, `src/coding_agent/Sdk.cpp`, `src/coding_agent/runtime/SessionFactory.*`, `src/coding_agent/ProjectResourceLoader.*`, `tests/coding_agent/SdkSessionTest.cpp` | Public SDK session creation routes opt-in project resources through `ProjectResourceLoader`. Host-provided skills and prompt templates take precedence over project-discovered duplicates, and resource diagnostics are returned as `CreateAgentSessionResult::diagnostics` values rather than printed. | `SdkSessionTest.cpp` and related SDK tests. |
| JSON / RPC output modes | `src/coding_agent/runtime/JsonEventPrinter.*`, `src/coding_agent/runtime/RpcMode.*`, `src/coding_agent/runtime/RpcJsonl.*`, `tests/coding_agent/runtime/JsonEventPrinterTest.cpp`, `tests/cli/CliSmokeTest.cpp` | `--mode json` writes JSONL to stdout with the session header, pi-named lifecycle events, and `runtime_terminal`. `--mode rpc` drives the session over stdin/stdout JSONL commands: `prompt`, `get_state`, `get_last_assistant_text`, and `shutdown`. | JSON/RPC printer tests; CLI smoke test if output mode behavior changes. |
| Public boundary / architecture guards | `tests/architecture/`, `CMakeLists.txt`, `include/cch/` | `include/cch/` is reserved for domain contracts and abstract seams. Implementation details belong in `src/`, with CMake using `PRIVATE` includes. If you change public headers, dependencies, include surfaces, provider/tool/session contracts, or CMake public/private boundaries, run the architecture tests. | `./build/cpp_harness_tests "[architecture]"` |
| Documentation and plans | `README.md`, `docs/plans/` | Update docs or plans when behavior, CLI, session format, or architecture boundaries change. For docs-only route maintenance, validate markdown links, headings, clear English optimized for agent execution, and no-information-loss instead of running unrelated C++ builds. | Markdown link/heading check; no C++ build unless code/build files changed. |

## Cross-reference anchors

- Provider, model, and OpenAI-compatible routing map to the provider rows above.
- Tool routing maps to the built-in tools row and, when shell/file capabilities are involved, the workspace/path/shell execution row.
- Session and resume routing map to the session JSONL / tree navigation row.
- CLI, REPL, JSON mode, RPC mode, and runtime routing map to the CLI/runtime and JSON/RPC rows.
- Public-boundary and architecture-sensitive routing map to the public boundary / architecture guards row.
- Documentation and pi-parity routing map to the documentation/plans row plus the pi C++ parity guidance in [`AGENTS.md`](../../AGENTS.md) and the active plans in `docs/plans/`.

## Historical AGENTS.md section map

Some active or historical plans refer to the pre-router numbered sections in `AGENTS.md`. Resolve those references to the current concepts instead of reintroducing section numbers:

| Historical reference | Current place |
| --- | --- |
| `AGENTS.md` §0 / `先读顺序` | `AGENTS.md` → Start Gate |
| `AGENTS.md` §1 / project positioning | `README.md` for behavior and safety context; `AGENTS.md` → Guardrails for always-on rules |
| `AGENTS.md` §2 or §2.3 / architecture rules | `AGENTS.md` → Guardrails, especially the move-only event-sink rule |
| `AGENTS.md` §2.5 / pi C++ parity route | `AGENTS.md` → pi C++ Parity Direction plus the active plans under `docs/plans/` |
| `AGENTS.md` §3 / routing table or route row style | `AGENTS.md` → Route for compact dispatch; this file's Route matrix for detailed rows |
| `AGENTS.md` §4 / build and validation commands | `README.md` for build/test commands; `AGENTS.md` → Verify Slice for validation choice |
| `AGENTS.md` §5 / change rules | `AGENTS.md` → Guardrails and Verify Slice; this file's relevant route rows for tool, provider, session, workspace, and documentation-specific rules |
| `AGENTS.md` §5.5 / branch, merge, and PR conventions | `AGENTS.md` → Worktree Discipline plus [`docs/agents/worktree-discipline.md`](worktree-discipline.md) |
| `AGENTS.md` §6 / pre-handoff checklist | `AGENTS.md` → Verify Slice and Handoff |

## Maintenance rules

- Treat this file as the stable home for detailed module routing. Root `AGENTS.md` may summarize routes, but this file must preserve full task-family coverage.
- When adding, removing, or renaming a module seam, update the matching task family, preferred entry points, routing notes, and validation slice here first.
- Preserve the current architecture guardrails: public contracts stay passive, capabilities stay behind seams, lifecycle events remain weakly coupled, and generic/serialization machinery stays localized.
- Route-documentation changes are documentation-only unless the user explicitly asks for code changes.
