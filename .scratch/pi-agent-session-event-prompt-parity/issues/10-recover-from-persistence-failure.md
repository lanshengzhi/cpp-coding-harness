# 10 — Recover from incremental persistence failure

Category: enhancement
**What to build:** Preserve useful in-process conversation state when durable append fails, while making restart behavior honestly reflect only data that reached storage.

**Blocked by:** 08 — Persist completed messages incrementally.

**Status:** implemented

- [x] Use spec stories 6–7, 23, and 25–26 as the live-versus-durable failure authority.
- [x] An injected append failure fails the active prompt with an explicit persistence error but does not roll back live history or close AgentSession.
- [x] A later prompt and later persistence attempt can succeed without a poisoned-state or automatic-retry protocol.
- [x] Reopening reconstructs exactly the durable prefix after failure, including honest behavior if a resumed tree append spans more than one physical write.
- [x] Any failure injection remains private and narrow; no broad public session-store interface, transaction entry, or repair mode is introduced.
- [x] Focused SDK, session lifecycle, reopen, and `[harness][session]` tests pass.

**Completion evidence:**
- Added a private `SessionJournal` test hook that fails one selected physical append without changing the public session-store seam.
- Added AgentSession tests proving a persistence error retains live history, leaves the session open, feeds that history to a later prompt, and never automatically retries the failed entry.
- A resumed-tree append now advances its in-process parent after the durable message write, and reopen treats a durable topology entry after the latest leaf marker as the honest resume point when the marker write fails.
- Focused `[sdk]`, `[coding-agent][runtime][session]`, and `[harness][session]` slices pass.
