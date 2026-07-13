# 01 — Add command metadata and deterministic registry introspection

**What to build:** Deepen `CommandRegistry` so it owns passive command metadata, explicit registration failures, lookup, and deterministic listing alongside move-only handlers.

**Blocked by:** None — can start immediately.

**Status:** implemented

- [x] Add aggregate-friendly `CommandInfo` with `name`, `description`, `argument_hint`, and optional `alias_for` in `include/cch/coding_agent/CommandRegistry.hpp`.
- [x] Add canonical registration accepting name, description, argument hint, and `CommandHandler`.
- [x] Keep a convenience canonical-registration overload that supplies empty description and argument hint for existing callers.
- [x] Return an explicit project expected/error result from registration.
- [x] Reject empty names, names containing whitespace or a leading `/`, empty handlers, and duplicate canonical names; failed registration leaves the registry unchanged.
- [x] Add `list_commands() const`, initially returning canonical entries sorted lexicographically by name; Ticket 02 extends it with alias entries.
- [x] Add `find_command_info(name) const`.
- [x] Preserve move-only handler support and direct canonical dispatch.
- [x] Update all registration call sites to handle or deliberately propagate registration failures.
- [x] Add focused tests for metadata defaults, deterministic order, lookup, name validation, empty-handler rejection, duplicate canonical names, and no partial mutation.
- [x] Run focused coding-agent and architecture tests.

## Comments

Implemented in commit `26eb443`.

Completed validation:

- `cmake --build build -j2`
- `./build/cpp_harness_tests "[coding_agent][command_registry]"` — 7 tests passed
- `./build/cpp_harness_tests "[coding_agent]"` — 139 tests passed
- `./build/cpp_harness_tests "[architecture]"` — 17 tests passed
- `./build/cpp_harness_tests` — 544 tests passed
- `git diff --check`

Code review:

- Standards review found no documented-standard violations; it noted only a low-severity judgement call about repeated expected-error forwarding in built-in registration.
- Spec review found no missing, partial, extra, or incorrect Ticket 01 behavior.
