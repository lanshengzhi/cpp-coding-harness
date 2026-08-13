---
status: accepted
---

# Make cancellation a supported end-to-end capability

Cancellation is a Supported Capability propagated from each active prompt through the agent, provider, policy hooks, tools, and process execution. The public C++ contract uses `std::stop_token` or a narrow value that bridges to it and Boost.Asio cancellation; an idempotent abort produces pi's assistant `aborted` terminal message and complete lifecycle rather than a second error channel or abrupt object destruction.

> Cancellation-shape clause superseded by [ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md): Owner operations receive cancellation explicitly as `std::stop_token`; a separate public narrow cancellation value and exposed Boost.Asio bridge are no longer permitted. Private implementations may adapt that token to private I/O machinery. The one prompt-scoped cancellation source, idempotent Abort/Close requests, safe-point ownership, and normal `aborted` lifecycle remain authoritative.

## Considered options

- Keep cancellation deferred: rejected because safe session close, awaitable hooks, provider requests, and long-running tools already require one coherent stop contract.
- Add separate cancellation mechanisms per layer: rejected because adapters could disagree about terminal state and cleanup ownership.
- Use one prompt-scoped cancellation source end to end: accepted because stop ownership and observation remain explicit.

## Consequences

- Each active prompt has one cancellation source and propagated token.
- Abort and close requests are idempotent; abort keeps the session reusable, while close proceeds through its closing lifecycle.
- Provider, hook, tool, and process adapters either honor cancellation or explicitly document the safe points at which it becomes observable.
- Cancellation emits the normal message, turn, and agent terminal events with stop reason `aborted`.
- A single active prompt remains the supported concurrency model; this decision does not approve concurrent prompts.
