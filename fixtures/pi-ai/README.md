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

The #339 retry and timeout evidence is intentionally transport-independent: tests pin retry classification,
Retry-After/exponential delays and their 60-second default bound, while `timeout_ms` is verified as a prepared
Provider value. Actual SSE response-header timeout and retry execution belong to the adapter transport tickets.
All fixture credential-like strings are distinguishable `dummy-*` values; none came from a live service.

The full adapter/auth/persistence capability checklist and shard hash land with the pi-ai completion gate (#347).
