---
status: accepted
---

# Let the agent module own the stateful Agent

The agent module owns a stateful Agent capability rather than exposing a one-shot loop as its primary public abstraction. The Agent owns live Agent Message history, Model and thinking state, tools, one active run, cancellation, subscriptions, and steering/follow-up queues; the coding-agent Agent Session composes it with persistence, project resources, trust, commands, and SDK presentation.

> Application-surface terminology refined by [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md) and [ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md): “SDK presentation” no longer exists, and “public Agent” below means the repository-internal `cch_agent_core` Owner Interface. Agent state ownership and Agent Session's durable/product-boundary responsibilities remain authoritative.

## Considered options

- Keep a stateless public loop and rebuild state in each runtime: rejected because state transitions and queue/cancellation ownership become duplicated above their natural module.
- Put all state in Agent Session: rejected because general agent consumers would have to depend on coding-agent persistence and product vocabulary.
- Own state in Agent and keep the loop private: accepted because it matches pi's module responsibility while allowing C++ PImpl and ownership semantics.

## Consequences

- The coroutine loop becomes a private Agent execution mechanism.
- Agent state is exposed through passive snapshots rather than mutable public implementation objects.
- Agent enforces one active run and owns abort, steering, follow-up, and model/tool state transitions.
- Agent Session remains the durable/product boundary and does not duplicate the agent state machine.
- The public Agent may use PImpl and move-only ownership; this decision does not promise ABI stability or concurrent prompts.
