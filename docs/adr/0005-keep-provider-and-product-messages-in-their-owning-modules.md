---
status: accepted
---

# Keep shared and session message values at their frozen module boundaries

The AI module owns the shared `cch::ai::MessageVariant` value used across provider, agent, and session paths. In addition to provider-ready system, user, assistant, and tool-result messages, that closed variant contains exactly the four pi extended runtime message forms already frozen by the architecture surface scan: bash execution, custom, branch summary, and compaction summary. The AI provider boundary converts those extended forms to provider-ready user messages before a request. The harness session module owns Session Entry values and serialization, while the coding-agent layer owns product orchestration and presentation rather than a second product-message variant.

This is a physical C++ module boundary, not a claim that every `cch::ai::MessageVariant` alternative is itself a Provider Message. It preserves the implemented shared value flow without allowing unrelated runtime or session-entry vocabulary to accumulate in the AI public surface.

## Considered options

- Move the four extended runtime forms into a second agent- or coding-agent-owned variant: rejected because the current agent history, provider conversion, and Session Store contract share one closed value across their physical seams.
- Allow new product or Session Entry alternatives to accumulate in `cch::ai::MessageVariant`: rejected because it would expand the lowest public message surface beyond the four forms the architecture tests deliberately lock.
- Keep the existing shared message variant, bound its extended alternatives, and keep Session Entry values in the harness session module: accepted because it matches the architecture enforced by the current public contracts and surface scan.

## Consequences

- `cch_ai` owns provider-ready messages and exactly four extended runtime message values in `cch::ai::MessageVariant`.
- `cch_agent` uses that shared variant for live history and lifecycle events; it does not define a second Agent Message union.
- Extended runtime messages are converted to provider-ready messages at the AI provider boundary.
- Session Entry values and serialization remain in `cch_harness`; coding-agent-specific orchestration and presentation remain in `cch_coding_agent_runtime`.
- Architecture tests must reject additional extended runtime structs in the bounded AI message section and keep unrelated product/session vocabulary out of the AI public surface.
