---
status: accepted
---

# Align the cch_ai capability subset with pi-ai at a new baseline and diverge on Kimi Code

The `cch_ai` capability subset is re-pinned from the frozen baseline `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (the `@earendil-works/pi-ai@0.83.0`-correlated commit) to `1a584a7a5`, the `../pi` checkout's current `HEAD` after `v0.87.0`. This decides four things that [ADR 0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md) fixed at the old baseline, and one thing it did not cover. It implements [#757](https://github.com/lanshengzhi/cpp-coding-harness/issues/757).

## One built-in provider is a mirror of the same-id pi built-in

Every built-in provider `cch_ai` ships must mirror the pi built-in of the same id: same id, same `baseUrl`, the same set of APIs, and the same authentication methods. A provider whose `baseUrl` or API family differs from its pi namesake is not an aligned subset of it; it is a separate capability wearing the same name, and its "semantic parity" has no oracle to be checked against.

This is measured, not assumed. pi's `deepseek` and `openrouter` providers have used `openai-completions` since the previous baseline, and `opencode-go` has always mixed three API families; the C++ entries for all three were synthesized from the user-`models.json` fixture (`fixtures/pi-ai/models/models.json`, a config-only `deepseek` provider using `openai-responses`) and promoted to built-ins. The consequence is a table of drift that cannot be closed by editing catalog data: `deepseek`, `openrouter`, and `opencode-go` are wrong in their API family, and `openai` is wrong in both its `baseUrl` (`.../v1/responses` instead of `.../v1`) and its model count (4 against pi's 38).

Two consequences follow. The private adapter count rises from three to four, so **the exact three-adapter surface of ADR 0033 is replaced by four adapters** — `openai-codex-responses`, `openai-responses`, `anthropic-messages`, and `openai-completions` — while the "no registry, no placeholder, registration stays private" rule of ADR 0033 stands. And the supported/Deferred criterion of ADR 0033 is applied to the newest pi surface: a compat field is Supported when a shipped catalog populates it and a scoped consumer can reach it, and Deferred when only unscoped producers can.

## The Kimi scoped path moves to the vendor-documented API-key surface

**Exception to the mirror rule.** `kimi-coding` keeps the pi provider's id but not its protocol or authentication: API key only (no subscription OAuth), `openai-completions` instead of `anthropic-messages`, and `https://api.kimi.com/coding/v1` instead of pi's `https://api.kimi.com/coding`.

The reason is a vendor contract, not a preference. Kimi Code's documentation directs third-party tools to obtain an API Key ("When integrating Kimi Code into third-party development tools, you need to manually configure an API Key to complete authentication"), publishes both the OpenAI-compatible (`https://api.kimi.com/coding/v1`, `https://api.kimi.ai/coding/v1` overseas) and Anthropic-compatible (`https://api.kimi.com/coding/`) base URLs over the same four model IDs, and states that altering the client identifier (`User-Agent`) is a violation that may suspend membership benefits. pi's `kimi-coding` path is a device-code OAuth flow against `KIMI_CODE_OAUTH_HOST`, which the public documentation does not describe, and the C++ catalog hardcoded `User-Agent: KimiCLI/1.5` on every Kimi model — impersonating a different vendor tool, exactly what the vendor prohibits. Following the vendor's documented surface is both the smaller implementation (the Kimi OAuth flow, its tests, and its persistence fixtures are deleted with no shim) and the defensible one.

**Consequences of the exception.** Kimi's catalog oracle is the vendor documentation rather than a pi data file, so its values are pinned by a cited fixture with each divergence from pi's catalog recorded — `kimi-for-coding`'s context window is `1048576` per the vendor's K2.8 Preview rollout against pi's `262144`, and the effort-capable models keep the vendor's three real levels (`low`/`high`/`max`, `off` unsupported). The `anthropic-messages` adapter's only remaining consumer is `opencode-go`'s four Anthropic-family models. Treating the exception as recorded rather than silent is what keeps the mirror rule above falsifiable.

**Every Kimi reasoning map is fully populated, and the no-knob model declares no usable effort level.** Reasoning-effort resolution consults the model's map only when the requested level's key is *present*; a missing key falls through to the level's own name. The vendor rejects an unknown effort value outright, so upstream's sparse maps — and its outright absent map on `kimi-for-coding-highspeed` — would put a rejected value on the wire. The three effort-capable models therefore carry all seven keys with the three real levels mapped and the rest explicitly unsupported, and `kimi-for-coding-highspeed`, which the vendor documents as thinking-always-on with no effort control, maps every level to unsupported so that resolution yields no effort parameter at all. A model with reasoning enabled and no selectable level is deliberate here, not an oversight.

## The bundled catalog becomes generated output

A faithful mirror of the six providers is 343 models and roughly 141 KB of data, 110 KB of it `openrouter` alone. Hand-transcribing that into `DefaultModelsJson.hpp` guarantees drift, and the previous 466-line header already drifted on two providers. The catalog is therefore generated: `src/ai/DefaultModelsJson.cpp` is produced by a committed script from the vendored pinned pi data, with a regeneration test asserting byte-identical output. The bundled, offline catalog of [ADR 0058](0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md) is unchanged in kind — compile-time data parsed by the existing support JSON reader — and only its authorship and storage unit change.

## Considered options

- **Keep the six built-ins but only refresh their model data**: rejected — three of them are wrong in API family, not merely stale, so refreshing ids would leave providers whose observable wire behavior differs from their pi namesake while claiming its name.
- **Drop the three synthesized providers and keep `openai-codex`, `kimi-coding`, `openai`** (no fourth adapter): rejected on measured usage — `deepseek`, `openrouter`, and `opencode-go` are in daily use, so the "simplification" would remove working providers rather than remove complexity.
- **Narrow to `openai-responses` only**: measured at roughly 15% of `src/ai` (the Anthropic adapter, Kimi OAuth, and their shared branches) while abandoning the Kimi and OpenCode Go paths; the complexity of `cch_ai` is concentrated in the Owner Interface contract, the transports, the Responses-family payload/event processing, and OAuth, none of which the cut touches.
- **Keep the hardcoded `KimiCLI/1.5` client identifier**: rejected — the vendor documents it as a violation, and it makes our requests claim an identity that is not ours.
- **Carry `supportsStrictMode` without implementing strict tool schema conversion**: rejected — the field is wire-visible (`strict` on tool definitions) and its producer is a tool declaration in `cch_agent` that does not exist yet, so the field would be inert (ADR 0019).
- **Reintroduce a generic compat JSON bag**: rejected, unchanged from ADR 0033 — compat stays typed per API, and only the fields a shipped catalog populates are representable.

## Consequences

- **Four private adapters.** `openai-completions` is selected only by a Model whose `api` is `openai-completions`; its typed compat is limited to `supportsStore`, `supportsDeveloperRole`, `maxTokensField`, `requiresReasoningContentOnAssistantMessages`, `thinkingFormat` (`deepseek`, `openrouter`, `qwen`), `cacheControlFormat` (`anthropic`), `supportsLongCacheRetention`, and `supportsReasoningEffort`. The remaining pi `thinkingFormat` values and compat fields are unrepresentable rather than defaulted.
- **Typed compat gains a second API.** `OpenAICompletionsCompat` is new; `OpenAIResponsesCompat` is introduced with only the wire-affecting fields a shipped catalog populates (`supportsMaxOutputTokens`, `supportsExplicitPromptCacheMode`). `AnthropicMessagesCompat` keeps `forceAdaptiveThinking`/`allowEmptySignature` and gains the fields the `opencode-go` catalog populates.
- **Deferred, with no placeholders**: strict/grammar tool schemas and tool search, mid-conversation system messages (`SystemMessage.sections`/`toolsAdded`/`toolsRemoved`), dynamic catalog refresh (`refreshModels`/`filterModels`/`ModelsStore` — none of the six providers declares one), `inputLimits`/image preprocessing, `samplingParams`, `toolChoice`, `metadata`, `onPayload`/`onResponse`, deferred responses, `promptCache`, telemetry, images API, and every provider family outside the six.
- **`openrouter` gains OAuth** alongside its API key, matching pi: a PKCE flow whose exchange yields a permanent API key, so the credential is api-key-shaped and has no refresh path.
- **Client identity is the harness's own.** No catalog or adapter sends another tool's identifier; the vendor-documented prohibition is the reason the Kimi catalog's `User-Agent` value is deleted rather than updated.
- **Catalog parity is a tested property.** pi's six provider data files are vendored verbatim under `fixtures/pi-ai/models/providers/`, sha256-pinned, and one exhaustive comparison test asserts every field `Model` carries for every built-in model. A drifted catalog fails; the hand-written shard expectations it replaces could not.
- **The baseline is reproducible.** `fixtures/pi-ai/README.md` and this ADR name the exact commit `1a584a7a5`, so "latest" is never implicit.
- **Downstream, next round**: Kimi's login presentation and the `cch_agent`-side tool declarations (strict sampling) are `cch_agent`/TUI work and are not part of this change; the Kimi OAuth removal makes the login surface smaller there.

## References

- Issue [#757](https://github.com/lanshengzhi/cpp-coding-harness/issues/757).
- [ADR 0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md) (superseded in the adapter count and the Kimi scoped path; its privacy, typed-compat, and Supported/Deferred rules stand), [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md), [ADR 0019](0019-make-model-and-stream-options-first-class-values.md), [ADR 0058](0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md).
- New baseline `1a584a7a5` (`../pi`), superseding `83114817c68f5413e4d7ba6d7003ddc511cd31d2`: `packages/ai/src/providers/*.ts`, `packages/ai/src/providers/data/*.json`, `packages/ai/src/api/{openai-completions,openai-responses,openai-responses-shared,anthropic-messages,openai-codex-responses}.ts`, `packages/ai/src/{models.ts,types.ts}`, `packages/ai/src/auth/*`.
- Kimi Code vendor documentation: <https://www.kimi.com/code/docs/> (API access, service endpoints, model IDs, effort mapping, client-identifier notice).
