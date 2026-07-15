# 09 — Recover from subscriber delivery failure

**What to build:** Let a subscriber reject an event without corrupting or closing the session: the active prompt fails, current live state remains visible, and the host may continue prompting.

**Blocked by:** 08 — Persist completed messages incrementally.

**Status:** implemented

- [x] Use PRD stories 5–6, 8, and 20–23 as the failure and ordering authority.
- [x] Subscribers run in registration order; the first failure prevents later subscribers and persistence for that event.
- [x] A message already added to live state remains there after subscriber failure.
- [x] Prompt completion reports an explicit listener-delivery error, while AgentSession remains open and accepts a later prompt.
- [x] Tests include a move-only subscriber that owns unique state and verify that no copyability regression occurs.
- [x] Focused SDK and move-only architecture tests pass.

**Completion evidence:**
- `AgentSessionRuntime` now records subscriber rejection independently of the returned error text, so prompt completion consistently reports `event_sink_failed` while preserving the subscriber's message.
- The AgentSession seam test registers an ordered move-only subscriber with unique state and a later subscriber, then proves rejection short-circuits later delivery and persistence while retaining live history.
- The same test removes only the rejecting subscriber, continues prompting on the open session, and verifies later messages resume incremental persistence.
- The focused `[sdk]` and `[architecture]` slices and the full `ctest --preset system` suite pass.
