---
status: accepted
---

# Source wire protocol specialization from the Model provider identity

`cch_ai` no longer infers a provider's wire protocol from the text of `Model::base_url`. The
`openai-completions` payload builder and adapter select OpenRouter, DeepSeek, and OpenAI
specialization from `Model::provider` — the explicit identity ADR 0029 already defines as the
Provider/authentication selector — and from a model's typed `compat`. `src/ai/api/ProviderDetection.hpp`
and its case-insensitive substring search are deleted.

## Context

ADR 0029 fixed the `Model` contract: `model.provider` selects Provider/authentication identity and
`model.api` selects protocol execution. ADR 0033 and ADR 0059 then deepened the per-API typed `compat`
so that a shipped catalog record can state its own wire behavior. What survived underneath both is a
second, parallel identity channel: `ProviderDetection.hpp` case-insensitively searched `base_url` for
`openrouter.ai` and `deepseek.com`, and `CompletionsPayload.cpp` independently searched it for
`api.openai.com`. Three consequences followed.

**A transport string decided protocol semantics.** A user-supplied `base_url` — written by the
operator, mutable at any time, and unrelated to who serves the request — silently changed the request
body and headers. Editing an endpoint could add `x-session-id`, swap `max_completion_tokens` for
`max_tokens`, or move the thinking channel between `reasoning.effort` and `reasoning_effort` with no
configuration change and no diagnostic.

**It failed in both directions.** A provider fronted by a corporate gateway or a self-hosted relay
keeps its own id and gets a non-vendor `base_url`; nothing about the vendor disappears, yet the URL
branch could not see it. Conversely a provider id of `corp-gateway` pointed at `https://openrouter.ai`
acquired OpenRouter's `x-session-id` affinity header and `reasoning` object, fields that endpoint family
does not document for it.

**It duplicated the typed compat contract and drifted from it.** The URL branch could only ever
reconstruct a fraction of what `OpenAICompletionsCompat` already states — the shipped OpenRouter
catalog carries `cacheControlFormat` and `supportsDeveloperRole` that the heuristic could not read —
so part of the specialization lived in a hidden string match while the rest lived in the catalog. The
heuristic also had no offline oracle: nothing tested it, and its own source carried a `// debt:`
marker rather than a tested contract.

**One catalog field was dropped on the floor.** `sendSessionAffinityHeaders` is populated on all 373
shipped OpenRouter `openai-completions` records and on no other record, but no C++ reader existed for
it: `BuiltinProviders.cpp` never parsed it and nothing consumed it, so the one decision that has
pinned catalog data behind it was the one decision hardcoded to a provider-id literal.

## Wire specialization is keyed by provider identity, then by typed compat

`Model::provider` is the default selector for provider-specific wire behavior. `base_url` is a
transport address only: it locates the endpoint and never changes what is sent to it. The resolved
defaults in `CompletionsPayload.cpp` are keyed on the provider id (`openrouter` and `deepseek` supply
thinking format, max-token field, developer-role, store, and cache-control defaults), each overridden
field-by-field by the model's typed `compat` when it carries one. The OpenAI `prompt_cache_key` in
`CompletionsPayload.cpp` reads the same `Model::provider` field, matching the existing provider-id
check already present in the same file (`normalize_completions_id`) and the `opencode-go` provider-id
checks in `RequestHeaders.hpp` and `CompletionsEvents.cpp`.

The session-affinity header is the one decision the catalog already answers, so it is stated in the
typed contract rather than only in code: `OpenAICompletionsCompat` gains
`send_session_affinity_headers`, `BuiltinProviders.cpp` parses the catalog's
`sendSessionAffinityHeaders`, and `OpenAICompletionsAdapter.cpp` follows the flag when the model
carries one and the `openrouter` provider id otherwise. The shipped catalog sets the flag on every
OpenRouter Completions record, so built-in wire behavior is identical through both paths; a model
that sets it either way now governs the header, which nothing could do before. The field is
representable under ADR 0059's rule — it is populated by the shipped catalog, exactly like the
`supportsStrictMode` field the same reconciliation carries.

`ProviderDetection.hpp` — `contains_case_insensitive`, `is_openrouter`, `is_deepseek`, and the
`// debt:` marker — is deleted, and the two translation units that included it read the provider id
directly. No new seam, enum, or interface is introduced.

## Considered options

- **Keep the URL heuristics and add a `compat` surface to `models.json`**: rejected. It keeps the
  fragile channel alive and only adds a second way to reach the same decision, while reintroducing the
  config surface ADR 0033 deliberately declined. The stable half of the heuristic — the provider-id
  match — is exactly what this decision keeps.
- **Delete the derived defaults and require every model to carry a typed `compat`**: rejected. A
  config-only provider composes models with `compat = std::nullopt` (ADR 0033), so the derived
  provider-id defaults are the only specialization a user-configured DeepSeek or OpenRouter endpoint
  gets. Deleting them would be a real functional regression, not a simplification.
- **Add a typed endpoint-identity enum on `Model`**: rejected. `Model::provider` is already that
  field, already authoritative, already populated on both the built-in and config paths, and already
  named by ADR 0029. A second identity field would be the duplication this decision removes.
- **Parse the `base_url` host into a typed value once and match on that**: rejected. It is the same
  transport string, only with better spelling; the proxy case still fails and the coupling remains.
- **Move vendor specialization into the composed `Provider`**: rejected. `provider` is a passive
  credential-free value on every request, and ADR 0047 keeps Provider composition private to
  `cch_ai`; making the payload depend on runtime Provider state rather than the request's own Model
  would invert that.
- **Stamp typed compat from `provider_id` at catalog load**: rejected as dead code where it was
  proposed. Every built-in `openai-completions` record already carries a typed `compat`, so stamping
  at catalog load would have nothing to stamp; the only models lacking one are the config-only
  providers of ADR 0033, which compose outside `cch_ai` and whose `compat` is fixed at `std::nullopt`
  precisely because "the C++ config schema has no compat surface at all". Stamping them instead would
  contradict that decision to make a value non-null for no observable gain. The one decision with
  pinned data behind it is now stated in the typed contract, which is the part of the stamp that had
  something to do.

## Consequences

- **`base_url` is inert with respect to the request body.** Every `openai-completions` vendor
  decision — the vendor default set and the OpenAI prompt-cache key — now follows `Model::provider`,
  and the session-affinity header follows the typed flag or that id as the default. `base_url` no
  longer reaches any of them; `src/ai/api/CompletionsPayload.cpp` and
  `src/ai/api/OpenAICompletionsAdapter.cpp` each read the field directly, and there is no detection
  header left in the module.
- **A custom `models.json` provider that fronts a vendor must be named after it.** A provider entry
  keyed `openrouter`, `deepseek`, or `openai` gets that vendor's wire behavior whatever its `baseUrl`
  says; an entry keyed `corp-gateway` pointed at `https://openrouter.ai` no longer does. This is the
  intended migration, and it is the same rule the CLI already exposes when a model is selected as
  `provider/model`. It is a user-visible change in three places, not one: the OpenRouter affinity
  header, the DeepSeek request defaults, and the OpenAI `prompt_cache_key`, the last of which the
  review of this decision found was not named in the originating analysis and is recorded here
  because it changes the same way.
- **The shipped catalog is unchanged and its `sendSessionAffinityHeaders` records start being read.**
  Every built-in `openai-completions` record already carries both its provider id and a typed
  `compat`, and the parsed flag is `true` on exactly the 373 OpenRouter records whose provider id
  already produced the header, so built-in wire behavior is identical before and after. Exactly one
  built-in decision still depends on the derived provider-id defaults — `supportsDeveloperRole` for
  OpenRouter's 107 `openai/`- and `anthropic/`-prefixed models, which the catalog leaves unset because
  the rule is structural — and the derived `anthropic/` cache-control rule agrees with the catalog's
  explicit `cacheControlFormat` records.
- **The contract is tested, not asserted.** `tests/ai/api/OpenAICompletionsAdapterTest.cpp` pins both
  directions: a vendor provider id behind a non-vendor gateway base URL keeps its `x-session-id`
  header, developer role, `reasoning.effort`, `thinking` type, and `max_tokens` field, while a
  non-vendor provider id on a vendor host gets the plain OpenAI defaults and no affinity header. The
  typed flag is pinned in both directions against the provider-id default, and the OpenAI
  `prompt_cache_key` is pinned against the endpoint host. The former `// debt:` marker is resolved by
  construction — the code it described no longer exists.
- **No backward compatibility.** There is no fallback read, no alias, and no configuration migration
  shim; the behavior change is the decision.

## References

- [ADR 0019](0019-make-model-and-stream-options-first-class-values.md) (typed per-API `compat`),
  [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md) (`model.provider`
  selects Provider/authentication identity), [ADR
  0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md) (config-only
  providers compose with no `compat`; the adapter surface), [ADR
  0047](0047-own-provider-assembly-inside-cch-ai-and-hide-the-provider-capability.md) (private
  Provider composition), [ADR
  0059](0059-align-the-cch-ai-capability-subset-with-pi-ai-and-diverge-on-kimi-code.md) (the shipped
  catalog's typed `OpenAICompletionsCompat`).
- `src/ai/BuiltinProviders.cpp`, `src/ai/api/CompletionsPayload.cpp`,
  `src/ai/api/OpenAICompletionsAdapter.cpp`, `src/ai/include/cch/ai/Model.hpp`,
  `tests/ai/api/OpenAICompletionsAdapterTest.cpp`.
- The originating analysis this decision answers: `.scratch/cch_ai.md` §5.1 item 3, §5.2 candidate 1,
  and §5.3. It proposed deepening `Model::compat` as the single source of truth and stamping typed
  tags from `provider_id` at catalog load; this ADR adopts the typed-contract half and rejects the
  stamping half for the reason recorded above.
