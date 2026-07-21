---
status: accepted
---

# Follow pi agent-turn lifecycle order

The supported agent loop follows pi's observable Agent Turn state transitions: after `turn_end`, prepare-next-turn runs before graceful stopping and queued steering/follow-up decisions; an assistant `error` or `aborted` terminal completes its message and turn lifecycle and then ends without consuming another queue item. C++ may use typed update values to express the hooks, but cannot reorder their effects.

## Considered options

- Preserve the current C++ order because it is already tested: rejected because existing tests can stabilize accidental drift and no ownership or type-safety benefit depends on the order.
- Let each frontend choose hook ordering: rejected because event and queue semantics belong to the agent-loop contract.
- Adopt pi's lifecycle and express it idiomatically in C++: accepted because consumers can reason from one authoritative state machine.

## Consequences

- The order is `turn_end`, prepare-next-turn, stop-after-turn, steering, then follow-up, subject to pi's terminal and tool-termination rules.
- The agent exposes an equivalent stop-after-turn seam.
- Prepare-next-turn can replace/update context with pi-equivalent effects rather than an append-only approximation.
- `agent_end.messages` contains messages produced by the current invocation, not the entire pre-existing history.
- Lifecycle tests cover error, aborted, tool termination, steering, follow-up, context replacement, and continuation from prior history.
