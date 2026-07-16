# 09 — Lock the session-assembly seam and complete integration validation

Category: enhancement
**What to build:** Protect the completed architecture from drifting back toward duplicated adapter assembly, document the actual CLI/SDK behavior, and verify the refactor across all affected seams.

**Blocked by:** 02 — Unify provider resolution and session metadata precedence; 04 — Preserve resource provenance and consistent failure semantics; 05 — Expose only enabled tools to providers; 06 — Make capability ownership explicit and retire the second assembly seam; 07 — Publish sessions atomically after successful assembly; 08 — Return diagnostics as values and leave presentation to adapters.

**Status:** implemented

- [x] Architecture checks establish that the session factory is the sole runtime assembly seam and adapters do not directly construct runtime services or the session runtime.
- [x] Guards preserve passive public contracts, private implementation types, dependency direction, and the prohibition on exposing implementation headers publicly.
- [x] Architecture tests protect ownership boundaries without freezing private helper names or stage ordering through brittle source-text assertions.
- [x] Agent routing and user-facing documentation describe the unified factory, external trust authority, ownership rules, and intentional CLI/SDK differences.
- [x] Focused SDK, CLI, session, provider/resource, and architecture tests pass.
- [x] The complete system test preset passes without live-provider validation.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
