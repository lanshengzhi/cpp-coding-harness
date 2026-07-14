# 09 — Recover from subscriber delivery failure

**What to build:** Let a subscriber reject an event without corrupting or closing the session: the active prompt fails, current live state remains visible, and the host may continue prompting.

**Blocked by:** 08 — Persist completed messages incrementally.

**Status:** ready-for-agent

- [ ] Use PRD stories 5–6, 8, and 20–23 as the failure and ordering authority.
- [ ] Subscribers run in registration order; the first failure prevents later subscribers and persistence for that event.
- [ ] A message already added to live state remains there after subscriber failure.
- [ ] Prompt completion reports an explicit listener-delivery error, while AgentSession remains open and accepts a later prompt.
- [ ] Tests include a move-only subscriber that owns unique state and verify that no copyability regression occurs.
- [ ] Focused SDK and move-only architecture tests pass.
