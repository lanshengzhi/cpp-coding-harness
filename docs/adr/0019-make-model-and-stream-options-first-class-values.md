---
status: accepted
---

# Make model and stream options first-class values

The AI module defines aggregate-friendly Model and per-call Stream Options values as the authoritative identity, capability, and request-policy contracts. Agent state holds a valid Model, and prepare-next-turn model or thinking changes must alter the next provider request; authentication, connection ownership, model discovery, and provider execution remain behind capability seams rather than being embedded in those passive values.

## Considered options

- Continue passing only a model string: rejected because capability checks, reasoning policy, request limits, and restored provider identity become scattered and updates can appear successful without affecting execution.
- Put provider clients and credentials into Model: rejected because passive data would acquire capability ownership and secret-lifetime responsibilities.
- Copy every current pi model field before it is supported: rejected because placeholder fields would create false capability claims.
- Add supported pi-semantic fields incrementally to passive Model and Stream Options contracts: accepted because values stay honest and extensible.

## Consequences

- Model identity and supported capabilities have one public representation.
- Stream Options carry supported per-call policy such as thinking, cancellation, timeouts, and headers without changing provider ownership.
- Agent model/thinking updates are validated and become observable in the next request.
- Provider registries resolve values to capabilities; they do not replace Model as an identity string map.
- Deferred provider/catalog features add no inert public fields.
