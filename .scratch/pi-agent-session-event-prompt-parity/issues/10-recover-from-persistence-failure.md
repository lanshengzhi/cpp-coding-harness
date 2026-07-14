# 10 — Recover from incremental persistence failure

**What to build:** Preserve useful in-process conversation state when durable append fails, while making restart behavior honestly reflect only data that reached storage.

**Blocked by:** 08 — Persist completed messages incrementally.

**Status:** ready-for-agent

- [ ] Use PRD stories 6–7, 23, and 25–26 as the live-versus-durable failure authority.
- [ ] An injected append failure fails the active prompt with an explicit persistence error but does not roll back live history or close AgentSession.
- [ ] A later prompt and later persistence attempt can succeed without a poisoned-state or automatic-retry protocol.
- [ ] Reopening reconstructs exactly the durable prefix after failure, including honest behavior if a resumed tree append spans more than one physical write.
- [ ] Any failure injection remains private and narrow; no broad public session-store interface, transaction entry, or repair mode is introduced.
- [ ] Focused SDK, session lifecycle, reopen, and `[harness][session]` tests pass.
