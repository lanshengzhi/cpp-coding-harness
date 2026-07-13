# 03 — Implement `/help` and `/commands`

**What to build:** Register metadata for the existing built-ins, add a passive registry snapshot to `CommandContext`, and implement deterministic command help without giving handlers a registry pointer.

**Blocked by:** 01 — Add command metadata and deterministic registry introspection; 02 — Add registry-owned command aliases.

**Status:** ready-for-agent

- [ ] Add `std::vector<CommandInfo> available_commands` to `CommandContext`; do not add `CommandRegistry*` or another mutable capability.
- [ ] Populate the snapshot centrally in `AgentSessionRuntime` from `command_registry_.list_commands()` before command dispatch.
- [ ] Register descriptions and argument hints for `/session`, `/quit`, `/new`, and `/resume`; describe `/new` and `/resume` as restart instructions rather than completed session operations, and preserve the current `/resume <session-id>` placeholder vocabulary.
- [ ] Implement canonical `/help [command]` using only the passive snapshot.
- [ ] With no argument, list all effective registry names in lexicographic order, including aliases and commands with empty descriptions.
- [ ] Accept `/help name` and `/help /name`.
- [ ] Show name, description, usage, and canonical target for detailed alias help.
- [ ] Return `Usage: /help [command]` for more than one argument.
- [ ] Return `Unknown command: /<name>` for an unknown detailed-help target without invoking the agent loop.
- [ ] Register `/commands` as an alias of `/help`.
- [ ] Add handler and pipeline tests plus the PRD mode tests; explicitly assert one-shot text `/help` output and the existing JSON/RPC terminal message field for `/help`.
- [ ] Run focused coding-agent, CLI, SDK, and architecture tests.
