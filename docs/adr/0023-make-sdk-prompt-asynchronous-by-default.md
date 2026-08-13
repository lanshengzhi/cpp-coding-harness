---
status: accepted
---

# Make SDK prompt asynchronous by default

The authoritative same-process SDK prompt operation is an awaitable coroutine, matching the asynchronous provider, hook, tool, cancellation, and Agent lifecycle beneath it. A separately named blocking facade may serve simple hosts, but it is not the core contract and must reject use from an execution context that would wait on itself.

> SDK clauses superseded by [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md) and [ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md): the same-process SDK, authoritative SDK `prompt()`, `prompt_blocking()`, SDK executor restrictions, and SDK-specific test obligations no longer exist. The underlying requirement that asynchronous work, cancellation, reentrancy, and Close must not be hidden behind a self-waiting blocking path remains authoritative and is now carried by the Runtime operation and lifecycle contract.

## Considered options

- Keep only blocking `prompt()`: rejected because it hides executor ownership, complicates cancellation/close, and can deadlock under callback reentrancy.
- Expose unrelated blocking and async implementations: rejected because lifecycle and error semantics could drift.
- Use one async implementation plus a blocking adapter: accepted because it preserves capability and simple-call ergonomics.

## Consequences

- `prompt()` returns `boost::asio::awaitable<std::expected<void, Error>>` and enforces one active prompt.
- Progress remains available through weak event subscriptions.
- `prompt_blocking()` delegates to the async path and documents executor/thread restrictions.
- Abort and two-phase close compose with suspended prompt operations.
- Tests cover async success/failure, reentry, cancellation, close from callbacks, and blocking-facade self-wait rejection.
