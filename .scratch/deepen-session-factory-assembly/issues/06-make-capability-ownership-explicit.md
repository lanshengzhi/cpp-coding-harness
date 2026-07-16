# 06 — Make capability ownership explicit and retire the second assembly seam

Category: enhancement
**What to build:** Every capability adopted during session creation must carry explicit lifecycle ownership. Session close must clean up only session-owned environments, and runtime services must become a passive internal bundle rather than a second way to assemble sessions.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** implemented

- [x] Factory-created execution environments are session-owned and cleaned up exactly once.
- [x] Host-injected shared execution environments remain host-owned and are never cleaned up by session close.
- [x] Chat clients and custom tools transferred by unique ownership remain session-owned, and the public SDK contract states that ownership transfer.
- [x] Execution-environment ownership is represented explicitly rather than inferred from shared-reference counts.
- [x] The independent runtime-services configuration/factory interface is removed, leaving only the passive internal runtime-services value.
- [x] CLI and SDK adapters no longer construct runtime services or the session runtime directly.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
