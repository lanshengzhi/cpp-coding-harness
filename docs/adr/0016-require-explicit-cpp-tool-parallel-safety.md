---
status: accepted
---

# Require explicit C++ tool parallel safety

Tool scheduling follows pi's parallel default at the loop level (`BoundedParallelToolExecution` with no explicit cap is the default run policy, #355 / ADR 0034), but the C++-specific safety mechanism of this ADR remains: tool adapters declare their per-tool execution mode through `concurrency()`, defaulting to `Exclusive` (pi `executionMode: "sequential"`), and a batch containing a call to such a tool runs through the sequential path. C++ custom tools can share mutable state whose unsynchronized concurrent access is undefined behavior, so compile-time/adapter-level opt-in provides a caller-safety benefit that outweighs identical default scheduling.

## Considered options

- Run every batch in parallel like pi: rejected because a generic C++ capability interface cannot infer thread safety and would make ordinary custom tools unsafe by default.
- Always execute sequentially: rejected because independent tools can safely overlap and pi's completion semantics remain valuable.
- Default to exclusive with bounded explicit opt-in: accepted because safety is the default without removing concurrency.

## Consequences

- The run-policy default is bounded parallel with no explicit cap, matching pi's `toolExecution` `"parallel"` default (ADR 0034 / #355); an explicit `SequentialToolExecution` or a cap of one executes through the sequential path.
- Tool adapters default to `Exclusive` (pi `executionMode: "sequential"`); a batch containing a call to an exclusive tool serializes the whole batch, exactly like pi's `hasSequentialToolCall` check in `executeToolCalls`.
- Parallel-safe adapters explicitly opt in, and the execution policy enforces a finite concurrency bound where one is configured.
- Transcript results retain source order and lifecycle completion events retain actual completion order where pi specifies those semantics.
- Hooks and event sinks remain serialized unless their public contracts explicitly acquire concurrent-call guarantees.
- Tests include shared-state tools, mixed exclusive/parallel-safe batches, concurrency bounds, and deterministic transcript ordering.
