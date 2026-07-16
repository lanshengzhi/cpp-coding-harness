# 02 — Add registry-owned command aliases

Category: enhancement
**What to build:** Add alias resolution to `CommandRegistry` so `/commands` and `/exit` reuse canonical handlers without copying move-only handlers or duplicating command behavior.

**Blocked by:** 01 — Add command metadata and deterministic registry introspection.

**Status:** implemented

- [x] Add `register_alias(alias, canonical_target)` returning an explicit project expected/error result.
- [x] Reject empty alias names and alias names containing whitespace or a leading `/`; failed registration leaves the registry unchanged.
- [x] Require the target to be an existing canonical command; reject missing targets and aliases targeting aliases.
- [x] Reject alias collisions with canonical names or existing aliases; failed registration leaves the registry unchanged.
- [x] Dispatch aliases through the canonical handler without copying the move-only handler.
- [x] Include aliases in `list_commands()` as distinct sorted `CommandInfo` entries.
- [x] Inherit the canonical description and argument hint and set `alias_for` to the canonical name.
- [x] Ensure `find_command_info()` resolves canonical names and aliases without erasing alias identity.
- [x] Add tests for alias-name validation, dispatch, metadata inheritance, collisions, missing targets, alias-chain rejection, deterministic listing, and no partial mutation.
- [x] Run focused coding-agent and architecture tests.

## Comments

Implemented in commit `06cf22a`.

Completed validation:

- `cmake --build build -j2`
- `./build/cpp_harness_tests "[coding_agent][command_registry]"` — 12 tests passed
- `./build/cpp_harness_tests "[coding_agent]"` — 144 tests passed
- `./build/cpp_harness_tests "[architecture]"` — 17 tests passed
- `./build/cpp_harness_tests` — 549 tests passed
- `git diff --check`

Code review:

- Standards review found no documented-standard violations or material Fowler smells.
- Spec review found no missing, partial, extra, or incorrect Ticket 02 behavior.
