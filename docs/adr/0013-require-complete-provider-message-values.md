---
status: accepted
---

# Require complete provider-message values

Provider Messages preserve pi's completeness invariants: every assistant value has API, provider, model, usage, stop reason, and timestamp, while provider-specific breakdowns and tool-result metadata remain optional only where pi makes them optional. Wire parsing rejects missing required fields instead of manufacturing partial domain values, giving C++ callers stronger compile-time and construction invariants.

## Considered options

- Make most metadata optional to accommodate incomplete providers: rejected because adapters can normalize unavailable counters while optional core identity forces every caller to handle invalid states.
- Preserve unknown usage by making the whole value optional: rejected because pi's stable zero-initialized usage shape is the supported semantic contract; genuinely unknown breakdowns remain optional fields within it.
- Require complete passive values and normalize at adapters: accepted because provider quirks stay behind the capability seam.

## Consequences

- Assistant construction always supplies API, provider, model, usage, stop reason, and a real timestamp.
- Usage includes pi-supported optional breakdowns such as reasoning; tool results include optional usage and added-tool names.
- Diagnostics preserve the supported string-or-number code shape.
- Deserialization of missing required fields fails explicitly.
- Implementations use designated initialization or named builders and response tests to prevent aggregate-field misbinding.
