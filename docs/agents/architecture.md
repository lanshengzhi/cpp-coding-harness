# Architecture guardrails

Read this for changes to architecture, public headers, dependency direction, provider/tool/session contracts, CMake visibility, physical capabilities, or security boundaries. Accepted ADRs record the rationale; `CODING_STANDARDS.md` records code-level consequences.

## Passive value contracts

Data crossing Owner Interfaces is passive value state. Use aggregate-friendly `struct`, `std::variant`, `std::expected`, and `cch::support::JsonValue` values.

## Capability seams

Physical capabilities cross explicit seams. Chat clients, stream transports, execution environments, session stores, and tools use narrow interfaces or dependency-heavy implementations hidden behind Owner Interfaces.

## Connection strength

Connection strength is explicit. Ordinary Agent Session, TUI, status, and diagnostic observers are weak. Model-stream delivery and Agent-to-Session commitment are separately named strong, awaited, backpressured connections. Stored operations on both paths remain move-only; copying must be an explicit contract rather than an accidental `std::function` requirement. See ADR 0040.

## Local generic machinery

Generic and serialization machinery stays local. Glaze DTOs, schema conversion, visitors, parsing helpers, and similar machinery belong in serialization or implementation layers rather than Owner Interfaces.

## Security and containment

Shell, file, environment-variable, provider, and session changes preserve workspace containment, secret redaction, output truncation, and the documented “not a sandbox” boundary.

## Retired surfaces

Keep the clean end state: the legacy synchronous tool surface, `util::Result`, Boost.JSON domain contracts, `src` as a public include surface, and compatibility-only empty flags remain absent.
