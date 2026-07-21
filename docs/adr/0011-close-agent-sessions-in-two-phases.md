---
status: accepted
---

# Close agent sessions in two phases

`AgentSession::close()` is an idempotent close request, not permission to destroy objects that are still executing. A busy session enters a closing state, rejects new prompts and subscriptions, and retains its loop, sinks, and capabilities until the active operation and callback stack have quiesced; calling close from an event subscriber is therefore safe and non-blocking.

## Considered options

- Immediately tear down on close: rejected because a subscriber can destroy itself and the coroutine that is currently invoking it, causing undefined behavior.
- Reject close while busy: rejected because a void/idempotent close and RAII cleanup should remain safe from any documented callback context.
- Block until idle: rejected for reentrant callback calls because waiting on the current operation would deadlock.
- Request closure and defer teardown: accepted because it preserves lifetime safety without requiring JavaScript object semantics.

## Consequences

- Closing rejects new prompts and subscriptions but does not invalidate active frames.
- When cancellation is supported, close requests it; until then, the active prompt reaches a safe terminal outcome before teardown.
- Final resource cleanup runs exactly once after quiescence.
- Destruction cannot leave active operations holding dangling runtime references.
- Tests cover close from a subscriber, repeated close, busy close, post-close rejection, and cleanup ordering.
