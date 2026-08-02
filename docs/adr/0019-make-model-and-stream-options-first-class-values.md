---
status: accepted
---

# Make model and stream options first-class values

The AI module defines aggregate-friendly Model and per-call Stream Options values as the authoritative identity, capability, and request-policy contracts. Agent state holds a valid Model, and prepare-next-turn model or thinking changes must alter the next provider request; authentication, connection ownership, model discovery, and provider execution remain behind capability seams rather than being embedded in those passive values.

> Refined by [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md): the Model field set is now fixed to pi's supported shape (including an optional typed per-API `compat` and the null-aware `thinkingLevelMap`), Agent state holds a concrete Model with an internal pi-aligned `kDefaultModel`, and Stream Options are the pi subset the three supported paths consume (`temperature`, `maxTokens`, cancellation, `apiKey`, `headers`, `env`, `transformHeaders`). The passive-value and capability-seam principles below are unchanged.

## Considered options

- Continue passing only a model string: rejected because capability checks, reasoning policy, request limits, and restored provider identity become scattered and updates can appear successful without affecting execution.
- Put provider clients and credentials into Model: rejected because passive data would acquire capability ownership and secret-lifetime responsibilities.
- Copy every current pi model field before it is supported: rejected because placeholder fields would create false capability claims. (ADR 0029 later fixed the exact supported field set at the parity baseline, including typed per-API `compat`; the prohibition on generic compatibility bags and inert fields stands.)
- Add supported pi-semantic fields incrementally to passive Model and Stream Options contracts: accepted because values stay honest and extensible.

## Consequences

- Model identity and supported capabilities have one public representation.
- Stream Options carry supported per-call policy such as thinking, cancellation, timeouts, and headers without changing provider ownership.
- Agent model/thinking updates are validated and become observable in the next request.
- Provider registries resolve values to capabilities; they do not replace Model as an identity string map.
- Deferred provider/catalog features add no inert public fields.
- Agent state holds a concrete Model, never an optional one: when no configured model resolves, an internal `kDefaultModel` mirrors pi's `DEFAULT_MODEL` (`"unknown"` identity, empty endpoint, zero capabilities/cost) and streaming fails through normal Provider lookup until a real model is selected (ADR 0029).
