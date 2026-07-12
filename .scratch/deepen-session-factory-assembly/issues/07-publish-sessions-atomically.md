# 07 — Publish sessions atomically after successful assembly

**What to build:** Persistent session state must become visible only after all fallible session prerequisites have succeeded. Failed new-session creation must leave no artifact, while failed resume must treat the existing session as read-only.

**Blocked by:** 01 — Unify session creation behind a private assembly plan; 06 — Make capability ownership explicit and retire the second assembly seam.

**Status:** ready-for-agent

- [ ] Provider, resource, tool, command, and capability validation complete before a new session becomes visible.
- [ ] Representative failures such as provider creation, duplicate tools, and invalid explicit resources leave no new session file.
- [ ] Failed resume leaves the original session byte-for-byte unchanged.
- [ ] Resume topology and active-path preparation reuse the existing session domain operations rather than duplicating traversal policy.
- [ ] Failures after factory-owned capabilities are created clean them up without touching host-owned capabilities.
- [ ] Tests prove observable visibility and preservation invariants without depending on a particular staging mechanism.
