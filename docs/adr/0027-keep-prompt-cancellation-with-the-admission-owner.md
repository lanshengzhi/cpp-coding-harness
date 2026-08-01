---
status: accepted
---

# Keep prompt cancellation with the admission owner

Each Agent Session prompt's cancellation source is created by the admission owner (`AgentSessionRuntime`) when the prompt is accepted, before the Agent coroutine starts, and is handed to the Agent through the private `agent::detail::AgentPromptAccess` seam. Session Abort and Session Close request stop on that admission-owned source rather than calling `Agent::abort()`, and `Agent::abort()` remains the cancellation entry only for hosts driving a bare `Agent` directly.

## Considered options

- Make the Agent the single cancellation owner and route Session Abort through `Agent::abort()`: rejected because the Agent's run-scoped stop source exists only once its prompt coroutine executes. A close or abort issued from the prompt preflight callback — after admission, before the Agent runs, as the RPC prompt acknowledgement path does — would be silently lost, breaking the pinned issue 41 contract that preflight close carries cancellation into the admitted run and still completes the ordinary `aborted` lifecycle.
- Make the caller-supplied stop source a public `Agent::prompt` parameter and delete the private access seam: rejected because it widens the public Agent execution contract with a parameter only session assembly needs; the narrow friend seam keeps that contract internal (ADR 0020's one-source-per-prompt rule is unchanged).
- Track a pre-start abort flag inside the Agent: rejected because a sticky idle abort would poison a later unrelated prompt and change `abort()`'s documented no-op-while-idle meaning.

## Consequences

- `AgentSessionRuntime` holds one `std::stop_source` per active prompt spanning admission, preflight, and the Agent run; copies share one stop state, so a stop requested in the admission window is observed when the run starts.
- `agent::detail::AgentPromptAccess` stays as the deliberate narrow seam for supplying that source; it is not a redundant forwarder, and architecture reviews should not re-propose collapsing it or unifying Session Abort onto `Agent::abort()`.
- The independent User Bash stop source is unaffected: a Session-owned User Shell execution is not an Agent run (ADR 0026).
