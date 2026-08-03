# pi-ai compatibility fixtures

These committed Model-shape goldens are copied from the supported contract at the frozen pi baseline
`83114817c68f5413e4d7ba6d7003ddc511cd31d2` (`packages/ai/src/types.ts` and
`packages/agent/src/agent.ts`). They contain no credentials or secret-derived values.

- `models/default-model.json` mirrors pi Agent's `DEFAULT_MODEL`; omitted optional members mean no thinking map,
  headers, cost tiers, or API compat.
- `models/complete-anthropic-model.json` exercises every supported Model field. Its absent `off` thinking key means
  provider default, while `"low": null` means that level is unsupported.
- `auth-storage/round-trip-input.json` and `auth-storage/round-trip-expected.json` pin the shared `auth.json`
  read-modify-write contract: known API-key/OAuth records, Codex `accountId`, unknown provider records, and OAuth
  extra fields survive whole-file persistence. Every credential-like value is an explicit `dummy-*` token; no fixture
  value was copied from a live credential.

Issue #339 adds transport-independent goldens derived from the same frozen baseline:

- `conversion/openai-codex-responses.json`, `conversion/openai-responses-deepseek.json`, and
  `conversion/anthropic-messages-kimi.json` pin normalized message histories as the three request payload shapes.
  The fixture tests construct the domain histories independently and compare canonical JSON. They cover system-prompt
  placement, image blocks, encrypted/thinking signatures, Responses text-signature v1 replay, normalized tool-call
  ids, grouped Anthropic tool results with `is_error`, adaptive thinking, and cache/session fields.
- `usage/normalization-and-cost.json` pins Responses cache subtraction, the DeepSeek uncached/cached split,
  Anthropic partial usage, reasoning details, cost-tier selection, and the one-hour cache-write rule.
- `termination/matrix.json` pins the Responses and Anthropic stop-reason matrices, including missing-terminal errors.

Issue #340 adds the DeepSeek `openai-responses` wire goldens:

- `wire/openai-responses-deepseek-ts-request.json` is the canonical byte snapshot of the exact text-only request
  produced by frozen TS `buildParams`/`convertResponsesMessages` at
  `packages/ai/src/api/{openai-responses,openai-responses-shared}.ts`. The C++ request is compared directly to these
  bytes through the `Models` seam; it includes the developer-role prompt, full-context payload, fixed compat
  defaults, cache fields, and no `previous_response_id`.
- `wire/openai-responses-deepseek.sse` pins the frozen `processResponsesStream` input sequence from that same TS
  source, including reasoning, text, function-call deltas, an unknown event, the completed terminal, and DeepSeek
  cached/reasoning usage fields. `wire/openai-responses-deepseek-ts-events.json` is the corresponding TS assistant
  event-sequence snapshot compared to the C++ output.

Issue #341 adds the frozen four-model Kimi catalog from the published `pi-ai@0.83.0` shard and the Kimi
`anthropic-messages` wire goldens. The catalog keeps `forceAdaptiveThinking` on all four models,
`allowEmptySignature` only on `k3` and `kimi-for-coding`, and `off: null` on the K3 variants.

- `wire/anthropic-messages-kimi-ts-request.json` is the canonical byte snapshot of the frozen Kimi request payload.
  It covers system extraction, images, empty and redacted thinking signatures, normalized tool-call ids, grouped
  `tool_result` blocks with `is_error`, adaptive thinking, effort, and cache markers.
- `wire/anthropic-messages-kimi.sse` pins the frozen raw Anthropic event sequence, including initial block content,
  text/thinking/signature/tool deltas, malformed-event and incremental-tool JSON repair, partial usage fields,
  one-hour cache-write detail, an unknown event, and the mandatory `message_stop` terminal.
  `wire/anthropic-messages-kimi-ts-events.json` is the corresponding TS event-name snapshot compared through the
  `Models` seam. Separate adapter cases pin every stop reason, missing terminals, raw SSE errors, retries, scratch
  cleanup, and cancellation.

The adapter tests execute configured retries and cancellation against injected fake HTTP. `timeout_ms` is forwarded
as the response-header bound; after headers the production SSE transport is governed by caller cancellation.
The transport-independent retry classification, terminal matrix, and usage/cost goldens from #339 remain shared by
all scoped adapters. All fixture credential-like strings are distinguishable `dummy-*` values; none came from a
live service.

The full adapter/auth/persistence capability checklist and shard hash land with the pi-ai completion gate (#347).
