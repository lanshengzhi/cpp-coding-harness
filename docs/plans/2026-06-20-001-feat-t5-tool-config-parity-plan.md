---
title: "feat: Align built-in tools with pi contracts and add config/model resolution"
type: feat
status: "completed"
date: "2026-06-20"
---

# feat: Align built-in tools with pi contracts and add config/model resolution

## Summary

Align the read, write, edit, and bash tool schemas and behavior with pi's tool contracts, and build a minimal configuration/model-resolution subsystem that resolves provider, model, base URL, and API key from config files, environment variables, and CLI flags. Deferred: extended runtime messages, slash-commands, and prompt-processing.

---

## Problem Frame

The C++ harness built-in tools (read, write, edit, bash) were built as an MVP seed and diverge from pi's tool contracts in both schema shape and runtime behavior. The edit tool only supports single replacements, read lacks truncation continuation hints, bash uses millisecond timeouts without ANSI stripping, and write exposes an unnecessary `create_parents` flag. Separately, the runtime has no config subsystem — provider/model/base-url/api-key values are hardcoded or passed only through CLI flags, with no config file loading, env var resolution chains, or model defaults. This plan addresses both gaps as the next phase (T5 items 1+3) of the pi parity roadmap.

---

## Requirements

- **R1.** Edit tool accepts `edits[]` array parameter with one or more `{oldText, newText}` pairs, matched against the original file (not incrementally), with backward-compatible single-arg `old_text`/`new_text` fallback.
- **R2.** Read tool appends truncation continuation hints when output exceeds limits, citing the next offset the model should use.
- **R3.** Bash tool accepts `timeout` in seconds (optional), strips ANSI escape sequences from output, and writes truncated output to a workspace temp file with an explicit path in the result.
- **R4.** Write tool implicitly creates parent directories; `create_parents` is removed from the tool schema.
- **R5.** A config file at `~/.cpp-harness/config.json` provides provider, model, base URL, and API key env var defaults that CLI flags can override.
- **R6.** On session resume, the provider and model stored in the session JSONL take precedence over current config/CLI values.
- **R7.** API key resolution supports env var fallback chains (e.g., try `OPENAI_API_KEY`, fall back to `KIMI_API_KEY`) resolved in the config layer before reaching the provider factory.
- **R8.** `enable_bash` remains CLI-only — it cannot be set via config file.
- **R9.** All architecture tests continue to pass; new public headers are added to `PublicHeaderBoundaryTest`; new CMake targets are added to `CMakeDependencyTest`.

---

## Scope Boundaries

- Extended runtime messages (bash execution messages, custom messages, compaction summary, branch summary) — deferred to T5 item 2 follow-up
- Slash-commands and prompt-processing seams — deferred to T5 item 4 follow-up
- grep, ls, find tools — not in scope; pi has these but they're additional tools beyond the core four
- Image detection and resize in read tool — deferred; this plan adds the truncation infrastructure that image support would slot into
- OAuth and subscription-provider flows — explicitly deferred per roadmap T2
- T4 branch navigation and context reconstruction — explicitly deferred per roadmap
- Config file hot-reload, multi-file settings/models/auth split, project-local `.cpp-harness.json` — deferred; this plan establishes the single-file baseline

---

## Context & Research

### Relevant Code and Patterns

- **Tool factories:** `src/tools/AsyncToolFactories.cpp` — all four tools live in one file as anonymous-namespace final classes inheriting `AsyncToolBase`. The factory pattern: `make_async_*_tool(shared_ptr<AsyncExecutionEnv>)` → `unique_ptr<AsyncAgentTool>`.
- **Tool interface:** `include/cch/agent/AgentTool.hpp` — `AsyncAgentTool` with `definition()` (returns `const ai::Tool&`), `execute(ToolInvocation)` (coroutine), and `execution_mode()`.
- **Tool schema:** `include/cch/ai/Tool.hpp` — `ai::Tool{name, description, JsonSchema}`, `ai::JsonSchema::object/string/integer/boolean`.
- **Execution environment:** `include/cch/harness/ExecutionEnv.hpp` — `AsyncExecutionEnv` with `read_file`, `write_file`, `edit_file`, `run_shell` plus pi-shaped FS/shell methods.
- **Result types:** `AsyncFileReadResult {content, truncated}`, `AsyncFileWriteResult {bytes_written}`, `AsyncFileEditResult {old_preview, new_preview}`, `AsyncShellResult {exit_code, output, timed_out}`.
- **Runtime services:** `src/coding_agent/runtime/RuntimeServices.cpp` — `make_runtime_services(RuntimeServicesConfig)` assembles provider registry, tools, and execution env.
- **CLI runtime:** `src/AsyncCliRuntime.cpp` — `run_async_cli(AsyncCliRuntimeConfig)` constructs `RuntimeServicesConfig` and drives the session.
- **CLI parsing:** `src/main.cpp` — CLI11-based argument parsing producing `CliConfig`, mapped to `AsyncCliRuntimeConfig`.
- **Provider factory:** `include/cch/ai/ProviderRegistry.hpp` — `ProviderFactoryContext {model, base_url, api_key, api_key_env, timeout, compat_flags}`.
- **Package boundaries:** `CMakeLists.txt` — dependency direction: `coding_agent_runtime → {agent, harness, tools}`, `tools → {agent, harness}`.
- **Architecture tests:** `tests/architecture/PublicHeaderBoundaryTest.cpp`, `CMakeDependencyTest.cpp`, `ArchitectureSurfaceScanTest.cpp`.

### External References

- **pi edit tool:** `pi:packages/coding-agent/src/core/tools/edit.ts` — multi-edit `edits[]` array, `prepareEditArguments` backward compat, `applyEditsToNormalizedContent`, `computeEditsDiff`, BOM/line-ending handling.
- **pi read tool:** `pi:packages/coding-agent/src/core/tools/read.ts` — offset/limit with `truncateHead`, continuation hints, image detection.
- **pi bash tool:** `pi:packages/coding-agent/src/core/tools/bash.ts` — `timeout` in seconds (optional), `OutputAccumulator`, `fullOutputPath`, cancellation.
- **pi write tool:** `pi:packages/coding-agent/src/core/tools/write.ts` — `path` + `content` only, implicit parent creation.
- **pi truncation:** `pi:packages/coding-agent/src/core/tools/truncate.ts` — `DEFAULT_MAX_LINES = 2000`, `DEFAULT_MAX_BYTES = 50 * 1024`.
- **pi config:** `pi:packages/coding-agent/src/config.ts`, `model-registry.ts`, `model-resolver.ts`, `settings-manager.ts`.
- **pi bash executor:** `pi:packages/coding-agent/src/core/bash-executor.ts` — `sanitizeBinaryOutput`, `stripAnsi`, temp file pattern.

---

## Key Technical Decisions

- **Edit multi-edit orchestrated in tool factory, not ExecutionEnv.** The tool factory reads the file once via `ExecutionEnv::read_file`, validates all `edits[]` matches against the original content in C++ tool code, applies replacements, and writes back once. ExecutionEnv's single-arg `edit_file` interface stays stable. Rationale: smaller blast radius, no virtual interface change, and the matching semantics (all edits against original, non-overlapping) are tool-logic concerns, not file-I/O concerns.

- **Bash timeout stays in milliseconds at the ExecutionEnv layer.** The tool factory converts the schema's `timeout` (seconds) to `std::chrono::milliseconds` for `ExecutionEnv::run_shell`. Rationale: `ExecutionEnv::run_shell` and `ProcessRunner` already use `std::chrono::milliseconds` consistently; changing them would cascade through `LocalExecutionEnv`, `AsyncLocalExecutionEnv`, and `ProcessRunner` for no benefit.

- **Config loading happens inside `run_async_cli()` in `AsyncCliRuntime.cpp`.** This is the natural merge point: it already receives `AsyncCliRuntimeConfig` (CLI values) and constructs `RuntimeServicesConfig`. A `ConfigLoader` loads `~/.cpp-harness/config.json` and fills defaults for any `RuntimeServicesConfig` fields not set by CLI. Rationale: keeps `main.cpp` CLI-only, respects the existing RuntimeServices seam.

- **`enable_bash` remains CLI-only.** Config files can set model, provider, base URL, and API key env vars, but not `enable_bash`. Rationale: silently enabling shell execution via config file is a security regression. The `--enable-bash` flag is the explicit opt-in gate.

- **Session resume preserves stored provider/model.** When `--resume` is active, the provider and model from the session JSONL metadata take precedence over config file and CLI values. Rationale: matches pi behavior; changing provider/model on resume would break the conversation context.

- **API key env var chains resolved in config layer.** `ConfigLoader` resolves env var chains (e.g., `["OPENAI_API_KEY", "KIMI_API_KEY"]`) and sets `ProviderFactoryContext::api_key` to the first found value. `ProviderFactoryContext::api_key_env` becomes a fallback-only field. Rationale: keeps the provider factory simple (it gets a resolved key or falls back to its own env lookup), and chain logic is a config concern, not a provider concern.

- **`AsyncFileReadResult` gains `lines_read` and `bytes_read` fields.** The tool factory needs line counts to compute truncation continuation offsets without re-parsing the returned content. These fields are populated from local counters already tracked in `LocalExecutionEnv::read_file()` (the existing `lines` and `bytes` locals, currently discarded before returning).

---

## Open Questions

### Deferred to Implementation

- Exact JSON schema for `~/.cpp-harness/config.json` — resolved during U5 implementation when the actual config keys are finalized from pi reference
- ANSI stripping regex or library choice — implementation detail; can use a simple regex or hand-rolled state machine
- Bash temp file naming convention and cleanup policy — determined during U3 implementation; default: workspace root, `bash-output-{timestamp}-{hash}.txt`, cleaned on session end
- Diff computation algorithm for edit tool — simple line-based unified diff is sufficient; richer diff (word-level, patience) deferred

---

## Implementation Units

### U1. Edit tool — `edits[]` array API with multi-edit semantics

**Goal:** Replace the single `old_text`/`new_text` schema with pi's `edits[]` array API, supporting multiple non-overlapping replacements matched against the original file.

**Requirements:** R1, R9

**Dependencies:** None

**Files:**
- Modify: `src/tools/AsyncToolFactories.cpp` (AsyncEditFileTool class)
- Modify: `tests/tools/AsyncToolsTest.cpp`

**Approach:**
- Change the edit tool schema from `{path, old_text, new_text}` to `{path, edits: [{oldText, newText}]}` with `additionalProperties: false` on each edit entry.
- Add `prepareEditArguments` logic: accept both the new `edits[]` array and the legacy single `old_text`/`new_text` form, converting legacy to a single-element array.
- Tool factory orchestrates multi-edit: reads file once via `ExecutionEnv::read_file`, validates each `oldText` matches exactly once in the original content, rejects overlapping edits, applies all replacements, and writes back via `ExecutionEnv::write_file`.
- Generate a basic line-based unified diff as `AsyncFileEditResult::new_preview`, replacing the current 80-char truncation.
- Arg struct changes from `EditFileArgs{path, old_text, new_text}` to `EditFileArgs{path, edits: vector<{oldText, newText}>}`.

**Patterns to follow:**
- Existing `AsyncEditFileTool` structure: `parse_invocation_args`, `env()` check, result construction via `agent::AsyncToolExecutionResult`.
- `ExecutionEnv::read_file` and `write_file` for file I/O within the tool factory.

**Test scenarios:**
- **Happy path:** Single edit with `edits: [{oldText: "foo", newText: "bar"}]` replaces one occurrence.
- **Happy path:** Two non-overlapping edits replace both occurrences correctly.
- **Happy path:** Legacy single `old_text`/`new_text` input converted to `edits[]` and applied.
- **Happy path:** Diff preview returned in result details shows correct line changes.
- **Edge case:** `oldText` matches zero occurrences → error result.
- **Edge case:** `oldText` matches multiple occurrences → error result.
- **Edge case:** Two edits overlap (second edit's range overlaps first's match region) → error result.
- **Edge case:** Empty `edits[]` array → error result.
- **Error path:** File not found → error result.
- **Error path:** File not writable → error result.

**Verification:**
- All existing edit tool tests continue to pass (with schema adjustments).
- New multi-edit tests pass: two non-overlapping edits, overlapping rejection, zero-match rejection.
- Architecture tests pass; no new public headers needed for this change.

---

### U2. Read tool — truncation continuation hints

**Goal:** Append continuation hints to read output when truncation occurs, so the model knows which offset to use for the next read.

**Requirements:** R2, R9

**Dependencies:** None (can be implemented in parallel with U1)

**Files:**
- Modify: `include/cch/harness/ExecutionEnv.hpp` (AsyncFileReadResult struct)
- Modify: `src/harness/AsyncLocalExecutionEnv.cpp` (read_file implementation, populate new fields)
- Modify: `src/tools/AsyncToolFactories.cpp` (AsyncReadFileTool::execute)
- Modify: `tests/tools/AsyncToolsTest.cpp`
- Modify: `tests/harness/AsyncLocalExecutionEnvTest.cpp`

**Approach:**
- Add `lines_read` (int) and `bytes_read` (size_t) fields to `AsyncFileReadResult`.
- Populate them in `AsyncLocalExecutionEnv::read_file` from the existing `OutputLimiter` statistics.
- In the tool factory, when `result.truncated` is true, append a continuation hint: `[Showing lines {offset}-{offset+lines_read-1} of {total}. Use offset={offset+lines_read} to continue.]` (pi-style).
- When truncation is by byte limit rather than line limit, adapt the message accordingly.
- Offset validation: if offset is beyond file end, return a clear error (not just empty content).

**Patterns to follow:**
- pi's `read.ts` `truncateHead` and continuation hint format.
- Existing `AsyncReadFileTool` structure and error result pattern.

**Test scenarios:**
- **Happy path:** File within limits → no truncation flag, no continuation hint, `lines_read` matches total.
- **Happy path:** File exceeds line limit → truncated flag set, continuation hint with next offset.
- **Happy path:** Read with explicit offset+limit within bounds → correct line window, no truncation.
- **Edge case:** Offset beyond end of file → error result with clear message.
- **Edge case:** Limit=0 (unlimited) on large file → truncation at DEFAULT_MAX_LINES with hint.
- **Edge case:** File truncated by byte limit, not line limit → hint reflects byte-limit truncation.
- **Edge case:** Truncation at very end of file (last few lines within limit) → no misleading continuation hint.

**Verification:**
- Read tool returns continuation hints only when truncated.
- `AsyncFileReadResult::lines_read` and `bytes_read` populated correctly.
- Existing read tool tests pass.
- Architecture tests pass.

---

### U3. Bash tool — timeout unit alignment and output sanitization

**Goal:** Change the bash tool schema to accept `timeout` in seconds (optional), strip ANSI escape sequences, and write truncated output to a workspace temp file.

**Requirements:** R3, R9

**Dependencies:** None (can be implemented in parallel with U1/U2)

**Files:**
- Modify: `src/tools/AsyncToolFactories.cpp` (AsyncBashTool class)
- Modify: `include/cch/harness/ExecutionEnv.hpp` (AsyncShellResult — add `full_output_path` and `lines_read` fields)
- Modify: `src/harness/AsyncLocalExecutionEnv.cpp` (run_shell implementation)
- Modify: `tests/tools/AsyncToolsTest.cpp`
- Modify: `tests/harness/AsyncLocalExecutionEnvTest.cpp`

**Approach:**
- Change schema parameter: `timeout_ms` (required integer ms) → `timeout` (optional number, seconds). Default: no timeout.
- Tool factory converts seconds to `std::chrono::milliseconds` before calling `ExecutionEnv::run_shell`.
- Add ANSI stripping: apply a simple regex or state machine in the tool factory to strip CSI sequences (`\x1b[...m`, etc.) from the combined stdout/stderr before returning to the model.
- Add truncation support: when combined output exceeds 2000 lines or 50KB, write full output to a workspace temp file (`bash-output-{timestamp}-{hash}.txt`) with `0600` permissions, truncate the in-message output, and include the temp file path in the result.
- Add `full_output_path` (optional string) and `lines_read` (int) to `AsyncShellResult`.
- Remove the current `kDefaultBashTimeoutMs` / `kMaxBashTimeoutMs` constants; replace with `kDefaultMaxOutputLines = 2000`, `kDefaultMaxOutputBytes = 50 * 1024`.

**Patterns to follow:**
- pi's `bash-executor.ts` for ANSI stripping, truncation, and temp file pattern.
- Existing `AsyncBashTool` structure: arg parsing, timeout clamping, environment check, result formatting with `exit_code=` prefix.

**Test scenarios:**
- **Happy path:** Bash with no timeout → runs to completion, outputs exit code.
- **Happy path:** Bash with `timeout: 5` (seconds) → tool converts to 5000ms, ExecutionEnv receives ms.
- **Happy path:** ANSI-colored output → stripped in result, plain text returned to model.
- **Happy path:** Output within limits → no truncation, no temp file path.
- **Edge case:** Output exceeds 50KB → truncation with `[output truncated]` marker, temp file created with full output, path in result.
- **Edge case:** `timeout: 0` → no timeout (or error — decide during implementation).
- **Edge case:** Very short timeout → command times out, `timed_out: true`.
- **Error path:** Command exits non-zero → `is_error: true` with exit code.
- **Error path:** Bash not enabled → error result from environment check.

**Verification:**
- Bash schema uses `timeout` in seconds (optional).
- ANSI escape sequences stripped from output.
- Temp file created and referenced when output is truncated.
- Existing bash tool tests pass (with schema adjustments).
- Architecture tests pass.

---

### U4. Write tool — remove `create_parents` from schema

**Goal:** Make parent directory creation implicit, remove the `create_parents` parameter from the tool schema.

**Requirements:** R4, R9

**Dependencies:** None (can be implemented in parallel with U1/U2/U3)

**Files:**
- Modify: `src/tools/AsyncToolFactories.cpp` (AsyncWriteFileTool class)
- Modify: `include/cch/harness/ExecutionEnv.hpp` (write_file signature — change `bool create_parents` default or deprecate)
- Modify: `src/harness/LocalExecutionEnv.cpp` (write_file implementation)
- Modify: `tests/tools/AsyncToolsTest.cpp`

**Approach:**
- Remove `create_parents` from the tool schema definition (keep only `path` and `content`).
- In the tool factory, always pass `true` for `create_parents` when calling `ExecutionEnv::write_file`.
- Optionally: change `ExecutionEnv::write_file` to remove the `create_parents` parameter and always create parents (cleaner, but changes the virtual interface). Default: keep the parameter but the tool factory always passes `true`.

**Patterns to follow:**
- pi's write tool schema (only `path` + `content`).
- Existing `AsyncWriteFileTool` structure.

**Test scenarios:**
- **Happy path:** Write to new file in existing directory → succeeds.
- **Happy path:** Write to new file in non-existent directory → parent directories created, write succeeds.
- **Happy path:** Overwrite existing file → succeeds.
- **Edge case:** Schema no longer includes `create_parents` → model cannot pass it.

**Verification:**
- Write tool schema has only `path` and `content`.
- Parent directories created automatically.
- Existing write tool tests pass (with schema adjustments).
- Architecture tests pass.

---

### U5. Config loader — provider/model/base-url/api-key resolution

**Goal:** Build a minimal config subsystem that loads `~/.cpp-harness/config.json`, resolves API keys from env var chains, and merges with CLI overrides.

**Requirements:** R5, R7, R8, R9

**Dependencies:** None (U6 consumes it)

**Files:**
- Create: `include/cch/coding_agent/Config.hpp` (ConfigData struct, ConfigLoader class declaration)
- Create: `src/coding_agent/ConfigLoader.cpp` (implementation)
- Create: `tests/coding_agent/ConfigLoaderTest.cpp`
- Modify: `src/AsyncCliRuntime.cpp` (insert ConfigLoader between AsyncCliRuntimeConfig and RuntimeServicesConfig)
- Modify: `src/main.cpp` (pass config path flag if needed)
- Modify: `tests/architecture/PublicHeaderBoundaryTest.cpp` (add new header)

**Approach:**
- Define `ConfigData` struct: `provider` (optional string), `model` (optional string), `base_url` (optional string), `api_key_env` (optional vector<string> — env var chain).
- `ConfigLoader` reads `~/.cpp-harness/config.json` (or path from `--config` CLI flag), parses into `ConfigData`.
- Resolution priority: CLI flags > config file values > built-in defaults.
- API key resolution: for each env var in the chain, check `getenv()`; first found value becomes the resolved `api_key`. If none found, fall back to individual `api_key_env` resolution in the provider factory.
- `enable_bash` is explicitly excluded from `ConfigData` — it remains CLI-only.
- Insert `ConfigLoader` into `run_async_cli()`: load config → fill `RuntimeServicesConfig` defaults → CLI overrides take precedence.
- Config file not found is not an error — defaults + CLI are sufficient; warn only if file is found but malformed.

**Patterns to follow:**
- `util::read_json<T>` / `util::write_json` for JSON file I/O (consistent with existing tool arg parsing).
- Existing `RuntimeServicesConfig` as the consumption point.
- Existing `ProviderFactoryContext` as the provider-facing boundary (add `api_key` direct-set support).

**Test scenarios:**
- **Happy path:** Config file with provider and model → values flow to RuntimeServicesConfig.
- **Happy path:** CLI flag overrides config file value → CLI takes precedence.
- **Happy path:** No config file exists → defaults used, no error.
- **Happy path:** `api_key_env: ["CUSTOM_KEY", "OPENAI_API_KEY"]` → first found env var resolves to `api_key`.
- **Edge case:** Config file is malformed JSON → warning, defaults used.
- **Edge case:** Config file has unknown keys → ignored (forward-compatible).
- **Edge case:** `enable_bash` in config file → ignored (not applied).
- **Integration:** Config-loaded values feed into `make_runtime_services()` and produce correct `ProviderFactoryContext`.

**Verification:**
- Config file at `~/.cpp-harness/config.json` is read and merged.
- Config file permissions: if world-readable, a warning is emitted (no enforcement at read-only stage; enforcement deferred to future config-write path).
- CLI overrides take precedence over config values.
- API key chains resolve correctly.
- `enable_bash` cannot be set via config.
- Architecture tests pass; new public header added to boundary test.

---

### U6. Model resolution and session resume integration

**Goal:** Add model defaults per provider and ensure session resume preserves the stored provider/model.

**Requirements:** R6, R9

**Dependencies:** U5 (consumes ConfigLoader)

**Files:**
- Modify: `include/cch/coding_agent/Config.hpp` (add model defaults map)
- Modify: `src/coding_agent/ConfigLoader.cpp` (model resolution logic)
- Modify: `src/AsyncCliRuntime.cpp` (session resume integration)
- Modify: `src/coding_agent/runtime/SessionLifecycle.cpp` (extract stored provider/model on resume)
- Modify: `tests/coding_agent/ConfigLoaderTest.cpp` (model resolution tests)
- Modify: `tests/cli/CliSmokeTest.cpp` (resume preservation test)

**Approach:**
- Add a built-in `defaultModelPerProvider` map: `{"openai-compatible": "gpt-4.1-mini"}` — extendable.
- When no model is specified (not in CLI, not in config), resolve from the provider default.
- On session resume: `SessionLifecycle::resume_session` already reads session metadata. Extract `provider` and `model` from session metadata and inject them into `RuntimeServicesConfig`, overriding config defaults but still allowing CLI flags to override (user intent trumps session).
- Populate `ProviderFactoryContext::api_key` from the U5-resolved API key value. Modify the provider factory to prefer `api_key` when non-empty over `api_key_env` lookup. (`api_key` already exists as a field on `ProviderFactoryContext` but is currently unused.)

**Patterns to follow:**
- pi's `model-resolver.ts` for default provider→model mapping.
- Existing `SessionLifecycle::resume_session` as the metadata extraction point.
- Existing `RuntimeServicesConfig` as the value carrier.

**Test scenarios:**
- **Happy path:** No model specified anywhere → provider default used.
- **Happy path:** Model in config file → used when CLI doesn't specify.
- **Happy path:** Session resume → stored provider/model used, config defaults overridden.
- **Happy path:** Session resume with explicit CLI `--model` → CLI overrides stored model.
- **Edge case:** Session resume with provider/model that no longer exists → fallback to config defaults with warning.
- **Edge case:** `--provider` flag on resume changes provider → model falls back to new provider's default, not stored model.

**Verification:**
- Model resolution follows priority: CLI > session-stored > config file > provider default.
- Session resume doesn't break when provider/model change between recordings.
- Architecture tests pass.

---

## System-Wide Impact

- **Interaction graph:** U5/U6 insert a `ConfigLoader` between `AsyncCliRuntimeConfig` and `RuntimeServicesConfig` in `AsyncCliRuntime.cpp`. Tools (U1-U4) change their LLM-facing schemas and internal behavior but keep the same factory signatures and `ExecutionEnv` boundaries.
- **Error propagation:** Config file parse errors produce warnings, not fatal errors. Tool argument validation errors produce `is_error: true` tool results as before. API key resolution failures propagate as provider creation errors.
- **State lifecycle risks:** Bash temp files are workspace-side artifacts that must be cleaned up on session end. Config file is read once at startup; no hot-reload race conditions.
- **API surface parity:** The edit tool schema change (`old_text`/`new_text` → `edits[]`) is the most impactful LLM-facing API change that could affect provider tool-call JSON compatibility. The bash timeout unit change (ms→s) and write tool `create_parents` removal are also LLM-facing schema changes but carry lower compatibility risk. Backward-compatible fallback mitigates this.
- **Unchanged invariants:** `AsyncAgentTool` interface, `AsyncExecutionEnv` interface (except minor result struct additions), `ProviderRegistry` interface, tool factory signatures, and CMake dependency direction all remain stable. `enable_bash` remains CLI-only. JSONL session format unchanged (new metadata fields are optional).

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Edit tool `edits[]` schema change causes provider tool-call parsing issues | Backward-compatible single-arg fallback in `prepareEditArguments`; test with both OpenAI and Kimi providers |
| Bash timeout unit change (ms→s) surprises existing scripts or recorded sessions | Add deprecation tolerance: if model sends `timeout_ms`, convert to seconds with warning; test with existing JSONL transcripts |
| Config file format evolves and breaks forward compatibility | Ignore unknown JSON keys; treat malformed files as warnings not errors |
| ANSI stripping removes intentional formatting from command output | Strip only CSI sequences (`\x1b[...m`); preserve other control characters; test with common CLI tools (ls --color, git diff) |
| Temp file cleanup leaks disk space or sensitive command output | Clean on session end; store under workspace (already contained); exclude from session transcripts |

---

## Sources & References

- **Roadmap:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T5 items 1+3)
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- **pi edit tool:** `pi:packages/coding-agent/src/core/tools/edit.ts`, `edit-diff.ts`
- **pi read tool:** `pi:packages/coding-agent/src/core/tools/read.ts`
- **pi bash tool:** `pi:packages/coding-agent/src/core/tools/bash.ts`
- **pi write tool:** `pi:packages/coding-agent/src/core/tools/write.ts`
- **pi truncation:** `pi:packages/coding-agent/src/core/tools/truncate.ts`
- **pi bash executor:** `pi:packages/coding-agent/src/core/bash-executor.ts`
- **pi output guard:** `pi:packages/coding-agent/src/core/output-guard.ts`
- **pi config:** `pi:packages/coding-agent/src/config.ts`, `model-registry.ts`, `model-resolver.ts`
