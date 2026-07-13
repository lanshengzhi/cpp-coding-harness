# 05 — Align command documentation and validate Phase 1 acceptance

**What to build:** Make user-facing documentation and the active parity roadmap accurately describe the implemented Phase 1 command set, then run the complete focused acceptance slice.

**Blocked by:** 03 — Implement `/help` and `/commands`; 04 — Implement `/exit` and text-frontend `/clear`.

**Status:** ready-for-agent

- [ ] Update README to list the actual built-in commands and their text/JSON/RPC mode behavior.
- [ ] Remove the false README claim that `/compact` is implemented.
- [ ] Preserve explicit wording that `/new` and `/resume` remain instructional placeholders.
- [ ] Update the T5 slash-command entry in `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` with the current pi built-in count and the Phase 1 completion state without claiming direct parity for CLI adaptations.
- [ ] Check PRD acceptance criteria against the final implementation and record any intentionally deferred item rather than silently broadening scope.
- [ ] Validate markdown headings, relative links, and command names.
- [ ] Run the focused coding-agent, CLI, SDK, and architecture test slices named by the preceding tickets.
- [ ] Record validation commands and results in this ticket under `## Comments` before marking it implemented.
