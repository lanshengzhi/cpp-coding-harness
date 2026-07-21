---
status: accepted
---

# Require explicit C++ tool parallel safety

Tool execution deliberately diverges from pi's default parallel scheduling: tools are exclusive by default and enter bounded parallel execution only when their adapter explicitly declares parallel safety. C++ custom tools can share mutable state whose unsynchronized concurrent access is undefined behavior, so compile-time/adapter-level opt-in provides a caller-safety benefit that outweighs identical default scheduling.

## Considered options

- Run every batch in parallel like pi: rejected because a generic C++ capability interface cannot infer thread safety and would make ordinary custom tools unsafe by default.
- Always execute sequentially: rejected because independent tools can safely overlap and pi's completion semantics remain valuable.
- Default to exclusive with bounded explicit opt-in: accepted because safety is the default without removing concurrency.

## Consequences

- Tool adapters default to exclusive scheduling.
- Parallel-safe adapters explicitly opt in, and the execution policy enforces a finite concurrency bound.
- Transcript results retain source order and lifecycle completion events retain actual completion order where pi specifies those semantics.
- Hooks and event sinks remain serialized unless their public contracts explicitly acquire concurrent-call guarantees.
- Tests include shared-state tools, mixed exclusive/parallel-safe batches, concurrency bounds, and deterministic transcript ordering.
