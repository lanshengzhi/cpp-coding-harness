---
status: accepted
---

# Bound agent input queues explicitly

Steering and follow-up queues deliberately add configurable item-count and byte-size limits beyond pi's queue semantics. Long-lived embedded C++ processes benefit from deterministic memory bounds; the divergence is limited to admission, while queue modes, ordering, and consumption follow pi. Capacity rejection is reported by the enqueue operation and never silently drops input or aborts an unrelated active run.

## Considered options

- Use unbounded queues like pi: rejected because host-controlled input could grow process memory without an explicit resource policy.
- Keep fixed hidden constants: rejected because callers could not reason about or adapt capacity.
- Drop oldest/newest entries automatically: rejected because conversation control input must not disappear silently.
- Expose configurable safe bounds and explicit rejection: accepted because memory behavior and failure ownership are deterministic.

## Consequences

- Agent configuration publishes count and byte limits with documented defaults.
- `steer` and `followUp` return a capacity error before mutating the queue.
- Rejection does not stop the current prompt or modify existing queued messages.
- Queue consumption modes and order remain pi-compatible.
- Tests cover exact boundaries, multibyte accounting, repeated rejection, and continued active-run behavior.
