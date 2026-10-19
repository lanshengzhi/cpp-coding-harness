---
status: accepted
---

> **#1041 amendment (one adapter and request headers).** `ProviderStreamOptions`
> and the field-by-field handover from `SimpleStreamOptions` are retired: the
> Chat Completions adapter receives the caller's stream options and trusted
> request authentication, then resolves the request once inside the adapter.
> The earlier decision that caller header overrides and the transform hook
> remain extension points is superseded in part: caller overrides remain, and
> explicit null values carry deletions through the resolved header map. Its
> case-insensitive comparator preserves caller spelling and supports direct
> tombstone lookup, so adapter defaults stay suppressed; the unused public transform hook is
> removed. The `openai-completions` contract id has one named Model constant.
> Implements [#1041](https://github.com/lanshengzhi/cpp-coding-harness/issues/1041).

# The Chat Completions adapter is the Magpie gateway contract

Candidate 3 of the cch_ai architecture review was confirmed by the user. The
`openai-completions` adapter no longer specializes its request, header or response handling by
`Model::provider` or by typed `Model::compat`: it executes exactly one wire contract — the one
the Local LLM Router (Magpie) accepts and returns. This supersedes ADR 0063 and retires the
typed `compat` vocabulary introduced by ADR 0033 and deepened by ADR 0059; ADR 0072 and ADR 0073
had already made the gateway the only request destination.

## Context

ADR 0063 replaced `base_url` sniffing with `Model::provider` identity plus typed `compat`, so a
shipped catalog record could state its own wire behavior. Both channels served the three direct
provider paths — OpenRouter, DeepSeek and OpenAI — that ADR 0072 retired: every model request now
goes to one gateway that owns upstream authentication, account rotation, failover and wire
protocol normalization. What remained inside the adapter was a second, parallel specialization
layer: vendor branches in the payload builder, request headers and event decoding, plus the
`OpenAICompletionsCompat` alternatives and their Model validation. That layer could only produce
behavior for a caller that is not the gateway, and its tests pinned per-vendor request bytes
rather than the one contract Pike now depends on.

## Decisions

- Remove `Model::compat` and the `OpenAICompletionsCompat` vocabulary (the struct and the
  `OpenAICompletionsMaxTokensField`, `OpenAICompletionsThinkingFormat` and
  `OpenAICompletionsCacheControlFormat` enums) together with the `validate_model` compat rule.
  A Model describes identity and capability; capability variation that survives — the thinking
  map, input modalities and cost — stays typed Model data. No generic compat bag replaces it.
- Build one request body for every model: `messages` (the instruction is always the `system`
  role), `model`, `stream`, `stream_options.include_usage`, `max_completion_tokens` when
  nonzero, `temperature` when set, `tools` when the context or history has any,
  `reasoning_effort` for a reasoning model, and `prompt_cache_key` for a session-scoped request.
  Retire `store`, the `max_tokens` alternate, the vendor `thinking`/`reasoning`/
  `enable_thinking` shapes, `prompt_cache_retention`, `cache_control` injection, the tool
  `strict` flag and the `developer` instruction role. Caller header overrides and the transform
  hook remain the extension point.
- State thinking as `reasoning_effort` alone. Off — an absent option or an explicit Off clamp —
  sends `"none"` whenever the model reasons; a level maps to its catalog value, and a level the
  map does not mention falls back to the level's own name. A catalog `{"effort":"none"}` record
  maps onto the Off level so the thinking cycle can offer off exactly where the gateway lists
  it; without that record Off stays unselectable. Capability continues to come only from the
  discovered catalog.
- Decode every reasoning spelling the gateway can relay — `reasoning_content`, `reasoning` and
  `reasoning_text` — plus `reasoning_details`, recording the field name that arrived as the
  thinking signature so replay sends it back the same way. Retire the `opencode-go`
  `reasoning`-to-`reasoning_content` remap.
- Carry session identity in one header, `X-Magpie-Session`, set when the request has a session
  id and neither a caller value nor a tombstone decides otherwise. Retire `x-opencode-session`
  and the OpenRouter-gated `x-session-id`. Keep `prompt_cache_key` (the session id under
  non-None cache retention, with the existing 64-character clamp) for every model.
- Pass tool-call ids through unchanged in both directions. The pi-compatible id normalization
  (`<call>_<item>` rewriting and the 40-character OpenAI truncation) served a retired direct
  path; with it gone, an assistant tool call and its tool result stay paired by construction
  instead of by cooperative translation.
- Keep one tolerant Chat usage normalizer, `normalize_completions_usage`: cached-token
  subtraction with the DeepSeek `prompt_cache_hit_tokens` fallback, cache-write subtraction,
  reasoning as informational, and the shared cost rules. Delete the Responses, deepseek and
  Anthropic usage helper families with the adapters that consumed them.
- Modernize the tests to assert the unified contract rather than vendor-boundary bytes. The
  DeepSeek Chat Completions wire bundle keeps its response-decode acceptance (the raw SSE and
  the `-ts-events.json` snapshot); its request bytes stay committed as captured provenance and
  are no longer compared byte-for-byte. Provider-identity cases assert the body is invariant
  across provider ids, and the affinity cases collapse to the gateway session header.

## Gateway evidence

Verified against the clean Magpie source checkout at commit `d783df46` in
`/home/lansy/Work/github/magpie`:

- `internal/gateway/chat.go` takes `max_completion_tokens`, `reasoning_effort` and
  `prompt_cache_key` as first-class Chat fields, reads `reasoning_effort: "none"`
  case-insensitively as thinking off, and leaves a request that states its own
  `reasoning_effort` as it is (no vendor re-derivation from a `thinking` object).
- `internal/gateway/gateway.go` defines `SessionHeader = "X-Magpie-Session"` and
  `cacheKeyField = "prompt_cache_key"`, and lists `store`, `prompt_cache_retention`,
  `thinking`, `enable_thinking` and `stream_options` among the optional fields it retries
  without when an upstream refuses one by name: field specialization for an upstream is the
  gateway's concern, not the client's.
- `internal/gateway/wire_effort.go` (`thinkingBackOn`) with `offRefused` records the gateway
  taking a thinking-off request back at the model's own level when an upstream refuses
  reasoning-off, so `"none"` is safe to state unconditionally.
- `internal/gateway/session_route.go` resolves the client-named session from
  `X-Magpie-Session` first, ahead of an agent's own session headers.

## Verification

Focused validation runs the `ai` label excluding `architecture` plus the `architecture` label
(the change touches the public Model header and the include surface); Full Validation runs the
whole suite. The acceptance is the property, not a stand-in: request-shape tests assert the
unified body and its invariance across provider ids (`provider identity does not change the
Chat Completions body`), the explicit off value (`an off thinking request states the gateway's
explicit none`, plus the Kimi level matrix), the two session carriers (`session affinity is one
gateway header for every provider`, `the prompt cache key follows the session not the provider
id`), the catalog mapping (`MagpieCatalog maps a gateway none level onto the Off thinking
level`), replay pairing with the received ids, and the Chat usage split (`Chat Completions
usage splits uncached cached and reasoning tokens`, still checked against
`usage/normalization-and-cost.json`). The `ModelTest` compat validation and compat-preservation
cases are removed with the field, and the DeepSeek fixture case keeps its SSE decode and event
snapshot with request assertions expressed as the unified contract.
