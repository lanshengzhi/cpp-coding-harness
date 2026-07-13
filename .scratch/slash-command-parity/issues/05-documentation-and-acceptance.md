# 05 — Align command documentation and validate Phase 1 acceptance

**What to build:** Make user-facing documentation and the active parity roadmap accurately describe the implemented Phase 1 command set, then run the complete focused acceptance slice.

**Blocked by:** 03 — Implement `/help` and `/commands`; 04 — Implement `/exit` and text-frontend `/clear`.

**Status:** implemented

- [x] Update README to list the actual built-in commands and their text/JSON/RPC mode behavior.
- [x] Remove the false README claim that `/compact` is implemented.
- [x] Preserve explicit wording that `/new` and `/resume` remain instructional placeholders.
- [x] Update the T5 slash-command entry in `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` with the current pi built-in count and the Phase 1 completion state without claiming direct parity for CLI adaptations.
- [x] Check PRD acceptance criteria against the final implementation and record any intentionally deferred item rather than silently broadening scope.
- [x] Validate markdown headings, relative links, and command names.
- [x] Run the focused coding-agent, CLI, SDK, and architecture test slices named by the preceding tickets.
- [x] Record validation commands and results in this ticket under `## Comments` before marking it implemented.

## Comments

Completed validation:

- `cmake --build build -j2` — passed
- `./build/cpp_harness_tests "[coding_agent][prompt]"` — 42 tests passed
- `./build/cpp_harness_tests "[cli][commands]"` — 6 tests passed
- `./build/cpp_harness_tests "[cli][json][commands]"` — 3 tests passed
- `./build/cpp_harness_tests "[cli][rpc][commands]"` — 3 tests passed
- `./build/cpp_harness_tests "[coding_agent]"` — 150 tests passed
- `./build/cpp_harness_tests "[cli]"` — 71 tests passed
- `./build/cpp_harness_tests "[sdk]"` — 48 tests passed
- `./build/cpp_harness_tests "[architecture]"` — 17 tests passed
- `./build/cpp_harness_tests` — 567 tests passed
- Inline Python documentation validator — headings and relative links valid; README command names match the 8 effective registry names; current pi reference count is 22; no forbidden `CommandAction` or `PromptResult::action` contract found
- `git diff --check` — passed

All PRD acceptance criteria were checked and satisfied. Direct pi slash-command parity beyond this Phase 1 line-CLI slice remains intentionally deferred, including operational `/new` and `/resume`, `/compact`, and the other pi built-ins listed in the roadmap. `/help`, `/commands`, `/clear`, and `/exit` remain explicitly classified as C++ line-CLI adaptations rather than direct pi parity.

Code review found two documentation consistency issues, both corrected before handoff: the active contract inventory still pointed to the old registry location and four-command baseline, and the implemented PRD described its pre-implementation baseline in present tense. Standards review found no remaining baseline code smells; spec review found no scope creep.
