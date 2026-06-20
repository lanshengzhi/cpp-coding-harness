---
title: "feat: Make loaded skills model-visible and user-invocable"
type: feat
status: active
date: 2026-06-20
---

# feat: Make loaded skills model-visible and user-invocable

## Summary

Make skill objects (already loaded into `RuntimeServices` by the discovery/loading plan) visible to the model and invocable by users. Build `formatSkillsForPrompt()` to emit the `<available_skills>` XML block per the [Agent Skills standard](https://agentskills.io/integrate-skills), inject it into agent context via the existing `get_steering_messages` hook, and register `/skill:name [args]` as a command that expands to a `<skill>` XML block containing the skill's full instructions plus optional user-supplied arguments.

---

## Problem Frame

Skill discovery and loading is complete (plan `docs/plans/2026-06-20-006-feat-skill-file-discovery-plan.md`): `Skill` objects with validated metadata are stored in `RuntimeServices::skill_load_result`. But skills are invisible — no model-visible listing exists, no `/skill:name` command is registered. The model cannot discover available skills to decide when to load them via `read`, and users cannot explicitly invoke loaded skills. This gap blocks the T6 skill work from delivering its intended payoff: specialized capability packages that augment agent behavior.

---

## Requirements

- **R1.** The model receives an `<available_skills>` XML block in the agent context, listing name, description, and file location for each visible skill (`disableModelInvocation != true`). The format follows pi's exact shape: prose intro paragraph, wrapped XML, one `<skill>` element per entry with `<name>`, `<description>`, `<location>` children. XML-escape all values.
- **R2.** When empty skills are loaded, or all skills have `disableModelInvocation: true`, the `<available_skills>` block is absent (empty string). No diagnostic — not having skills is a normal state.
- **R3.** `/skill:name [args]` input expands inline (in `process_prompt`, before `CommandRegistry` dispatch) to a `<skill>` XML block containing the skill's full content (body after frontmatter), a reference-relative preamble, and optional user-supplied arguments appended after the closing `</skill>` tag. The expanded text is then passed to the agent loop as a regular user prompt.
- **R4.** `disableModelInvocation: true` skills are excluded from the `<available_skills>` block but remain invocable via `/skill:name`.
- **R5.** Unknown skill names in `/skill:unknown` print a diagnostic to stderr and the original input is passed through to the agent loop unchanged (matching pi's behavior — unknown `/skill:` is treated as a regular prompt).
- **R6.** Skill file read failures during `/skill:name` expansion print an error diagnostic to stderr and the original input is passed through to the agent loop unchanged.

---

## Scope Boundaries

- Full system prompt infrastructure (pi's `buildSystemPrompt` with tool descriptions, guidelines, project context, docs paths, date/cwd) — out of scope. This plan only adds the skills block; a full system prompt is a separate concern.
- Config-driven skill directories (`ConfigData` / `RuntimeServicesConfig` skill directory paths) — deferred to follow-up.
- `<available_skills>` injection on every turn vs. first-turn-only — this plan injects on every turn via `get_steering_messages`, which is simple and matches pi's effective behavior. First-turn-only optimization is deferred.

### Deferred to Follow-Up Work

- **Config-driven skill directories**: Extend `ConfigData` with user-configurable skill directory paths (separate plan).
- **Prompt template loading**: `pi:packages/agent/src/harness/prompt-templates.ts` (separate T6 item).
- **Package discovery/installation**: (separate T6 item).
- **Full system prompt**: pi's `buildSystemPrompt` with tool descriptions, guidelines, project context (separate plan, likely post-T6).

---

## Context & Research

### Relevant Code and Patterns

- **`Skill` and `SkillLoadResult`** (`include/cch/coding_agent/Skill.hpp`): Passive value types with `name`, `description`, `content`, `filePath`, `disableModelInvocation`. Already loaded into `RuntimeServices::skill_load_result`.
- **`CommandRegistry`** (`include/cch/coding_agent/PromptProcessing.hpp`): Maps command names to `CommandHandler` (move-only function). Built-in commands registered via `register_builtin_commands()`. `process_prompt()` dispatches before agent loop.
- **`AsyncAgentOptions`** (`include/cch/agent/AgentContext.hpp`): Contains hooks including `get_steering_messages` — a `std::move_only_function<Expected<vector<MessageVariant>>()>` that returns messages to prepend before each LLM call. Currently unused in CLI wiring.
- **`AgentSessionRunner`** (`src/coding_agent/runtime/AgentSessionRunner.hpp`): Owns the `AsyncAgentLoop` and processes prompts through `process_prompt` before the loop. Currently receives `PromptTemplate` and `CommandRegistry*` but not skills.
- **`AsyncCliRuntime.cpp`** (lines ~132–170): Constructs `RuntimeServices`, builds `AgentSessionRunner` with `AsyncAgentOptions{max_turns, model}` — no skills or hooks wired.
- **`MessageVariant`** (`include/cch/ai/Message.hpp`): Variant of `SystemMessage`, `UserMessage`, `AssistantMessage`, `ToolResultMessage`, `BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage`. `SystemMessage` exists as a contract type but is not currently used in the agent loop.
- **Pi `formatSkillsForPrompt()`** (`pi:packages/coding-agent/src/core/skills.ts`): Filters out `disableModelInvocation`, escapes XML, emits prose intro + `<available_skills>` block with `<name>`, `<description>`, `<location>` per skill.
- **Pi `formatSkillInvocation()`** (`pi:packages/agent/src/harness/skills.ts`): Wraps skill content in `<skill name="..." location="...">` XML block with dirname-relative reference note. Optionally appends `additionalInstructions`.
- **Pi `_expandSkillCommand()`** (`pi:packages/coding-agent/src/core/agent-session.ts`): Parses `/skill:name args`, finds skill by name, reads file, strips frontmatter, calls `formatSkillInvocation`. Returns original text if skill not found or on read error.

### External References

- [Agent Skills specification — Integrate Skills](https://agentskills.io/integrate-skills)
- [Pi skills documentation](pi:packages/coding-agent/docs/skills.md)
- Pi `formatSkillsForPrompt()`: `pi:packages/coding-agent/src/core/skills.ts`
- Pi `formatSkillInvocation()`: `pi:packages/agent/src/harness/skills.ts`
- Pi `_expandSkillCommand()`: `pi:packages/coding-agent/src/core/agent-session.ts` (line ~1168)
- C++ skill discovery plan: `docs/plans/2026-06-20-006-feat-skill-file-discovery-plan.md`
- C++ roadmap: `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T6 item 2)

---

## Key Technical Decisions

- **Steering messages on every turn, not SystemMessage on first turn**: The C++ harness has no system prompt infrastructure. Using the existing `get_steering_messages` hook to inject the `<available_skills>` block on every turn is the simplest approach — it uses tested infrastructure and matches pi's effective behavior (skills are always in context). The overhead is negligible (a few hundred bytes per turn). A `SystemMessage` approach would require new agent-loop semantics for first-turn-only injection; this can be optimized later if needed.

- **Skills block format matches pi exactly**: The `<available_skills>` XML format (prose intro + `<available_skills>` wrapper + per-skill `<name>`/`<description>`/`<location>` elements) follows pi's `formatSkillsForPrompt()` character-for-character. This ensures model behavior parity — models trained on pi sessions will recognize the same skill format.

- **`/skill:name` is inline expansion in `process_prompt`, not a CommandRegistry entry**: Pi expands `/skill:name` inline before command dispatch (in `_expandSkillCommand`), treating it as a prompt transformation. The C++ harness follows this pattern exactly. Inline expansion avoids the REPL loop gap (where `CommandRegistry` dispatch prints output but doesn't re-invoke the agent loop), naturally handles unknown skills (passthrough), and keeps `CommandRegistry` focused on session-lifecycle commands (`/help`, `/clear`, `/compact`, `/exit`).

- **`formatSkillsForPrompt` and `formatSkillInvocation` are free functions in `cch::coding_agent` namespace**: They live alongside `Skill` and `SkillLoader` in the `cch_coding_agent_runtime` CMake target. No new library target needed — the runtime consumer is the only caller.

- **Skill file re-read on `/skill:name`, not using cached `Skill::content`**: Pi reads the file fresh on each invocation via `readFileSync`, stripping frontmatter. The C++ loader already stores `Skill::content` (body after frontmatter), but re-reading via `WorkspaceFileSystem::readTextFile()` matches pi's behavior and ensures the latest file content is used if skills were reloaded. The `content` field remains available for potential future caching.

- **`disableModelInvocation` filtering in `formatSkillsForPrompt`**: Skills with `disableModelInvocation: true` are excluded from the `<available_skills>` block but their names are still recognized by `/skill:name` command lookup. This matches pi's behavior precisely.

---

## Open Questions

### Resolved During Planning

- **Steering messages vs. SystemMessage for skills block**: Steering messages — resolved above in Key Technical Decisions.
- **CommandRegistry vs. inline expansion for `/skill:name`**: Inline expansion — resolved above in Key Technical Decisions.
- **File re-read vs. cached content for `/skill:name`**: File re-read via WorkspaceFileSystem — resolved above.

### Deferred to Implementation

- **Exact error message wording for unknown skill / read failure**: The display text (`Unknown skill: <name>`, `Failed to read skill file: <message>`) is a presentation choice best finalized when reviewing actual CLI output.
- **`WorkspaceFileSystem` availability at command-dispatch time**: `RuntimeServices` already holds the env; whether to pass a `WorkspaceFileSystem*` through `CommandContext` or construct it on-demand in the command handler depends on the exact call site plumbing.
- **Skill content size budget**: Pi doesn't enforce a limit on individual skill content size for `/skill:name` expansion. The C++ harness can match this initially; a budget can be added later if models show truncation issues with large skills.

---

## Implementation Units

### U1. `formatSkillsForPrompt()` — agent context XML block

**Goal:** Produce the `<available_skills>` XML block string from a `std::vector<Skill>`.

**Requirements:** R1, R2, R4

**Dependencies:** None (uses existing `Skill` type from `Skill.hpp`)

**Files:**
- Create: `include/cch/coding_agent/SkillFormatting.hpp`
- Create: `src/coding_agent/SkillFormatting.cpp`
- Test: `tests/coding_agent/SkillFormattingTest.cpp` (new)

**Approach:**
- `formatSkillsForPrompt(skills) -> std::string` — free function in `cch::coding_agent` namespace.
- Filter out `disableModelInvocation == true` skills.
- Guard: if no visible skills remain, return empty string (no output, no prose intro).
- Output shape:
  ```
  \n\nThe following skills provide specialized instructions for specific tasks.
  Use the read tool to load a skill's file when the task matches its description.
  When a skill file references a relative path, resolve it against the skill directory (parent of SKILL.md / dirname of the path) and use that absolute path in tool commands.
  
  <available_skills>
    <skill>
      <name>escaped-name</name>
      <description>escaped-description</description>
      <location>escaped-filePath</location>
    </skill>
  </available_skills>
  ```
- XML-escape: `&` → `&amp;`, `<` → `&lt;`, `>` → `&gt;`, `"` → `&quot;`, `'` → `&apos;`. Private helper `escapeXml()`.
- No dynamic memory beyond the returned string — simple string building.

**Patterns to follow:**
- Pi `formatSkillsForPrompt()` in `pi:packages/coding-agent/src/core/skills.ts` (lines ~export function formatSkillsForPrompt).
- Existing C++ `Skill` type in `include/cch/coding_agent/Skill.hpp`.

**Test scenarios:**
- Happy path: Three skills, all visible → XML block with all three, XML-escaped names/descriptions/locations.
- Happy path: One skill → XML block with single `<skill>` entry.
- Edge case: Empty skills vector → returns empty string (no prose intro, no XML).
- Edge case: All skills have `disableModelInvocation: true` → returns empty string.
- Edge case: Mixed visible and disabled → only visible skills in output.
- Edge case: Skill with `&`, `<`, `>`, `"`, `'` in name/description/location → properly XML-escaped.
- Edge case: Multi-line description (description with embedded newlines) → preserved verbatim but XML-escaped.
- Edge case: Long description (>1024 chars, validated-as-warning) → included in full.

**Verification:**
- Output matches pi's `formatSkillsForPrompt()` shape for equivalent inputs.
- Empty/disabled-only cases produce empty string (not an empty XML block).
- XML escaping handles all five special characters.
- Function is callable as a free function with no side effects.

---

### U2. `formatSkillInvocation()` — per-skill invocation XML block

**Goal:** Produce the `<skill>` XML block string for `/skill:name` command expansion.

**Requirements:** R3

**Dependencies:** U1 (same file, same namespace, shared `escapeXml` helper)

**Files:**
- Modify: `include/cch/coding_agent/SkillFormatting.hpp` (add declaration)
- Modify: `src/coding_agent/SkillFormatting.cpp` (add implementation)
- Extend: `tests/coding_agent/SkillFormattingTest.cpp`

**Approach:**
- `formatSkillInvocation(skill, additional_instructions) -> std::string` — free function.
- `additional_instructions` is an optional `std::string_view` (empty = absent).
- Output shape:
  ```
  <skill name="escaped-name" location="escaped-filePath">
  References are relative to <dirname of filePath>.
  
  <full skill content (body after frontmatter)>
  </skill>
  ```
- When `additional_instructions` is non-empty: append `\n\n<additional_instructions>` after the closing `</skill>`.
- Dirname extraction: use `std::filesystem::path(filePath).parent_path().string()`.
- Skill content is passed from the caller (the inline expansion handler will re-read the file).

**Patterns to follow:**
- Pi `formatSkillInvocation()` in `pi:packages/agent/src/harness/skills.ts`.
- Existing free-function pattern in `SkillLoader`.

**Test scenarios:**
- Happy path: Skill with name, filePath, content → correctly formatted `<skill>` block with dirname.
- Happy path: Skill with `additional_instructions` → block followed by `\n\n<instructions>`.
- Edge case: Empty content → block with empty body between tags.
- Edge case: Content with XML special characters → content is NOT escaped (raw body, `<skill>` is the protocol wrapper, not XML data).
- Edge case: filePath at filesystem root (`/skill.md`) → dirname is `/`.
- Edge case: `additional_instructions` with leading/trailing whitespace → preserved verbatim.

**Verification:**
- Output matches pi's `formatSkillInvocation()` shape.
- Dirname extraction works for absolute paths.
- Empty and non-empty `additional_instructions` handled correctly.

---

### U3. Pass skills from `RuntimeServices` to `AgentSessionRunner`

**Goal:** Wire the skills loaded at startup through to the `AgentSessionRunner` so they can be used for model context injection and command registration.

**Requirements:** R1, R3 (enabling dependency)

**Dependencies:** U1 (types must exist)

**Files:**
- Modify: `src/coding_agent/runtime/AgentSessionRunner.hpp` (add `std::vector<Skill> skills` field, extend constructor)
- Modify: `src/coding_agent/runtime/AgentSessionRunner.cpp` (store skills, pass to command registration)
- Modify: `src/AsyncCliRuntime.cpp` (pass `services->skill_load_result.skills` to runner constructor)
- Modify: `tests/cli/CliSmokeTest.cpp` (verify no crash with empty skills)

**Approach:**
- Add `std::vector<Skill> skills_` field to `AgentSessionRunner`.
- Extend constructor: `AgentSessionRunner(client, registry, options, templates, command_registry, skills)` — skills parameter with default `{}`.
- In `AsyncCliRuntime.cpp`, after constructing `RuntimeServices`, pass `std::move(services->skill_load_result.skills)` to the `AgentSessionRunner` constructor.
- `AgentSessionRunner` holds skills by value for the session lifetime.

**Patterns to follow:**
- Existing `AgentSessionRunner` constructor extension pattern (previously added `templates` and `command_registry` parameters).
- Move-semantics for `std::vector<Skill>` — `Skill` is an aggregate with movable strings.

**Test scenarios:**
- Integration: Construct `AgentSessionRunner` with empty skills → no crash.
- Integration: Construct `AgentSessionRunner` with one skill → `skills_` field populated.
- Smoke: CLI startup with no skills directory → no crash, empty skills list.
- Smoke: CLI startup with a valid skill directory → skills passed to runner.

**Verification:**
- `AgentSessionRunner` owns the skill list for its lifetime.
- Existing tests (CliSmokeTest, REPL tests) continue to pass.
- No observable behavior change yet (U4 and U5 add behavior).

---

### U4. Inject `<available_skills>` into agent context

**Goal:** Wire the `get_steering_messages` hook to emit the `<available_skills>` XML block on every turn, making skills visible to the model.

**Requirements:** R1, R2, R4

**Dependencies:** U1, U3

**Files:**
- Modify: `src/coding_agent/runtime/AgentSessionRunner.hpp` (declare private method `build_steering_messages`)
- Modify: `src/coding_agent/runtime/AgentSessionRunner.cpp` (implement `build_steering_messages`, set `get_steering_messages` on `AsyncAgentOptions`)
- Modify: `src/AsyncCliRuntime.cpp` (pass empty `get_steering_messages` default — the runner sets its own)
- Test: `tests/coding_agent/SkillIntegrationTest.cpp` (new)

**Approach:**
- In `AgentSessionRunner` constructor, after storing skills:
  1. Call `formatSkillsForPrompt(skills_)` once to produce the static skills block.
  2. Set `options.get_steering_messages` to a lambda that returns a single `UserMessage` containing the skills block.
     - If the skills block is empty (no visible skills), return an empty vector (no overhead).
  3. The existing `AsyncAgentLoop` calls `get_steering_messages` at the start of each turn and pushes the returned messages into the context.
- `AsyncAgentOptions` is move-only — hooks must be set before constructing `AsyncAgentLoop` (already the case).
- The skills block is computed once at construction time and captured by value in the lambda.

**Patterns to follow:**
- Existing `get_steering_messages` hook pattern in `AgentLoop.cpp` (lines ~333–354).
- `std::move_only_function` for hook callbacks.

**Test scenarios:**
- Integration: Runner with one visible skill → `get_steering_messages` returns a user message containing the skills XML block.
- Integration: Runner with no visible skills (empty skills list) → `get_steering_messages` returns empty vector.
- Integration: Runner with all skills `disableModelInvocation: true` → `get_steering_messages` returns empty vector.
- End-to-end: Run a prompt through the runner with one skill present → model receives the skills block in context (observe via fake provider message capture).
- Edge case: Multiple prompts (multiple turns) → skills block present on every turn (repeatable, no single-use state leak).

**Verification:**
- Skills block is visible in the context messages sent to the chat client.
- Empty skills case produces no extra context overhead.
- Hook is move-only (no copyable callback regression).

---

### U5. Expand `/skill:name` inline in `process_prompt`

**Goal:** Expand `/skill:name [args]` to a `<skill>` XML block inline in `process_prompt`, before `CommandRegistry` dispatch, so the expanded content reaches the agent loop as a regular user prompt.

**Requirements:** R3, R4, R5, R6

**Dependencies:** U2, U3

**Files:**
- Modify: `include/cch/coding_agent/PromptProcessing.hpp` (add `expand_skill_command()` declaration)
- Modify: `src/coding_agent/PromptProcessing.cpp` (add `expand_skill_command()`, call from `process_prompt`)
- Test: `tests/coding_agent/SkillIntegrationTest.cpp` (new — inline expansion tests)

**Approach:**
- Add `expand_skill_command(input, skills, fs) -> std::string` free function in `PromptProcessing.cpp`.
  1. If input doesn't start with `/skill:`, return input unchanged.
  2. Parse skill name from input: text between `/skill:` and first space (or end of string). Args: everything after the skill name.
  3. If name is empty (bare `/skill:`): return input unchanged (passthrough).
  4. Look up skill by name in the skills list.
  5. If not found: print `[skill:warn] unknown skill: <name>` to stderr, return input unchanged (passthrough, matching pi).
  6. Read `SKILL.md` via `WorkspaceFileSystem::readTextFile(skill.filePath)`.
  7. If read fails: print `[skill:error] failed to read <path>: <message>` to stderr, return input unchanged.
  8. Parse frontmatter (reuse `parseFrontmatter` from `SkillFrontmatterParser`), extract body.
  9. Call `formatSkillInvocation(skill, body, args)` and return the expanded `<skill>` XML block string.
- In `process_prompt()`, call `expand_skill_command(raw_input, skills, fs)` before checking for slash-commands. If the result differs from `raw_input` (skill was expanded), set `expanded_prompt` to the result and return immediately — skip command dispatch and template expansion for this input.
- `process_prompt` signature extended with `const std::vector<Skill>& skills` and `const WorkspaceFileSystem& fs` parameters, defaulting to empty skills and a no-op filesystem for backward compatibility.
- `AgentSessionRunner` passes `skills_` and a `WorkspaceFileSystem` reference to `process_prompt`.

**Why inline expansion, not CommandRegistry**: The REPL loop in `AsyncCliRuntime.cpp` (lines 252–260) intercepts slash-commands, dispatches through `process_prompt`, prints `display_text`, and `continue`s back to the REPL prompt — it never calls `run_prompt` with the expanded result. Inline expansion in `process_prompt` bypasses this gap: the expanded text becomes `expanded_prompt`, which `AgentSessionRunner::run_prompt` feeds directly into the agent loop. This also matches pi's `_expandSkillCommand` pattern exactly.

**WorkspaceFileSystem availability**: The `WorkspaceFileSystem` can be constructed from the workspace path stored in `AgentSessionRunner` (via `RuntimeServices` metadata). The exact plumbing (store a `WorkspaceFileSystem` member, or construct on-demand per-invocation) is deferred to implementation.

**Patterns to follow:**
- Pi `_expandSkillCommand()` in `pi:packages/coding-agent/src/core/agent-session.ts` (line ~1168).
- Existing `process_prompt()` parameter extension pattern (previously extended with templates and `CommandRegistry`).
- Existing stderr diagnostic format: `[skill:warn] <code>: <message>` from skill loading diagnostics.

**Test scenarios:**
- Happy path: `/skill:my-skill` with valid skill loaded → expanded to `<skill name="my-skill" ...>` XML block with full content.
- Happy path: `/skill:my-skill extra args here` → expanded `<skill>` block followed by `\n\nextra args here`.
- Happy path: `/skill:my-skill` with `disableModelInvocation: true` → works (disabled only affects system prompt visibility, not explicit invocation).
- Edge case: `/skill:unknown-skill` → stderr diagnostic, input returned unchanged (passthrough to agent loop).
- Edge case: `/skill:` with no name → input returned unchanged (passthrough).
- Edge case: Input not starting with `/skill:` → returned unchanged (fast path, no skill lookup).
- Edge case: Input starts with `/skill` but not `/skill:` (e.g., `/skills`) → returned unchanged.
- Error path: Skill file deleted between loading and invocation → stderr diagnostic, input returned unchanged.
- Error path: Skill content has no frontmatter (just body) → body used as-is (empty frontmatter is valid).

**Verification:**
- `/skill:name` expands to the correct `<skill>` XML block matching pi's `formatSkillInvocation` output.
- Unknown skills and read failures produce stderr diagnostics without crashing.
- Input without `/skill:` prefix passes through `expand_skill_command` unchanged (fast path).
- `disableModelInvocation` skills are invocable but absent from agent context (U4 already tested).

---

## System-Wide Impact

- **Interaction graph:** Skills flow through `RuntimeServices` → `AgentSessionRunner` → `AsyncAgentOptions.get_steering_messages` (agent context injection) and inline `process_prompt` expansion (user invocation). No changes to `AgentLoop`, `ChatClient`, or `CommandRegistry` contracts.
- **Error propagation:** Skill file read failures in `/skill:name` produce display messages; they don't abort the session or the agent loop. Malformed skills were already caught at load time.
- **State lifecycle risks:** Skills are loaded once at startup and held by value in `AgentSessionRunner`. No dynamic reload mid-session — consistent with current pi behavior.
- **API surface parity:** No changes to public headers except the new `SkillFormatting.hpp` (additive). Existing `runtime` headers get new fields on constructor/config structs (additive).
- **Integration coverage:** End-to-end test: load a skill file on disk, start CLI, verify model receives `<available_skills>` in fake provider request, verify `/skill:name` command expands correctly.
- **Unchanged invariants:** `AgentLoop` hooks contract unchanged (`get_steering_messages` already exists). `CommandRegistry` unchanged — skills are inline-expanded in `process_prompt` before command dispatch, not registered as commands. No tool schema changes.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `<available_skills>` block per-turn overhead biases token usage | Block is static, roughly 150-500 bytes per visible skill, XML-escaped. Typical overhead for 3-5 skills is under 3KB — negligible against typical context windows. First-turn-only optimization can be added later if needed. |
| Command handler writes `WorkspaceFileSystem` — synchronous I/O in command dispatch path | `readTextFile()` is synchronous and the command handler runs before the coroutine agent loop. I/O is on a small file (<100KB typical). No blocking concern. |
| `/skill:name` command name collisions with future extension commands | Skill commands use `skill:<name>` prefix; built-in commands use bare names (`/help`, `/clear`). Extension commands in the future would use a different prefix or separate registry. No collision risk. |

---

## Sources & References

- **Roadmap:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T6 item 2)
- **Discovery plan:** `docs/plans/2026-06-20-006-feat-skill-file-discovery-plan.md`
- **Pi `formatSkillsForPrompt()`:** `pi:packages/coding-agent/src/core/skills.ts`
- **Pi `formatSkillInvocation()`:** `pi:packages/agent/src/harness/skills.ts`
- **Pi `_expandSkillCommand()`:** `pi:packages/coding-agent/src/core/agent-session.ts`
- **Pi skills docs:** `pi:packages/coding-agent/docs/skills.md`
- **Agent Skills spec (Integrate):** https://agentskills.io/integrate-skills
- **C++ Skill type:** `include/cch/coding_agent/Skill.hpp`
- **C++ CommandRegistry:** `include/cch/coding_agent/PromptProcessing.hpp`
- **C++ AgentContext hooks:** `include/cch/agent/AgentContext.hpp`
- **C++ AgentSessionRunner:** `src/coding_agent/runtime/AgentSessionRunner.hpp`
