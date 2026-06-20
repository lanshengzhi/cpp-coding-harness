---
title: "feat: Implement skill file discovery and loading"
type: feat
status: completed
date: 2026-06-20
---

# feat: Implement skill file discovery and loading

## Summary

Add skill file discovery and loading — scan configured directories for `SKILL.md` files, parse YAML frontmatter, validate metadata against the Agent Skills standard (with pi's leniency on name/directory mismatch), and return `Skill` objects with diagnostics. The feature stops at loadable, validated skill objects; model-visible integration (system prompt, `/skill:name` command) is deferred.

## Completion Status

Completed on 2026-06-20. The shipped slice adds passive `Skill`/`SkillDiagnostic` contracts, flat frontmatter parsing, recursive `SkillDirSpec` loading with deduplication and diagnostics, runtime startup loading, and CLI stderr diagnostics. The reusable loader supports arbitrary directories and global-style root `.md` files via `includeRootFiles`; current CLI wiring loads project-local `.cpp-harness/skills` only. Global `~/.cpp-harness/skills` and config-driven skill directories remain deferred follow-up work.

---

## Problem Frame

The `cpp-coding-harness` currently has no skill loading. The pi reference harness discovers skills at startup from global (`~/.pi/agent/skills/`) and project directories, parses their `SKILL.md` frontmatter, validates metadata, and makes them available for system prompt injection and explicit invocation. This gap blocks progress on T6 (Resources, Skills, Extensions, Packages) and leaves agents without specialized capability packages that pi agents rely on.

The T0–T5 parity slices are complete: the execution environment has all needed pi-shaped filesystem methods (`readTextFile`, `listDir`, `fileInfo`, `canonicalPath`), and the runtime assembly point (`make_runtime_services`) is ready for a skills handoff.

---

## Requirements

- **R1.** Define `Skill`, `SkillDiagnostic`, and `SkillDiagnosticCode` as passive value types matching pi's contract shape.
- **R2.** Parse YAML frontmatter from `SKILL.md` files — extract `name`, `description`, and `disable-model-invocation`; tolerate unknown keys.
- **R3.** Validate skill metadata against the Agent Skills standard with pi's leniency: `[a-z0-9-]+`, ≤64 chars, no leading/trailing/consecutive hyphens; name/directory mismatch is a warning, not a rejection. Description required (empty → reject); >1024 chars → warning but skill still loaded.
- **R4.** Discover skills recursively from one or more configured directories. Skip dot-prefixed entries and `node_modules`, resolve symlinks via `canonicalPath`, load at most one `SKILL.md` per directory.
- **R5.** Deduplicate skills by name (first found wins); emit a `SkillDiagnostic` for each dropped duplicate.
- **R6.** Wire skill loading into the runtime startup sequence via `RuntimeServices`, making skills available to future system-prompt and command consumers.
- **R7.** Print loading diagnostics to stderr in CLI text mode so users can see malformed or shadowed skills.

---

## Scope Boundaries

- Model-visible integration (system prompt injection, `<available_skills>` XML block, `/skill:name` command registration) — deferred to follow-up plan after skill objects are loadable and validated.
- Prompt template loading (`pi:packages/agent/src/harness/prompt-templates.ts`) — separate T6 item.
- Package discovery/installation — separate T6 item.
- Project trust controls — separate T6 item.
- Extension boundary design — separate T6 item.
- `.gitignore`/`.ignore`/`.fdignore` support during directory traversal — deferred to follow-up.

### Deferred to Follow-Up Work

- **Ignore-file support**: Add `.gitignore`/`.ignore`/`.fdignore` pattern matching to the directory walk (separate PR).
- **System prompt skill injection**: Build `<available_skills>` XML block and integrate into system prompt context (separate plan).
- **`/skill:name` command registration**: Wire into `CommandRegistry` once skill objects are accessible (separate plan).
- **Config-driven skill directories**: Extend `ConfigData` / `RuntimeServicesConfig` with user-configurable skill directory paths (separate plan).

---

## Context & Research

### Relevant Code and Patterns

- **ExecutionEnv seam** (`include/cch/harness/ExecutionEnv.hpp`): Pi-shaped filesystem methods (`readTextFile`, `listDir`, `fileInfo`, `canonicalPath`, `joinPath`). Skill loading must go through this interface, not `std::filesystem` directly.
- **RuntimeServices** (`src/coding_agent/runtime/RuntimeServices.hpp`): Assembles `client`, `env`, and `tools` at startup. Skill loading should extend `RuntimeServicesConfig` and add a `skills` field to `RuntimeServices`.
- **TempWorkspace** (`tests/support/TempWorkspace.hpp`): Creates isolated temp directories with `write(relative, content)` for filesystem-dependent tests.
- **Aggregate struct convention** (`include/cch/ai/Content.hpp`, `include/cch/harness/ExecutionEnv.hpp`): Passive value types with designated initializers, no hidden state.
- **Error handling**: `util::Expected<T>` for internal errors, `std::expected<T, FileError>` for filesystem ops via ExecutionEnv.
- **CMake target pattern**: `add_library(cch_<name>)` with `cch_target_defaults`, then add to `cpp_harness_lib` transitive dep chain.

### Institutional Learnings

- Pi's `skills.ts` reference implementation uses a hand-written `parseFrontmatter()` that normalizes line endings, splits on `---`, and parses the flat YAML block. No full YAML parser is needed for the narrow frontmatter schema used by skills.
- `WorkspaceFileSystem::listDir()` is non-recursive — skill loading must implement its own recursive walk, which naturally inherits workspace containment.
- Pi's `resolveKind` pattern (canonicalPath → fileInfo on resolved target) is the right way to handle symlinks in the skill directory walk.
- `yaml-cpp` is available in vcpkg but adds build complexity for a schema that is flat key-value pairs. The hand-written approach matches pi's approach and avoids a new dependency.

### External References

- [Agent Skills specification](https://agentskills.io/specification)
- [Pi skills documentation](pi:packages/coding-agent/docs/skills.md)
- [Pi skills.ts reference implementation](pi:packages/agent/src/harness/skills.ts)
- [Pi Skill type definition](pi:packages/agent/src/harness/types.ts)

---

## Key Technical Decisions

- **Hand-written YAML frontmatter parser over yaml-cpp dependency**: The skill frontmatter schema is flat (string/bool keys, no nesting, no sequences). A hand-written parser following pi's approach (normalize line endings, split on `---`, parse `key: value` lines) is sufficient, avoids a new vcpkg dependency, and keeps the build simple. If later T6 items (packages, extensions) need richer YAML, yaml-cpp can be added then.

- **Skill types and loader live in `cch_coding_agent_runtime`**: The `cch_coding_agent_runtime` target already depends on `cch_harness` (for ExecutionEnv) and is the natural bridge between harness capabilities and agent-facing resources. A separate `cch_resources` target would be over-engineering for a single feature; it can be extracted later when prompt templates and packages are added.

- **Synchronous skill loading via `WorkspaceFileSystem`**: Skills directories are typically small (a few dozen files). The skill loader accepts a `const WorkspaceFileSystem&` — the synchronous, non-coroutine filesystem implementation behind `AsyncLocalExecutionEnv`. This avoids forcing `make_runtime_services` (a synchronous factory) into a coroutine, keeps the loader simple, and preserves the ExecutionEnv seam for testing. `WorkspaceFileSystem` is a private header (`src/harness/WorkspaceFileSystem.hpp`) that `src/coding_agent/` can include because the CMake build adds `src/` as a private include directory for all targets.

- **Duplicate name diagnostics**: When a skill name collides, the second skill is dropped and a `SkillDiagnostic` with code `duplicate_name` is emitted. Pi doesn't dedup at load time (dedup happens later in agent-harness), but emitting diagnostics here gives users immediate feedback about shadowed skills, which is more debuggable.

- **Project-local runtime path, design for arbitrary dirs**: `loadSkills(fs, dirs)` accepts arbitrary `SkillDirSpec` lists for testability and future config-driven paths. The runtime wiring currently produces a project-local `SkillDirSpec` for `<workspace>/.cpp-harness/skills/` (`includeRootFiles=false`). The loader supports global-style root `.md` files via `includeRootFiles=true`, but CLI wiring for `~/.cpp-harness/skills/` is deferred because it needs a filesystem root outside the workspace guard. Config-driven paths are also deferred.

- **Description >1024 chars is a warning, not a rejection**: Matching pi's behavior — the diagnostic is emitted but the skill still loads. This avoids breaking skills that work in pi.

---

## Open Questions

### Resolved During Planning

- **YAML parsing strategy**: Hand-written parser — resolved above in Key Technical Decisions.
- **Integration point**: Extend `RuntimeServices` — resolved above.
- **Description length behavior**: Warn and load — resolved above.
- **Diagnostic surface**: Print to stderr in text/CLI mode — resolved above.
- **Symlink resolution**: Follow via `canonicalPath` → `fileInfo` — resolved above.
- **Async vs. sync**: Synchronous loading — resolved above.

### Deferred to Implementation

- **Exact diagnostic format on stderr**: The format string (e.g., `[skill:warn] <code>: <message> (<path>)`) is a presentation choice best decided when seeing real output.
- **BOM handling precision**: Whether to strip UTF-8 BOM before frontmatter detection — depends on whether real-world `SKILL.md` files ever carry BOMs.
- **Signal handling during skill loading**: Ctrl-C during synchronous file I/O — deferred until async loading exists.

---

## Implementation Units

### U1. Skill and SkillDiagnostic types

**Goal:** Define `Skill`, `SkillDiagnostic`, and `SkillDiagnosticCode` as passive value types matching pi's contract.

**Requirements:** R1

**Dependencies:** None

**Files:**
- Create: `include/cch/coding_agent/Skill.hpp`
- Test: `tests/coding_agent/SkillTest.cpp` (new)

**Approach:**
- `Skill` is an aggregate struct: `name`, `description`, `content` (body after frontmatter), `filePath` (absolute path to SKILL.md), `disableModelInvocation` (default `false`).
- `SkillDiagnosticCode` is an enum: `file_info_failed`, `list_failed`, `read_failed`, `parse_failed`, `invalid_metadata`, `duplicate_name`.
- `SkillDiagnostic` is an aggregate struct: `type` (always `"warning"`), `code` (SkillDiagnosticCode), `message` (human-readable), `path` (associated file/directory path).
- `SkillLoadResult` is an aggregate struct: `skills` (vector of Skill), `diagnostics` (vector of SkillDiagnostic).
- All types in namespace `cch::coding_agent`. Follow the existing aggregate struct convention with designated initializer-friendly defaults.

**Patterns to follow:**
- `FileError` / `FileInfo` in `include/cch/harness/ExecutionEnv.hpp` — enum + aggregate struct pattern
- `Content` types in `include/cch/ai/Content.hpp` — variant-based passive value pattern

**Test scenarios:**
- Happy path: Construct a `Skill` with all fields populated; verify field access.
- Happy path: Construct a `SkillDiagnostic`; verify fields.
- Happy path: Construct a `SkillLoadResult` with one skill and one diagnostic.
- Edge case: `Skill` with empty `content` string.
- Edge case: `Skill` with `disableModelInvocation = true`.
- Edge case: `SkillLoadResult` with empty skills and empty diagnostics vectors.

**Verification:**
- `Skill.hpp` compiles as a standalone header with no implementation dependencies.
- All fields are publicly accessible (aggregate).
- `SkillLoadResult` is movable and returned by value from loading functions.

---

### U2. YAML frontmatter parser

**Goal:** Parse `SKILL.md` content into `{frontmatter map, body string}` using a hand-written parser for the flat YAML subset used by skills.

**Requirements:** R2

**Dependencies:** U1

**Files:**
- Create: `src/coding_agent/SkillFrontmatterParser.hpp` (private header)
- Create: `src/coding_agent/SkillFrontmatterParser.cpp`
- Test: `tests/coding_agent/SkillFrontmatterParserTest.cpp` (new)

**Approach:**
- `parseFrontmatter(content: string_view) -> util::Expected<SkillFrontmatter>` where `SkillFrontmatter` holds `std::map<std::string, std::string> fields` and `std::string body`.
- Normalize `\r\n` → `\n` before processing.
- If content doesn't start with `---\n`, return the entire content as body with empty frontmatter.
- Find the closing `\n---` (or `\n---\n`); if not found, treat entire content as body with empty frontmatter.
- Extract the YAML block between the two `---` delimiters. Parse line by line as flat `key: value` pairs.
- Trim whitespace from keys and values. Support quoted values (`"..."` or `'...'`).
- Recognize `true`/`false` as boolean values; store as string for the generic map (callers cast as needed).
- Unknown keys are preserved in the map (tolerated).
- Empty YAML block (just whitespace between `---` delimiters) returns empty frontmatter with the body after the second `---`.

**Patterns to follow:**
- Pi's `parseFrontmatter` in `skills.ts` — normalize, split on `---`, parse YAML block.
- `util::Expected<T>` error handling convention.

**Test scenarios:**
- Happy path: `---\nname: my-skill\ndescription: Does things.\n---\n# Body content` → parsed with name, description, body.
- Happy path: `disable-model-invocation: true` → field preserved as string `"true"`.
- Happy path: Unknown key `license: MIT` → preserved in fields map.
- Edge case: Content without frontmatter — `# Just a heading` → empty frontmatter, body = entire content.
- Edge case: Opening `---` but no closing `---` → empty frontmatter, body = entire content.
- Edge case: Empty YAML block — `---\n---\nBody` → empty frontmatter, body = `"Body"`.
- Edge case: `\r\n` line endings normalized correctly.
- Edge case: Quoted values — `description: "Does things."` → value `"Does things."` (quotes stripped or preserved consistently).
- Edge case: Value with colon — `description: Check this: important` → full value preserved.
- Error path: YAML block with malformed line (no colon) → `parse_failed` error.

**Verification:**
- Parser returns correct `{fields, body}` for all valid skill frontmatter shapes.
- Parser returns `parse_failed` for malformed YAML.
- Parser tolerates unknown keys without error.

---

### U3. Single skill file loading and validation

**Goal:** Load a single `SKILL.md` file from disk via ExecutionEnv, parse frontmatter, validate metadata, and return a `Skill` or diagnostics.

**Requirements:** R2, R3

**Dependencies:** U1, U2

**Files:**
- Create: `include/cch/coding_agent/SkillLoader.hpp`
- Create: `src/coding_agent/SkillLoader.cpp`
- Test: `tests/coding_agent/SkillLoaderTest.cpp` (new)

**Approach:**
- `loadSkillFromFile(fs, filePath) -> SkillLoadResult` — reads file via `fs.readTextFile()` (synchronous), parses frontmatter, validates, returns result.
- Name resolution: use frontmatter `name` if present, otherwise derive from parent directory basename.
- Name validation: `[a-z0-9-]+`, ≤64 chars, no leading/trailing hyphens, no consecutive hyphens. Name/directory mismatch → `invalid_metadata` diagnostic but skill still loaded.
- Description validation: required (empty/missing → skill rejected, no `Skill` returned). >1024 chars → `invalid_metadata` diagnostic but skill still loaded.
- `disableModelInvocation`: extract from frontmatter, default `false`.
- File read failure → `read_failed` diagnostic, no skill.
- Frontmatter parse failure → `parse_failed` diagnostic, no skill.
- `loadSkillFromFile` is a free function, not a method on ExecutionEnv (matches pi).

**Patterns to follow:**
- Pi's `loadSkillFromFile()` in `skills.ts`.
- `WorkspaceFileSystem` (`src/harness/WorkspaceFileSystem.hpp`) for synchronous filesystem I/O via `std::expected<T, FileError>`.

**Test scenarios:**
- Happy path: Valid `SKILL.md` with name, description, body → returns `Skill` with correct fields, zero diagnostics.
- Happy path: Valid `SKILL.md` with `disable-model-invocation: true` → `Skill.disableModelInvocation == true`.
- Happy path: Frontmatter omits `name` → name derived from parent directory basename.
- Edge case: Description >1024 chars → `invalid_metadata` diagnostic, skill still loaded.
- Edge case: Name contains uppercase → `invalid_metadata` diagnostic, skill still loaded.
- Edge case: Name has leading hyphen → `invalid_metadata` diagnostic, skill still loaded.
- Edge case: Name has consecutive hyphens → `invalid_metadata` diagnostic, skill still loaded.
- Edge case: Name doesn't match parent directory → `invalid_metadata` diagnostic, skill still loaded.
- Edge case: Frontmatter has unknown keys → tolerated, not in diagnostics.
- Error path: Description missing → no Skill returned, `invalid_metadata` diagnostic.
- Error path: Description empty string → no Skill returned, `invalid_metadata` diagnostic.
- Error path: Description only whitespace → no Skill returned, `invalid_metadata` diagnostic.
- Error path: File read fails → `read_failed` diagnostic, no Skill.
- Error path: YAML parse fails → `parse_failed` diagnostic, no Skill.

**Verification:**
- `loadSkillFromFile` processes all valid frontmatter shapes correctly.
- Validation warnings never prevent skill loading (except missing/empty description).
- Diagnostics are emitted for all validation violations.
- Read failures and parse failures produce diagnostics without crashing.

---

### U4. Recursive directory skill discovery

**Goal:** Scan one or more directories recursively, discover all `SKILL.md` files, load them, deduplicate by name, and return a combined `SkillLoadResult`.

**Requirements:** R4, R5

**Dependencies:** U3

**Files:**
- Modify: `include/cch/coding_agent/SkillLoader.hpp` (add `SkillDirSpec` struct and `loadSkills` declaration)
- Modify: `src/coding_agent/SkillLoader.cpp` (add `loadSkills` and internal walk)
- Test: `tests/coding_agent/SkillLoaderTest.cpp` (extend with directory walk tests)

**Approach:**
- `SkillDirSpec` aggregate struct: `std::string path` + `bool includeRootFiles{false}`.
- `loadSkills(fs, dirs) -> SkillLoadResult` — iterates input `SkillDirSpec` entries, calls `loadSkillsFromDir` for each.
- `loadSkillsFromDir(fs, dir, includeRootFiles)` — internal recursive walk using synchronous `WorkspaceFileSystem` methods:
  1. `fs.fileInfo(dir)` — skip on `not_found` (silent), diagnostic on other errors.
  2. `fs.canonicalPath(dir)` + `fs.fileInfo(resolved)` for symlink resolution (pi's `resolveKind`).
  3. `fs.listDir(dir)` — diagnostic on error.
  4. For each entry (sorted by name):
     - `name == "SKILL.md"` → `loadSkillFromFile`, stop recursing this directory (one skill per dir).
     - `name` starts with `.` → skip (hidden files/dirs).
     - `name == "node_modules"` → skip.
     - `kind == Directory` (or symlink resolving to directory) → recurse.
     - `kind == File && includeRootFiles && name.ends_with(".md")` → `loadSkillFromFile` (root `.md` files treated as skills, used for global `~/.cpp-harness/skills/`).
- Deduplication: track seen names in an `unordered_set`. For each loaded skill, if name already seen → emit `duplicate_name` diagnostic, drop skill. Otherwise add to result.
- Missing input directories → silently skipped (no diagnostic). Other `fileInfo` errors → diagnostic.

**Patterns to follow:**
- Pi's `loadSkills()` and `loadSkillsFromDirInternal()` in `skills.ts`.
- `WorkspaceFileSystem` (`src/harness/WorkspaceFileSystem.hpp`) for synchronous `listDir()`, `readTextFile()`, `fileInfo()`, `canonicalPath()`.
- `TempWorkspace` for test fixtures with directory structures.

**Test scenarios:**
- Happy path: Single directory with one `SKILL.md` → one skill loaded.
- Happy path: Nested directories, each with a `SKILL.md` → multiple skills loaded.
- Happy path: Root `.md` file in global-style dir with `includeRootFiles=true` → loaded as skill.
- Happy path: Two input directories, each with skills → all loaded.
- Edge case: Symlink to a directory containing `SKILL.md` → followed and loaded.
- Edge case: Directory with no `SKILL.md` files → empty skills, no diagnostics.
- Edge case: Dot-prefixed directory (`.hidden/skill/SKILL.md`) → skipped entirely.
- Edge case: `node_modules/some-skill/SKILL.md` → skipped entirely.
- Edge case: Two skills with same name from different directories → first loaded, second dropped with `duplicate_name` diagnostic.
- Edge case: Malformed `SKILL.md` in one directory → diagnostic emitted, walk continues to other directories.
- Error path: Input directory doesn't exist → silently skipped.
- Error path: `listDir` fails on a subdirectory → diagnostic emitted, subtree skipped.
- Error path: `fileInfo` fails with non-not_found error → diagnostic emitted, directory skipped.

**Verification:**
- `loadSkills` discovers all valid skills in a directory tree.
- Deduplication preserves first-found and emits diagnostics for collisions.
- Symlinks to directories are followed; symlinks to files are resolved.
- Missing/inaccessible directories don't crash the loader.
- Dot-prefixed and `node_modules` entries are correctly skipped.

---

### U5. Runtime integration — wire skill loading into startup

**Goal:** Extend `RuntimeServices` to carry skills, call `loadSkills` during `make_runtime_services`, and print diagnostics to stderr.

**Requirements:** R6, R7

**Dependencies:** U4

**Files:**
- Modify: `src/coding_agent/runtime/RuntimeServices.hpp` (add `SkillLoadResult skills` field, extend `RuntimeServicesConfig` with `skill_dirs` and `print_skill_diagnostics`)
- Modify: `src/coding_agent/runtime/RuntimeServices.cpp` (construct `WorkspaceFileSystem`, call `loadSkills`)
- Modify: `tests/cli/CliSmokeTest.cpp` (verify startup doesn't crash with/without skills dirs)
- Modify: `tests/coding_agent/SkillLoaderTest.cpp` (integration test with `WorkspaceFileSystem` and `SkillDirSpec`)

**Approach:**
- Extend `RuntimeServicesConfig`:
  - Add `std::vector<std::string> skill_dirs` (empty by default — no skill loading).
  - Add `bool print_skill_diagnostics{true}` (for test scenarios that want silent loading).
- Extend `RuntimeServices`:
  - Add `SkillLoadResult skill_load_result`.
- In `make_runtime_services`:
  - After tool registration, if `config.skill_dirs` is non-empty, construct a `WorkspaceFileSystem` for the workspace root, call `loadSkills(fs, config.skill_dirs)`.
  - Store result in `services.skill_load_result`.
  - If `config.print_skill_diagnostics`, print each diagnostic to stderr: `[skill:warn] <code>: <message> (<path>)`.
- `RuntimeServicesConfig::skill_dirs` type: `std::vector<SkillDirSpec>` where `SkillDirSpec = {path, includeRootFiles}`.
- In `main.cpp` / `run_async_cli`: pass the project-local skill directory to `RuntimeServicesConfig`:
  - Project: `SkillDirSpec{<workspace>/.cpp-harness/skills/, includeRootFiles=false}` (only nested `SKILL.md`, not root `.md` files).
  - Global `~/.cpp-harness/skills/` runtime wiring is deferred until the harness has an explicit filesystem/root policy for user-level resources outside the workspace guard.

**Patterns to follow:**
- `make_runtime_services` existing error-handling pattern — early return on failure.
- Existing `RuntimeServicesConfig` extension pattern (adding fields with defaults).

**Test scenarios:**
- Integration: `make_runtime_services` with empty `skill_dirs` → no crash, `skill_load_result` empty.
- Integration: `make_runtime_services` with a valid skill dir (via TempWorkspace) → skill loaded into `skill_load_result.skills`.
- Integration: `make_runtime_services` with a non-existent skill dir → silently skipped, no crash.
- Integration: `print_skill_diagnostics = false` → no stderr output.
- Smoke: CLI startup with no `~/.cpp-harness/skills/` directory → no crash, no error.
- Smoke: CLI startup with malformed skill in skills dir → diagnostic printed to stderr, startup continues.

**Verification:**
- `RuntimeServices` exposes skills for downstream consumers.
- Startup doesn't crash when skill directories are missing or contain malformed files.
- Diagnostics are printed to stderr and don't contaminate stdout.
- Existing tests (CliSmokeTest, agent loop tests) continue to pass.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Hand-written YAML parser misses edge cases in real-world `SKILL.md` files | Comprehensive test coverage for known frontmatter shapes; yaml-cpp can replace the parser later without changing the public contract |
| `WorkspaceFileSystem` is a private header (`src/harness/`) included from `src/coding_agent/` — if `WorkspaceFileSystem` changes its include surface, the skill loader breaks | Both `src/harness/` and `src/coding_agent/` are in the CMake private include path; compilation failures on `WorkspaceFileSystem` changes are caught at build time |
| Boot-time I/O for skill loading adds startup latency | Skills dirs are typically small; synchronous loading is acceptable. If it becomes a problem, async loading can be added later without changing the Skill/SkillLoader contracts |
| Symlink following could escape workspace containment | Pi's `resolveKind` pattern (canonicalPath → fileInfo on resolved target) is followed; workspace containment is already enforced by `WorkspaceFileSystem` |

---

## Sources & References

- **Roadmap:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T6 item 2)
- **Pi skills.ts:** `pi:packages/agent/src/harness/skills.ts`
- **Pi Skill type:** `pi:packages/agent/src/harness/types.ts`
- **Pi skills docs:** `pi:packages/coding-agent/docs/skills.md`
- **Agent Skills spec:** https://agentskills.io/specification
- **C++ ExecutionEnv:** `include/cch/harness/ExecutionEnv.hpp`
- **C++ RuntimeServices:** `src/coding_agent/runtime/RuntimeServices.hpp`
- **C++ TempWorkspace (test support):** `tests/support/TempWorkspace.hpp`
- **C++ CMakeLists.txt:** `CMakeLists.txt`
