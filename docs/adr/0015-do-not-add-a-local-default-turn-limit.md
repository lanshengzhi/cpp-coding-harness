---
status: accepted
---

# Do not add a local default turn limit

The agent loop follows pi's stopping policy without a C++-specific default turn cap. The current implicit eight-turn limit and its provider-error outcome are removed: a run stops through pi-equivalent terminal conditions, stop-after-turn policy, termination decisions, or cancellation, and any future product limit is added only when pi exposes the corresponding policy or as a separately approved explicit extension.

> Package terminology refined by [ADR 0039](0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md): the `cch_agent` name below now denotes Agent behavior owned by `cch_agent_core`; it is not a separate Capability Owner Package. The no-default-turn-limit decision remains authoritative.

## Considered options

- Keep eight turns as a coding-agent safety default: rejected because the decision is to follow pi strictly rather than introduce a local product policy.
- Make the core unlimited but retain a silent CLI default: rejected because it preserves the same observable drift at another layer.
- Follow pi's stopping seams without a local cap: accepted because it keeps one authoritative lifecycle policy.

## Consequences

- `cch_agent` has no implicit `max_turns` termination.
- Turn exhaustion is no longer reported as a provider error.
- Explicit stopping uses the pi-aligned stop-after-turn and cancellation contracts.
- A future budget or turn-limit extension requires an explicit product decision, distinct terminal vocabulary, and documentation rather than a hidden default.
