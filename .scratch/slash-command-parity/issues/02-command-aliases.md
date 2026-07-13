# 02 — Add registry-owned command aliases

**What to build:** Add alias resolution to `CommandRegistry` so `/commands` and `/exit` reuse canonical handlers without copying move-only handlers or duplicating command behavior.

**Blocked by:** 01 — Add command metadata and deterministic registry introspection.

**Status:** ready-for-agent

- [ ] Add `register_alias(alias, canonical_target)` returning an explicit project expected/error result.
- [ ] Reject empty alias names and alias names containing whitespace or a leading `/`; failed registration leaves the registry unchanged.
- [ ] Require the target to be an existing canonical command; reject missing targets and aliases targeting aliases.
- [ ] Reject alias collisions with canonical names or existing aliases; failed registration leaves the registry unchanged.
- [ ] Dispatch aliases through the canonical handler without copying the move-only handler.
- [ ] Include aliases in `list_commands()` as distinct sorted `CommandInfo` entries.
- [ ] Inherit the canonical description and argument hint and set `alias_for` to the canonical name.
- [ ] Ensure `find_command_info()` resolves canonical names and aliases without erasing alias identity.
- [ ] Add tests for alias-name validation, dispatch, metadata inheritance, collisions, missing targets, alias-chain rejection, deterministic listing, and no partial mutation.
- [ ] Run focused coding-agent and architecture tests.
