# 03 — Unify queued-message lifecycle events

**What to build:** Make internally queued steering and follow-up messages indistinguishable from ordinary messages at the event seam, while preserving their existing delivery order.

**Blocked by:** 02 — Complete the no-tool message lifecycle.

**Status:** ready-for-agent

- [ ] Use PRD stories 16 and 19 and the current pi pending-message loop as the behavioral authority.
- [ ] Every delivered queued message emits ordinary `message_start` and `message_end` carrying the message value.
- [ ] No queue-origin-specific lifecycle event is advertised or emitted.
- [ ] Single-message, multi-message, and cross-turn queue ordering are covered through the public agent-loop surface.
- [ ] Focused `[agent][async]` queue tests pass, with public queue APIs remaining out of scope.
