---
status: accepted
---

# Make agent policy hooks awaitable

Agent policy hooks support asynchronous completion and are awaited in the pi-aligned lifecycle order. The C++ contract uses move-only coroutine callables returning `boost::asio::awaitable<std::expected<...>>`; synchronous policies remain ergonomic through adapters that produce an immediately ready awaitable rather than by narrowing the core capability.

## Considered options

- Keep hooks synchronous: rejected because policies that consult storage, permissions, or external services would have to block an executor thread or escape the lifecycle.
- Offer unrelated synchronous and asynchronous hook paths: rejected because duplicate paths can diverge in ordering and failure semantics.
- Use one awaitable contract with synchronous adapters: accepted because it preserves capability while keeping simple callers concise.

## Consequences

- Before/after tool hooks, context conversion/transformation, prepare-next-turn, stop-after-turn, and equivalent policy seams can suspend safely.
- The agent awaits hooks in deterministic lifecycle order and applies the established per-call or agent-global failure boundary.
- Hook callables remain move-only and may own unique state.
- Ordinary event subscribers remain weak observers rather than awaitable policy middleware.
- Tests cover suspending hooks, executor resumption, ordering, cancellation, and sync-adapter behavior.
