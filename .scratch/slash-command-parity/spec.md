# Spec: Phase 1 slash commands for the line-oriented CLI

**Feature slug:** `slash-command-parity`  
**Target repo:** `cpp-coding-harness`  
Category: enhancement
Status: implemented
**Date:** 2026-07-13

---

## 1. Summary

Before Phase 1, the README claimed the text REPL supported `/help`, `/clear`, `/compact`, and `/exit`, but the registry contained only `/session`, `/quit`, `/new`, and `/resume`; `/new` and `/resume` only printed restart instructions.

This slice corrects the highest-value documentation and interaction gap without introducing session replacement, runtime reconfiguration, or tree mutation. It adds four CLI-oriented commands:

- `/help [command]`
- `/commands` as an alias for `/help`
- `/exit` as an alias for `/quit`
- `/clear` as a text-frontend operation

The work also deepens `CommandRegistry`: command metadata, alias resolution, deterministic introspection, and collision handling live behind one registry interface rather than being reimplemented by command handlers or frontends.

The pi reference currently defines 22 built-in commands in `pi:packages/coding-agent/src/core/slash-commands.ts`. `/help`, `/commands`, `/clear`, and `/exit` are deliberate C++ line-CLI adaptations, not claims of direct pi command parity.

---

## 2. Goal

Make the documented Phase 1 slash commands truthful and testable while preserving these seams:

1. `CommandRegistry` owns canonical command registration, aliases, metadata, dispatch, and introspection.
2. Command handlers remain pure value-returning functions.
3. Terminal presentation remains in the text frontend.
4. JSON and RPC output remain structured; terminal escape bytes never appear in structured output.
5. Session replacement and in-session mutation remain outside this slice.

---

## 3. Non-goals

- Do not introduce `CommandAction` or add an action field to public `PromptResult`.
- Do not implement operational `/new` or `/resume`; their existing instructional behavior remains unchanged.
- Do not implement `/name`, `/export`, `/model`, or `/reload`.
- Do not implement `/fork`, `/clone`, `/tree`, or `/compact`.
- Do not add session switching, runtime replacement, model reconfiguration, resource reload, or public session-tree APIs.
- Do not implement TUI, autocomplete, extension discovery, auth management, clipboard, sharing, themes, or keybindings.
- Do not list prompt templates or `/skill:<name>` invocations as registry commands; registry help covers effective `CommandRegistry` entries only.
- Do not change the JSON/RPC record schema.

---

## 4. Pre-implementation baseline

The following was the baseline when this spec was written:

- `include/cch/coding_agent/CommandRegistry.hpp` stored only `name -> CommandHandler` in an unordered map.
- Duplicate registration was silently ignored by `unordered_map::emplace`.
- The registry could not list commands or describe them.
- `CommandContext` contained session facts but no passive command metadata snapshot.
- `/quit` used the existing `shutdown_requested` result field.
- `CommandContext` was constructed centrally in `AgentSessionRuntime`, not separately by CLI and RPC adapters.
- Text, JSON, and RPC paths all eventually called `AgentSession::prompt()`, except text-only frontend operations deliberately intercepted by the text adapter.

---

## 5. Command behavior

### 5.1 `/help [command]`

With no argument, list every effective registry name, including aliases, sorted lexicographically by name.

Example shape:

```text
Available commands:
  /clear                  Clear the terminal screen
  /commands               Alias for /help
  /exit                   Alias for /quit
  /help [command]         Show available commands or help for one command
  /new                    Show restart instructions for a new session
  /quit                   Quit the session
  /resume <session-id>    Show restart instructions for resuming a session
  /session                Show current session information
```

Requirements:

- Output order is deterministic.
- A command with no description is still listed.
- `/help name` and `/help /name` both address the same registry name.
- `/help <command>` shows the command name, description, usage, and canonical target when the name is an alias.
- More than one argument returns `Usage: /help [command]`.
- An unknown name returns `Unknown command: /<name>` and does not invoke the agent loop.

### 5.2 `/commands`

- Alias for `/help`.
- Dispatches through registry alias resolution; it does not own a duplicate handler.
- Appears in introspection as an alias of `help`.

### 5.3 `/exit`

- Alias for `/quit`.
- Uses the existing `/quit` handler and existing shutdown result semantics.
- Appears in introspection as an alias of `quit`.

### 5.4 `/clear`

- In interactive text REPL mode, exact `/clear` writes `\033[2J\033[H` through the text frontend, flushes output, and does not call `AgentSession::prompt()`.
- In one-shot text mode, exact `/clear` performs the same frontend operation and exits successfully.
- In JSON mode and in an RPC `prompt` request, `/clear` is consumed as `command_handled` with an empty message.
- `/clear` never calls `std::system`, a shell, or an execution environment.
- `/clear` with arguments is not a frontend clear operation; its registered no-op handler returns `Usage: /clear`.

---

## 6. Mode behavior matrix

| Command | Text REPL / one-shot | JSON mode | RPC prompt |
| --- | --- | --- | --- |
| `/help`, `/commands` | Display help text | `runtime_terminal` with `code: command_handled` and help text in the existing message field | `runtime_terminal` with `code: command_handled` and help text in the existing message field |
| `/quit`, `/exit` | Display shutdown text and exit successfully | Emit terminal result and exit successfully | Emit terminal result, then end the RPC loop successfully |
| `/clear` | Clear through the text frontend | `command_handled`, empty message, no ANSI | `command_handled`, empty message, no ANSI |

This matrix does not add fields or command types to the JSON/RPC protocols.

---

## 7. Registry interface

### 7.1 Passive metadata

Add an aggregate-friendly public value:

```cpp
struct CommandInfo {
    std::string name;
    std::string description;
    std::string argument_hint;
    std::optional<std::string> alias_for;
};
```

`name` never includes the leading `/`. Alias entries inherit the canonical command's description and argument hint.

### 7.2 Registration and collision rules

`CommandRegistry` provides:

- canonical registration with name, description, argument hint, and move-only handler;
- a convenience canonical-registration overload with empty description and hint for existing callers;
- `register_alias(alias, canonical_target)`;
- `list_commands() const`;
- `find_command_info(name) const`;
- existing dispatch behavior through canonical or alias names.

Registration returns an explicit project expected/error result. It must reject:

- an empty canonical or alias name;
- a canonical or alias name containing whitespace or a leading `/`;
- an empty canonical handler;
- duplicate canonical names;
- a canonical name colliding with an alias;
- an alias colliding with a canonical name or another alias;
- an alias whose target does not exist;
- an alias targeting another alias.

Failed registration does not change the registry. `list_commands()` returns canonical and alias entries sorted lexicographically by `name`.

### 7.3 Help access

Do not place a `CommandRegistry*` in `CommandContext`.

Add a passive `std::vector<CommandInfo>` snapshot to `CommandContext`. `AgentSessionRuntime` populates it from `command_registry_.list_commands()` at the central context-construction point before dispatch. `/help` reads only this snapshot.

This trades a small per-command copy for simple ownership, stable lifetimes, SDK safety, and a passive public contract.

---

## 8. Error and output rules

- Registry registration failures use structured project errors; no collision is silently ignored.
- Command usage and unknown-command responses are handled command results, not runtime failures.
- No Phase 1 command performs fallible session mutation.
- Structured modes must not receive ANSI escape bytes.
- Existing `shutdown_requested` behavior remains; replacing it is a separate design task.

---

## 9. Test strategy

### Unit tests

`tests/coding_agent/BuiltinCommandsTest.cpp` and focused registry tests cover:

- metadata registration and lookup;
- deterministic listing;
- duplicate and collision failures with no partial registration;
- alias dispatch and metadata inheritance;
- missing targets and alias-chain rejection;
- `/help`, `/help <name>`, `/help /<name>`, unknown names, and invalid arity;
- `/commands`, `/exit`, and `/clear` handler behavior.

### Integration tests

`tests/cli/CliSmokeTest.cpp` covers:

- text REPL `/help`, `/commands`, `/exit`, and `/clear`, including shutdown display text;
- one-shot text `/help` message output and `/clear`;
- JSON `/help`, `/exit`, and `/clear` terminal records, including the `/help` message field;
- RPC prompt `/help`, `/exit`, and `/clear` behavior, including the `/help` message field;
- RPC `/exit` emits its shutdown terminal record and does not process a subsequent input record;
- absence of ANSI bytes in JSON and RPC output.

### SDK and architecture tests

- Update SDK command-registration tests for the explicit registration result and metadata defaults.
- Run architecture tests because public coding-agent value contracts change.
- No live provider or network tests are required.

---

## 10. Acceptance criteria

- [x] `CommandRegistry` owns passive command metadata, aliases, dispatch, and deterministic introspection.
- [x] Invalid, duplicate, and colliding registrations fail explicitly without changing the registry.
- [x] `/help` lists all effective registry names in deterministic order.
- [x] `/help <command>` works for canonical names and aliases.
- [x] `/commands` dispatches as an alias of `/help`.
- [x] `/exit` dispatches as an alias of `/quit` and shuts down each supported frontend as specified.
- [x] `/clear` emits ANSI only from text frontend code.
- [x] JSON and RPC output contain no terminal escape bytes.
- [x] No `CommandAction` or `PromptResult::action` is introduced.
- [x] Existing `/new` and `/resume` behavior is unchanged.
- [x] README lists only the actually implemented command set and no longer claims `/compact` support.
- [x] Focused coding-agent, CLI, SDK, and architecture tests pass.

---

## 11. Deferred follow-up efforts

These require separate `/grill-with-docs -> /to-spec -> /to-tickets` flows:

1. **Session metadata and export:** `/name`, `/export`; resolve effective-name reconstruction, mutation ownership, export containment, symlink/overwrite policy, atomicity, and permissions.
2. **Runtime reconfiguration:** `/model`, `/reload`; design atomic client/loop/tool reconstruction, provider rollback, resource provenance, trust, diagnostics, and model-visible skill refresh.
3. **Session controller and tree operations:** `/new`, `/resume`, `/fork`, `/clone`, `/tree`, `/compact`; separate session replacement from in-file navigation and model-backed compaction.

---

## 12. References

- `pi:packages/coding-agent/src/core/slash-commands.ts`
- `include/cch/coding_agent/CommandRegistry.hpp`
- `src/coding_agent/CommandRegistry.cpp`
- `src/coding_agent/runtime/AgentSessionRuntime.cpp`
- `src/coding_agent/runtime/AsyncCliRuntime.cpp`
- `src/coding_agent/runtime/RpcMode.cpp`
- `include/cch/coding_agent/Sdk.hpp`
- `.scratch/pi-cpp-parity/map.md` for current parity decisions
- `.scratch/deepen-session-factory-assembly/spec.md` and `docs/adr/0001-centralize-session-assembly-policy.md` for the completed session-assembly boundary
