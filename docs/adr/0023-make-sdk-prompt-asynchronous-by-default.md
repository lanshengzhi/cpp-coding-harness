---
status: accepted
---

# Make SDK prompt asynchronous by default

The authoritative same-process SDK prompt operation is an awaitable coroutine, matching the asynchronous provider, hook, tool, cancellation, and Agent lifecycle beneath it. A separately named blocking facade may serve simple hosts, but it is not the core contract and must reject use from an execution context that would wait on itself.

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
