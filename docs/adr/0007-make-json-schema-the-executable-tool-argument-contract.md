---
status: accepted
---

# Make JSON Schema the executable tool-argument contract

A tool's parameter schema is an executable Tool Argument Contract, not only provider metadata. The public contract stores lossless JSON Schema as the project's passive `JsonValue`; the agent validates every tool call against it before policy hooks or execution, matching pi semantics while keeping schema visitors, validators, serialization, and convenience builders out of domain-facing headers.

## Considered options

- Continue with a closed recursive C++ schema struct: rejected because each new JSON Schema keyword expands a generic public type and still risks silently narrowing provider contracts.
- Trust providers to validate arguments: rejected because malformed or non-conforming calls can reach hooks and tool implementations.
- Let every tool parse and validate independently: rejected because hooks would still receive unvalidated values and error behavior would drift across adapters.

## Consequences

- `Tool::parameters` can represent arbitrary JSON Schema without loss.
- Validation occurs before `beforeToolCall`; hooks and tool implementations receive schema-valid argument values.
- Invalid arguments become error tool results for their calls and do not abort the whole agent run.
- Schema builders may improve C++ ergonomics but are not a second authoritative representation.
- Contract tests cover complex schema keywords, validation ordering, and invalid-argument lifecycle events.
