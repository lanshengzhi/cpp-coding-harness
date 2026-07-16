# 04 — Preserve resource provenance and consistent failure semantics

Category: enhancement
**What to build:** Session creation must keep caller-authorized resources distinct from auto-discovered project resources so trust, precedence, failures, and diagnostics reflect where each resource came from.

**Blocked by:** 03 — Enforce external trust-store authority for SDK sessions.

**Status:** implemented

- [x] Host-provided and explicitly requested resources are treated as caller-authorized and are not project-trust gated.
- [x] Auto-discovered project resources load only when permitted by the selected trust authority.
- [x] Caller-authorized resources take precedence when duplicate resources are resolved.
- [x] Failure to load an explicit resource aborts session creation.
- [x] Invalid auto-discovered resources are skipped with bounded, stable diagnostics rather than aborting a resource-free session.
- [x] CLI discovery defaults and SDK opt-in discovery remain intentionally different and are covered by equivalent policy tests.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
