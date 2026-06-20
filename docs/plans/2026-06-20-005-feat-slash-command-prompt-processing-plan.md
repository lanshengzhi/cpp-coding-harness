---
title: "feat: Add slash-command and prompt-template processing seams"
type: feat
status: "completed"
date: "2026-06-20"
---

# feat: Add slash-command and prompt-template processing seams

## Summary

Add a prompt-processing pipeline between CLI/RPC entry points and the agent loop to intercept slash-commands and expand prompt templates. Implement a built-in command registry for session-lifecycle operations, a pure-function template expander with bash-style argument substitution, and REPL-level command detection. Template file discovery, extension command registration, skill-as-command expansion, and TUI autocomplete are deferred to T6/T7.

---

## Problem Frame

The C++ harness currently passes raw user input from CLI/REPL/RPC directly to the agent loop as a `user_text_message`. There is no interception point for `/command` syntax — users cannot initiate a new session, resume a previous session, query session state, or expand named prompt templates without restarting the binary. pi's architecture inserts a command-dispatch and template-expansion layer between user input and the agent session, drawing from three command sources (built-in, extensions, prompt templates). The C++ harness needs an equivalent processing seam before T6 (resources/extensions) and T7 (TUI) can build on it.

---

## Requirements

### Processing Seam

- **R1.** A `process_prompt()` function intercepts raw user input before it reaches the agent loop, returning whether a command consumed the input and the expanded text.
- **R2.** The seam is integrated into both `AgentSessionRunner::run_prompt()` (CLI/REPL path) and RPC mode's prompt handler at `src/coding_agent/runtime/RpcMode.cpp`.

### Built-in Commands

- **R3.** A `CommandRegistry` maps command names to handlers. Handlers receive the raw argument string and a command context with access to the session lifecycle.
- **R4.** Session-lifecycle built-in commands are implemented: `/new` (end current session, start new), `/resume <session-id>` (queue resume request), `/session` (print current session info), `/quit` (signal shutdown).
- **R5.** Unknown commands produce a clear error message without reaching the LLM.
- **R6.** The `!` prefix (shell passthrough) is detected but returns a deferred-not-implemented message — full bash execution is gated on T7 TUI availability.

### Prompt Template Expansion

- **R7.** A `PromptTemplate` struct carries name, description, and content as passive values.
- **R8.** `expand_prompt_template(raw_input, templates)` detects `/templateName args` patterns, parses bash-style arguments (single/double-quote splitting), and substitutes `$1`, `$2`, `$@`, `$ARGUMENTS`, `${N:-default}`, `${@:N}`, `${@:N:L}` into the template body. Non-matching input is returned unchanged.
- **R9.** Template expansion is a pure function with no filesystem, network, or provider dependencies — fully deterministically testable.
- **R10.** When template expansion produces text, the expanded result is passed to the agent loop as the user prompt (with original input preserved in session metadata).

### Quality Gate

- **R11.** Architecture tests continue to pass; new public headers are added to `PublicHeaderBoundaryTest`.
- **R12.** All new behavior has tests that do not require provider calls.

---

## Scope Boundaries

- Template file loading from `~/.cpp-harness/prompts/*.md` and `.cpp-harness/prompts/*.md` — deferred to T6 (resources/extensions)
- Extension command registration (`registerCommand` API) — deferred to T6
- Skill-as-command expansion (`/skill:name`) — deferred to T6
- TUI autocomplete for commands — deferred to T7
- Full pi built-in command catalog (25+ commands) — explicitly narrowed to 4 session-lifecycle commands; the remaining commands depend on T6/T7 infrastructure
- Bash `!` execution implementation — deferred to T7 (TUI bash tool integration)
- Project trust enforcement for template files — deferred to T6
- `expandPromptTemplates` flag to suppress expansion within command handlers — deferred; not needed until extension commands exist (T6)

### Deferred to Follow-Up Work

- Wire `!` prefix to `BashExecutionMessage` creation and bash tool execution — separate plan under T7
- Add the remaining pi built-in commands — separate plan after T6/T7 boundaries stabilize
- `InputEvent` / `BeforeAgentStartEvent` lifecycle events for extension interception — T6

---

## Context & Research

### Relevant Code and Patterns

- **RPC command dispatch:** `src/coding_agent/runtime/RpcMode.cpp` — string-keyed `if-else` chain dispatching `prompt`, `get_state`, `get_last_assistant_text`, `shutdown`. Same pattern applies to slash-command dispatch.
- **REPL loop:** `src/AsyncCliRuntime.cpp:227` — the single point where interactive user input enters the system. Slash-command interception must happen here.
- **CLI arg parsing:** `src/main.cpp` — CLI11-based, producing `CliConfig`. Existing `--resume`, `--mode`, `--provider` flags.
- **Agent session runner:** `src/coding_agent/runtime/AgentSessionRunner.hpp` — `run_prompt(prompt, sink)` is the entry point called from both CLI REPL and RPC mode.
- **Runtime services:** `src/coding_agent/runtime/RuntimeServices.cpp` — `make_runtime_services(RuntimeServicesConfig)` assembles provider registry, tools, execution env.
- **Session lifecycle:** `src/coding_agent/runtime/SessionLifecycle.cpp` — `resume_session()`, `create_session()`, already manages session metadata.
- **Agent event sink:** `include/cch/agent/AgentEvent.hpp` — `AgentEventSink` and `AgentLifecycleEvent` variants.

### External References

- **pi slash-commands:** `pi:packages/coding-agent/src/core/slash-commands.ts` — catalog of 25 built-in commands with descriptions and autocomplete metadata
- **pi prompt-templates:** `pi:packages/coding-agent/src/core/prompt-templates.ts` — `parseCommandArgs()`, `substituteArgs()`, `expandPromptTemplate()`, template discovery
- **pi agent-session:** `pi:packages/coding-agent/src/core/agent-session.ts` — command dispatch inside `prompt()`, `_tryExecuteExtensionCommand()`, `_expandSkillCommand()`
- **pi prompt-templates docs:** `pi:packages/coding-agent/docs/prompt-templates.md`
- **pi usage docs:** `pi:packages/coding-agent/docs/usage.md`

---

## Key Technical Decisions

- **Processing seam lives in the coding-agent runtime layer, not the agent loop.** pi places command dispatch in `AgentSession.prompt()`. The C++ equivalent is `AgentSessionRunner::run_prompt()`. Placing it here keeps the agent loop provider-agnostic and follows the architecture rule: user-visible command wiring belongs in the CLI/runtime layer.

- **Command registry uses a map of name → handler, not if-else chains.** Unlike the RPC mode (which has 4 fixed commands and uses if-else for simplicity), the slash-command system is expected to grow to 25+ built-in commands plus extension-registered commands (T6). A `std::unordered_map<std::string, CommandHandler>` registry scales better and mirrors the architecture's "capability across physical boundaries" rule.

- **Template expansion is a pure free function, not a class.** `expand_prompt_template(std::string_view input, const std::vector<PromptTemplate>&) → std::string` has no side effects and no dependencies on the filesystem, network, or agent state. This makes it trivially testable. The function signature mimics `pi:packages/agent/src/harness/prompt-templates.ts`.

- **`!` prefix is detected but returns a deferred message.** The REPL loop already sees lines starting with `!`. Rather than silently ignoring it or routing it to the agent loop as a regular prompt (which would confuse the model), the command dispatcher returns a clear "shell passthrough not yet implemented" message. This avoids a silent semantic change and makes the T7 integration point explicit.

- **New types live in `include/cch/coding_agent/PromptProcessing.hpp`, not in the AI or agent layer.** The AI layer (`include/cch/ai/`) is provider-facing messages and content. The agent layer (`include/cch/agent/`) is the loop orchestration. Prompt processing (commands, templates) is a coding-agent concern — following the existing `include/cch/coding_agent/Config.hpp` precedent.

---

## Open Questions

### Deferred to Implementation

- Exact `CommandContext` shape — how much session state (session id, workspace path, metadata) is passed to command handlers. Determined during U2 when handlers are implemented.
- Whether `/new` should prompt for confirmation in text mode — deferred; start with immediate action and add confirmation as a follow-up if user feedback warrants it.
- Exact `CommandDispatchResult` fields — deferred; the initial shape has `handled` (bool) and `response_text` (string for display), which may expand when T6 adds extension lifecycle hooks.
- `$ARGUMENTS` expansion scope — whether it includes the template name or just the arguments — deferred; follow pi's convention (just the arguments portion of the input).

---

## Implementation Units

### U1. PromptProcessingResult struct and process_prompt() seam

**Goal:** Define the processing result type and the `process_prompt()` function signature, then integrate it into the two input entry points (`AgentSessionRunner::run_prompt()` and RPC mode).

**Requirements:** R1, R2

**Dependencies:** None

**Files:**
- Create: `include/cch/coding_agent/PromptProcessing.hpp`
- Modify: `src/coding_agent/runtime/AgentSessionRunner.cpp`
- Modify: `src/coding_agent/runtime/RpcMode.cpp`
- Modify: `tests/architecture/PublicHeaderBoundaryTest.cpp`

**Approach:**
- Define `PromptProcessingResult` struct with `command_handled` (bool) and `display_text` (optional string — message to show user when a command consumed input) and `expanded_prompt` (string — text to send to agent loop, empty if command_handled)
- Define `PromptTemplate` struct: `name` (string), `description` (optional string), `content` (string)
- Declare `process_prompt(std::string_view raw_input, const std::vector<PromptTemplate>& templates) → PromptProcessingResult` — initially a no-op pass-through returning `{false, std::nullopt, std::string{raw_input}}`
- In `AgentSessionRunner::run_prompt()`: call `process_prompt()` before `loop_.continue_with()`. If `command_handled`, co_return with the display text without activating the agent loop.
- In RPC mode's prompt handler: same pattern — call `process_prompt()` before `runner.run_prompt()`.
- Template parameter is a `const std::vector<PromptTemplate>&` for now — empty vector means no template expansion (the no-op case).

**Patterns to follow:**
- `include/cch/coding_agent/Config.hpp` — same directory, same passive-value struct style
- `AgentSessionRunner::run_prompt()` existing coroutine pattern with `co_return PromptRunResult`

**Test scenarios:**
- **Happy path:** `process_prompt("hello", {})` returns `command_handled=false`, `expanded_prompt="hello"`
- **Happy path:** Agent session runner passes through normal prompts unchanged
- **Integration:** RPC mode's prompt handler passes through normal prompts unchanged
- **Edge case:** Empty input passes through without error

**Verification:**
- Existing CLI smoke tests and RPC tests continue to pass
- New header added to `PublicHeaderBoundaryTest`

---

### U2. Built-in command registry and session-lifecycle dispatch

**Goal:** Build a command registry that maps command names to handlers, implement four session-lifecycle built-in commands, and wire the registry into `process_prompt()`.

**Requirements:** R3, R4, R5, R6

**Dependencies:** U1 (process_prompt seam must exist)

**Files:**
- Modify: `include/cch/coding_agent/PromptProcessing.hpp` (add CommandRegistry, CommandContext, CommandHandler types)
- Create: `src/coding_agent/BuiltinCommands.cpp`
- Modify: `src/coding_agent/runtime/AgentSessionRunner.cpp` (populate registry, pass to process_prompt)
- Modify: `src/coding_agent/runtime/RpcMode.cpp` (populate registry for RPC mode)
- Create: `tests/coding_agent/BuiltinCommandsTest.cpp`

**Approach:**
- Define `CommandContext` struct: `session_id` (optional string), `workspace_path` (string), `agent_session_runner` reference or callback — minimal context needed by session-lifecycle commands
- Define `CommandHandler` as `std::move_only_function<CommandResult(CommandContext&, std::string_view args)>` — takes context and raw argument string, returns a result with `display_text` and optional `shutdown_requested` flag
- Define `CommandRegistry` as a class with `register_command(name, handler)` and `dispatch(name, args, context) → std::optional<CommandResult>`
- Implement four built-in commands:
  - `/new`: signals the session runner to end current session and create a new one. Since the agent loop is stateless between turns, this can return a special result that `AgentSessionRunner` uses to re-invoke `create_session()`.
  - `/resume <id>`: reads the session path from the session store and returns a "restart with --resume <path>" instruction (or, if session store provides ID→path lookup, signals for session swap). MVP: returns instruction text.
  - `/session`: prints current session ID, workspace, provider, model, and message count from `CommandContext`.
  - `/quit`: sets `shutdown_requested = true` on the result, causing `AgentSessionRunner` to cleanly shut down.
- Wire the registry into `process_prompt()`: if input starts with `/`, extract command name and args, dispatch, return `command_handled = true` with result text. If command not found, return error text ("Unknown command: /foo").
- Detect `!` prefix before slash-command routing: if first non-whitespace char is `!`, return "Shell passthrough (!) is not yet implemented."

**Patterns to follow:**
- RPC mode's string-keyed dispatch pattern, adapted to map-based dispatch
- `std::move_only_function` for handlers (architecture rule §3)
- `util::Error` for structured error returns

**Test scenarios:**
- **Happy path:** `dispatch("/session", "", context)` returns session info with session_id
- **Happy path:** `dispatch("/quit", "", context)` returns `shutdown_requested = true`
- **Happy path:** `dispatch("/new", "", context)` returns a new-session signal
- **Happy path:** `dispatch("/resume", "abc123", context)` returns resume instruction
- **Edge case:** `dispatch("/unknown", "", context)` returns "Unknown command" error
- **Edge case:** `/` with no command name returns error
- **Edge case:** `!echo hello` returns "not yet implemented" message
- **Edge case:** `!` with no arguments returns "not yet implemented" message
- **Integration:** `process_prompt("/session", templates, registry)` returns `command_handled=true` with session info text

**Verification:**
- All four commands return expected text or signals
- Unknown commands produce errors without reaching the agent loop
- `!` prefix is detected and deferred message returned
- Architecture tests pass; `std::move_only_function` is not `std::function`

---

### U3. Prompt template expansion (parseCommandArgs, substituteArgs, expandPromptTemplate)

**Goal:** Implement pure-function template expansion with bash-style argument parsing and substitution.

**Requirements:** R7, R8, R9, R10

**Dependencies:** U1 (process_prompt seam)

**Files:**
- Create: `src/coding_agent/PromptExpander.cpp`
- Create: `tests/coding_agent/PromptExpanderTest.cpp`
- Verify: `include/cch/coding_agent/PromptProcessing.hpp` (PromptTemplate struct defined in U1; U3 consumes it)

**Approach:**
- Implement `parse_command_args(std::string_view input) → std::vector<std::string>`:
  - Split on whitespace, respecting single-quote and double-quote boundaries
  - Handle escaped quotes within quoted strings (`\"` inside double quotes)
  - Empty input returns empty vector
  - Returned vector excludes the command/template name (first token) — only the arguments
- Implement `substitute_args(std::string_view template_body, const std::vector<std::string>& args) → std::string`:
  - `$1`, `$2`, ... `$9` → positional argument
  - `$@` → all arguments joined by spaces
  - `$ARGUMENTS` → all arguments joined by spaces (alias for `$@`)
  - `${N:-default}` → position N with default fallback
  - `${@:N}` → arguments from position N onward
  - `${@:N:L}` → L arguments starting from position N
  - Out-of-range positional references → empty string (or default if provided)
  - Single-pass substitution — no recursive expansion of substituted values
- Implement `expand_prompt_template(std::string_view input, const std::vector<PromptTemplate>& templates) → std::string`:
  - If input starts with `/`, extract the template name (first token after `/`)
  - Match against `templates` by `name`
  - If no match, return input unchanged
  - Extract args via `parse_command_args()`, expand via `substitute_args()`
  - If matched template has no args in template body, substitution is a no-op
- Wire `expand_prompt_template()` into `process_prompt()`: if input starts with `/` and no built-in command matches, try template expansion. If expansion produces different text, set `command_handled = true` and `expanded_prompt` to the expanded text.

**Patterns to follow:**
- `pi:packages/coding-agent/src/core/prompt-templates.ts` — the reference implementation for argument parsing and substitution logic
- Pure functions: no `this`, no member state, no I/O

**Test scenarios:**
- **Happy path:** `expand_prompt_template("/greet world", [{"greet", "", "Hello $1!"}])` → `"Hello world!"`
- **Happy path:** `$@` expands to all arguments joined by spaces
- **Happy path:** `$ARGUMENTS` is alias for `$@`
- **Happy path:** `${1:-there}` uses default when no argument provided
- **Happy path:** `${@:2}` skips first argument
- **Happy path:** `${@:2:1}` takes one argument starting from position 2
- **Happy path:** Non-matching `/command` passes through unchanged
- **Happy path:** Input without `/` prefix passes through unchanged
- **Edge case:** Quoted arguments: `/greet "hello world"` → `$1` = `hello world` (single token)
- **Edge case:** Single-quoted arguments with `$` inside: `'/greet $HOME'` → `$1` = `/greet $HOME` (literal)
- **Edge case:** `$9` beyond args → empty string
- **Edge case:** `${@:10}` from 2 args → empty string
- **Edge case:** Empty template list → input unchanged
- **Edge case:** Template with no argument placeholders → substitution is no-op
- **Integration:** `process_prompt("/greet world", templates, registry)` where no built-in matches → template expansion fires → `expanded_prompt = "Hello world!"`, `command_handled = false`

**Verification:**
- All arg parsing and substitution cases pass deterministically
- No filesystem or provider calls in any code path
- `PromptTemplate` struct is an aggregate

---

### U4. REPL interception point and CLI integration

**Goal:** Intercept `/command` and `!command` input in the REPL loop and the single-shot CLI path, routing them through `process_prompt()`.

**Requirements:** R1, R2 (integration verification)

**Dependencies:** U1, U2, U3

**Files:**
- Modify: `src/AsyncCliRuntime.cpp` (REPL loop interception)
- Modify: `src/AsyncCliRuntime.hpp` (maybe add command registry to runtime config)
- Modify: `src/main.cpp` (populate built-in command registry, pass to runtime)
- Modify: `tests/cli/CliSmokeTest.cpp`

**Approach:**
- In `AsyncCliRuntime::run_repl()`: after reading a line, check if it starts with `/` or `!`. If so, call `process_prompt()` with the registry and templates. If `command_handled`, print `display_text` (if present) and continue the REPL loop without calling `run_prompt()`. If `shutdown_requested`, break the REPL loop.
- In single-shot CLI mode: after parsing CLI args, if the prompt starts with `/` or `!`, call `process_prompt()` before `run_prompt()`. If `command_handled`, print `display_text` and exit cleanly without activating the agent loop.
- `AsyncCliRuntimeConfig` gains an optional `CommandRegistry` reference and `std::vector<PromptTemplate>` — both populated in `main.cpp` from built-in registration.
- The registry and templates are passed through `RuntimeServicesConfig` or directly to `AgentSessionRunner` via the existing configuration path.

**Patterns to follow:**
- Existing REPL loop at `AsyncCliRuntime.cpp:227`: `while (std::cout << "> " && std::getline(std::cin, line))`
- Existing `CliConfig` → `AsyncCliRuntimeConfig` mapping in `main.cpp`

**Test scenarios:**
- **Happy path:** Type `/session` in REPL → session info printed, prompt re-displayed, no agent loop activation
- **Happy path:** Type `/quit` in REPL → REPL exits cleanly
- **Happy path:** Type normal prompt in REPL → reaches agent loop unchanged
- **Happy path:** Type `/greet world` with registered template → expanded text sent to agent loop
- **Edge case:** Type `!echo hi` in REPL → "not yet implemented" printed, REPL continues
- **Edge case:** Type `/unknown` in REPL → "Unknown command" printed, REPL continues
- **Integration:** CLI smoke tests verify `/session` output format is stable

**Verification:**
- REPL does not exit on unknown commands
- `/quit` cleanly exits REPL
- Normal prompts are unaffected
- CLI smoke tests pass

---

### U5. Tests and documentation

**Goal:** Add comprehensive tests for all new behavior, update contract inventory and TODO document.

**Requirements:** R11, R12

**Dependencies:** U1, U2, U3, U4

**Files:**
- Modify: `tests/coding_agent/PromptExpanderTest.cpp` (created in U3; add edge case tests)
- Modify: `tests/coding_agent/BuiltinCommandsTest.cpp` (created in U2; add integration tests)
- Modify: `tests/cli/CliSmokeTest.cpp`
- Modify: `tests/architecture/PublicHeaderBoundaryTest.cpp` (verify new headers)
- Modify: `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- Modify: `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`

**Approach:**
- BuiltinCommandsTest: test each command handler in isolation, test registry dispatch, test unknown command error, test `!` detection
- PromptExpanderTest: test all argument parsing and substitution cases from U3 scenarios
- CliSmokeTest: add REPL-level tests for `/session` and `/quit` using the existing test harness pattern
- Update contract inventory: add rows for `PromptTemplate`, `SlashCommand`, `CommandRegistry`, `process_prompt()`
- Update TODO: mark T5 item 5 as complete, mark T5 section as done

**Test scenarios:**
All test scenarios from U2 and U3 are implemented here.

**Verification:**
- All new tests pass (no provider calls needed)
- Existing test suites continue to pass
- Contract inventory reflects the new types
- TODO reflects T5 completion

---

## System-Wide Impact

- **Interaction graph:** `process_prompt()` sits between the two input entry points (REPL loop in `AsyncCliRuntime.cpp` and RPC prompt handler in `RpcMode.cpp`) and `AgentSessionRunner::run_prompt()`. It is the single dispatch point for all user input. The agent loop is never activated for command-handled input.
- **Error propagation:** Command handler errors produce structured text messages, not agent errors. Template expansion failures (malformed input) return the original text unchanged rather than raising errors — templates are user conveniences, not security boundaries.
- **State lifecycle risks:** The command registry is populated once at startup (`main.cpp`) and is read-only during operation. No hot-reload or dynamic registration in this plan. Template lists are also read-only.
- **API surface parity:** `PromptTemplate` struct mirrors pi's shape. `parse_command_args` and `substitute_args` mirror pi's `parseCommandArgs` and `substituteArgs`. The `process_prompt()` function is the C++ equivalent of pi's `AgentSession.prompt()` dispatch logic.
- **Integration coverage:** REPL-level tests exercise the full chain: input → process_prompt → command dispatch or template expansion → output. Unit tests cover each function in isolation.
- **Unchanged invariants:** Agent loop interface, tool interface, execution environment, provider registry, session store, and CMake dependency direction are all untouched. The processing seam is additive — when no command/template matches, input flows through unchanged.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `/command` syntax conflicts with model-generated text that happens to start with `/` | This is a user-input interception layer, not a model-output filter. Model output never passes through `process_prompt()`. Zero risk of false positives. |
| `!` shell detection false-positives on model output containing `!` | Same as above — interception is on user input only. Zero risk. |
| Command registry grows unbounded in T6/T7 | Map-based dispatch scales to 100+ commands efficiently. Registry is populated at startup, not runtime. |
| Template argument parsing diverges from pi's bash-style parsing | Reference implementation is `pi:packages/coding-agent/src/core/prompt-templates.ts` — test cases derived directly from pi's behavior. |

---

## Sources & References

- **Roadmap:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T5 item 5)
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- **pi slash-commands:** `pi:packages/coding-agent/src/core/slash-commands.ts`
- **pi prompt-templates:** `pi:packages/coding-agent/src/core/prompt-templates.ts`
- **pi agent-session:** `pi:packages/coding-agent/src/core/agent-session.ts`
- **pi prompt-templates docs:** `pi:packages/coding-agent/docs/prompt-templates.md`
- Related code: `src/AsyncCliRuntime.cpp`, `src/coding_agent/runtime/AgentSessionRunner.cpp`, `src/coding_agent/runtime/RpcMode.cpp`, `src/ai/providers/OpenAIChatClient.cpp` (Overloaded visitor pattern)
