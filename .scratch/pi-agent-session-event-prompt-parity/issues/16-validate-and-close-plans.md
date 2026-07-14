# 16 — Validate the parity slice and close verified plans

**What to build:** Produce final evidence that the event/prompt parity slice and the already-implemented SessionFactory assembly satisfy their contracts before changing any plan status.

**Blocked by:** 15 — Synchronize domain, routing, and parity documentation.

**Status:** ready-for-agent

- [ ] Use PRD stories 48–52, the parity definition of done, and every SessionFactory completion criterion as the validation authority.
- [ ] Run the focused agent, tool-executor, SDK, prompt, CLI, session, JSON-event, RPC, and architecture slices and record the commands and outcomes.
- [ ] Build the system preset and finish with `ctest --preset system`; any failure is resolved or recorded as a blocker rather than waived.
- [ ] Mark the SessionFactory plan complete only if its focused checks and full suite pass and every completion criterion has observable evidence.
- [ ] Confirm removed event alternatives, PromptResult models, per-prompt sinks, session-owned CommandRegistry, SDK commands, schema metadata, and terminal records cannot return unnoticed.
- [ ] Record that live-provider/network tests were intentionally skipped because fake providers and local sessions satisfy this PRD's validation scope.
