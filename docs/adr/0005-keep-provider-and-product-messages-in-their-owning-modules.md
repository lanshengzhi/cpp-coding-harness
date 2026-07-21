---
status: accepted
---

# Keep provider and product messages in their owning modules

The AI module owns only Provider Messages that a chat provider can directly consume or produce. The agent module owns Agent Message extensibility and the conversion seam to Provider Messages, while coding-agent product values such as bash execution, custom, branch-summary, and compaction-summary messages remain in the coding-agent layer. This follows pi's semantic ownership while using C++ variants at each layer; placing every alternative in one bottom-layer variant is convenient for implementation but couples AI-only consumers to session and product vocabulary.

## Considered options

- Keep one universal `cch::ai::MessageVariant`: rejected because closed variants make every upper-layer message a dependency of the lowest layer and invert ownership.
- Put all extended messages in the agent module: rejected because coding-agent product messages are not general agent-loop concepts.
- Preserve separate ownership and convert at the agent-to-provider boundary: accepted because it keeps each public contract cohesive without sacrificing type-safe C++ values.

## Consequences

- `cch_ai` contains only provider-ready user, assistant, and tool-result messages plus their content values.
- `cch_agent` defines the agent-facing message contract and an explicit conversion seam.
- Coding-agent-specific messages and their session serialization remain above the agent and AI packages.
- Architecture tests must reject product/session message vocabulary in the AI public surface.
