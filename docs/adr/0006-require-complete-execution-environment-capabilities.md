---
status: accepted
---

# Require complete execution-environment capabilities

The public execution environment is composed from complete filesystem and shell capability contracts whose required operations have no default `NotSupported` implementations. The existing tool-shaped file and shell methods are removed in favor of one pi-aligned authoritative surface: capability availability is decided explicitly during session/tool assembly, not discovered after a method is invoked.

## Considered options

- Retain legacy tool-shaped methods beside pi-shaped methods: rejected because two authoritative paths can drift and compatibility-only surfaces are outside this repository's policy.
- Keep default `NotSupported` methods for easier custom implementations: rejected because an object can claim a complete capability while omitting essential behavior.
- Split every operation into a separate interface: rejected because it would make ordinary environments and tool assembly unnecessarily shallow and fragmented.
- Use complete filesystem and shell interfaces and compose them as an execution environment: accepted because it preserves pi's capability semantics and makes C++ substitutability explicit.

## Consequences

- Built-in tools depend only on the authoritative filesystem and shell interfaces.
- Bash availability is represented by tool registration/session policy rather than `bash_enabled()` or a placeholder shell method.
- Truly optional future capabilities use a distinct interface or an explicit optional/value contract.
- External execution-environment implementations must satisfy the complete advertised contract at compile time.
