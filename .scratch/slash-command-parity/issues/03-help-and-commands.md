# 03 — Implement `/help` and `/commands`

Category: enhancement
**What to build:** Register metadata for the existing built-ins, add a passive registry snapshot to `CommandContext`, and implement deterministic command help without giving handlers a registry pointer.

**Blocked by:** 01 — Add command metadata and deterministic registry introspection; 02 — Add registry-owned command aliases.

**Status:** implemented

- [x] Add `std::vector<CommandInfo> available_commands` to `CommandContext`; do not add `CommandRegistry*` or another mutable capability.
- [x] Populate the snapshot centrally in `AgentSessionRuntime` from `command_registry_.list_commands()` before command dispatch.
- [x] Register descriptions and argument hints for `/session`, `/quit`, `/new`, and `/resume`; describe `/new` and `/resume` as restart instructions rather than completed session operations, and preserve the current `/resume <session-id>` placeholder vocabulary.
- [x] Implement canonical `/help [command]` using only the passive snapshot.
- [x] With no argument, list all effective registry names in lexicographic order, including aliases and commands with empty descriptions.
- [x] Accept `/help name` and `/help /name`.
- [x] Show name, description, usage, and canonical target for detailed alias help.
- [x] Return `Usage: /help [command]` for more than one argument.
- [x] Return `Unknown command: /<name>` for an unknown detailed-help target without invoking the agent loop.
- [x] Register `/commands` as an alias of `/help`.
- [x] Add handler and pipeline tests plus the spec mode tests; explicitly assert one-shot text `/help` output and the existing JSON/RPC terminal message field for `/help`.
- [x] Run focused coding-agent, CLI, SDK, and architecture tests.

## Comments

Implemented in commit `37a0528`.

Completed validation:

- `cmake --build build -j2`
- `./build/cpp_harness_tests "[coding_agent][prompt]"` — 41 tests passed
- `./build/cpp_harness_tests "[commands]"` — 5 tests passed
- `./build/cpp_harness_tests "[coding_agent]"` — 149 tests passed
- `./build/cpp_harness_tests "[cli]"` — 63 tests passed
- `./build/cpp_harness_tests "[sdk]"` — 48 tests passed
- `./build/cpp_harness_tests "[architecture]"` — 17 tests passed
- `./build/cpp_harness_tests` — 559 tests passed
- `git diff --check`

Code review:

- Standards review found no code or architecture violations after command-result presentation was consolidated in the text frontend. README command-list alignment remains intentionally deferred to Ticket 05.
- Spec review found the passive snapshot, deterministic help, alias dispatch, detailed lookup, and structured-mode message behavior correct. It suggested an explicit unknown-target no-model test; that CLI regression test was added before final validation.
