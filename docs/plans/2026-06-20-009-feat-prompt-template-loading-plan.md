---
title: "feat: Prompt template file loading and runtime wiring"
type: feat
status: completed
date: "2026-06-20"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---


# feat: Prompt template file loading and runtime wiring

## Summary

Load `.md` prompt template files from project-local `.cpp-harness/prompts/` (trust-gated) and explicit `--prompt-template <path>` CLI paths, parse YAML frontmatter for name and description, and wire the loaded templates into the existing `process_prompt()` / `AgentSessionRunner` pipeline. The bash-style argument substitution engine (`$1`, `$@`, `${N:-default}`, `${@:N:L}`, quoted args) is already implemented and tested; this plan fills the loading gap.

---

## Problem Frame

The parity roadmap (T6 step 8) calls for prompt template loading as the next resource kind after skills. The expansion engine (`expand_prompt_template()`) and `PromptTemplate` passive struct already exist, as do the `process_prompt()` and `AgentSessionRunner` injection points. But templates are currently never loaded from disk — the template vector is always empty. Users cannot define project-local or explicit prompt templates.

The skill loading infrastructure (directory discovery, frontmatter parsing, diagnostic collection, trust gating, runtime wiring) provides a direct structural pattern to follow. Prompt template loading is a smaller, simpler variant: flat `.md` files in a single directory, non-recursive, with the same frontmatter parser reused directly.

---

## Requirements

- R1. Load `.md` files as `PromptTemplate` instances from `.cpp-harness/prompts/` (project-local, non-recursive) when the project trust/resource load plan allows it.
- R2. Load `.md` files from explicit `--prompt-template <path>` CLI paths (file or directory, repeatable).
- R3. Parse YAML frontmatter from each `.md` file: extract `description` (optional) and `argument-hint` (optional); use the body as `PromptTemplate::content`.
- R4. Emit diagnostics (stderr warnings) for unreadable files, parse failures, and duplicate template names; never abort loading on recoverable failures.
- R5. Deduplicate templates by name: first discovered wins, subsequent occurrences produce a diagnostic.
- R6. Integrate loaded templates into the existing `process_prompt()` → `expand_prompt_template()` pipeline so `/templateName args` works in REPL and one-shot modes.
- R7. Gate project-local prompt loading behind the existing `ProjectResources` trust pipeline, matching the skill loading security posture.
- R8. Preserve all existing prompt expansion behavior: the 11 `PromptExpanderTest` test cases must continue to pass.

---

## Scope Boundaries

- Global `~/.cpp-harness/prompts/`: deferred — follows the same deferred dependency as global skill directories (user-resource filesystem root outside workspace guard).
- Package-driven template discovery (`package.json` `pi.prompts`, package `prompts/` directories): deferred to later T6 package work.
- Settings-driven template paths (`prompts` array in config): deferred.
- Template autocomplete in REPL: deferred until TUI exists.
- Live reload on file changes: deferred.
- Recursive subdirectory scanning: intentionally excluded — pi spec is non-recursive.
- `argument-hint` display: stored on the struct, but rendering is deferred until TUI autocomplete exists.

### Deferred to Follow-Up Work

- Global `~/.cpp-harness/prompts/` loading: blocked on user-resource filesystem policy (same as global skills).

---

## Context & Research

### Relevant Code and Patterns

- **`include/cch/coding_agent/PromptProcessing.hpp`** — `PromptTemplate` struct, `expand_prompt_template()`, `process_prompt()` overloads. The injection surface.
- **`src/coding_agent/PromptExpander.cpp`** — bash-style arg parser, `$1`-`$9`, `$@`/`$ARGUMENTS`, `${N:-default}`, `${@:N:L}`, quote handling.
- **`include/cch/coding_agent/SkillLoader.hpp` + `src/coding_agent/SkillLoader.cpp`** — structural template: `SkillDirSpec`, free-function loader, `SkillLoadResult`/`SkillDiagnostic`, dedup, diagnostics to stderr. Prompt template loading mirrors this pattern at smaller scale.
- **`src/coding_agent/SkillFrontmatterParser.hpp/.cpp`** — `parseFrontmatter(string_view) -> Expected<{map<string,string> fields, string body}>`. Directly reusable for prompt templates.
- **`src/coding_agent/runtime/RuntimeServices.hpp/.cpp`** — `make_runtime_services()` loads skills at startup; prompt templates insert at the same point.
- **`src/coding_agent/runtime/AgentSessionRunner.hpp`** — constructor already accepts `vector<PromptTemplate> templates = {}`.
- **`src/AsyncCliRuntime.cpp`** — REPL loop calls `process_prompt(line, {} /* templates */, ...)` with empty templates; resource plan builds `skill_dirs`; two marked injection points.
- **`include/cch/coding_agent/ProjectResources.hpp` + `src/coding_agent/ProjectResources.cpp`** — `ProjectResourceKind::ProjectPrompts` already defined; `has_implemented_loader()` currently returns `false` for it.
- **`tests/coding_agent/PromptExpanderTest.cpp`** — 11 expansion tests already pass; must continue to pass.
- **`tests/coding_agent/SkillIntegrationTest.cpp`** — integration test pattern for end-to-end resource loading.
- **Pi reference:** `pi:packages/agent/src/harness/prompt-templates.ts`, `pi:packages/coding-agent/docs/prompt-templates.md`.

### Institutional Learnings

No `docs/solutions/` entries exist yet. The skill loading plans (`docs/plans/2026-06-20-006-feat-skill-file-discovery-plan.md`, `docs/plans/2026-06-20-007-feat-skill-model-visible-integration-plan.md`) and trust plan (`docs/plans/2026-06-20-008-feat-project-trust-resource-controls-plan.md`) serve as the closest institutional record.

### External References

None — local patterns are sufficient (skill loading provides 3+ direct pattern examples).

---

## Key Technical Decisions

- **Reuse SkillFrontmatterParser directly.** The parser is content-agnostic (flat YAML → `map<string,string>` + body). Prompt template files have the same `---` delimited frontmatter shape. No extraction to a shared utility is needed; the parser already tolerates unknown keys.
- **Follow the SkillLoader architecture at reduced scale.** `PromptTemplateLoadResult` = `{vector<PromptTemplate>, vector<PromptTemplateDiagnostic>}`, free-function `loadPromptTemplates(fs, dirs)`, dedup by name (first wins), diagnostics to stderr via `[template:warn]` prefix.
- **`PromptTemplate` gains `argument_hint` optional field with explicit default.** Declared as `std::optional<std::string> argument_hint = std::nullopt;` — the `= std::nullopt` default member initializer is required for C++20 aggregate initialization backward compatibility (existing positional initializers like `{"greet", std::nullopt, "Hello $1!"}` provide 3 values for 4 members and rely on the trailing default). Stored now for forward compatibility with future TUI autocomplete; not rendered in any current output path.
- **Project-local loading is trust-gated.** `.cpp-harness/prompts/` marker already triggers trust resolution via `detect_project_resources()`. `has_implemented_loader()` must return `true` for `ProjectPrompts` to unblock the resource plan. A `project_prompts_allowed()` helper mirrors `project_skills_allowed()`.
- **CLI `--prompt-template <path>` (repeatable) and `--no-prompt-templates` (disable all).** Follow the pi CLI contract: `--prompt-template` accepts file or directory paths; directory inputs load non-recursively. `--no-prompt-templates` disables both project-local and explicit path loading.
- **Non-recursive directory scanning.** Per pi spec: `prompts/` directories are scanned for direct `.md` children only. Subdirectories are ignored.
- **Missing directories are silent.** Like skills, a directory that doesn't exist produces no diagnostic.

---

## Implementation Units

### U1. Prompt template loading types and implementation

**Goal:** Define `PromptTemplateDiagnostic`, `PromptTemplateLoadResult`, `PromptTemplateDirSpec`, and implement the file loading / directory scanning logic. Existing expansion engine and frontmatter parser are reused without change.

**Requirements:** R1, R3, R4, R5

**Dependencies:** None

**Files:**
- Modify: `include/cch/coding_agent/PromptProcessing.hpp` — add `argument_hint` field to `PromptTemplate`
- Create: `include/cch/coding_agent/PromptTemplateLoader.hpp` — `PromptTemplateDiagnostic`, `PromptTemplateLoadResult`, `PromptTemplateDirSpec`, `loadPromptTemplateFromFile()`, `loadPromptTemplates()`
- Create: `src/coding_agent/PromptTemplateLoader.cpp` — implementation
- Create: `tests/coding_agent/PromptTemplateLoaderTest.cpp`
- Modify: `CMakeLists.txt` — add `src/coding_agent/PromptTemplateLoader.cpp` to `cch_coding_agent_runtime`

**Approach:**
- `PromptTemplateDiagnostic` mirrors `SkillDiagnostic`: `{type: "warning", code: enum, message: string, path: string}`. Codes: `file_info_failed`, `list_failed`, `read_failed`, `parse_failed`, `duplicate_name`.
- `PromptTemplateDirSpec` is `{path: string}` (project-local dirs don't need `includeRootFiles` — all `.md` children are templates). Explicit `--prompt-template` paths are resolved to absolute and passed as-is.
- `loadPromptTemplateFromFile(fs, path)`: read via `WorkspaceFileSystem::readTextFile()`, call `parseFrontmatter()`, extract `name` from filename (strip `.md`), `description` from frontmatter `"description"` key, `argument_hint` from `"argument-hint"` key, validate name is non-empty. Return `{optional<PromptTemplate>, vector<diagnostic>}`.
- `loadPromptTemplates(fs, vector<PromptTemplateDirSpec>)`: for each dir spec, call `listDir()`, filter to `.md` files, load each via `loadPromptTemplateFromFile()`, deduplicate by name (first wins). Missing dirs and non-`.md` files are silently skipped.
- Diagnostics are collected and returned; the caller prints them (U3 wires this into `make_runtime_services()`).

**Execution note:** Implement the loader test-first. Write tests that create temp `.md` files via `TempWorkspace`, call the loader, and assert template fields and diagnostics.

**Patterns to follow:**
- `src/coding_agent/SkillLoader.cpp` — directory walk, dedup, diagnostic accumulation
- `src/coding_agent/SkillFrontmatterParser.hpp` — `parseFrontmatter()` call signature

**Test scenarios:**
- Happy path: single `.md` file with frontmatter `description:` → `PromptTemplate` with correct name, description, and body content
- Happy path: `.md` file without frontmatter → `PromptTemplate` with name from filename, empty description, full file content as body
- Happy path: `.md` file with `argument-hint:` frontmatter → `argument_hint` field populated
- Happy path: directory with 3 `.md` files → 3 templates returned, sorted by name
- Happy path: explicit file path → single template loaded
- Edge case: empty `.md` file → template with empty content (valid)
- Edge case: `.md` file with only frontmatter, no body → empty body
- Edge case: filename determines template name (`hello-world.md` → `/hello-world`)
- Edge case: duplicate template name across two directories → first wins, duplicate diagnostic emitted
- Error path: unreadable file → diagnostic with `read_failed` code, template skipped
- Error path: malformed YAML frontmatter → diagnostic with `parse_failed` code, template skipped
- Error path: missing directory → silent (no diagnostic, no templates)
- Error path: `listDir` failure → diagnostic with `list_failed` code
- Error path: file with no `.md` extension in directory → silently skipped

**Verification:**
- All test scenarios pass
- Existing 11 `PromptExpanderTest` cases continue to pass
- `CMakeLists.txt` builds without errors
- No new vcpkg dependencies

---

### U2. Trust gating for project prompt templates

**Goal:** Route project-local `.cpp-harness/prompts/` through the existing `ProjectResources` trust pipeline so templates are only loaded when the project is trusted.

**Requirements:** R7

**Dependencies:** U1

**Files:**
- Modify: `src/coding_agent/ProjectResources.cpp` — update `has_implemented_loader()` and add `project_prompts_allowed()`
- Modify: `include/cch/coding_agent/ProjectResources.hpp` — add `project_prompts_allowed()` declaration
- Modify: `tests/coding_agent/ProjectResourcesTest.cpp` — add trust gating tests

**Approach:**
- `has_implemented_loader()` currently returns `true` only for `ProjectSkills`. Add `ProjectPrompts` as a second supported kind.
- Add `project_prompts_allowed(const ProjectResourceLoadPlan&) -> bool`: returns `true` when prompts were detected, allowed, and not skipped for untrusted/unsupported reasons. Mirrors `project_skills_allowed()` exactly.
- The existing `build_project_resource_load_plan()` already processes `ProjectPrompts` through the trust decision matrix — it just returns `Unsupported` because `has_implemented_loader()` says `false`. Updating the function unblocks it.
- No new `ProjectResourcePolicy` fields needed — `project_skills` enablement is intentionally the gate for project-local resources generically. If a separate prompts toggle is desired later, it can be added to the policy struct.

**Patterns to follow:**
- `project_skills_allowed()` in `src/coding_agent/ProjectResources.cpp` — copy the predicate shape

**Test scenarios:**
- Happy path: trusted project with `.cpp-harness/prompts/` marker → `project_prompts_allowed()` returns `true`
- Happy path: trusted project without `.cpp-harness/prompts/` marker → `project_prompts_allowed()` returns `false` (nothing to load)
- Edge case: untrusted project with marker, default trust=ask → `project_prompts_allowed()` returns `false`
- Edge case: `--no-approve` with marker → `project_prompts_allowed()` returns `false`
- Edge case: `--approve` with marker → `project_prompts_allowed()` returns `true`
- Edge case: `--no-skills` with marker → `project_prompts_allowed()` returns `false` (resource policy `Off` wins)

**Verification:**
- All test scenarios pass
- Existing `ProjectResourcesTest` cases continue to pass
- Trust-gating behavior for prompts is identical to skills (same decision matrix)

---

### U3. Runtime services wiring, CLI flags, and end-to-end integration

**Goal:** Wire prompt template loading into `RuntimeServices` startup, pass loaded templates to `AgentSessionRunner` and the REPL loop, add `--prompt-template` and `--no-prompt-templates` CLI flags, and verify end-to-end template invocation works.

**Requirements:** R2, R6, R8

**Dependencies:** U1, U2

**Files:**
- Modify: `src/coding_agent/runtime/RuntimeServices.hpp` — add `prompt_dirs` to config, `prompt_load_result` to services
- Modify: `src/coding_agent/runtime/RuntimeServices.cpp` — add prompt loading call in `make_runtime_services()`
- Modify: `src/AsyncCliRuntime.hpp` — add `prompt_template_paths` and `no_prompt_templates` to config
- Modify: `src/AsyncCliRuntime.cpp` — build prompt dirs from resource plan + CLI paths, pass templates to `AgentSessionRunner` and REPL loop
- Modify: `src/main.cpp` — add `--prompt-template` and `--no-prompt-templates` CLI flags
- Modify: `tests/cli/CliSmokeTest.cpp` — add smoke tests for new flags
- Modify: `tests/coding_agent/SkillIntegrationTest.cpp` — optionally add end-to-end template loading test

**Approach:**

**RuntimeServices wiring:**
- Add `std::vector<std::string> prompt_dirs` to `RuntimeServicesConfig` (workspace-relative or absolute paths).
- In `make_runtime_services()`, after the skill loading block: if `prompt_dirs` is non-empty, create a `WorkspaceFileSystem`, construct `PromptTemplateDirSpec` entries, call `loadPromptTemplates()`, store result in `RuntimeServices::prompt_load_result`.
- Print diagnostics to stderr using `[template:warn]` prefix, mirroring the skill diagnostic printing pattern.

**AsyncCliRuntime wiring:**
- In `run_async_cli()`, after the existing skill dirs block:
  1. If `project_prompts_allowed(resource_plan)` → push `".cpp-harness/prompts"` to `prompt_dirs`.
  2. Append each `--prompt-template <path>` value to `prompt_dirs`.
  3. If `--no-prompt-templates` → clear `prompt_dirs`.
- Pass `prompt_dirs` into `RuntimeServicesConfig`.
- Pass `services->prompt_load_result.templates` to `AgentSessionRunner` constructor (replacing the `{} /* templates — empty until U3 */` placeholder).
- In the REPL loop, replace `process_prompt(line, {} /* templates */, ...)` with `process_prompt(line, runner.templates(), ...)` — the runner already stores templates, expose them via a const accessor if needed, or capture the loaded templates vector in a local variable.

**CLI flags:**
- `--prompt-template <path>`: `CLI::App::add_option()`, repeatable (`->expected(0,-1)`), accepts file or directory paths. Stored as `std::vector<std::string>`.
- `--no-prompt-templates`: `CLI::App::add_flag()`, disables all prompt template loading (both project-local and explicit paths).
- Flow through `AsyncCliRuntimeConfig` → `run_async_cli()`.

**Patterns to follow:**
- Skill loading block in `make_runtime_services()` (lines ~55-85 in `RuntimeServices.cpp`)
- `--no-skills` flag pattern in `main.cpp` / `AsyncCliRuntime.cpp`
- `project_skills_allowed()` usage in `AsyncCliRuntime.cpp`

**Test scenarios:**
- Happy path: project with `.cpp-harness/prompts/greet.md` → `/greet world` expands to template content with `$1` substitution
- Happy path: `--prompt-template /path/to/custom.md` → template available as `/custom`
- Happy path: `--prompt-template /path/to/dir/` → all `.md` files in directory loaded
- Edge case: `--no-prompt-templates` with project prompts present → no templates loaded
- Edge case: `--no-prompt-templates` with `--prompt-template` → no templates loaded (disable wins)
- Edge case: untrusted project → project-local prompts not loaded, but `--prompt-template` explicit paths still work
- Edge case: duplicate template name from project + explicit path → project-local wins (loaded first)
- Integration: REPL `/greet world` with loaded template → template expanded, result passed to agent loop

**Verification:**
- All test scenarios pass
- Existing CLI smoke tests continue to pass
- Existing 11 `PromptExpanderTest` cases continue to pass
- `--help` output shows new flags
- Templates loaded at startup are available in both REPL and one-shot modes

---

## System-Wide Impact

- **Interaction graph:** New loading code runs synchronously in `make_runtime_services()` (same call site as skill loading). No new async operations. Templates are consumed by `process_prompt()` (already accepts the vector) and `AgentSessionRunner` constructor (already accepts the vector).
- **Error propagation:** Loading failures produce stderr diagnostics; never abort startup. A template with a parse error is silently skipped with a warning — the rest of the session proceeds normally.
- **State lifecycle risks:** None. Templates are immutable after startup loading.
- **API surface parity:** `PromptTemplate` gains one new optional field (`argument_hint = std::nullopt`). The explicit default member initializer makes this backward-compatible under C++20 aggregate initialization rules — existing positional initializers (e.g., `{"greet", std::nullopt, "Hello $1!"}` with 3 values for 4 members) continue to compile because the trailing field has a default member initializer.
- **Integration coverage:** The REPL loop integration test (`/greet world` → template expansion → agent loop) is the key cross-layer verification.
- **Unchanged invariants:** `expand_prompt_template()` behavior is completely unchanged. `process_prompt()` pipeline order (skill expansion → command dispatch → template expansion) is unchanged. All existing slash-commands continue to work identically.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `PromptTemplate` struct change (new `argument_hint` field) breaks existing designated-initializer construction sites. | New field is `std::optional<std::string>` defaulting to `std::nullopt`. All existing sites omit the field in designated initializers — this compiles correctly per C++20/23 aggregate initialization rules. Verified by searching for `PromptTemplate{` in the codebase (only found in test files). |
| `WorkspaceFileSystem` is workspace-bound; explicit `--prompt-template` paths outside the workspace may fail. | Explicit paths passed via CLI are resolved to absolute by the caller before reaching the loader. For workspace-relative lookups, the loader uses the runtime's workspace filesystem. Explicit absolute paths outside the workspace will need to use `std::filesystem` directly or a second `WorkspaceFileSystem` instance rooted at the parent. Decision: create a separate `WorkspaceFileSystem` for each absolute directory path. Deferred to implementation if edge cases surface. |
| Trust gating logic diverges between skills and prompts. | Both use the same `ProjectResources` pipeline and `project_*_allowed()` helper pattern. Tests verify identical decision matrix behavior. |

---

## Sources & References

- **Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T6 item)
- Pi reference contract: `pi:packages/agent/src/harness/prompt-templates.ts`
- Pi documentation: `pi:packages/coding-agent/docs/prompt-templates.md`
- Skill loader pattern: `include/cch/coding_agent/SkillLoader.hpp`, `src/coding_agent/SkillLoader.cpp`
- Skill file discovery plan: `docs/plans/2026-06-20-006-feat-skill-file-discovery-plan.md`
- Skill integration plan: `docs/plans/2026-06-20-007-feat-skill-model-visible-integration-plan.md`
- Project trust plan: `docs/plans/2026-06-20-008-feat-project-trust-resource-controls-plan.md`
- Prompt processing contract: `include/cch/coding_agent/PromptProcessing.hpp`
- Runtime services: `src/coding_agent/runtime/RuntimeServices.hpp`
