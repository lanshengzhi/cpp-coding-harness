# 07 — Publish sessions atomically after successful assembly

Category: enhancement
**What to build:** Persistent session state must become visible only after all fallible session prerequisites have succeeded. Failed new-session creation must leave no artifact, while failed resume must treat the existing session as read-only.

**Blocked by:** 01 — Unify session creation behind a private assembly plan; 06 — Make capability ownership explicit and retire the second assembly seam.

**Status:** implemented

- [x] Provider, resource, tool, command, and capability validation complete before a new session becomes visible.
- [x] Representative failures such as provider creation, duplicate tools, and invalid explicit resources leave no new session file.
- [x] Failed resume leaves the original session byte-for-byte unchanged.
- [x] Resume topology and active-path preparation reuse the existing session domain operations rather than duplicating traversal policy.
- [x] Failures after factory-owned capabilities are created clean them up without touching host-owned capabilities.
- [x] Tests prove observable visibility and preservation invariants without depending on a particular staging mechanism.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
