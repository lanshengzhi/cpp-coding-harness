---
status: accepted
---

# Follow pi agent-turn lifecycle order

The supported agent loop follows pi's observable Agent Turn state transitions: after `turn_end`, stop-after-turn and the queued steering/follow-up decisions run first, and prepare-next-turn runs only when the loop will start another assistant response; an assistant `error` or `aborted` terminal completes its message and turn lifecycle and then ends without consuming another queue item. C++ may use typed update values to express the hooks, but cannot reorder their effects.

## Considered options

- Preserve the current C++ order because it is already tested: rejected because existing tests can stabilize accidental drift and no ownership or type-safety benefit depends on the order.
- Let each frontend choose hook ordering: rejected because event and queue semantics belong to the agent-loop contract.
- Adopt pi's lifecycle and express it idiomatically in C++: accepted because consumers can reason from one authoritative state machine.

## Consequences

- The order is `turn_end`, stop-after-turn, steering, then follow-up, and — only when the loop will start another assistant response — prepare-next-turn, a steering re-poll (only when the earlier poll returned nothing), and `turn_start` before the next response, subject to pi's terminal and tool-termination rules. The run's final turn invokes prepare-next-turn zero times; a final threshold crossing is handled by the host's post-run compaction dispatch, not the between-turn hook.
- The agent exposes an equivalent stop-after-turn seam.
- Prepare-next-turn can replace/update context with pi-equivalent effects rather than an append-only approximation.
- `agent_end.messages` contains messages produced by the current invocation, not the entire pre-existing history.
- Lifecycle tests cover error, aborted, tool termination, steering, follow-up, context replacement, continuation from prior history, and the zero-invocation final turn.

## Addendum: prepare-next-turn runs only when another assistant response follows (Issue #745)

The Consequences order above originally recorded `turn_end`, prepare-next-turn,
stop-after-turn, steering, then follow-up, matching pi 0.85.1's agent-loop source
reading at adoption time. #745 re-inspected both implementations: upstream pi
invokes `prepareNextTurn` only at the top of a continuing inner-loop iteration
(under `if (lastCompletedTurn)`), so it never runs when the loop is about to
exit, while the C++ loop awaited it after every `TurnEndEvent` including the
run's final turn. The correction is a pi-parity fix of the previously recorded
order, recorded here rather than as a new decision: stop-after-turn moves to
immediately after the turn (observing the pre-prepare context), and
prepare-next-turn moves after the continuation decision with a steering re-poll
(only when the earlier poll returned nothing, preserving one-at-a-time
delivery). A final turn that crosses the compaction threshold therefore
reaches the host's post-run dispatch (`overflow` for usage beyond the window,
`threshold` without overflow) instead of emitting a between-turn
`threshold` compaction before `agent_end`.

## Addendum: finishTurn replaces stop-after-turn at pi-v1.3.0 (Issue #1107)

pi-v1.3.0 no longer exposes `shouldStopAfterTurn`; `AgentOptions.finishTurn`
takes its place and runs after the turn's assistant and tool-result messages,
immediately before `turn_end`. It returns `{ action: "continue" }` or
`{ action: "end" }` (or nothing). `end` stops the run without polling either
input queue and without invoking prepare-next-turn; `continue` guarantees one
next provider request, which a natural tool-result, steering or follow-up
request satisfies without an extra one. An error or aborted assistant still
calls the hook before `turn_end`, but its decision cannot extend the hard exit.

`AgentOptions.prepareRequest` also belongs to this lifecycle: it runs after the
pending messages have been appended and emitted, immediately before every
provider request including the first, and it never polls an input queue. Its
returned context, model and thinking level replace the runtime values for that
request and later requests in the run.

This supersedes the "stop-after-turn" seam name and the claim that the stop
check runs after `turn_end`; the steering, follow-up, prepare-next-turn and
final-turn rules above are unchanged.
