# 08 — Return diagnostics as values and leave presentation to adapters

Category: enhancement
**What to build:** Factory and runtime logic must report recoverable creation and prompt issues as values. Each adapter then chooses presentation appropriate to its contract, keeping embedded and machine-readable modes silent and protocol-clean.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** implemented

- [x] Session factory and runtime logic do not write diagnostics directly to standard output or standard error.
- [x] Recoverable creation, resource, skill, and template diagnostics are returned as values through every entry point that needs them.
- [x] SDK session creation and prompting remain silent while exposing diagnostics to the caller.
- [x] Text-mode CLI renders human-facing diagnostics through its adapter.
- [x] JSON and RPC modes keep standard output protocol-clean and preserve their documented diagnostic behavior.
- [x] Output-capture tests cover factory/runtime silence and adapter-owned presentation.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
